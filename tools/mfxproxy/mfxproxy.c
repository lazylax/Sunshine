/*
 * mfxproxy -- a timing shim that sits in front of Intel's oneVPL dispatcher (libvpl.dll).
 *
 * Why: Sunshine's "Frame processing latency" and even our per-stage syslat probes can only say that
 * the whole frame time is spent inside avcodec_send_frame(). ffmpeg's QSV encoder reaches the GPU
 * through MFX, and on this machine there is no tracing facility to ask (Intel's libvpl.dll 2.15.0.0
 * and libmfxhw64.dll contain no ONEvPL_TRACE / MFX_TRACE / libvpl_tracer strings at all, and no
 * tracer DLL is installed). So the split has to be measured by interposing on the API boundary:
 *
 *   MFXVideoENCODE_EncodeFrameAsync -> "submit" (this is where ffmpeg blocks when no surface is free)
 *   MFXVideoCORE_SyncOperation       -> "wait for the GPU to finish this frame"
 *
 * Everything else in libvpl.dll's export set is re-exported as a PE forwarder by libvpl.def, so no
 * wrapper code is needed for calls we do not care about. The forwarders point at libvpl_real.dll -- a
 * byte copy of the untouched dispatcher that must be reachable through the normal DLL search path
 * (a copy in System32 works, because the app directory is searched first and only the *proxy* is
 * placed next to sunshine.exe).
 *
 * Build: cl /nologo /LD /O2 /Fe:libvpl.dll mfxproxy.c /link /DEF:libvpl.def
 *
 * Log format (one line per logged event, machine-parseable, append mode):
 *   enc  n=<frame counter> tid=<thread> us=<elapsed> st=<mfxStatus> surf=<surface ptr> sync=<syncpoint>
 *   sync n=<call counter>   tid=<thread> us=<elapsed> st=<mfxStatus> wait=<mfxU32> sync=<syncpoint>
 *   # mfxproxy loaded real=... enc=... sync=...
 * Submit calls are always logged; sync calls are always logged (they are the interesting ones and
 * there is at most one per frame), which keeps the overhead far below one line per frame.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int mfxStatus;      /* MFX_ERR_NONE == 0 */
typedef void *mfxSession;
typedef void *mfxSyncPoint;

typedef mfxStatus (*fn_encode)(mfxSession, void *, void *, void *, mfxSyncPoint *);
typedef mfxStatus (*fn_sync)(mfxSession, mfxSyncPoint, uint32_t);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static HMODULE g_real;
static FILE *g_log;
static fn_encode g_encode;
static fn_sync g_sync;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_t0;   /* set once at load, so every line carries a monotonic ms stamp */
static volatile LONG g_n_encode;
static volatile LONG g_n_sync;

static BOOL CALLBACK proxy_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  const char *real_path;
  const char *log_path;

  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_t0);
  InitializeCriticalSection(&g_lock);

  /* Full path first so the search cannot come back to this proxy. An env override makes it easy to
   * point at a different dispatcher without rebuilding. */
  real_path = getenv("MFXPROXY_REAL");
  if (!real_path || !*real_path) {
    real_path = "libvpl_real.dll";
  }
  g_real = LoadLibraryA(real_path);
  if (g_real) {
    g_encode = (fn_encode) GetProcAddress(g_real, "MFXVideoENCODE_EncodeFrameAsync");
    g_sync = (fn_sync) GetProcAddress(g_real, "MFXVideoCORE_SyncOperation");
  }

  log_path = getenv("MFXPROXY_LOG");
  if (!log_path || !*log_path) {
    log_path = "C:\\mfxproxy.log";
  }
  g_log = fopen(log_path, "a");
  if (g_log) {
    fprintf(g_log, "# mfxproxy loaded real=%s handle=%p encode=%p sync=%p\n",
            real_path, (void *) g_real, (void *) g_encode, (void *) g_sync);
    fflush(g_log);
  }
  return TRUE;
}

static double elapsed_us(LARGE_INTEGER start) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_freq.QuadPart) {
    return 0.0;
  }
  return (double) (now.QuadPart - start.QuadPart) * 1e6 / (double) g_freq.QuadPart;
}

/* Milliseconds since this proxy was initialised. Lets the MFX log be lined up with sunshine.log /
 * the syslat probes (which are wall clock) and gives every line a rate context; without it the two
 * logs can only be compared by event order. */
static double ms_since_load(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_freq.QuadPart || !g_t0.QuadPart) {
    return 0.0;
  }
  return (double) (now.QuadPart - g_t0.QuadPart) * 1e3 / (double) g_freq.QuadPart;
}

static void log_line(const char *fmt, ...) {
  va_list ap;
  if (!g_log) {
    return;
  }
  EnterCriticalSection(&g_lock);
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
  LeaveCriticalSection(&g_lock);
}

mfxStatus MFXVideoENCODE_EncodeFrameAsync(mfxSession session, void *ctrl, void *surface, void *bitstream, mfxSyncPoint *syncp) {
  LARGE_INTEGER start;
  mfxStatus status;
  LONG n;

  InitOnceExecuteOnce(&g_once, proxy_init, NULL, NULL);
  QueryPerformanceCounter(&start);
  status = g_encode ? g_encode(session, ctrl, surface, bitstream, syncp) : -3 /* MFX_ERR_UNDEFINED_BEHAVIOR */;
  n = InterlockedIncrement(&g_n_encode);

  /* Log every call: at 4K/48 fps this is ~48 lines per second, far below the rate at which logging
   * itself would perturb the measurement, and a sub-sampled view would hide the blocking submits. */
  log_line("enc t=%.0f n=%ld tid=%lu us=%.0f st=%d surf=%p sync=%p",
           ms_since_load(), (long) n, (unsigned long) GetCurrentThreadId(), elapsed_us(start), (int) status,
           surface, (void *) (syncp ? *syncp : NULL));
  return status;
}

mfxStatus MFXVideoCORE_SyncOperation(mfxSession session, mfxSyncPoint syncp, uint32_t wait) {
  LARGE_INTEGER start;
  mfxStatus status;
  LONG n;

  InitOnceExecuteOnce(&g_once, proxy_init, NULL, NULL);
  QueryPerformanceCounter(&start);
  status = g_sync ? g_sync(session, syncp, wait) : -3;
  n = InterlockedIncrement(&g_n_sync);

  log_line("sync t=%.0f n=%ld tid=%lu us=%.0f st=%d wait=%lu sync=%p",
           ms_since_load(), (long) n, (unsigned long) GetCurrentThreadId(), elapsed_us(start), (int) status,
           (unsigned long) wait, (void *) syncp);
  return status;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
  (void) self;
  (void) reserved;
  if (reason == DLL_PROCESS_DETACH && g_log) {
    log_line("# mfxproxy detach enc=%ld sync=%ld", (long) g_n_encode, (long) g_n_sync);
    fclose(g_log);
    g_log = NULL;
  }
  return TRUE;
}
