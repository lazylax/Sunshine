/*
 * mfxhook -- inject into sunshine.exe and time the two MFX calls that make up avcodec_send_frame().
 *
 * Why not a proxy libvpl.dll: that needs every one of the real dispatcher's 62 exports re-exported,
 * and MSVC's link.exe refuses to emit PE forwarders from a .def (LNK1120: 60 unresolved externals),
 * which would force a hand-written thunk per function. Injecting and hooking the single
 * GetProcAddress import of the host process needs no export table at all: whatever ffmpeg looks up,
 * we see the lookup, and only the calls we care about are wrapped.
 *
 * Mechanism:
 *   DllMain (process attach) -> install_hook()
 *   install_hook() walks the main module's import directory, finds the IAT slot for
 *   KERNEL32!GetProcAddress, and replaces it with hook_get_proc_address().
 *   hook_get_proc_address() passes everything through to the real GetProcAddress, except:
 *     MFXVideoENCODE_EncodeFrameAsync -> hook_encode  ("submit"; where ffmpeg blocks when no
 *                                        surface is free)
 *     MFXVideoCORE_SyncOperation       -> hook_sync    ("wait for the GPU to finish a frame")
 *   Each wrapper calls the real pointer that the hook itself obtained, and appends one line per call
 *   to C:\mfxproxy.log (override with MFXPROXY_LOG).
 *
 * This is deliberately non-destructive: nothing on disk is replaced, and restarting sunshine.exe
 * removes the hook completely. It only affects the process it is injected into, and it refuses to
 * act at all on any other image name.
 *
 * Log format (parse with scripts/parse-mfxproxy.py):
 *   enc  t=<ms since hook install> n=<count> tid=<thread> us=<elapsed> st=<mfxStatus> surf=<ptr> sync=<ptr>
 *   sync t=<ms since hook install> n=<count> tid=<thread> us=<elapsed> st=<mfxStatus> wait=<mfxU32> sync=<ptr>
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int mfxStatus;      /* MFX_ERR_NONE == 0 */
typedef void *mfxSession;
typedef void *mfxSyncPoint;

typedef mfxStatus (*fn_encode)(mfxSession, void *, void *, void *, mfxSyncPoint *);
typedef mfxStatus (*fn_sync)(mfxSession, mfxSyncPoint, uint32_t);
typedef FARPROC(WINAPI *fn_getprocaddress)(HMODULE, LPCSTR);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static FILE *g_log;
static fn_getprocaddress g_real_get_proc_address;
static fn_encode g_real_encode;
static fn_sync g_real_sync;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_t0;
static volatile LONG g_n_encode;
static volatile LONG g_n_sync;

/* ---------------------------------------------------------------- logging */

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  const char *path = getenv("MFXPROXY_LOG");
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_t0);
  InitializeCriticalSection(&g_lock);
  if (!path || !*path) {
    path = "C:\\mfxproxy.log";
  }
  g_log = fopen(path, "a");
  if (g_log) {
    fprintf(g_log, "# mfxhook installed in pid=%lu\n", (unsigned long) GetCurrentProcessId());
    fflush(g_log);
  }
  return TRUE;
}

static void log_line(const char *fmt, ...) {
  va_list ap;
  InitOnceExecuteOnce(&g_once, log_init, NULL, NULL);
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

static double elapsed_us(LARGE_INTEGER start) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_freq.QuadPart) {
    return 0.0;
  }
  return (double) (now.QuadPart - start.QuadPart) * 1e6 / (double) g_freq.QuadPart;
}

static double ms_since_install(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_freq.QuadPart || !g_t0.QuadPart) {
    return 0.0;
  }
  return (double) (now.QuadPart - g_t0.QuadPart) * 1e3 / (double) g_freq.QuadPart;
}

/* ------------------------------------------------------------- hook funcs */

static mfxStatus hook_encode(mfxSession session, void *ctrl, void *surface, void *bitstream, mfxSyncPoint *syncp) {
  LARGE_INTEGER start;
  mfxStatus status;
  LONG n;

  QueryPerformanceCounter(&start);
  status = g_real_encode ? g_real_encode(session, ctrl, surface, bitstream, syncp)
                         : -3 /* MFX_ERR_UNDEFINED_BEHAVIOR */;
  n = InterlockedIncrement(&g_n_encode);
  log_line("enc t=%.0f n=%ld tid=%lu us=%.0f st=%d surf=%p sync=%p",
           ms_since_install(), (long) n, (unsigned long) GetCurrentThreadId(), elapsed_us(start),
           (int) status, surface, (void *) (syncp ? *syncp : NULL));
  return status;
}

