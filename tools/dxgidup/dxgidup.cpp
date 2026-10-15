// dxgidup -- an independent DXGI Desktop Duplication present-rate meter.
//
// Why: the "source is only presenting every 17 ms at 4K" finding has so far been measured either
// through Sunshine (which owns the display mode and competes for the duplication interface) or through
// ffmpeg's ddagrab (whose dup_frames option pads the output up to the requested framerate and made a
// 60 Hz mode report 90-119 fps). This is a purpose-built meter with no padding and no other client:
// every successful AcquireNextFrame is one real desktop update, and it reports the wall-clock gaps
// between them together with DXGI's own LastPresentTime deltas and AccumulatedFrames.
//
// Usage:  dxgidup.exe [--seconds N] [--output INDEX] [--name SUBSTRING] [--verbose]
//   --output   select the DXGI output by index (default: first output of the first adapter)
//   --name     select the first output whose DeviceName contains SUBSTRING (e.g. \\.\DISPLAY3)
//   --verbose  print one line per acquired frame
//
// Note: DXGI allows limited concurrent duplication clients per output. If Sunshine is streaming, this
// tool will fail with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE -- which is itself a useful signal that a
// stream is live. Run it with no stream for a clean desktop-cadence measurement.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

static const char *hr_text(HRESULT hr) {
  switch (hr) {
    case S_OK: return "S_OK";
    case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
    case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    default: return "other";
  }
}

