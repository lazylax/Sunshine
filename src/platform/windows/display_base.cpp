/**
 * @file src/platform/windows/display_base.cpp
 * @brief Definitions for the Windows display base code.
 */
// standard includes
#include <cmath>
#include <cstdlib>
#include <thread>

// platform includes
#include <initguid.h>

// lib includes
#include <boost/algorithm/string/join.hpp>
#include <boost/process/v1.hpp>
#include <MinHook.h>

// local includes
#include "utf_utils.h"

// We have to include boost/process/v1.hpp before display.h due to WinSock.h,
// but that prevents the definition of NTSTATUS so we must define it ourself.
/**
 * @brief Windows NT status code returned by native APIs.
 */
typedef long NTSTATUS;

// Definition from the WDK's d3dkmthk.h
/**
 * @brief Enumerates supported d3 DKMT GPU PREFERENCE QUERY STATE options.
 */
typedef enum _D3DKMT_GPU_PREFERENCE_QUERY_STATE : DWORD {
  D3DKMT_GPU_PREFERENCE_STATE_UNINITIALIZED,  ///< The GPU preference isn't initialized.
  D3DKMT_GPU_PREFERENCE_STATE_HIGH_PERFORMANCE,  ///< The highest performing GPU is preferred.
  D3DKMT_GPU_PREFERENCE_STATE_MINIMUM_POWER,  ///< The minimum-powered GPU is preferred.
  D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED,  ///< A GPU preference isn't specified.
  D3DKMT_GPU_PREFERENCE_STATE_NOT_FOUND,  ///< A GPU preference isn't found.
  D3DKMT_GPU_PREFERENCE_STATE_USER_SPECIFIED_GPU  ///< A specific GPU is preferred.
} D3DKMT_GPU_PREFERENCE_QUERY_STATE;  ///< Alias for D3 DKMT GPU PREFERENCE QUERY STATE.

