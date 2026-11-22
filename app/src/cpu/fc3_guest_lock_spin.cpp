// fc3-switch 2026-11-05: bounded spin before blocking on the guest global lock (Switch only).
//
// Why: the recompiled code emulates mfmsr/mtmsrd (interrupt disable around ldarx/stdcx and guest spinlocks) with
// REX_CHECK_GLOBAL_LOCK / REX_ENTER_GLOBAL_LOCK (fc3_pch.h, 21398 sites), i.e. rex::thread::global_critical_region,
// a std::recursive_mutex. On Horizon a contended libnx mutex goes straight to svcArbitrateLock (no spinning).
// rex_profile.log of 05.11 (work/perf-20261105): game threads spend 30-50 % of their samples inside
// pthread_mutex_lock of this mutex (XThread25006900 50 %, Core_0/1/2 21-41 %, AudioRend 41 %).
// The SDK already spins for its own uses of the region (sdk/src/system/global_lock_spin.h, cvar global_lock_spin);
// the generated code calls mutex().lock() inline, so it cannot be changed without regenerating and recompiling
// every game TU. Instead the link wraps pthread_mutex_lock (-Wl,--wrap=pthread_mutex_lock, app/CMakeLists.txt):
// for this one mutex, try-lock up to fc3_guest_lock_spin times before the normal blocking lock.
//
// Same mutual exclusion and recursion as before, only the waiting strategy changes. 0 = exactly the old
// behaviour (one pointer compare per lock). Default 256 since 2026-11-05 (hardware A/B: +44 % FPS). A/B by TOML.
#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread/mutex.h>

// ppc_msr_global_lock_() (the lock-free switch of the generated mfmsr/mtmsrd brackets, fc3_pch.h).
#include "../../generated/default/fc3_pch.h"

// fc3-switch 2026-11-07 EXPERIMENT (all platforms): false (default) = old behaviour, the brackets take the global
// mutex. true = mfmsr/mtmsrd brackets are only a per-thread depth (no mutex). Rationale and limits: fc3_pch.h,
// work/lockfree-scan/report.txt and host-audit.txt (the host graphics interrupt still takes the mutex and no
// longer excludes guest brackets in this mode).
REXCVAR_DEFINE_BOOL(fc3_guest_msr_lockfree, false, "fc3 enhancements",
                    "EXPERIMENT: guest mfmsr/mtmsrd brackets (lwarx..stwcx. sequences) without the global lock; "
                    "false = old behaviour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
#include <pthread.h>

REXCVAR_DEFINE_INT32(fc3_guest_lock_spin, 256, "fc3 enhancements",
                     "Switch: try-locks of the guest global lock (mfmsr/mtmsrd emulation) before blocking in the "
                     "kernel; 0 = block at once (old behaviour); default 256 (hardware A/B 2026-11-05)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// Set once by fc3::GuestLockSpinInstall() after the configuration is read. Never touched before: the wrapper must not
// call global_critical_region::mutex() itself, because the first call runs a static-init guard that takes a pthread
// mutex and would re-enter the wrapper.
std::atomic<pthread_mutex_t*> g_guest_lock{nullptr};
int32_t g_spin = 0;

inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}
}  // namespace

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* m);

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {
  if (m == g_guest_lock.load(std::memory_order_relaxed)) {
    for (int32_t i = g_spin; i > 0; --i) {
      if (pthread_mutex_trylock(m) == 0) return 0;
      CpuRelax();
    }
  }
  return __real_pthread_mutex_lock(m);
}

#endif  // __SWITCH__

namespace fc3 {
void GuestLockSpinInstall() {
  // Called from enh::Resolve (ReXApp::OnPostInitLogging): after the config is read, before the runtime and any guest
  // thread exist, so every bracket of the run sees the same mode.
  const bool lockfree = REXCVAR_GET(fc3_guest_msr_lockfree);
  ppc_msr_global_lock_() = !lockfree;
  if (lockfree) {
    REXLOG_WARN("[lockfree] fc3_guest_msr_lockfree=true: EXPERIMENT, guest mfmsr/mtmsrd brackets run without the "
                "global lock (per-thread depth only)");
  } else {
    REXLOG_INFO("[lockfree] fc3_guest_msr_lockfree=false: guest mfmsr/mtmsrd brackets take the global lock");
  }
#if defined(__SWITCH__)
  g_spin = REXCVAR_GET(fc3_guest_lock_spin);
  if (g_spin <= 0) {
    REXLOG_INFO("[lockspin] fc3_guest_lock_spin=0: guest global lock blocks at once (old behaviour)");
    return;
  }
  pthread_mutex_t* handle = rex::thread::global_critical_region::mutex().native_handle();
  g_guest_lock.store(handle, std::memory_order_release);
  REXLOG_INFO("[lockspin] fc3_guest_lock_spin={}: guest global lock {} spins before blocking", g_spin,
              static_cast<void*>(handle));
#endif
}
}  // namespace fc3