int main(int argc, char **argv) {
  int seconds = 20;
  int want_index = -1;
  const char *want_name = nullptr;
  int verbose = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--seconds") && i + 1 < argc) { seconds = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) { want_index = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) { want_name = argv[++i]; }
    else if (!strcmp(argv[i], "--verbose")) { verbose = 1; }
  }

  HRESULT hr;
  IDXGIFactory1 *factory = nullptr;
  hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory);
  if (FAILED(hr)) { printf("FATAL CreateDXGIFactory1 %s 0x%08lx\n", hr_text(hr), (unsigned long) hr); return 2; }

  IDXGIAdapter1 *chosen_adapter = nullptr;
  IDXGIOutput *chosen_output = nullptr;
  int seen = 0;
  DXGI_OUTPUT_DESC chosen_desc {};

  for (UINT a = 0; !chosen_output; a++) {
    IDXGIAdapter1 *adapter = nullptr;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) { break; }
    DXGI_ADAPTER_DESC1 adesc {};
    adapter->GetDesc1(&adesc);
    for (UINT o = 0; !chosen_output; o++) {
      IDXGIOutput *output = nullptr;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) { break; }
      DXGI_OUTPUT_DESC odesc {};
      output->GetDesc(&odesc);
      printf("# output[%d] adapter=\"%ls\" device=\"%ls\" attached=%d\n", seen, adesc.Description, odesc.DeviceName, (int) odesc.AttachedToDesktop);
      int take = 0;
      if (want_name) {
        char narrow[64] = {0};
        wcstombs(narrow, odesc.DeviceName, sizeof(narrow) - 1);
        if (strstr(narrow, want_name)) { take = 1; }
      } else if (want_index >= 0) {
        if (seen == want_index) { take = 1; }
      } else if (seen == 0) {
        take = 1;
      }
      if (take) {
        chosen_adapter = adapter;
        chosen_output = output;
        chosen_desc = odesc;
      } else {
        output->Release();
      }
      seen++;
    }
    if (chosen_adapter != adapter) { adapter->Release(); }
  }

  if (!chosen_output) { printf("FATAL no matching output (saw %d)\n", seen); return 2; }
  printf("# using device=\"%ls\" attached=%d\n", chosen_desc.DeviceName, (int) chosen_desc.AttachedToDesktop);

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL level {};
  hr = D3D11CreateDevice(chosen_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr)) { printf("FATAL D3D11CreateDevice %s 0x%08lx\n", hr_text(hr), (unsigned long) hr); return 2; }

  IDXGIOutput1 *output1 = nullptr;
  hr = chosen_output->QueryInterface(__uuidof(IDXGIOutput1), (void **) &output1);
  if (FAILED(hr)) { printf("FATAL QueryInterface(IDXGIOutput1) %s\n", hr_text(hr)); return 2; }

  IDXGIOutputDuplication *dupl = nullptr;
  hr = output1->DuplicateOutput(device, &dupl);
  if (FAILED(hr)) {
    printf("FATAL DuplicateOutput %s 0x%08lx", hr_text(hr), (unsigned long) hr);
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) { printf("  (a stream or another duplication client is active)"); }
    printf("\n");
    return 3;
  }

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  std::vector<double> walls;      // wall-clock ms (QPC since start) of each successful acquire
  std::vector<double> present;    // DXGI LastPresentTime in ms
  std::vector<unsigned int> accums;
  const char *stop_reason = "time";
  int presents = 0;

  while (1) {
    QueryPerformanceCounter(&t1);
    if ((t1.QuadPart - t0.QuadPart) / (double) freq.QuadPart >= seconds) { break; }

    DXGI_OUTDUPL_FRAME_INFO info {};
    IDXGIResource *res = nullptr;
    hr = dupl->AcquireNextFrame(500, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { continue; }
    if (hr == DXGI_ERROR_ACCESS_LOST) { stop_reason = "access-lost"; break; }
    if (FAILED(hr)) { stop_reason = hr_text(hr); break; }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double wall = (double) (now.QuadPart - t0.QuadPart) * 1000.0 / (double) freq.QuadPart;
    walls.push_back(wall);

    if (info.LastPresentTime.QuadPart) {
      double p = (double) info.LastPresentTime.QuadPart * 1000.0 / (double) freq.QuadPart;
      present.push_back(p);
    }
    accums.push_back(info.AccumulatedFrames);
    if (verbose) {
      printf("t=%.1fms present_qpc=%lld accum=%u\n", wall, (long long) info.LastPresentTime.QuadPart, info.AccumulatedFrames);
    }
    presents++;
    if (res) { res->Release(); }
    dupl->ReleaseFrame();
  }

  QueryPerformanceCounter(&t1);
  double span_ms = (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) freq.QuadPart;

  printf("# presents=%d span=%.0fms rate=%.2f Hz stop=%s\n", presents, span_ms, presents * 1000.0 / span_ms, stop_reason);

  // Wall-clock gaps between successful acquires: should match the present-time gaps when every present
  // is picked up, and diverge when the desktop presents while we are busy (a missed frame shows up as a
  // doubled wall gap).
  if (walls.size() > 2) {
    std::vector<double> wg;
    for (size_t i = 1; i < walls.size(); i++) { wg.push_back(walls[i] - walls[i - 1]); }
    std::sort(wg.begin(), wg.end());
    double sum = 0; for (double v : wg) { sum += v; }
    printf("# wall-interval n=%d mean=%.2f median=%.2f p90=%.2f max=%.2f ms\n",
           (int) wg.size(), sum / wg.size(), wg[wg.size() / 2], wg[(size_t) (wg.size() * 0.9)], wg.back());
  }

  std::vector<double> present_gaps;
  for (size_t i = 1; i < present.size(); i++) { present_gaps.push_back(present[i] - present[i - 1]); }
  if (!present_gaps.empty()) {
    std::vector<double> s = present_gaps;
    std::sort(s.begin(), s.end());
    double sum = 0; for (double v : s) { sum += v; }
    const int buckets[8] = {5, 7, 9, 11, 14, 18, 22, 30};
    int counts[9] = {0};
    for (double v : s) {
      int b = 0;
      while (b < 8 && v >= buckets[b]) { b++; }
      counts[b]++;
    }
    printf("# present-interval(presenttime delta) n=%d mean=%.2f median=%.2f p90=%.2f max=%.2f ms\n",
           (int) s.size(), sum / s.size(), s[s.size() / 2], s[(size_t) (s.size() * 0.9)], s.back());
    printf("# histogram ms: <5=%d 5-7=%d 7-9=%d 9-11=%d 11-14=%d 14-18=%d 18-22=%d 22-30=%d >30=%d\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7], counts[8]);
    printf("# => median rate %.1f Hz\n", 1000.0 / s[s.size() / 2]);
  }
  if (!accums.empty()) {
    std::vector<unsigned int> a = accums;
    std::sort(a.begin(), a.end());
    printf("# AccumulatedFrames median=%u min=%u max=%u\n", a[a.size() / 2], a.front(), a.back());
  }

  dupl->Release();
  output1->Release();
  context->Release();
  device->Release();
  chosen_output->Release();
  chosen_adapter->Release();
  factory->Release();
  return 0;
}