static mfxStatus hook_sync(mfxSession session, mfxSyncPoint syncp, uint32_t wait) {
  LARGE_INTEGER start;
  mfxStatus status;
  LONG n;

  QueryPerformanceCounter(&start);
  status = g_real_sync ? g_real_sync(session, syncp, wait) : -3;
  n = InterlockedIncrement(&g_n_sync);
  log_line("sync t=%.0f n=%ld tid=%lu us=%.0f st=%d wait=%lu sync=%p",
           ms_since_install(), (long) n, (unsigned long) GetCurrentThreadId(), elapsed_us(start),
           (int) status, (unsigned long) wait, (void *) syncp);
  return status;
}

static FARPROC WINAPI hook_get_proc_address(HMODULE module, LPCSTR name) {
  if (!g_real_get_proc_address) {
    return NULL;
  }
  if (name) {
    if (strcmp(name, "MFXVideoENCODE_EncodeFrameAsync") == 0) {
      g_real_encode = (fn_encode) g_real_get_proc_address(module, name);
      log_line("# enc resolved real=%p", (void *) g_real_encode);
      return (FARPROC) &hook_encode;
    }
    if (strcmp(name, "MFXVideoCORE_SyncOperation") == 0) {
      g_real_sync = (fn_sync) g_real_get_proc_address(module, name);
      log_line("# sync resolved real=%p", (void *) g_real_sync);
      return (FARPROC) &hook_sync;
    }
  }
  return g_real_get_proc_address(module, name);
}

/* ---------------------------------------------------------------- IAT patch */

static int name_matches(LPCSTR a, const char *b) {
  if (!a) {
    return 0;
  }
  return _stricmp(a, b) == 0;
}

static int patch_main_module_iat(const char *dll, const char *func, void *replacement, void **original) {
  BYTE *base = (BYTE *) GetModuleHandleW(NULL);
  IMAGE_DOS_HEADER *dos;
  IMAGE_NT_HEADERS64 *nt;
  IMAGE_IMPORT_DESCRIPTOR *imp;
  DWORD protect;

  if (!base) {
    return 0;
  }
  dos = (IMAGE_DOS_HEADER *) base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return 0;
  }
  nt = (IMAGE_NT_HEADERS64 *) (base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return 0;
  }
  imp = (IMAGE_IMPORT_DESCRIPTOR *) (base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

  for (; imp->Name; imp++) {
    const char *module_name = (const char *) (base + imp->Name);
    IMAGE_THUNK_DATA64 *thunk;
    IMAGE_THUNK_DATA64 *iat;

    if (dll && !name_matches(module_name, dll)) {
      continue;
    }
    thunk = (IMAGE_THUNK_DATA64 *) (base + imp->OriginalFirstThunk);
    iat = (IMAGE_THUNK_DATA64 *) (base + imp->FirstThunk);
    for (; thunk->u1.AddressOfData; thunk++, iat++) {
      const char *import_name;
      if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
        continue;
      }
      import_name = (const char *) (base + (DWORD) thunk->u1.AddressOfData + 2);  /* skip Hint */
      if (!name_matches(import_name, func)) {
        continue;
      }
      if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protect)) {
        return 0;
      }
      if (original) {
        *original = (void *) iat->u1.Function;
      }
      iat->u1.Function = (ULONGLONG) replacement;
      VirtualProtect(&iat->u1.Function, sizeof(void *), protect, &protect);
      return 1;
    }
  }
  return 0;
}

/* -------------------------------------------------------------- entry point */

static void install_hook(void) {
  wchar_t exe[MAX_PATH];
  const wchar_t *base_name;

  if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
    return;
  }
  base_name = wcsrchr(exe, L'\\');
  base_name = base_name ? base_name + 1 : exe;
  /* Only ever act on the process we were built for; this DLL may be injected by a loop or left in a
   * debugger's list, and hooking an unrelated program's GetProcAddress would be a real bug. */
  if (_wcsicmp(base_name, L"sunshine.exe") != 0) {
    return;
  }

  log_line("# mfxhook attaching to %ls", exe);
  if (patch_main_module_iat("KERNEL32.dll", "GetProcAddress", (void *) &hook_get_proc_address,
                            (void **) &g_real_get_proc_address)) {
    log_line("# GetProcAddress IAT patched (original=%p)", (void *) g_real_get_proc_address);
  } else {
    log_line("# FAILED to patch GetProcAddress IAT");
  }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
  (void) self;
  (void) reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    /* Only kernel32 calls here: we are inside the loader lock. The log is opened lazily on first use
     * so no file I/O happens under that lock. */
    install_hook();
  } else if (reason == DLL_PROCESS_DETACH && g_log) {
    log_line("# mfxhook detaching enc=%ld sync=%ld", (long) g_n_encode, (long) g_n_sync);
    fclose(g_log);
    g_log = NULL;
  }
  return TRUE;
}