#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  namespace bp = boost::process::v1;

  /**
   * DDAPI-specific initialization goes here.
   */
  int duplication_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;

    // Capture format will be determined from the first call to AcquireNextFrame()
    display->capture_format = DXGI_FORMAT_UNKNOWN;

    // FIXME: Duplicate output on RX580 in combination with DOOM (2016) --> BSOD
    {
      // IDXGIOutput5 is optional, but can provide improved performance and wide color support
      dxgi::output5_t output5 {};
      status = display->output->QueryInterface(IID_IDXGIOutput5, (void **) &output5);
      if (SUCCEEDED(status)) {
        // Ask the display implementation which formats it supports
        auto supported_formats = display->get_supported_capture_formats();
        if (supported_formats.empty()) {
          BOOST_LOG(warning) << "No compatible capture formats for this encoder"sv;
          return -1;
        }

        // We try this twice, in case we still get an error on reinitialization
        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output5->DuplicateOutput1((IUnknown *) display->device.get(), 0, supported_formats.size(), supported_formats.data(), &dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        // We don't retry with DuplicateOutput() because we can hit this codepath when we're racing
        // with mode changes and we don't want to accidentally fall back to suboptimal capture if
        // we get unlucky and succeed below.
        if (FAILED(status)) {
          BOOST_LOG(warning) << "DuplicateOutput1 Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      } else {
        BOOST_LOG(warning) << "IDXGIOutput5 is not supported by your OS. Capture performance may be reduced."sv;

        dxgi::output1_t output1 {};
        status = display->output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
          return -1;
        }

        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output1->DuplicateOutput((IUnknown *) display->device.get(), &dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        if (FAILED(status)) {
          BOOST_LOG(error) << "DuplicateOutput Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      }
    }

    DXGI_OUTDUPL_DESC dup_desc;
    dup->GetDesc(&dup_desc);

    BOOST_LOG(info) << "Desktop resolution ["sv << dup_desc.ModeDesc.Width << 'x' << dup_desc.ModeDesc.Height << ']';
    BOOST_LOG(info) << "Desktop format ["sv << display->dxgi_format_to_string(dup_desc.ModeDesc.Format) << ']';

    display->display_refresh_rate = dup_desc.ModeDesc.RefreshRate;
    double display_refresh_rate_decimal = (double) display->display_refresh_rate.Numerator / display->display_refresh_rate.Denominator;
    BOOST_LOG(info) << "Display refresh rate [" << display_refresh_rate_decimal << "Hz]";
    if (display->client_frame_rate_strict.Numerator > 0) {
      int num = display->client_frame_rate_strict.Numerator;
      int den = display->client_frame_rate_strict.Denominator;
      BOOST_LOG(info) << "Requested frame rate [" << num << "/" << den << " exactly " << av_q2d(AVRational {num, den}) << " fps]";
    } else {
      BOOST_LOG(info) << "Requested frame rate [" << display->client_frame_rate << "fps]";
    }
    display->display_refresh_rate_rounded = lround(display_refresh_rate_decimal);
    return 0;
  }

  capture_e duplication_t::next_frame(DXGI_OUTDUPL_FRAME_INFO &frame_info, std::chrono::milliseconds timeout, resource_t::pointer *res_p) {
    auto capture_status = release_frame();
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto status = dup->AcquireNextFrame(timeout.count(), &frame_info, res_p);

    switch (status) {
      case S_OK:
        // ProtectedContentMaskedOut seems to semi-randomly be TRUE or FALSE even when protected content
        // is on screen the whole time, so we can't just print when it changes. Instead we'll keep track
        // of the last time we printed the warning and print another if we haven't printed one recently.
        if (frame_info.ProtectedContentMaskedOut && std::chrono::steady_clock::now() > last_protected_content_warning_time + 10s) {
          BOOST_LOG(warning) << "Windows is currently blocking DRM-protected content from capture. You may see black regions where this content would be."sv;
          last_protected_content_warning_time = std::chrono::steady_clock::now();
        }

        has_frame = true;
        return capture_e::ok;
      case DXGI_ERROR_WAIT_TIMEOUT:
        return capture_e::timeout;
      case WAIT_ABANDONED:
      case DXGI_ERROR_ACCESS_LOST:
      case DXGI_ERROR_ACCESS_DENIED:
        return capture_e::reinit;
      default:
        BOOST_LOG(error) << "Couldn't acquire next frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  capture_e duplication_t::reset(dup_t::pointer dup_p) {
    auto capture_status = release_frame();

    dup.reset(dup_p);

    return capture_status;
  }

  capture_e duplication_t::release_frame() {
    if (!has_frame) {
      return capture_e::ok;
    }

    auto status = dup->ReleaseFrame();
    has_frame = false;
    switch (status) {
      case S_OK:
        return capture_e::ok;

      case DXGI_ERROR_INVALID_CALL:
        BOOST_LOG(warning) << "Duplication frame already released";
        return capture_e::ok;

      case DXGI_ERROR_ACCESS_LOST:
        return capture_e::reinit;

      default:
        BOOST_LOG(error) << "Error while releasing duplication frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  duplication_t::~duplication_t() {
    release_frame();
  }

  capture_e display_base_t::capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) {
    auto adjust_client_frame_rate = [&]() -> DXGI_RATIONAL {
      // Use exactly the requested rate if the client sent an X100 value
      if (client_frame_rate_strict.Numerator > 0) {
        return client_frame_rate_strict;
      }
      // Adjust capture frame interval when display refresh rate is not integral but very close to requested fps.
      if (display_refresh_rate.Denominator > 1) {
        DXGI_RATIONAL candidate = display_refresh_rate;
        if (client_frame_rate % display_refresh_rate_rounded == 0) {
          candidate.Numerator *= client_frame_rate / display_refresh_rate_rounded;
        } else if (display_refresh_rate_rounded % client_frame_rate == 0) {
          candidate.Denominator *= display_refresh_rate_rounded / client_frame_rate;
        }
        double candidate_rate = (double) candidate.Numerator / candidate.Denominator;
        // Can only decrease requested fps, otherwise client may start accumulating frames and suffer increased latency.
        if (client_frame_rate > candidate_rate && candidate_rate / client_frame_rate > 0.99) {
          BOOST_LOG(info) << "Adjusted capture rate to " << candidate_rate << "fps to better match display";
          return candidate;
        }
      }

      return {(uint32_t) client_frame_rate, 1};
    };

    DXGI_RATIONAL client_frame_rate_adjusted = adjust_client_frame_rate();

    // syslat diagnostic: record what the frame-rate adjustment actually decided. The adjustment can only
    // DECREASE the requested rate (see the comment inside the lambda above), so without this line we cannot
    // tell whether "40 fps at a 120 fps request" was the hardware or Sunshine quietly re-targeting us.
    BOOST_LOG(verbose) << "syslat rate requested ["sv << client_frame_rate << "] adjusted ["
                       << client_frame_rate_adjusted.Numerator << '/' << client_frame_rate_adjusted.Denominator << ']';

    // syslat diagnostic: the PACING POLICY. Upstream added exactly this decision for Pipewire in
    // cb72dffa ("add support for unpaced capture when appropriate"): pace only when the captured source
    // can run FASTER than the client asked for, because decimating a faster source is the entire point
    // of the grid. The Windows path never had it, so it paces even when the display is SLOWER than the
    // request -- and when the grid period G is close to the source period P the zero-timeout grid probe
    // sits on a knife edge: one miss drops the loop into a 200ms blocking acquire, which then waits for
    // the NEXT present and so skips the very frame we wanted. Measured consequence on this rig: a 4K@120
    // desktop (dxgidup, no stream: 120.2 Hz) captured at ~58.8 fps with DXGI reporting accum=2, i.e.
    // every other present taken. `auto` is the fix; `on`/`off` exist so the two halves can be A/B'd in
    // one build. These are environment variables rather than config keys on purpose: a config key must
    // be registered in config.html, docs/configuration.md and en.json or the consistency test fails,
    // which is far too much surface for a knob that only exists to answer one diagnostic question (same
    // rationale as SUNSHINE_SYSLAT_CAP_SLEEP_MS below).
    //   SUNSHINE_SYSLAT_PACING               auto | on | off
    //   SUNSHINE_SYSLAT_PACING_MARGIN        percent headroom `auto` demands of the display rate
    //   SUNSHINE_SYSLAT_PACING_RECOVER_MS    short-timeout retry length after a grid miss (0 = off)
    //   SUNSHINE_SYSLAT_PACING_RECOVER_TRIES max retries per miss
    //   SUNSHINE_SYSLAT_PACING_FEEDBACK      1 = also stop pacing if DXGI keeps reporting accum >= 2
    static const auto syslat_env_int = [](const char *name, int fallback, int lo, int hi) {
      if (const char *raw = std::getenv(name)) {
        const int parsed = std::atoi(raw);
        if (parsed >= lo && parsed <= hi) {
          return parsed;
        }
        BOOST_LOG(warning) << "syslat ignoring out-of-range "sv << name << " ["sv << raw << ']';
      }
      return fallback;
    };
    static const std::string syslat_pacing_mode = []() {
      if (const char *raw = std::getenv("SUNSHINE_SYSLAT_PACING")) {
        const std::string mode {raw};
        if (mode == "on" || mode == "off") {
          return mode;
        }
      }
      return std::string {"auto"};
    }();
    static const int syslat_pacing_margin_pct = syslat_env_int("SUNSHINE_SYSLAT_PACING_MARGIN", 10, 0, 900);
    static const int syslat_pacing_recover_ms = syslat_env_int("SUNSHINE_SYSLAT_PACING_RECOVER_MS", 2, 0, 200);
    static const int syslat_pacing_recover_tries = syslat_env_int("SUNSHINE_SYSLAT_PACING_RECOVER_TRIES", 4, 0, 100);
    static const int syslat_pacing_feedback = syslat_env_int("SUNSHINE_SYSLAT_PACING_FEEDBACK", 1, 0, 1);

    const double syslat_client_rate = client_frame_rate_adjusted.Denominator > 0
                                        ? (double) client_frame_rate_adjusted.Numerator / client_frame_rate_adjusted.Denominator
                                        : 0.0;
    const double syslat_display_rate = display_refresh_rate.Denominator > 0
                                         ? (double) display_refresh_rate.Numerator / display_refresh_rate.Denominator
                                         : 0.0;
    const double syslat_pacing_margin = 1.0 + (double) syslat_pacing_margin_pct / 100.0;
    const double syslat_pacing_ratio = (syslat_client_rate > 0.0 && syslat_display_rate > 0.0)
                                         ? syslat_display_rate / syslat_client_rate
                                         : 0.0;

    // How many presents DXGI SHOULD accumulate between two of our paced acquisitions -- i.e. how many
    // source frames a legitimately decimating grid is allowed to skip. At source 200Hz / client 120fps
    // that is 2, and seeing accum=2 there is correct behaviour, not a fault. The feedback guard below
    // compares against this instead of against a flat ">= 2", because a flat threshold would switch off
    // pacing exactly in the decimation regime where pacing is the point and let the encoder queue absorb
    // 200fps. 1 when the ratio is unknown: then pacing is never disabled on accum evidence alone.
    const uint32_t syslat_ratio_ceil = syslat_pacing_ratio > 0.0 ? (uint32_t) std::ceil(syslat_pacing_ratio) : 0;
    const uint32_t syslat_expected_accum = syslat_ratio_ceil > 1 ? syslat_ratio_ceil : 1;

    bool syslat_pacing = true;
    bool syslat_display_unknown = false;
    if (syslat_pacing_mode == "on") {
      syslat_pacing = true;
    } else if (syslat_pacing_mode == "off") {
      syslat_pacing = false;
    } else if (syslat_pacing_ratio <= 0.0) {
      // The reported refresh rate is unusable: keep upstream behaviour rather than guess.
      syslat_display_unknown = true;
    } else {
      syslat_pacing = syslat_pacing_ratio > syslat_pacing_margin;
    }

    BOOST_LOG(info) << "syslat pacing policy mode ["sv << syslat_pacing_mode
                    << "] display_rate ["sv << syslat_display_rate
                    << "] client_rate ["sv << syslat_client_rate
                    << "] ratio ["sv << syslat_pacing_ratio
                    << "] margin_pct ["sv << syslat_pacing_margin_pct
                    << "] required ["sv << (syslat_pacing ? 1 : 0)
                    << "] expected_accum ["sv << syslat_expected_accum
                    << "] unknown_display ["sv << (syslat_display_unknown ? 1 : 0)
                    << "] recover_ms ["sv << syslat_pacing_recover_ms
                    << "] recover_tries ["sv << syslat_pacing_recover_tries
                    << "] feedback ["sv << syslat_pacing_feedback << ']';
    if (!syslat_pacing && syslat_pacing_mode == "auto") {
      // The upstream wording for the same decision, so a log reader can grep one phrase across platforms.
      BOOST_LOG(info) << "syslat frame pacing: disabled (event-driven capture) -- display "sv << syslat_display_rate
                      << "Hz is not "sv << syslat_pacing_margin << "x the requested "sv << syslat_client_rate << "fps"sv;
    } else if (syslat_pacing && syslat_pacing_mode == "on" && syslat_pacing_ratio > 0.0 && syslat_pacing_ratio <= syslat_pacing_margin) {
      BOOST_LOG(warning) << "syslat frame pacing: forced ON although the display ("sv << syslat_display_rate
                         << "Hz) is not "sv << syslat_pacing_margin << "x the requested "sv << syslat_client_rate
                         << "fps -- expect grid misses"sv;
    }

    // Per-capture-session counters behind the aggregate `syslat pacing stats` line. They are what turns
    // "fps went up" into an attribution: how many frames came from the grid, from the short-timeout
    // recovery, and from the blocking acquire, and how often the grid missed.
    uint64_t syslat_frames_paced = 0;
    uint64_t syslat_frames_recov = 0;
    uint64_t syslat_frames_block = 0;
    uint64_t syslat_frames_event = 0;
    uint64_t syslat_group_misses = 0;
    uint64_t syslat_recoveries = 0;
    uint64_t syslat_recover_failures = 0;
    uint64_t syslat_accum_high = 0;
    int64_t syslat_sum_sleep_us = 0;
    int64_t syslat_sum_age_us = 0;

    std::optional<std::chrono::steady_clock::time_point> frame_pacing_group_start;
    uint32_t frame_pacing_group_frames = 0;

    // Keep the display awake during capture. If the display goes to sleep during
    // capture, best case is that capture stops until it powers back on. However,
    // worst case it will trigger us to reinit DD, waking the display back up in
    // a neverending cycle of waking and sleeping the display of an idle machine.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    auto clear_display_required = util::fail_guard([]() {
      SetThreadExecutionState(ES_CONTINUOUS);
    });

    sleep_overshoot_logger.reset();

    while (true) {
      // This will return false if the HDR state changes or for any number of other
      // display or GPU changes. We should reinit to examine the updated state of
      // the display subsystem. It is recommended to call this once per frame.
      if (!factory->IsCurrent()) {
        return platf::capture_e::reinit;
      }

      platf::capture_e status = capture_e::ok;
      std::shared_ptr<img_t> img_out;

      // syslat: the producer's per-iteration budget. The deliberate 10ms sleep further down happens
      // OUTSIDE snapshot(), so only this line can attribute it -- and it directly caps how fast frames
      // can be produced.
      const auto syslat_t_iter = std::chrono::steady_clock::now();
      int64_t syslat_sleep10_us = 0;
      bool syslat_pushed = false;
      // Set when this iteration's grid probe came up empty, which is what arms the short-timeout
      // recovery below. Kept separate from `status` because a missing frame is not always a timeout.
      bool syslat_recover_needed = false;

      // Try to continue frame pacing group, snapshot() is called with zero timeout after waiting for client frame interval
      if (syslat_pacing && frame_pacing_group_start) {
        const uint32_t seconds = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator / client_frame_rate_adjusted.Numerator;
        const uint32_t remainder = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator % client_frame_rate_adjusted.Numerator;
        const auto sleep_target = *frame_pacing_group_start +
                                  std::chrono::nanoseconds(1s) * seconds +
                                  std::chrono::nanoseconds(1s) * remainder / client_frame_rate_adjusted.Numerator;
        const auto sleep_period = sleep_target - std::chrono::steady_clock::now();

        if (sleep_period <= 0ns) {
          // We missed next frame time, invalidating current frame pacing group
          BOOST_LOG(verbose) << "syslat group miss sleep_us ["sv
                             << std::chrono::duration_cast<std::chrono::microseconds>(sleep_period).count()
                             << "] group_frames ["sv << frame_pacing_group_frames << "] recover [1]";
          ++syslat_group_misses;
          syslat_recover_needed = true;
          frame_pacing_group_start = std::nullopt;
          frame_pacing_group_frames = 0;
          status = capture_e::timeout;
        } else {
          timer->sleep_for(sleep_period);
          sleep_overshoot_logger.first_point(sleep_target);
          sleep_overshoot_logger.second_point_now_and_log();

          status = snapshot(pull_free_image_cb, img_out, 0ms, *cursor);

          // syslat diagnostic: this path deliberately sleeps to the client's frame grid and then takes
          // whatever frame is ALREADY available (zero timeout). So age_us -- the age of the frame we just
          // picked up, measured against its own desktop-present timestamp -- is latency Sunshine adds by
          // design, and it is included in "Frame processing latency" downstream. sleep_us is how much we
          // slept. ONE line per frame on purpose: several probes per frame would perturb the measurement.
          if (status == capture_e::ok && img_out && img_out->frame_timestamp) {
            const auto syslat_age_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - *img_out->frame_timestamp)
                                         .count();
            const auto syslat_sleep_us = std::chrono::duration_cast<std::chrono::microseconds>(sleep_period).count();
            BOOST_LOG(verbose) << "syslat cap path [pacing0] sleep_us ["sv << syslat_sleep_us
                               << "] age_us ["sv << syslat_age_us
                               << "] group_frames ["sv << frame_pacing_group_frames << ']';
            syslat_sum_sleep_us += syslat_sleep_us;
            syslat_sum_age_us += syslat_age_us;
          }

          if (status == capture_e::ok && img_out) {
            frame_pacing_group_frames += 1;
            ++syslat_frames_paced;
          } else {
            BOOST_LOG(verbose) << "syslat group miss sleep_us [0] group_frames ["sv << frame_pacing_group_frames << "] recover [1]";
            ++syslat_group_misses;
            syslat_recover_needed = true;
            frame_pacing_group_start = std::nullopt;
            frame_pacing_group_frames = 0;
          }
        }
      }

      // syslat: grid-miss recovery. A missed grid point is normally only microseconds away from the
      // present we wanted, so retry with a SHORT timeout instead of falling straight into the 200ms
      // blocking acquire -- that acquire waits for the NEXT present and therefore skips a whole source
      // frame, which is exactly how a 120Hz desktop ends up captured at 60fps. When a frame is obtained
      // this way the group is RE-ANCHORED on that frame's own timestamp, so the grid phase follows the
      // source instead of drifting against it. Setting SUNSHINE_SYSLAT_PACING_RECOVER_MS=0 restores the
      // old "one miss costs one whole present" behaviour exactly, which is the control arm for the A/B.
      if (syslat_recover_needed && syslat_pacing && syslat_pacing_recover_ms > 0 && syslat_pacing_recover_tries > 0) {
        const auto syslat_t_recov = std::chrono::steady_clock::now();
        int syslat_tries = 0;
        while (syslat_tries < syslat_pacing_recover_tries) {
          ++syslat_tries;
          status = snapshot(pull_free_image_cb, img_out, std::chrono::milliseconds(syslat_pacing_recover_ms), *cursor);
          if (status == capture_e::ok && img_out) {
            break;
          }
          if (status != capture_e::timeout) {
            // reinit/error/interrupted: leave it for the switch below to classify.
            break;
          }
          // snapshot() went to the trouble of a blocking AcquireNextFrame; yield so we do not spin on the
          // unfair D3D11 device lock and starve the encoding thread (see the long note further down).
          std::this_thread::yield();
        }
        const auto syslat_recov_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - syslat_t_recov)
                                       .count();

        if (status == capture_e::ok && img_out) {
          ++syslat_recoveries;
          ++syslat_frames_recov;
          const auto syslat_age_us = img_out->frame_timestamp
                                       ? std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - *img_out->frame_timestamp)
                                           .count()
                                       : 0;
          frame_pacing_group_start = img_out->frame_timestamp.value_or(std::chrono::steady_clock::now());
          frame_pacing_group_frames = 1;
          syslat_sum_age_us += syslat_age_us;
          BOOST_LOG(verbose) << "syslat cap path [recov] tries ["sv << syslat_tries
                             << "] wait_us ["sv << syslat_recov_us
                             << "] age_us ["sv << syslat_age_us << "] group_frames [1]";
        } else if (status == capture_e::timeout || (status == capture_e::ok && !img_out)) {
          ++syslat_recover_failures;
          BOOST_LOG(verbose) << "syslat recov failed tries ["sv << syslat_tries
                             << "] wait_us ["sv << syslat_recov_us << "] status ["sv << (int) status << ']';
          status = capture_e::timeout;
        } else {
          BOOST_LOG(verbose) << "syslat recov aborted wait_us ["sv << syslat_recov_us
                             << "] status ["sv << (int) status << ']';
        }
      }

      // Start new frame pacing group if necessary, snapshot() is called with non-zero timeout
      if (status == capture_e::timeout || (status == capture_e::ok && !frame_pacing_group_start)) {
        status = snapshot(pull_free_image_cb, img_out, 200ms, *cursor);
        ++syslat_frames_block;
        if (!syslat_pacing) {
          // With pacing off this is not a "fallback", it is the whole loop: wait for the desktop to
          // present and take it. Counted separately so the aggregate can prove event-driven capture took
          // over (`event` == `block` and `paced` + `recov` == 0).
          ++syslat_frames_event;
        }

        // syslat diagnostic: the blocking path -- we waited (up to 200 ms) for the desktop to present
        // something new, so age_us here should be small. Comparing age_us between [pacing0] and [block200]
        // is exactly how we tell whether the grid pacing is what costs us latency at high resolutions.
        if (status == capture_e::ok && img_out && img_out->frame_timestamp) {
          BOOST_LOG(verbose) << "syslat cap path [block200] sleep_us [0] age_us ["sv
                             << std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - *img_out->frame_timestamp).count()
                             << "] pacing ["sv << (syslat_pacing ? 1 : 0) << ']';
        }

        // The group is only ever (re)anchored while pacing is on; with pacing off the loop above never
        // starts a group, which is what makes every iteration fall into this blocking acquire -- i.e.
        // event-driven capture driven by the compositor's own presents.
        if (status == capture_e::ok && img_out && syslat_pacing) {
          frame_pacing_group_start = img_out->frame_timestamp;

          if (!frame_pacing_group_start) {
            BOOST_LOG(warning) << "snapshot() provided image without timestamp";
            frame_pacing_group_start = std::chrono::steady_clock::now();
          }

          frame_pacing_group_frames = 1;
        } else if (status == platf::capture_e::timeout) {
          // The D3D11 device is protected by an unfair lock that is held the entire time that
          // IDXGIOutputDuplication::AcquireNextFrame() is running. This is normally harmless,
          // however sometimes the encoding thread needs to interact with our ID3D11Device to
          // create dummy images or initialize the shared state that is used to pass textures
          // between the capture and encoding ID3D11Devices.
          //
          // When we're in a state where we're not actively receiving frames regularly, we will
          // spend almost 100% of our time in AcquireNextFrame() holding that critical lock.
          // Worse still, since it's unfair, we can monopolize it while the encoding thread
          // is starved. The encoding thread may acquire it for a few moments across a few
          // ID3D11Device calls before losing it again to us for another long time waiting in
          // AcquireNextFrame(). The starvation caused by this lock contention causes encoder
          // reinitialization to take several seconds instead of a fraction of a second.
          //
          // To avoid starving the encoding thread, sleep without the lock held for a little
          // while each time we reach our max frame timeout. This will only happen when nothing
          // is updating the display, so no visible stutter should be introduced by the sleep.
          // syslat: the 10ms anti-starvation sleep is the leading candidate for capping how fast frames
          // can be produced, so it is made overridable from the environment (SUNSHINE_SYSLAT_CAP_SLEEP_MS).
          // An environment variable rather than a config key on purpose: a new config key would have to be
          // registered in config.html, docs/configuration.md and en.json to satisfy the consistency test,
          // and that is a lot of surface for a diagnostic knob that only exists to answer one question.
          // NOTE: 0 removes the sleep entirely, which leaves the timeout path spinning on the D3D11 device
          // lock -- use 1ms rather than 0 unless CPU burn is acceptable.
          static const auto syslat_sleep10_ms = []() {
            int ms = 10;
            if (const char *override_ms = std::getenv("SUNSHINE_SYSLAT_CAP_SLEEP_MS")) {
              const int parsed = std::atoi(override_ms);
              if (parsed >= 0 && parsed <= 1000) {
                ms = parsed;
                BOOST_LOG(info) << "syslat capture-timeout sleep overridden to "sv << ms << "ms"sv;
              }
            }
            return ms;
          }();
          const auto syslat_t_sleep = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(std::chrono::milliseconds(syslat_sleep10_ms));
          syslat_sleep10_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - syslat_t_sleep).count();
        }
      }

      const auto syslat_snap_status = (int) status;

      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::interrupted:
          return status;
        case platf::capture_e::timeout:
          if (!push_captured_image_cb(std::move(img_out), false)) {
            return capture_e::ok;
          }
          break;
        case platf::capture_e::ok:
          if (!push_captured_image_cb(std::move(img_out), true)) {
            return capture_e::ok;
          }
          syslat_pushed = true;
          break;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return status;
      }

      status = release_snapshot();
      if (status != platf::capture_e::ok) {
        return status;
      }

      // syslat diagnostic: the pacing feedback guard. Even with the ratio policy, the refresh rate DXGI
      // reports can be a lie relative to what the desktop actually sustains -- on this rig 4K advertises
      // 120Hz and does present at ~120Hz when idle, yet with the encoder running the compositor only
      // hands over every other present. DXGI tells us that directly: AccumulatedFrames is the number of
      // source presents that happened while we were not looking, so it must be compared with what the
      // grid intends to skip (syslat_expected_accum), not with a flat 2 -- at source 200Hz / client 120fps
      // accum=2 is the design, and disabling pacing there would push 200fps into a ~67fps encoder.
      // Only the DXGI duplication backend reports accum (see display_t::last_frame_accumulated_frames).
      if (syslat_pacing && syslat_pacing_feedback && last_frame_accumulated_frames > syslat_expected_accum) {
        ++syslat_accum_high;
        if (syslat_accum_high >= 30) {
          syslat_pacing = false;
          frame_pacing_group_start = std::nullopt;
          frame_pacing_group_frames = 0;
          BOOST_LOG(info) << "syslat pacing feedback: disabled (event-driven capture) -- DXGI reported accum ["sv
                          << last_frame_accumulated_frames << "] above the ["sv << syslat_expected_accum
                          << "] this grid intends to skip, for ["sv << syslat_accum_high << "] consecutive frames"sv;
        }
      } else if (last_frame_accumulated_frames <= syslat_expected_accum) {
        syslat_accum_high = 0;
      }

      // syslat diagnostic: one aggregate line per 120 produced frames. The per-frame probes say what each
      // frame cost; this says WHERE the frames came from, which is the only way to tell "the fix worked"
      // from "the loop found another route to the same frame count". avg_sleep_us/avg_age_us are over the
      // frames that were actually paced (plus recovered, for age), so they must be read together with the
      // counts.
      const uint64_t syslat_frames_total = syslat_frames_paced + syslat_frames_recov + syslat_frames_block;
      if (syslat_frames_total > 0 && syslat_frames_total % 120 == 0) {
        const uint64_t syslat_n_aged = syslat_frames_paced + syslat_frames_recov;
        BOOST_LOG(info) << "syslat pacing stats frames ["sv << syslat_frames_total
                        << "] paced ["sv << syslat_frames_paced
                        << "] recov ["sv << syslat_frames_recov
                        << "] block ["sv << syslat_frames_block
                        << "] event ["sv << syslat_frames_event
                        << "] group_misses ["sv << syslat_group_misses
                        << "] recoveries ["sv << syslat_recoveries
                        << "] recov_fail ["sv << syslat_recover_failures
                        << "] avg_sleep_us ["sv << (syslat_frames_paced ? syslat_sum_sleep_us / (int64_t) syslat_frames_paced : 0)
                        << "] avg_age_us ["sv << (syslat_n_aged ? syslat_sum_age_us / (int64_t) syslat_n_aged : 0)
                        << "] pacing ["sv << (syslat_pacing ? 1 : 0) << ']';
      }

      // syslat: one line per capture-loop iteration. sleep10_us is the deliberate 10ms sleep taken
      // whenever a snapshot times out or a frame-pacing group misses; it is spent outside snapshot(), so
      // no capture-side probe can see it, yet it caps the rate at which frames can be produced.
      BOOST_LOG(verbose) << "syslat capthr iter_us ["sv
                         << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - syslat_t_iter).count()
                         << "] sleep10_us ["sv << syslat_sleep10_us << "] pushed ["sv << (syslat_pushed ? 1 : 0)
                         << "] snap_status ["sv << syslat_snap_status << ']';
    }

    return capture_e::ok;
  }

  /**
   * @brief Tests to determine if the Desktop Duplication API can capture the given output.
   * @details When testing for enumeration only, we avoid resyncing the thread desktop.
   * @param adapter The DXGI adapter to use for capture.
   * @param output The DXGI output to capture.
   * @param enumeration_only Specifies whether this test is occurring for display enumeration.
   *
   * @return True when Desktop Duplication can capture the requested output.
   */
  bool test_dxgi_duplication(adapter_t &adapter, output_t &output, bool enumeration_only) {
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    device_t device;
    auto status = D3D11CreateDevice(
      adapter.get(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      nullptr
    );
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device for DD test [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    output1_t output1;
    status = output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
      return false;
    }

    // Check if we can use the Desktop Duplication API on this output
    for (int x = 0; x < 2; ++x) {
      dup_t dup;

      // Only resynchronize the thread desktop when not enumerating displays.
      // During enumeration, the caller will do this only once to ensure
      // a consistent view of available outputs.
      if (!enumeration_only) {
        syncThreadDesktop();
      }

      status = output1->DuplicateOutput((IUnknown *) device.get(), &dup);
      if (SUCCEEDED(status)) {
        return true;
      }

      // If we're not resyncing the thread desktop and we don't have permission to
      // capture the current desktop, just bail immediately. Retrying won't help.
      if (enumeration_only && status == E_ACCESSDENIED) {
        break;
      } else {
        std::this_thread::sleep_for(200ms);
      }
    }

    BOOST_LOG(error) << "DuplicateOutput() test failed [0x"sv << util::hex(status).to_string_view() << ']';
    return false;
  }

  /**
   * @brief Hook for NtGdiDdDDIGetCachedHybridQueryValue() from win32u.dll.
   * @param gpuPreference A pointer to the location where the preference will be written.
   * @return Always STATUS_SUCCESS if valid arguments are provided.
   */
  NTSTATUS __stdcall NtGdiDdDDIGetCachedHybridQueryValueHook(D3DKMT_GPU_PREFERENCE_QUERY_STATE *gpuPreference) {
    // By faking a cached GPU preference state of D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED, this will
    // prevent DXGI from performing the normal GPU preference resolution that looks at the registry,
    // power settings, and the hybrid adapter DDI interface to pick a GPU. Instead, we will not be
    // bound to any specific GPU. This will prevent DXGI from performing output reparenting (moving
    // outputs from their true location to the render GPU), which breaks DDA.
    if (gpuPreference) {
      *gpuPreference = D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED;
      return 0;  // STATUS_SUCCESS
    } else {
      return STATUS_INVALID_PARAMETER;
    }
  }

  int display_base_t::init(const ::video::config_t &config, const std::string &display_name) {
    std::once_flag windows_cpp_once_flag;

    std::call_once(windows_cpp_once_flag, []() {
      DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);

      typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

      {
        auto user32 = LoadLibraryA("user32.dll");
        auto f = (User32_SetProcessDpiAwarenessContext) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (f) {
          f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }

        FreeLibrary(user32);
      }

      {
        // We aren't calling MH_Uninitialize(), but that's okay because this hook lasts for the life of the process
        MH_Initialize();
        MH_CreateHookApi(L"win32u.dll", "NtGdiDdDDIGetCachedHybridQueryValue", (void *) NtGdiDdDDIGetCachedHybridQueryValueHook, nullptr);
        MH_EnableHook(MH_ALL_HOOKS);
      }
    });

    // Get rectangle of full desktop for absolute mouse coordinates
    env_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    env_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HRESULT status;

    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    auto adapter_name = utf_utils::from_utf8(config::video.adapter_name);
    auto output_name = utf_utils::from_utf8(display_name);

    adapter_t::pointer adapter_p;
    for (int tries = 0; tries < 2; ++tries) {
      for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
        dxgi::adapter_t adapter_tmp {adapter_p};

        DXGI_ADAPTER_DESC1 adapter_desc;
        adapter_tmp->GetDesc1(&adapter_desc);

        if (!adapter_name.empty() && adapter_desc.Description != adapter_name) {
          continue;
        }

        dxgi::output_t::pointer output_p;
        for (int y = 0; adapter_tmp->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
          dxgi::output_t output_tmp {output_p};

          DXGI_OUTPUT_DESC desc;
          output_tmp->GetDesc(&desc);

          if (!output_name.empty() && desc.DeviceName != output_name) {
            continue;
          }

          if (desc.AttachedToDesktop && test_dxgi_duplication(adapter_tmp, output_tmp, false)) {
            output = std::move(output_tmp);

            offset_x = desc.DesktopCoordinates.left;
            offset_y = desc.DesktopCoordinates.top;
            width = desc.DesktopCoordinates.right - offset_x;
            height = desc.DesktopCoordinates.bottom - offset_y;

            display_rotation = desc.Rotation;
            if (display_rotation == DXGI_MODE_ROTATION_ROTATE90 || display_rotation == DXGI_MODE_ROTATION_ROTATE270) {
              width_before_rotation = height;
              height_before_rotation = width;
            } else {
              width_before_rotation = width;
              height_before_rotation = height;
            }

            // left and bottom may be negative, yet absolute mouse coordinates start at 0x0
            // Ensure offset starts at 0x0
            offset_x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
            offset_y -= GetSystemMetrics(SM_YVIRTUALSCREEN);

            break;
          }
        }

        if (output) {
          adapter = std::move(adapter_tmp);
          break;
        }
      }

      if (output) {
        break;
      }

      // If we made it here without finding an output, try to power on the display and retry.
      if (tries == 0) {
        SetThreadExecutionState(ES_DISPLAY_REQUIRED);
        Sleep(500);
      }
    }

    if (!output) {
      BOOST_LOG(error) << "Failed to locate an output device"sv;
      return -1;
    }

    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    status = adapter->QueryInterface(IID_IDXGIAdapter, (void **) &adapter_p);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIAdapter interface"sv;
      return -1;
    }

    status = D3D11CreateDevice(
      adapter_p,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      &feature_level,
      &device_ctx
    );

    adapter_p->Release();

    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';

      return -1;
    }

    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    auto description = utf_utils::to_utf8(adapter_desc.Description);
    BOOST_LOG(info)
      << std::endl
      << "Device Description : " << description << std::endl
      << "Device Vendor ID   : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
      << "Device Device ID   : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
      << "Device Video Mem   : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
      << "Device Sys Mem     : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Share Sys Mem      : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Feature Level      : 0x"sv << util::hex(feature_level).to_string_view() << std::endl
      << "Capture size       : "sv << width << 'x' << height << std::endl
      << "Offset             : "sv << offset_x << 'x' << offset_y << std::endl
      << "Virtual Desktop    : "sv << env_width << 'x' << env_height;

    // Bump up thread priority
    {
      const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
      TOKEN_PRIVILEGES tp;
      HANDLE token;
      LUID val;

      if (OpenProcessToken(GetCurrentProcess(), flags, &token) && !!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME, &val)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = val;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!AdjustTokenPrivileges(token, false, &tp, sizeof(tp), nullptr, nullptr)) {
          BOOST_LOG(warning) << "Could not set privilege to increase GPU priority";
        }
      }

      CloseHandle(token);

      HMODULE gdi32 = GetModuleHandleA("GDI32");
      if (gdi32) {
        auto check_hags = [&](const LUID &adapter) -> bool {
          auto d3dkmt_open_adapter = (PD3DKMTOpenAdapterFromLuid) GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
          auto d3dkmt_query_adapter_info = (PD3DKMTQueryAdapterInfo) GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
          auto d3dkmt_close_adapter = (PD3DKMTCloseAdapter) GetProcAddress(gdi32, "D3DKMTCloseAdapter");
          if (!d3dkmt_open_adapter || !d3dkmt_query_adapter_info || !d3dkmt_close_adapter) {
            BOOST_LOG(error) << "Couldn't load d3dkmt functions from gdi32.dll to determine GPU HAGS status";
            return false;
          }

          D3DKMT_OPENADAPTERFROMLUID d3dkmt_adapter = {.AdapterLuid = adapter};
          if (FAILED(d3dkmt_open_adapter(&d3dkmt_adapter))) {
            BOOST_LOG(error) << "D3DKMTOpenAdapterFromLuid() failed while trying to determine GPU HAGS status";
            return false;
          }

          bool result;

          D3DKMT_WDDM_2_7_CAPS d3dkmt_adapter_caps = {};
          D3DKMT_QUERYADAPTERINFO d3dkmt_adapter_info = {};
          d3dkmt_adapter_info.hAdapter = d3dkmt_adapter.hAdapter;
          d3dkmt_adapter_info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
          d3dkmt_adapter_info.pPrivateDriverData = &d3dkmt_adapter_caps;
          d3dkmt_adapter_info.PrivateDriverDataSize = sizeof(d3dkmt_adapter_caps);

          if (SUCCEEDED(d3dkmt_query_adapter_info(&d3dkmt_adapter_info))) {
            result = d3dkmt_adapter_caps.HwSchEnabled;
          } else {
            BOOST_LOG(warning) << "D3DKMTQueryAdapterInfo() failed while trying to determine GPU HAGS status";
            result = false;
          }

          D3DKMT_CLOSEADAPTER d3dkmt_close_adapter_wrap = {d3dkmt_adapter.hAdapter};
          if (FAILED(d3dkmt_close_adapter(&d3dkmt_close_adapter_wrap))) {
            BOOST_LOG(error) << "D3DKMTCloseAdapter() failed while trying to determine GPU HAGS status";
          }

          return result;
        };

        auto d3dkmt_set_process_priority = (PD3DKMTSetProcessSchedulingPriorityClass) GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass");
        if (d3dkmt_set_process_priority) {
          auto priority = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
          bool hags_enabled = check_hags(adapter_desc.AdapterLuid);
          if (adapter_desc.VendorId == 0x10DE) {
            // As of 2023.07, NVIDIA driver has unfixed bug(s) where "realtime" can cause unrecoverable encoding freeze or outright driver crash
            // This issue happens more frequently with HAGS, in DX12 games or when VRAM is filled close to max capacity
            // Track OBS to see if they find better workaround or NVIDIA fixes it on their end, they seem to be in communication
            if (hags_enabled && !config::video.nv_realtime_hags) {
              priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
            }
          }
          BOOST_LOG(info) << "Active GPU has HAGS " << (hags_enabled ? "enabled" : "disabled");
          BOOST_LOG(info) << "Using " << (priority == D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH ? "high" : "realtime") << " GPU priority";
          if (FAILED(d3dkmt_set_process_priority(GetCurrentProcess(), priority))) {
            BOOST_LOG(warning) << "Failed to adjust GPU priority. Please run application as administrator for optimal performance.";
          }
        } else {
          BOOST_LOG(error) << "Couldn't load D3DKMTSetProcessSchedulingPriorityClass function from gdi32.dll to adjust GPU priority";
        }
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase capture GPU thread priority. Please run application as administrator for optimal performance.";
      }
    }

    // Try to reduce latency
    {
      dxgi::dxgi1_t dxgi {};
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetMaximumFrameLatency(1);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to set maximum frame latency [0x"sv << util::hex(status).to_string_view() << ']';
      }
    }

    client_frame_rate = config.framerate;
    client_frame_rate_strict = {0, 0};
    if (config.framerateX100 > 0) {
      const AVRational fps = ::video::framerate_to_rational(config);
      client_frame_rate_strict = DXGI_RATIONAL {static_cast<UINT>(fps.num), static_cast<UINT>(fps.den)};
    }

    dxgi::output6_t output6 {};
    status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (SUCCEEDED(status)) {
      DXGI_OUTPUT_DESC1 desc1;
      output6->GetDesc1(&desc1);

      BOOST_LOG(info)
        << std::endl
        << "Colorspace         : "sv << colorspace_to_string(desc1.ColorSpace) << std::endl
        << "Bits Per Color     : "sv << desc1.BitsPerColor << std::endl
        << "Red Primary        : ["sv << desc1.RedPrimary[0] << ',' << desc1.RedPrimary[1] << ']' << std::endl
        << "Green Primary      : ["sv << desc1.GreenPrimary[0] << ',' << desc1.GreenPrimary[1] << ']' << std::endl
        << "Blue Primary       : ["sv << desc1.BluePrimary[0] << ',' << desc1.BluePrimary[1] << ']' << std::endl
        << "White Point        : ["sv << desc1.WhitePoint[0] << ',' << desc1.WhitePoint[1] << ']' << std::endl
        << "Min Luminance      : "sv << desc1.MinLuminance << " nits"sv << std::endl
        << "Max Luminance      : "sv << desc1.MaxLuminance << " nits"sv << std::endl
        << "Max Full Luminance : "sv << desc1.MaxFullFrameLuminance << " nits"sv;
    }

    if (!timer || !*timer) {
      BOOST_LOG(error) << "Uninitialized high precision timer";
      return -1;
    }

    return 0;
  }

  bool display_base_t::is_hdr() {
    dxgi::output6_t output6 {};

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  }

  bool display_base_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    dxgi::output6_t output6 {};

    std::memset(&metadata, 0, sizeof(metadata));

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    // The primaries reported here seem to correspond to scRGB (Rec. 709)
    // which we then convert to Rec 2020 in our scRGB FP16 -> PQ shader
    // prior to encoding. It's not clear to me if we're supposed to report
    // the primaries of the original colorspace or the one we've converted
    // it to, but let's just report Rec 2020 primaries and D65 white level
    // to avoid confusing clients by reporting Rec 709 primaries with a
    // Rec 2020 colorspace. It seems like most clients ignore the primaries
    // in the metadata anyway (luminance range is most important).
    desc1.RedPrimary[0] = 0.708f;
    desc1.RedPrimary[1] = 0.292f;
    desc1.GreenPrimary[0] = 0.170f;
    desc1.GreenPrimary[1] = 0.797f;
    desc1.BluePrimary[0] = 0.131f;
    desc1.BluePrimary[1] = 0.046f;
    desc1.WhitePoint[0] = 0.3127f;
    desc1.WhitePoint[1] = 0.3290f;

    metadata.displayPrimaries[0].x = desc1.RedPrimary[0] * 50000;
    metadata.displayPrimaries[0].y = desc1.RedPrimary[1] * 50000;
    metadata.displayPrimaries[1].x = desc1.GreenPrimary[0] * 50000;
    metadata.displayPrimaries[1].y = desc1.GreenPrimary[1] * 50000;
    metadata.displayPrimaries[2].x = desc1.BluePrimary[0] * 50000;
    metadata.displayPrimaries[2].y = desc1.BluePrimary[1] * 50000;

    metadata.whitePoint.x = desc1.WhitePoint[0] * 50000;
    metadata.whitePoint.y = desc1.WhitePoint[1] * 50000;

    metadata.maxDisplayLuminance = desc1.MaxLuminance;
    metadata.minDisplayLuminance = desc1.MinLuminance * 10000;

    // These are content-specific metadata parameters that this interface doesn't give us
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;

    metadata.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;

    return true;
  }

  /**
   * @brief Names for DXGI_FORMAT values used in diagnostic logging.
   */
  const char *format_str[] = {
    "DXGI_FORMAT_UNKNOWN",
    "DXGI_FORMAT_R32G32B32A32_TYPELESS",
    "DXGI_FORMAT_R32G32B32A32_FLOAT",
    "DXGI_FORMAT_R32G32B32A32_UINT",
    "DXGI_FORMAT_R32G32B32A32_SINT",
    "DXGI_FORMAT_R32G32B32_TYPELESS",
    "DXGI_FORMAT_R32G32B32_FLOAT",
    "DXGI_FORMAT_R32G32B32_UINT",
    "DXGI_FORMAT_R32G32B32_SINT",
    "DXGI_FORMAT_R16G16B16A16_TYPELESS",
    "DXGI_FORMAT_R16G16B16A16_FLOAT",
    "DXGI_FORMAT_R16G16B16A16_UNORM",
    "DXGI_FORMAT_R16G16B16A16_UINT",
    "DXGI_FORMAT_R16G16B16A16_SNORM",
    "DXGI_FORMAT_R16G16B16A16_SINT",
    "DXGI_FORMAT_R32G32_TYPELESS",
    "DXGI_FORMAT_R32G32_FLOAT",
    "DXGI_FORMAT_R32G32_UINT",
    "DXGI_FORMAT_R32G32_SINT",
    "DXGI_FORMAT_R32G8X24_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
    "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
    "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
    "DXGI_FORMAT_R10G10B10A2_TYPELESS",
    "DXGI_FORMAT_R10G10B10A2_UNORM",
    "DXGI_FORMAT_R10G10B10A2_UINT",
    "DXGI_FORMAT_R11G11B10_FLOAT",
    "DXGI_FORMAT_R8G8B8A8_TYPELESS",
    "DXGI_FORMAT_R8G8B8A8_UNORM",
    "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
    "DXGI_FORMAT_R8G8B8A8_UINT",
    "DXGI_FORMAT_R8G8B8A8_SNORM",
    "DXGI_FORMAT_R8G8B8A8_SINT",
    "DXGI_FORMAT_R16G16_TYPELESS",
    "DXGI_FORMAT_R16G16_FLOAT",
    "DXGI_FORMAT_R16G16_UNORM",
    "DXGI_FORMAT_R16G16_UINT",
    "DXGI_FORMAT_R16G16_SNORM",
    "DXGI_FORMAT_R16G16_SINT",
    "DXGI_FORMAT_R32_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT",
    "DXGI_FORMAT_R32_FLOAT",
    "DXGI_FORMAT_R32_UINT",
    "DXGI_FORMAT_R32_SINT",
    "DXGI_FORMAT_R24G8_TYPELESS",
    "DXGI_FORMAT_D24_UNORM_S8_UINT",
    "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
    "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
    "DXGI_FORMAT_R8G8_TYPELESS",
    "DXGI_FORMAT_R8G8_UNORM",
    "DXGI_FORMAT_R8G8_UINT",
    "DXGI_FORMAT_R8G8_SNORM",
    "DXGI_FORMAT_R8G8_SINT",
    "DXGI_FORMAT_R16_TYPELESS",
    "DXGI_FORMAT_R16_FLOAT",
    "DXGI_FORMAT_D16_UNORM",
    "DXGI_FORMAT_R16_UNORM",
    "DXGI_FORMAT_R16_UINT",
    "DXGI_FORMAT_R16_SNORM",
    "DXGI_FORMAT_R16_SINT",
    "DXGI_FORMAT_R8_TYPELESS",
    "DXGI_FORMAT_R8_UNORM",
    "DXGI_FORMAT_R8_UINT",
    "DXGI_FORMAT_R8_SNORM",
    "DXGI_FORMAT_R8_SINT",
    "DXGI_FORMAT_A8_UNORM",
    "DXGI_FORMAT_R1_UNORM",
    "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
    "DXGI_FORMAT_R8G8_B8G8_UNORM",
    "DXGI_FORMAT_G8R8_G8B8_UNORM",
    "DXGI_FORMAT_BC1_TYPELESS",
    "DXGI_FORMAT_BC1_UNORM",
    "DXGI_FORMAT_BC1_UNORM_SRGB",
    "DXGI_FORMAT_BC2_TYPELESS",
    "DXGI_FORMAT_BC2_UNORM",
    "DXGI_FORMAT_BC2_UNORM_SRGB",
    "DXGI_FORMAT_BC3_TYPELESS",
    "DXGI_FORMAT_BC3_UNORM",
    "DXGI_FORMAT_BC3_UNORM_SRGB",
    "DXGI_FORMAT_BC4_TYPELESS",
    "DXGI_FORMAT_BC4_UNORM",
    "DXGI_FORMAT_BC4_SNORM",
    "DXGI_FORMAT_BC5_TYPELESS",
    "DXGI_FORMAT_BC5_UNORM",
    "DXGI_FORMAT_BC5_SNORM",
    "DXGI_FORMAT_B5G6R5_UNORM",
    "DXGI_FORMAT_B5G5R5A1_UNORM",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
    "DXGI_FORMAT_B8G8R8X8_UNORM",
    "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
    "DXGI_FORMAT_B8G8R8A8_TYPELESS",
    "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
    "DXGI_FORMAT_B8G8R8X8_TYPELESS",
    "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
    "DXGI_FORMAT_BC6H_TYPELESS",
    "DXGI_FORMAT_BC6H_UF16",
    "DXGI_FORMAT_BC6H_SF16",
    "DXGI_FORMAT_BC7_TYPELESS",
    "DXGI_FORMAT_BC7_UNORM",
    "DXGI_FORMAT_BC7_UNORM_SRGB",
    "DXGI_FORMAT_AYUV",
    "DXGI_FORMAT_Y410",
    "DXGI_FORMAT_Y416",
    "DXGI_FORMAT_NV12",
    "DXGI_FORMAT_P010",
    "DXGI_FORMAT_P016",
    "DXGI_FORMAT_420_OPAQUE",
    "DXGI_FORMAT_YUY2",
    "DXGI_FORMAT_Y210",
    "DXGI_FORMAT_Y216",
    "DXGI_FORMAT_NV11",
    "DXGI_FORMAT_AI44",
    "DXGI_FORMAT_IA44",
    "DXGI_FORMAT_P8",
    "DXGI_FORMAT_A8P8",
    "DXGI_FORMAT_B4G4R4A4_UNORM",

    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,

    "DXGI_FORMAT_P208",
    "DXGI_FORMAT_V208",
    "DXGI_FORMAT_V408"
  };

  const char *display_base_t::dxgi_format_to_string(DXGI_FORMAT format) {
    return format_str[format];
  }

  const char *display_base_t::colorspace_to_string(DXGI_COLOR_SPACE_TYPE type) {
    const char *type_str[] = {
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_RESERVED",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020",
    };

    if (type < ARRAYSIZE(type_str)) {
      return type_str[type];
    } else {
      return "UNKNOWN";
    }
  }

}  // namespace platf::dxgi

namespace platf {
  /**
   * Pick a display adapter and capture method.
   * @param hwdevice_type enables possible use of hardware encoder
   */
  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (config::video.capture == "ddx" || config::video.capture.empty()) {
      if (hwdevice_type == mem_type_e::dxgi) {
        auto disp = std::make_shared<dxgi::display_ddup_vram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      } else if (hwdevice_type == mem_type_e::system) {
        auto disp = std::make_shared<dxgi::display_ddup_ram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      }
    }

    if (config::video.capture == "wgc" || config::video.capture.empty()) {
      if (hwdevice_type == mem_type_e::dxgi) {
        auto disp = std::make_shared<dxgi::display_wgc_vram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      } else if (hwdevice_type == mem_type_e::system) {
        auto disp = std::make_shared<dxgi::display_wgc_ram_t>();

        if (!disp->init(config, display_name)) {
          return disp;
        }
      }
    }

    // ddx and wgc failed
    return nullptr;
  }

  std::vector<std::string> display_names(mem_type_e) {
    std::vector<std::string> display_names;

    HRESULT status;

    BOOST_LOG(debug) << "Detecting monitors..."sv;

    // We sync the thread desktop once before we start the enumeration process
    // to ensure test_dxgi_duplication() returns consistent results for all GPUs
    // even if the current desktop changes during our enumeration process.
    // It is critical that we either fully succeed in enumeration or fully fail,
    // otherwise it can lead to the capture code switching monitors unexpectedly.
    syncThreadDesktop();

    dxgi::factory1_t factory;
    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return {};
    }

    dxgi::adapter_t::pointer adapter_p;
    for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
      dxgi::adapter_t adapter {adapter_p};
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter->GetDesc1(&adapter_desc);

      BOOST_LOG(debug)
        << std::endl
        << "====== ADAPTER ====="sv << std::endl
        << "Device Name      : "sv << utf_utils::to_utf8(adapter_desc.Description) << std::endl
        << "Device Vendor ID : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
        << "Device Device ID : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
        << "Device Video Mem : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
        << "Device Sys Mem   : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
        << "Share Sys Mem    : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
        << std::endl
        << "    ====== OUTPUT ======"sv << std::endl;

      dxgi::output_t::pointer output_p {};
      for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
        dxgi::output_t output {output_p};

        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        auto device_name = utf_utils::to_utf8(desc.DeviceName);

        auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        BOOST_LOG(debug)
          << "    Output Name       : "sv << device_name << std::endl
          << "    AttachedToDesktop : "sv << (desc.AttachedToDesktop ? "yes"sv : "no"sv) << std::endl
          << "    Resolution        : "sv << width << 'x' << height << std::endl
          << std::endl;

        // Don't include the display in the list if we can't actually capture it
        if (desc.AttachedToDesktop && dxgi::test_dxgi_duplication(adapter, output, true)) {
          display_names.emplace_back(std::move(device_name));
        }
      }
    }

    return display_names;
  }

  /**
   * @brief Check whether DXGI reports that adapter or driver enumeration is stale.
   *
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration() {
    // Serialize access to the static DXGI factory
    static std::mutex reenumeration_state_lock;
    auto lg = std::lock_guard(reenumeration_state_lock);

    // Keep a reference to the DXGI factory, which will keep track of changes internally.
    static dxgi::factory1_t factory;
    if (!factory || !factory->IsCurrent()) {
      factory.reset();

      auto status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
        factory.release();
      }

      // Always request reenumeration on the first streaming session just to ensure we
      // can deal with any initialization races that may occur when the system is booting.
      BOOST_LOG(info) << "Encoder reenumeration is required"sv;
      return true;
    } else {
      // The DXGI factory from last time is still current, so no encoder changes have occurred.
      return false;
    }
  }
}  // namespace platf
