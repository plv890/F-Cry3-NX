// Temporary boot-debugging hooks: log who calls a function and with which arguments, then call the original.
// Each entry says which crash it is chasing; remove it once the cause is fixed. Empty while no crash needs one.
//
// Pattern (the setjmp/longjmp case of the fourth boot is written up in docs/fc3-notes.md):
//   extern "C" REX_FUNC(__imp__sub_XXXXXXXX);
//   REX_HOOK_RAW(sub_XXXXXXXX) {
//     FC3_DBG_WARN("[dbg] sub_XXXXXXXX(r3={:08X}) from lr={:08X}", ctx.r3.u32, uint32_t(ctx.lr));
//     __imp__sub_XXXXXXXX(ctx, base);
//   }

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>
// PROTOTYPE (fc3-switch, 2026-11-01): the [dbg] trace lines are synchronous log writes (~2.5 ms each on the Switch)
// and stretch boot from ~1 s to ~75 s there, changing fiber/job timing. Compiled out on the Switch unless
// FC3_SWITCH_DBG_LOG is defined; on PC they stay.
#if !defined(_WIN32) && !defined(FC3_SWITCH_DBG_LOG)
#define FC3_DBG_WARN(...) ((void)0)
#else
#define FC3_DBG_WARN(...) REXLOG_WARN(__VA_ARGS__)
#endif

// One-shot write watch for the corruptible node (defined in fc3_crash_log.cpp on
// Windows; no-op stub elsewhere so the Switch build links).
void Fc3ArmNodeWatch(uint8_t* host_base, uint32_t guest_addr);
#if !defined(_WIN32)
namespace {
struct Fc3WatchStub {
  static void Arm(uint8_t*, uint32_t) {}
};
}  // namespace
#define Fc3ArmNodeWatch Fc3WatchStub::Arm
#endif

// ---------------------------------------------------------------------------------------------
// CRASH UNDER INVESTIGATION: out\pc\logs\fc3_002.log onwards (2026-10-10)
//
//   [error] Unhandled guest access violation: read of guest 0x0000013C on thread 0xF8000060
//   [error] Unhandled guest access violation at host PC ... (fc3.exe+0x79E9405)
//
// Six guest threads fault in the same millisecond, all reading exactly 0x13C. Six threads reading one
// address at one instant is not register corruption: it is a single shared value that is still zero.
// 0x13C is offset 316 (0x13C) from a null base.
//
// llvm-symbolizer blames sub_8237E408 (fc3_recomp.347.cpp:319, "lwz r11,316(r28)") because that is the
// nearest preceding symbol in the PDB, but the hooks below show the fault happens several calls deeper:
// sub_8237E408 -> sub_8362B1C0 -> sub_82342460 -> sub_8240B808 -> sub_83611CD0. The symbolizer is wrong,
// not the translation - the generated code is right, the trace below is what actually runs.
//
// The heap the allocator checks against still holds its initial values at the moment of the fault
// ([0xA0000000, 0xFFFFFFFF]), which is the thing worth explaining. Everything else here is scaffolding to
// find where that happens.
// ---------------------------------------------------------------------------------------------

namespace {

// The boot crashes within the first few hundred calls, so a small cap is enough to see the pattern
// and keeps a 6-thread crash loop from filling the log.
constexpr int kMaxLogs = 400;

// One slot per traced function. A flat per-function cap is not enough here: the boot makes several hundred
// calls to sub_83611CD0 before the faulting thread arrives, so the cap is always spent on the noise and the
// last call - the one that crashes - is exactly the one that goes unlogged. The counters are rewound on
// first entry into the faulting function instead, which puts the whole crash window inside the cap.
std::atomic<int> g_callee[16]{};

std::atomic<int> g_entries{0};
std::atomic<int> g_clobbers{0};
std::atomic<int> g_b1c0_calls{0};
std::atomic<int> g_b1c0_changes{0};

// REX_LOAD_U32 lives in the generated PCH, which the app does not include. Same thing by hand: the guest arena
// is big-endian, and on Windows addresses at or above 0xE0000000 are MMIO and get a 0x1000 host offset
// (REX_PHYS_HOST_OFFSET in the PCH). Neither applies to a heap-bound word, but the offset is kept so this
// reads exactly what the callee reads.
// Host offset of a guest address, exactly as the generated code computes it (REX_PHYS_HOST_OFFSET in fc3_pch.h):
// 0x1000 for >= 0xE0000000 on Windows (and macOS arm64), 0 elsewhere. fc3-switch 2026-11-01: this was hard-coded to
// 0x1000, so on the Switch every hook read (and the PROTOTYPE link-null hack wrote) one page off — the "BAD table
// 0xA3A3A3A3" seen only on the Switch was the hook reading the neighbouring page.
inline uint32_t Fc3HostOff(uint32_t addr) {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
  return addr >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)addr;
  return 0u;
#endif
}
inline uint32_t GuestLoad32(uint8_t* base, uint32_t addr) {
  const uint32_t host_off = Fc3HostOff(addr);
  uint32_t v;
  __builtin_memcpy(&v, base + addr + host_off, sizeof(v));
  return __builtin_bswap32(v);
}

// The heap bounds sub_8240B808 compares against: lis r11,-31774 / addi r11,r11,-19200 = 0x83E1B500, and the
// word after it. Plus "lis r11,16384" = 0x40000000, the sentinel that routes a pointer to the error handler.
constexpr uint32_t kHeapMin = 0x83E1B500;
constexpr uint32_t kHeapMax = 0x83E1B504;
constexpr uint32_t kSentinel = 0x40000000;

// sub_8237E408's indirect calls all go through one entry: r28 = 0x83E1A148 (lis r11,-31774 / addi
// r11,r11,-24248), then "lwz r11,316(r28)" loads a function pointer from 0x83E1A284 and "bctrl" calls it.
constexpr uint32_t kCallbackTable = 0x83E1A284;

}  // namespace

// The function the first log line names. Its first entry is the crash point every time, so that is where the
// trace window is rewound; entry and exit also answer whether r28 survives the calls, which is what the
// first two hypotheses about this crash were about.
// The target of the three indirect calls in sub_8237E408, found by following the table it goes through:
//   [0x83E1A284] = 0x83EDCE44  ->  [0x83EDCE44] = 0x82405188  ->  bctrl
// sub_82405188 is a two-instruction thunk ("b 0x82412ee8"), and sub_82412EE8 is the allocator's front door:
// it calls sub_82412D40 first (the heap-lazy-init check) and then either sub_8240D130 or sub_8240B778.
// None of them is on the path: the last traced line before the fault is "OUT sub_8362B1C0", which is a direct
// call further down sub_8237E408. So the fault is between the two - in the code the SDK emits for bctrl
// itself, which is why no hook fires. ctx.last_indirect_target is the SDK's record of what bctrl was about
// to call (set unconditionally, per rex/ppc/context.h), so it names the real target without a hook.
REX_EXTERN(__imp__sub_8237E408);
REX_HOOK_RAW(sub_8237E408) {
  const uint32_t r28_in = ctx.r28.u32;
  const uint32_t lr_in = static_cast<uint32_t>(ctx.lr);
  const uint32_t ind_in = ctx.last_indirect_target;

  const int n = g_entries.fetch_add(1) + 1;
  if (n == 1) {
    for (auto& c : g_callee) {
      c.store(0);
    }
    FC3_DBG_WARN("[dbg] === trace window reset (first entry into the function the crash log names) ===");
  }
  if (n <= kMaxLogs) {
    FC3_DBG_WARN("[dbg] sub_8237E408 #{:d} from lr={:08X}: r28={:08X} r31={:08X} r30={:08X} r29={:08X} r3={:08X} "
                "last_indirect_target={:08X}",
                n, lr_in, r28_in, ctx.r31.u32, ctx.r30.u32, ctx.r29.u32, ctx.r3.u32, ind_in);
  }

  __imp__sub_8237E408(ctx, base);

  const uint32_t r28_out = ctx.r28.u32;
  if (r28_out != r28_in) {
    const int c = g_clobbers.fetch_add(1) + 1;
    if (c <= kMaxLogs) {
      FC3_DBG_WARN("[dbg] sub_8237E408 RETURNED with r28 {:08X} -> {:08X} (clobbered)",
                  r28_in, r28_out);
    }
  }
  return;
}

// sub_8362B1C0 allocates the thread's kernel stack: "mr r28,r4" and no save of r28 anywhere in the function.
// It is the last call that returns before the fault, so its exit registers are the ones the faulting load
// uses: after it returns, sub_8237E408 does "stw r3,12(r30)" and then "lwz r10,300(r31)". r30 and r31 are
// non-volatile, so whatever they hold here is what the caller still believes. r28 survives (its logged value
// comes straight out of the lis/addi pair in the caller), which ruled out the first hypothesis about this
// crash; r30 and r31 are what this hook is here to measure.
REX_EXTERN(__imp__sub_8362B1C0);
REX_HOOK_RAW(sub_8362B1C0) {
  const uint32_t r28_in = ctx.r28.u32;
  const uint32_t r4_in = ctx.r4.u32;

  const int n = g_b1c0_calls.fetch_add(1) + 1;
  if (n <= kMaxLogs) {
    FC3_DBG_WARN("[dbg] IN  sub_8362B1C0 #{:d}: r4={:08X} r28={:08X} r30={:08X} r31={:08X}", n, r4_in, r28_in,
                ctx.r30.u32, ctx.r31.u32);
  }

  __imp__sub_8362B1C0(ctx, base);
  if (n <= kMaxLogs) {
    FC3_DBG_WARN("[dbg] OUT sub_8362B1C0 #{:d}: r3={:08X} r28={:08X} r30={:08X} r31={:08X} "
                "(next: [r30+12]={:08X} [r31+300]={:08X})",
                n, ctx.r3.u32, ctx.r28.u32, ctx.r30.u32, ctx.r31.u32, ctx.r30.u32 + 12, ctx.r31.u32 + 300);
  }

  if (ctx.r28.u32 != r28_in) {
    const int c = g_b1c0_changes.fetch_add(1) + 1;
    if (c <= kMaxLogs) {
      FC3_DBG_WARN("[dbg] sub_8362B1C0 CLOBBERED r28: {:08X} -> {:08X} (r4 was {:08X})",
                  r28_in, ctx.r28.u32, r4_in);
    }
  }
  return;
}

// Traces one callee: logs entry and exit, so a fault that happens inside shows up as an IN line with no
// matching OUT line. The return address is logged too, because reading the caller's C++ cannot say which of
// its several call sites was taken - the flags in r4 select among them - and guessing from the flags got the
// wrong answer twice.
#define FC3_TRACE_CALLEE(fn, slot) FC3_TRACE_CALLEE_AFTER(fn, slot, 0)

#define FC3_TRACE_CALLEE_AFTER(fn, slot, skip)                                     \
  REX_EXTERN(__imp__##fn);                                                         \
  REX_HOOK_RAW(fn) {                                                               \
    const int n = g_callee[slot].fetch_add(1) + 1;                                \
    const bool log = n >= (skip) && n < (skip) + kMaxLogs;                       \
    if (log) {                                                                    \
      FC3_DBG_WARN("[dbg] IN  {} #{} from lr={:08X} r3={:08X} r4={:08X} r5={:08X}", \
                  #fn, n, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32,   \
                  ctx.r5.u32);                                                     \
    }                                                                              \
    __imp__##fn(ctx, base);                                                        \
    if (log) {                                                                    \
      FC3_DBG_WARN("[dbg] OUT {} #{} r3={:08X}", #fn, n, ctx.r3.u32);              \
    }                                                                              \
    return;                                                                        \
  }

FC3_TRACE_CALLEE(sub_82342460, 0)   // allocate(size, flags); the flags select one of three paths
FC3_TRACE_CALLEE(sub_82346BF0, 1)
FC3_TRACE_CALLEE(sub_823425D0, 2)
FC3_TRACE_CALLEE(sub_8360DD80, 3)   // memset
// free(): manual hook (same log format as the macro) that also reports frees of the tracked name-table block.
REX_EXTERN(__imp__sub_8240A080);
REX_HOOK_RAW(sub_8240A080) {
  const int n = g_callee[13].fetch_add(1) + 1;
  const bool log = n < kMaxLogs;
  if (log) {
    FC3_DBG_WARN("[dbg] IN  {} #{} from lr={:08X} r3={:08X} r4={:08X} r5={:08X}", "sub_8240A080", n,
                 static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  // DIAGNOSTIC (fc3-switch, 2026-11-01): who frees F90AC130 (the name table read as heap fill on the Switch), on which
  // guest thread (r13 = its PCR). The free may take the block pointer in r3 or r4: log both matches.
  if (ctx.r3.u32 == 0xF90AC130u || ctx.r4.u32 == 0xF90AC130u) {
    static std::atomic<int> nf{0};
    if (nf.fetch_add(1) < 24)
      REXLOG_WARN("[track] free F90AC130 #{} lr={:08X} r3={:08X} r4={:08X} pcr={:08X}", n,
                  static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r13.u32);
  }
  __imp__sub_8240A080(ctx, base);
  if (log) {
    FC3_DBG_WARN("[dbg] OUT {} #{} r3={:08X}", "sub_8240A080", n, ctx.r3.u32);
  }
}
FC3_TRACE_CALLEE(sub_83611CD0, 14)  // the heap's invalid-pointer handler

// The target of the three indirect calls in sub_8237E408, found by following the table it goes through:
//   [0x83E1A284] = 0x83EDCE44  ->  [0x83EDCE44] = 0x82405188  ->  bctrl
// sub_82405188 is a two-instruction thunk ("b 0x82412ee8"), and sub_82412EE8 is the allocator's front door:
// it calls sub_82412D40 first (the heap-lazy-init check) and then either sub_8240D130 or sub_8240B778.
// All four are traced, because "every traced function returned" pointed at one of them.
FC3_TRACE_CALLEE(sub_82405188, 8)
FC3_TRACE_CALLEE(sub_82412EE8, 9)
FC3_TRACE_CALLEE(sub_82412D40, 10)
// sub_8240D130: manual hook (same log format as the macro) that also arms the F90CAA20
// write watch when the deterministic corrupt node comes out of the pool allocator.
REX_EXTERN(__imp__sub_8240D130);
REX_HOOK_RAW(sub_8240D130) {
  const int n = g_callee[11].fetch_add(1) + 1;
  const bool log = n < kMaxLogs;
  if (log) {
    FC3_DBG_WARN("[dbg] IN  {} #{} from lr={:08X} r3={:08X} r4={:08X} r5={:08X}", "sub_8240D130",
                n, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  const uint32_t req_size = ctx.r3.u32;
  __imp__sub_8240D130(ctx, base);
  if (log) {
    FC3_DBG_WARN("[dbg] OUT {} #{} r3={:08X}", "sub_8240D130", n, ctx.r3.u32);
  }
  if (ctx.r3.u32 == 0xF90AC130u) {
    // DIAGNOSTIC (fc3-switch, 2026-11-01): who gets the name-table block, on which guest thread.
    static std::atomic<int> na{0};
    if (na.fetch_add(1) < 24)
      REXLOG_WARN("[track] alloc F90AC130 #{} size {:X} lr={:08X} pcr={:08X}", n, req_size,
                  static_cast<uint32_t>(ctx.lr), ctx.r13.u32);
  }
  {
    // DIAGNOSTIC (PC/Switch delta, 2026-11-01): FC3_WATCH_GUEST=<addr> arms the write watch as soon as the block
    // that contains it is allocated (the name table F90AC130 is filled before sub_8241B790 first runs).
    static const uint32_t watch = [] {
      const char* e = std::getenv("FC3_WATCH_GUEST");
      return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    if (watch && ctx.r3.u32 && watch >= ctx.r3.u32 && watch - ctx.r3.u32 < (req_size ? req_size : 4u)) {
      REXLOG_WARN("[watch-early] block {:08X} size {:X} contains {:08X} (lr={:08X}), arming", ctx.r3.u32, req_size,
                  watch, static_cast<uint32_t>(ctx.lr));
      Fc3ArmNodeWatch(base, watch);
    }
  }
  if (ctx.r3.u32 == 0xF90CAA20u) {
    FC3_DBG_WARN("[dbg] alloc40988: node={:08X}, arming watch", ctx.r3.u32);
    Fc3ArmNodeWatch(base, ctx.r3.u32);
  }
  return;
}
FC3_TRACE_CALLEE(sub_8240B778, 12)
FC3_TRACE_CALLEE(sub_824053B8, 15)

// sub_83611CD0(ptr) with ptr != 0 goes to sub_82346BE0 then sub_82344988(0, 0, ptr). If the last argument
// turns out to be a format string, the hook below prints what the allocator was complaining about.
FC3_TRACE_CALLEE(sub_82346BE0, 4)
FC3_TRACE_CALLEE(sub_82344988, 5)
FC3_TRACE_CALLEE(sub_836148C8, 6)
FC3_TRACE_CALLEE(sub_83611EA0, 7)

// sub_8240B808 is the free(). Reading its C++ showed four outcomes and none of them touches 0x13C, so the
// trace reads the same two words the callee is about to read and prints which branch actually runs.
REX_EXTERN(__imp__sub_8240B808);
REX_HOOK_RAW(sub_8240B808) {
  const int n = g_callee[12].fetch_add(1) + 1;
  if (n <= kMaxLogs) {
    const uint32_t ptr = ctx.r3.u32;
    const uint32_t lo = GuestLoad32(base, kHeapMin);
    const uint32_t hi = GuestLoad32(base, kHeapMax);
    const char* branch = "?";
    if (ptr < lo || ptr > hi) {
      branch = (ptr >= kSentinel) ? "-> sub_83611CD0" : "-> lwz r3,-16(ptr); ret";
    } else {
      branch = "-> sub_8240A080; ret";
    }
    FC3_DBG_WARN("[dbg] sub_8240B808 #{} ptr={:08X} heap=[{:08X},{:08X}] {} (r4={:08X} r5={:08X})",
                n, ptr, lo, hi, branch, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_8240B808(ctx, base);
  if (n <= kMaxLogs) {
    FC3_DBG_WARN("[dbg] OUT sub_8240B808 #{} r3={:08X}", n, ctx.r3.u32);
  }
  return;
}

// ---------------------------------------------------------------------------------------------
// WHERE r28 DIES (2026-10-10)
//
// The faulting instruction was disassembled instead of symbolised, because the previous note above says the
// symbolizer is wrong. It is not. At fc3.exe+0x79E9405 the machine code is:
//
//   movl 0xe0(%rdi), %eax     ; eax = ctx.r28.u32      (r28 sits at 0xE0 of PPCContext)
//   movl $0x13c, %ecx
//   addl %ecx, %eax           ; eax = r28 + 316
//   leaq (%rsi,%rax), %rcx    ; guest base + that
//   cmpl $0xe0000000, %eax
//   shll $0xc, %edx
//   movl (%rdx,%rcx), %eax    ; <-- the fault, guest 0x13C means r28 == 0
//
// That is the ReXGlue guest-load sequence for "lwz r11,316(r28)" (fc3_recomp.347.cpp:319), so the fault is
// where the generated code says it is and r28 is exactly zero.
//
// The open question is only which call destroys it. The body's candidates are sub_8362B0E0, sub_8362B1C0
// (it does "mr r28,r4" -> 0x8237E168, not zero) and sub_8362B300, which is a two-line thunk
// ("b 0x8363ce50") onto sub_8363CE50 - a routine with no prologue that copies 128 vector registers and does
// "ld r28,264(r3)" before tail-branching into KeSetCurrentStackPointers.
//
// Answered by the hooks below (fc3_004.log):
//
//   IN  sub_8363CE50 #1 r28=83E1A148 r3=40007730 [r3+264]=00000000
//   OUT sub_8363CE50 #1 r28=00000000  <<< DESTROYED
//
// sub_8363CE50 is the kernel thread switch: "std r28,264(r5)" saves the outgoing context's r28 into the
// thread that owns this PCR and "ld r28,264(r3)" loads the incoming one's. r3 is the context sub_8362B1C0
// just built, so its +264 is zero - correct, the thread never ran. On the console the caller never reads
// r28 again, because the export that ends the routine does not return: the thread is dispatched.
// ReXGlue's KeSetCurrentStackPointers returns, so sub_8237E408 walks back into its loop with r28 = 0.
//
// WORKAROUND, not the fix. Keeping the non-volatile set across the call makes the return look to the guest
// like the branch it was, and the boot gets past the fault. The real fix is to teach the codegen that this
// export does not return, so the tail call stops being a call - see docs/fc3/DIAGNOSTICO-BOOT-0x13C.md.
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// PROTOTYPE (2026-11-01): guest fibers on host fibers (Windows only).
//
// sub_8363CE50 is not a kernel thread switch but the statically linked XAPI SwitchToFiber: sub_8362B0E0 is
// ConvertThreadToFiber (fiber = [KTHREAD+356]), sub_8362B1C0 is CreateFiber (LR slot +28 = 0x8363CE40, the
// fiber start thunk, r31 slot +288 = start routine, r1 slot +48 = new stack), sub_8362B300 is the call thunk.
// Layout of the guest fiber: +4/+8/+12 stack alloc/base/limit, +28 LR, +48 r1, +152..+288 r14..r31,
// +296 CR, +304 FPSCR, +424.. f14..f31, +1616.. v64..v127 (from the routine's own loads and stores).
//
// The workaround below the #else (keep r14..r31 and return) makes every switch a no-op: no fiber ever runs, so
// FC3's job system (sub_8237BCE0 wait -> sub_8235E6B8 "switch to a free fiber and help") starves, the job
// counters never reach zero, the main thread waits forever and the game never calls VdSwap
// (logs/sample-threads-*.txt). Here each guest fiber gets a host fiber and its own PPCContext (fibers may
// resume on another guest thread, and the frames of a suspended fiber hold a reference to its context):
// the guest routine saves the outgoing registers and loads the incoming ones, then the host switches stacks.
// ---------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8363CE50);
#if defined(_WIN32)
extern "C" REX_FUNC(sub_8363CE40);
void Fc3ArmAvBacktrace(uint8_t* host_base);  // fc3_crash_log.cpp (diagnostic)
namespace {
struct Fc3Fiber {
  void* host = nullptr;          // host fiber running this guest fiber
  PPCContext* ctx = nullptr;     // the context its frames use
  std::atomic<bool> running{false};
  uint8_t* base = nullptr;
  uint32_t last_os = 0;          // OS thread it last ran on (statistics)
};
std::mutex g_fiber_mutex;
std::unordered_map<uint32_t, Fc3Fiber*> g_fibers;  // guest fiber address -> host fiber
std::atomic<int> g_fiber_creates{0};
thread_local Fc3Fiber* t_switched_from = nullptr;  // marked not running by whoever runs next on this thread

void Fc3FiberArrived() {
  if (Fc3Fiber* prev = t_switched_from) {
    t_switched_from = nullptr;
    prev->running.store(false, std::memory_order_release);
  }
}

void WINAPI Fc3FiberStart(void* param) {
  auto* f = static_cast<Fc3Fiber*>(param);
  Fc3FiberArrived();
  sub_8363CE40(*f->ctx, f->base);  // never returns on the console (KeBugCheck after the start routine)
  REXLOG_ERROR("[fiber] start routine of a guest fiber returned; parking the host fiber");
  for (;;) ::Sleep(1000);
}

Fc3Fiber* Fc3FiberFor(uint32_t guest, PPCContext* ctx) {
  std::lock_guard lock(g_fiber_mutex);
  Fc3Fiber*& f = g_fibers[guest];
  if (!f) {
    f = new Fc3Fiber();
    f->ctx = ctx;
  }
  return f;
}
}  // namespace

REX_HOOK_RAW(sub_8363CE50) {
  const uint32_t in = ctx.r3.u32;
  const uint32_t out = GuestLoad32(base, GuestLoad32(base, ctx.r13.u32 + 256) + 356);
  if (!in || !out || in == out) {
    __imp__sub_8363CE50(ctx, base);
    return;
  }
  // The current host fiber is the outgoing guest fiber (a thread root becomes a host fiber on first use).
  if (!::IsThreadAFiber()) ::ConvertThreadToFiber(nullptr);
  Fc3ArmAvBacktrace(base);
  Fc3Fiber* self = Fc3FiberFor(out, &ctx);
  self->host = ::GetCurrentFiber();
  self->ctx = &ctx;
  self->base = base;
  self->running.store(true, std::memory_order_relaxed);

  // Saves the outgoing registers to [out], loads lr/cr/fpr/vmx of [in], sets [KTHREAD+356] = in and the
  // kernel stack pointers. The SDK's KeSetCurrentStackPointers keeps r1/r14..r31 (it assumes the switch never
  // happens), so take those from the incoming fiber's save area.
  __imp__sub_8363CE50(ctx, base);
  ctx.r1.u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + in + 48));
  for (int i = 0; i < 18; ++i) {
    (&ctx.r14)[i].u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + in + 152 + 8 * i));
  }

  Fc3Fiber* target;
  {
    std::lock_guard lock(g_fiber_mutex);
    auto it = g_fibers.find(in);
    target = it == g_fibers.end() ? nullptr : it->second;
  }
  if (!target || !target->host) {
    if (static_cast<uint32_t>(ctx.lr) != 0x8363CE40) {
      REXLOG_ERROR("[fiber] switch to unknown guest fiber {:08X} with lr {:08X}: not started (no-op switch)",
                   in, static_cast<uint32_t>(ctx.lr));
      self->running.store(false, std::memory_order_relaxed);
      // Behave like the old workaround: stay on the outgoing fiber.
      for (int i = 0; i < 18; ++i) {
        (&ctx.r14)[i].u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + out + 152 + 8 * i));
      }
      ctx.r1.u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + out + 48));
      return;
    }
    target = Fc3FiberFor(in, nullptr);
    target->ctx = new PPCContext(ctx);
    target->base = base;
    target->running.store(true, std::memory_order_relaxed);
    // Recompiled guest code is stack hungry: reserve 16 MB, commit on demand.
    target->host = ::CreateFiberEx(256 * 1024, 16 * 1024 * 1024, 0, &Fc3FiberStart, target);
    const int n = g_fiber_creates.fetch_add(1) + 1;
    if (n <= 64) REXLOG_INFO("[fiber] #{} host fiber for guest fiber {:08X} (from {:08X})", n, in, out);
    if (!target->host) {
      REXLOG_ERROR("[fiber] CreateFiberEx failed ({})", ::GetLastError());
      std::abort();
    }
  } else {
    // A fiber that is still on its way out on another thread cannot be entered yet.
    bool expected = false;
    while (!target->running.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
      expected = false;
      ::SwitchToThread();
    }
    if (target->ctx != &ctx) *target->ctx = ctx;
  }
  {
    // Statistics: switches, and resumes on another OS thread than the one the fiber last ran on.
    static std::atomic<uint64_t> switches{0}, migrations{0};
    static thread_local uint32_t t_os = ::GetCurrentThreadId();
    const uint64_t n = switches.fetch_add(1) + 1;
    if (target->last_os && target->last_os != t_os) migrations.fetch_add(1);
    target->last_os = t_os;
    if ((n & (n - 1)) == 0 && n >= 64)
      REXLOG_INFO("[fiber] {} switches, {} resumes on another thread", n, migrations.load());
  }
  t_switched_from = self;
  ::SwitchToFiber(target->host);
  // Back on this fiber: whoever switched here loaded our saved registers into our context.
  Fc3FiberArrived();
}
#else
// PROTOTYPE (2026-11-01, Switch): the same guest-fiber scheme as the _WIN32 block above, on the SDK's
// rex::thread::Fiber (sdk/src/core/fiber_switch.cpp: hand-written AArch64 context switch, no ucontext on Horizon).
// Differences from Windows: the host stack is a plain heap block (no reserve/commit split, no guard page), so its
// size is a fixed guess (kFiberStack) and an overflow would corrupt the heap silently; the number of fibers is
// logged to size it from evidence. Resuming a fiber on another OS thread keeps that thread's TLS (TPIDR is not
// swapped), as Windows fibers do.
#include <chrono>
#include <thread>
#include <rex/thread/fiber.h>
extern "C" REX_FUNC(sub_8363CE40);
namespace {
constexpr size_t kFiberStack = 4u << 20;
struct Fc3Fiber {
  rex::thread::Fiber* host = nullptr;  // host fiber running this guest fiber
  PPCContext* ctx = nullptr;           // the context its frames use
  std::atomic<bool> running{false};
  uint8_t* base = nullptr;
};
std::mutex g_fiber_mutex;
std::unordered_map<uint32_t, Fc3Fiber*> g_fibers;  // guest fiber address -> host fiber
std::atomic<int> g_fiber_creates{0};
thread_local Fc3Fiber* t_switched_from = nullptr;  // marked not running by whoever runs next on this thread

void Fc3FiberArrived() {
  if (Fc3Fiber* prev = t_switched_from) {
    t_switched_from = nullptr;
    prev->running.store(false, std::memory_order_release);
  }
}

void Fc3FiberStart(void* param) {
  auto* f = static_cast<Fc3Fiber*>(param);
  Fc3FiberArrived();
  sub_8363CE40(*f->ctx, f->base);  // never returns on the console (KeBugCheck after the start routine)
  REXLOG_ERROR("[fiber] start routine of a guest fiber returned; parking the host fiber");
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

Fc3Fiber* Fc3FiberFor(uint32_t guest, PPCContext* ctx) {
  std::lock_guard lock(g_fiber_mutex);
  Fc3Fiber*& f = g_fibers[guest];
  if (!f) {
    f = new Fc3Fiber();
    f->ctx = ctx;
  }
  return f;
}
}  // namespace

REX_HOOK_RAW(sub_8363CE50) {
  const uint32_t in = ctx.r3.u32;
  const uint32_t out = GuestLoad32(base, GuestLoad32(base, ctx.r13.u32 + 256) + 356);
  if (!in || !out || in == out) {
    __imp__sub_8363CE50(ctx, base);
    return;
  }
  // The current host fiber is the outgoing guest fiber (a thread root becomes a host fiber on first use).
  if (!rex::thread::Fiber::Current()) rex::thread::Fiber::ConvertCurrentThread();
  Fc3Fiber* self = Fc3FiberFor(out, &ctx);
  self->host = rex::thread::Fiber::Current();
  self->ctx = &ctx;
  self->base = base;
  self->running.store(true, std::memory_order_relaxed);

  // Saves the outgoing registers to [out], loads lr/cr/fpr/vmx of [in]; r1/r14..r31 from [in]'s save area
  // (see the _WIN32 block).
  __imp__sub_8363CE50(ctx, base);
  ctx.r1.u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + in + 48));
  for (int i = 0; i < 18; ++i) {
    (&ctx.r14)[i].u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + in + 152 + 8 * i));
  }

  Fc3Fiber* target;
  {
    std::lock_guard lock(g_fiber_mutex);
    auto it = g_fibers.find(in);
    target = it == g_fibers.end() ? nullptr : it->second;
  }
  if (!target || !target->host) {
    if (static_cast<uint32_t>(ctx.lr) != 0x8363CE40) {
      REXLOG_ERROR("[fiber] switch to unknown guest fiber {:08X} with lr {:08X}: not started (no-op switch)",
                   in, static_cast<uint32_t>(ctx.lr));
      self->running.store(false, std::memory_order_relaxed);
      for (int i = 0; i < 18; ++i) {
        (&ctx.r14)[i].u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + out + 152 + 8 * i));
      }
      ctx.r1.u64 = __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + out + 48));
      return;
    }
    target = Fc3FiberFor(in, nullptr);
    target->ctx = new PPCContext(ctx);
    target->base = base;
    target->running.store(true, std::memory_order_relaxed);
    target->host = rex::thread::Fiber::Create(kFiberStack, &Fc3FiberStart, target);
    const int n = g_fiber_creates.fetch_add(1) + 1;
    if (n <= 64 || (n & (n - 1)) == 0)
      REXLOG_INFO("[fiber] #{} host fiber ({} KiB stack) for guest fiber {:08X} (from {:08X})", n,
                  kFiberStack >> 10, in, out);
  } else {
    // A fiber that is still on its way out on another thread cannot be entered yet.
    bool expected = false;
    while (!target->running.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
      expected = false;
      std::this_thread::yield();
    }
    if (target->ctx != &ctx) *target->ctx = ctx;
  }
  {
    static std::atomic<uint64_t> switches{0};
    const uint64_t n = switches.fetch_add(1) + 1;
    if ((n & (n - 1)) == 0 && n >= 64) REXLOG_INFO("[fiber] {} switches", n);
  }
  t_switched_from = self;
  rex::thread::Fiber::SwitchTo(target->host);
  // Back on this fiber: whoever switched here loaded our saved registers into our context.
  Fc3FiberArrived();
}
#endif

// ---------------------------------------------------------------------------------------------
// DIAGNOSTIC (2026-11-01): the ~15 s crash reads 0x5555AAAA+12 as a name pointer in sub_8241B790 (lookup by
// name: obj [+4] = groups (stride 28, count [+8]>>14), group [+8] = entry pointers (count [+12]>>14),
// entry +12 = name). Log the first bad table seen, with where it lives, to find the writer.
// ---------------------------------------------------------------------------------------------
// DIAGNOSTIC (fc3-switch, 2026-11-01): the writer of the name table. On the PC sub_823F33F8 fills F90AC130 right after
// sub_8240D130 returns it (same thread); on the Switch the block still holds the 0xA3 fill when sub_8241B790 reads it.
// Log the arguments and what the three words read back as (GuestLoad32 applies the per-platform host offset).
// DIAGNOSTIC (fc3-switch, 2026-11-01): the D3D push-buffer cursor ([dev+48] cur, [+52], [+56] end) that is garbage on
// the Switch when sub_83659E88 writes through it (runs 11 and 13). sub_8365CC90 wraps the buffer (waits for the GPU
// read pointer), sub_8365BA80 reserves space. Log the first calls of each, every wrap, and any call that leaves the
// cursor beyond the end or in another 16 MB region than the end. Device = r3 on entry (assumed; also printed).
namespace {
void Fc3PushLog(const char* what, int n, uint32_t dev, uint8_t* base, uint32_t lr, uint32_t r3_out, bool always) {
  const uint32_t cur = GuestLoad32(base, dev + 48), mid = GuestLoad32(base, dev + 52), end = GuestLoad32(base, dev + 56);
  const bool odd = cur > end + 0x10000u || (cur >> 24) != (end >> 24);
  static std::atomic<int> odd_count{0};
  if (always || (odd && odd_count.fetch_add(1) < 32))
    REXLOG_WARN("[push] {} #{} dev={:08X} lr={:08X} ret={:08X} cur={:08X} [+52]={:08X} end={:08X}{}", what, n, dev, lr,
                r3_out, cur, mid, end, odd ? " ODD" : "");
}
}  // namespace
REX_EXTERN(__imp__sub_8365BA80);
REX_HOOK_RAW(sub_8365BA80) {
  static std::atomic<int> calls{0};
  const int n = calls.fetch_add(1) + 1;
  const uint32_t dev = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8365BA80(ctx, base);
  if (dev >= 0x40000000u) Fc3PushLog("reserve", n, dev, base, lr, ctx.r3.u32, n <= 8);
}
REX_EXTERN(__imp__sub_8365CC90);
REX_HOOK_RAW(sub_8365CC90) {
  static std::atomic<int> calls{0};
  const int n = calls.fetch_add(1) + 1;
  const uint32_t dev = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  if (dev >= 0x40000000u && n <= 64) Fc3PushLog("wrap-in", n, dev, base, lr, 0, true);
  __imp__sub_8365CC90(ctx, base);
  if (dev >= 0x40000000u) Fc3PushLog("wrap-out", n, dev, base, lr, ctx.r3.u32, n <= 64);
}

REX_EXTERN(__imp__sub_823F33F8);
REX_HOOK_RAW(sub_823F33F8) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_823F33F8(ctx, base);
  if (r3 == 0xF90AC130u || r4 == 0xF90AC130u || r5 == 0xF90AC130u) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 12) {
      const uint32_t a = 0xF90AC130u;
      REXLOG_WARN("[track] fill sub_823F33F8 lr={:08X} r3={:08X} r4={:08X} r5={:08X} pcr={:08X} -> [F90AC130] {:08X} {:08X} {:08X}",
                  lr, r3, r4, r5, ctx.r13.u32, GuestLoad32(base, a), GuestLoad32(base, a + 4), GuestLoad32(base, a + 8));
    }
  }
}

REX_EXTERN(__imp__sub_8241B790);
REX_HOOK_RAW(sub_8241B790) {
  static std::atomic<int> reported{0};
  static std::atomic<int> calls{0};
  const int nc = calls.fetch_add(1) + 1;
  const uint32_t obj = ctx.r3.u32;
#if defined(_WIN32)
  // DIAGNOSTIC (PC vs Switch delta, 2026-11-01): the Switch reads this table (obj F90AC130, lr 8240AA38) while its
  // group pointer is still the heap fill. Log the PC's view of the same first call once.
  if (static_cast<uint32_t>(ctx.lr) == 0x8240AA38u || obj == 0xF90AC130u) {
    static std::atomic<int> delta_logged{0};
    if (delta_logged.fetch_add(1) < 3) {
      REXLOG_WARN("[delta] sub_8241B790 call #{} obj={:08X} lr={:08X} [obj]={:08X} [obj+4]={:08X} [obj+8]={:08X}", nc,
                  obj, static_cast<uint32_t>(ctx.lr), obj ? GuestLoad32(base, obj) : 0,
                  obj ? GuestLoad32(base, obj + 4) : 0, obj ? GuestLoad32(base, obj + 8) : 0);
    }
  }
#endif
  static const char* const watch_env = std::getenv("FC3_WATCH_GUEST");
  if (watch_env) {
    Fc3ArmNodeWatch(base, static_cast<uint32_t>(std::strtoul(watch_env, nullptr, 16)));
  }
  if (obj && reported.load(std::memory_order_relaxed) < 8) {
    const uint32_t groups = GuestLoad32(base, obj + 4);
    const uint32_t ng = GuestLoad32(base, obj + 8) >> 14;
#if !defined(_WIN32)
    // TEMPORARY (Switch): the group array pointer itself can be the heap fill (run 3: groups = 0xA3A3A3A3).
    if (ng && (groups & 0xFFu) * 0x01010101u == groups) {
      if (reported.fetch_add(1) < 8) {
        REXLOG_WARN("[dbg] sub_8241B790 call #{} BAD table (pcr {:08X}): obj={:08X} groups={:08X} (fill pattern) ng={} lr={:08X} "
                    "[obj]={:08X} [obj+8]={:08X}",
                    nc, ctx.r13.u32, obj, groups, ng, static_cast<uint32_t>(ctx.lr), GuestLoad32(base, obj),
                    GuestLoad32(base, obj + 8));
        rex::FlushLogging();
      }
      goto done;
    }
#endif
    for (uint32_t g = 0; g < ng && g < 4096; ++g) {
      const uint32_t grp = groups + 28 * g;
      const uint32_t ents = GuestLoad32(base, grp + 8);
      const uint32_t ne = GuestLoad32(base, grp + 12) >> 14;
#if !defined(_WIN32)
      // TEMPORARY (Switch, 2026-11-01): on the console this table's entry pointer is a fill pattern (0xA3A3A3A3,
      // unmapped there) and this hook faulted reading it before the game would. Report it and stop instead. The
      // bad table is the real bug; this only keeps the diagnostic from crashing first.
      if (ne && (ents & 0xFFu) * 0x01010101u == ents) {
        if (reported.fetch_add(1) < 8) {
          REXLOG_WARN("[dbg] sub_8241B790 call #{} BAD group: obj={:08X} groups={:08X} ng={} g={} grp={:08X} "
                      "ents={:08X} (fill pattern) ne={} lr={:08X} [grp+0..]={:08X} {:08X} {:08X} {:08X}",
                      nc, obj, groups, ng, g, grp, ents, ne, static_cast<uint32_t>(ctx.lr), GuestLoad32(base, grp),
                      GuestLoad32(base, grp + 4), GuestLoad32(base, grp + 16), GuestLoad32(base, grp + 20));
          rex::FlushLogging();
        }
        goto done;
      }
#endif
      for (uint32_t e = 0; e < ne && e < 65536; ++e) {
        const uint32_t v = GuestLoad32(base, ents + 4 * e);
        if (v && (v < 0x40000000u || (v >= 0x50000000u && v < 0x70000000u))) {
          if (reported.fetch_add(1) < 8) {
            FC3_DBG_WARN("[dbg] sub_8241B790 call #{} BAD entry: obj={:08X} groups={:08X} ng={} g={} grp={:08X} "
                        "ents={:08X} ne={} e={} slot={:08X} value={:08X} lr={:08X} [obj]={:08X} [obj+8]={:08X} "
                        "[grp+0..]={:08X} {:08X} {:08X} {:08X}",
                        nc, obj, groups, ng, g, grp, ents, ne, e, ents + 4 * e, v,
                        static_cast<uint32_t>(ctx.lr), GuestLoad32(base, obj), GuestLoad32(base, obj + 8),
                        GuestLoad32(base, grp), GuestLoad32(base, grp + 4), GuestLoad32(base, grp + 16),
                        GuestLoad32(base, grp + 20));
            rex::FlushLogging();
          }
          goto done;
        }
      }
    }
  }
done:
  __imp__sub_8241B790(ctx, base);
}

// ---------------------------------------------------------------------------------------------
// SWITCH CRASH: sub_8365D018 (2026-10-12)
//
// On Switch the game dies writing [r31+..] in sub_8365D018 (guest heap, uncommitted page).
// This hook answers whether the PC build even reaches it, and with which r3/r4. If the PC never
// calls it, the Switch diverged before; if it does with the same shape, the divergence is in the
// Switch commit path.
// ---------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8365D018);
REX_HOOK_RAW(sub_8365D018) {
  const int n = g_b1c0_calls.fetch_add(1) + 1;
  if (n <= 12) {
    FC3_DBG_WARN("[dbg] IN  sub_8365D018 #{} r3={:08X} r4={:08X} lr={:08X}", n, ctx.r3.u32,
                ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8365D018(ctx, base);
  if (n <= 12) {
    FC3_DBG_WARN("[dbg] OUT sub_8365D018 #{} r3={:08X}", n, ctx.r3.u32);
  }
  return;
}

// (XamShowDirtyDiscErrorUI hook removido: REX_HOOK_RAW so atua em funcoes do guest, nao em exports
// do kernel. O chamador e registrado direto em sdk/src/kernel/xam/am_ui.cpp como [dirtycaller].)

// A flag global [0x83F26860] decide o dirty-disc (sub_82B33DD8: byte 0 -> suja). O escritor e
// sub_82B34020 (stb r30,26720(r9), r30 = r5 do chamador) e o unico chamador passa r5 = 1. Estes dois
// hooks dizem se o escritor roda e com que r5, e que valor o leitor ve.
REX_EXTERN(__imp__sub_82B34020);
REX_HOOK_RAW(sub_82B34020) {
  FC3_DBG_WARN("[dbg] WRITEFLAG r5={:08X} r3={:08X} r4={:08X} lr={:08X}", ctx.r5.u32, ctx.r3.u32,
              ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  __imp__sub_82B34020(ctx, base);
  return;
}

REX_EXTERN(__imp__sub_82B33DD8);
REX_HOOK_RAW(sub_82B33DD8) {
  FC3_DBG_WARN("[dbg] READFLAG r3={:08X} lr={:08X}", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  __imp__sub_82B33DD8(ctx, base);
  return;
}

// Dirty-disc path 2 (the one the PC actually takes): sub_8241BBF0 shows the UI unconditionally
// when called (both its branches reach sub_8362CD00), so the caller IS the error. Three callers exist;
// this names which one and with what args.
REX_EXTERN(__imp__sub_8241BBF0);
REX_HOOK_RAW(sub_8241BBF0) {
  // r3 is a title/message string in the image (e.g. 0x8200EDD8 from sub_8241BD28, which formats the
  // detail into its own frame first). Byte loads are endian-neutral: copy straight from the arena.
  // The detail string: sub_8241BD28 did stwu r1,-352 before calling, and formatted into [r1+80] of
  // its own frame, so here it is at [[r1+432]].
  auto read_guest_str = [&](uint32_t addr, char* dst, size_t cap, bool image_only) {
    if (image_only && (addr < 0x82000000u || addr >= 0x84000000u)) return;
    for (size_t i = 0; i < cap - 1; ++i) {
      const uint32_t a = addr + uint32_t(i);
      const uint32_t host_off = a + Fc3HostOff(a);
      const char c = reinterpret_cast<const char*>(base)[host_off];
      dst[i] = c;
      if (!c) break;
    }
  };
  char title[128] = {};
  read_guest_str(ctx.r3.u32, title, sizeof(title), true);
  // Detail: the hook runs BEFORE sub_8241BBF0's own prologue, so ctx.r1 is still the CALLER's
  // (sub_8241BD28's) r1, and sub_8360ED78 sprintf'd into [r1+80] of that same frame. Inline text.
  char detail[256] = {};
  read_guest_str(ctx.r1.u32 + 80u, detail, sizeof(detail), false);
  FC3_DBG_WARN("[dbg] IN  sub8241BBF0 r3={:08X} r4={:08X} lr={:08X} title='{}' detail='{}'", ctx.r3.u32,
              ctx.r4.u32, static_cast<uint32_t>(ctx.lr), title, detail);
  __imp__sub_8241BBF0(ctx, base);
  return;
}

// The detail formatter behind the dirty-disc message: sub_8360ED78(dest, fmt, arg). Log the format
// string (image) and what landed in dest.
REX_EXTERN(__imp__sub_8360ED78);
REX_HOOK_RAW(sub_8360ED78) {
  const uint32_t dest = ctx.r3.u32;
  const uint32_t fmt = ctx.r4.u32;
  auto rdstr = [&](uint32_t addr, char* dst, size_t cap) {
    if (addr < 0x82000000u || addr >= 0x84000000u) return;
    for (size_t i = 0; i < cap - 1; ++i) {
      const uint32_t a = addr + uint32_t(i);
      const char c = reinterpret_cast<const char*>(base)[a];
      dst[i] = c;
      if (!c) break;
    }
  };
  char fmtstr[160] = {};
  rdstr(fmt, fmtstr, sizeof(fmtstr));
  const int n = g_clobbers.fetch_add(1) + 1;
  if (n <= 8) {
    FC3_DBG_WARN("[dbg] IN  fmt8360ED78 #{} dest={:08X} fmt={:08X}('{:.100}') r5={:08X}", n, dest, fmt,
                fmtstr, ctx.r5.u32);
  }
  __imp__sub_8360ED78(ctx, base);
  if (n <= 8) {
    // Dest is caller stack (mapped): read back what was formatted.
    char out[192] = {};
    for (size_t i = 0; i < sizeof(out) - 1; ++i) {
      const uint32_t a = dest + uint32_t(i);
      const char c = reinterpret_cast<const char*>(base)[a];
      out[i] = c;
      if (!c) break;
    }
    FC3_DBG_WARN("[dbg] OUT fmt8360ED78 #{} dest='{}' r3={:08X}", n, out, ctx.r3.u32);
  }
  return;
}

// The RB maintainer behind the vec crash (fc3_062/063/064/065): sub_829EF010 recolored
// node F90CAA20's byte+5 (00->02) via `stb r11,5(r31)` with r31=node (watch-proven, writer
// PC + guest_lr 0x829EF018). Entry args (r3/r4/r5 -> r30/r31/r29) say which object it was
// told to fix: right object + right write = vec model wrong; wrong object = hunt one up.
REX_EXTERN(__imp__sub_829EF010);
REX_HOOK_RAW(sub_829EF010) {
  static std::atomic<int> n_rb{0};
  const int n = n_rb.fetch_add(1) + 1;
  if (n <= 25) {
    // [r4] is the chain head this call walks (0x40 heap, mapped: the callee reads it
    // first thing). The corrupting call is the one whose chain holds F90CAA20.
    // r28=[r3+16] feeds the recolor (lbz r11,20(r28) xor 3 ... stb color): stale manager
    // bytes here explain a wrong color. Both read by the callee itself, so mapped.
    const uint32_t slot = ctx.r4.u32;
    uint32_t head = 0;
    bool has_head = false;
    if (slot >= 0x40000000u && slot < 0x80000000u) {
      head = GuestLoad32(base, slot);
      has_head = true;
    }
    const uint32_t mgr = ctx.r3.u32;
    uint32_t r28v = 0, b20 = 0;
    bool has_mgr = false;
    if (mgr >= 0x40000000u && mgr < 0x80000000u) {
      r28v = GuestLoad32(base, mgr + 16);
      has_mgr = true;
      if (r28v >= 0x40000000u && r28v < 0x80000000u) {
        b20 = reinterpret_cast<const uint8_t*>(base)[r28v];
      } else {
        has_mgr = false;
      }
    }
    FC3_DBG_WARN("[dbg] IN  rbF010 #{} r3={:08X} slot(r4)={:08X} [slot]={:08X}({}) r5={:08X} "
                "[r3+16]={:08X} byte20={:02X}({}) lr={:08X}",
                n, ctx.r3.u32, slot, head, has_head, ctx.r5.u32, r28v, b20, has_mgr,
                static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_829EF010(ctx, base);
  return;
}

// The rbF010 calls all come from one loop in sub_829EF5D0 (bl with lr=0x829EF724).
// (0x829EF724 itself is a return address inside it, not a function entry: hooking it
// breaks the link.) This hook names the array owner and bounds.
REX_EXTERN(__imp__sub_829EF5D0);
REX_HOOK_RAW(sub_829EF5D0) {
  static std::atomic<int> n_rbloop{0};
  const int n = n_rbloop.fetch_add(1) + 1;
  if (n <= 10) {
    FC3_DBG_WARN("[dbg] IN  rbLOOP #{} r3={:08X} r4={:08X} r5={:08X} lr={:08X}", n, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_829EF5D0(ctx, base);
  return;
}

// The node creator behind the vec/tree crashes: sub_82340988 allocates a 0x20-byte node
// (via sub_82412EE8, lr=0x823409C0), links it as [r30] and inits [node+0]=1. Arms the
// write watch when the deterministic corrupt node comes out of it.
REX_EXTERN(__imp__sub_82340988);
REX_HOOK_RAW(sub_82340988) {
  static std::atomic<int> n_mk{0};
  const int n = n_mk.fetch_add(1) + 1;
  // r30 is restored by the callee epilogue before this hook regains control, so the
  // tree (entry r3) must be captured here, not at exit.
  const uint32_t tree_in = ctx.r3.u32;
  if (n <= 15) {
    FC3_DBG_WARN("[dbg] IN  mk40988 #{} tree(r3)={:08X} r4={:08X} lr={:08X}", n, tree_in,
                ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82340988(ctx, base);
  if (n <= 600) {
    uint32_t node = 0;
    bool has_node = false;
    if (tree_in >= 0x82000000u && tree_in < 0x84060000u) {
      node = GuestLoad32(base, tree_in);
      has_node = true;
    }
    if (n <= 15 || tree_in == 0x83E97FC8u || node == 0xF90CAA20u) {
      FC3_DBG_WARN("[dbg] OUT mk40988 #{} tree={:08X} [tree]={:08X}({})", n, tree_in, node,
                  has_node);
    }
    if (node == 0xF90CAA20u) {
      FC3_DBG_WARN("[dbg] creator40988: tree={:08X} node={:08X}, arming watch", tree_in, node);
      Fc3ArmNodeWatch(base, node);
    }
  }
  return;
}

// Crash fc3_045.log (2026-10-13, COM TU): guest read of 0x04020000 at fc3.exe+0x65BC600,
// symbolized (VA) to sub_82385378 (fc3_recomp.288.cpp:326, "lwz r4,0(r7)"): a vector-append
// whose data pointer [r3+4] is 0x04020000 (unmapped). Register-only (never dereferences):
// match the crashing r3 against sub_82405188 OUT lines to see if it is a fresh object.
REX_EXTERN(__imp__sub_82385378);
REX_HOOK_RAW(sub_82385378) {
  static std::atomic<int> n_vec{0};
  const int n = n_vec.fetch_add(1) + 1;
  if (n <= 40) {
    // r3 is the caller's stack descriptor (mapped): [r3+4] is the bucket pointer the
    // crash reads, [r3+140] the count. Constant-bad = never initialized; changing = corruption.
    const uint32_t d = ctx.r3.u32;
    const uint32_t bkt = GuestLoad32(base, d + 4);
    const uint32_t cnt = reinterpret_cast<const uint8_t*>(base)[d + 140];
    // TEMP DIAG (1 boot): if the head is the corrupt node, null its link word so the
    // callee takes the beq path instead of faulting. Answers whether ONE bad link blocks
    // the boot (it proceeds) or the structure is systemically corrupt (next crash nearby).
    // REMOVE after the verdict, whatever it is.
    // PROTOTYPE HACK (fc3-switch, 2026-10-31): the upstream test was keyed on its own heap address F90CAA20; ours
    // differs (base build F90CA8C0, TU5 F90CA940). Same test, keyed on the symptom instead: the head's link word
    // points into unmapped low guest memory (the crash reads 0x04020000). NOT a fix: the corruption's writer is
    // still unknown (docs/FC3NX-STATE.md, blocker A1).
    const uint32_t link = (bkt >= 0x40000000u) ? GuestLoad32(base, bkt + 4) : 0;
    if (link >= 0x01000000u && link < 0x40000000u) {
      uint32_t be0 = 0;
      __builtin_memcpy(&be0, base + bkt + 4 + Fc3HostOff(bkt),
                       sizeof(be0));
      uint32_t zero = 0;
      __builtin_memcpy(base + bkt + 4 + Fc3HostOff(bkt), &zero,
                       sizeof(zero));
      FC3_DBG_WARN("[dbg] PROTOTYPE HACK: nulled [{:08X}+4]: {:08X} -> 0", bkt, be0);
    }
    // OUT sub_82405188 lines to find its allocation site (fresh+uninit vs old+corrupted).
    // Plus: dump the deterministic corrupt node (allocated, hence committed) to see whether
    // only [0] is bad (targeted write) or the whole block is garbage (uninitialized).
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0, w6 = 0, w7 = 0;
    bool has_node = false;
    // Both chain nodes are allocated long before the vec loop (hence committed); anything
    // else is left alone so the logger itself can never fault.
    if (bkt == 0xF90CAA20u || bkt == 0xF90E9844u) {
      has_node = true;
      w0 = GuestLoad32(base, bkt);
      w1 = GuestLoad32(base, bkt + 4);
      w2 = GuestLoad32(base, bkt + 8);
      w3 = GuestLoad32(base, bkt + 12);
      w4 = GuestLoad32(base, bkt + 16);
      w5 = GuestLoad32(base, bkt + 20);
      w6 = GuestLoad32(base, bkt + 24);
      w7 = GuestLoad32(base, bkt + 28);
    }
    FC3_DBG_WARN("[dbg] IN  vec85378 #{} obj(r3)={:08X} [r3+4]={:08X} [r3+140]={} mgr(r31)={:08X} "
                "lr={:08X}",
                n, d, bkt, cnt, ctx.r31.u32, static_cast<uint32_t>(ctx.lr));
    if (has_node) {
      FC3_DBG_WARN("[dbg] node {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  bkt, w0, w1, w2, w3, w4, w5, w6, w7);
    }
  }
  __imp__sub_82385378(ctx, base);
  return;
}

// Crash fc3_043.log (2026-10-13): guest read of 0x0502FF08 at fc3.exe+0x3E57B5B, symbolized
// (VA form) to sub_82386818 (fc3_recomp.173.cpp:308, "lwz r10,8(r11)"): a binary-tree walk
// whose node pointer became 0x0502FF00 (unmapped; no commit covers 0x05xxxxxx). Single static
// caller sub_82389218 (r3=obj, r4=tree, r5=key; tree root=[r4], key word=[r5]). This hook says
// whether the root is already garbage at entry (bad tree pointer) or turns bad mid-walk.
REX_EXTERN(__imp__sub_82389218);
REX_HOOK_RAW(sub_82389218) {
  static std::atomic<int> n_tree{0};
  const int n = n_tree.fetch_add(1) + 1;
  const uint32_t tree = ctx.r4.u32;
  const uint32_t key = ctx.r5.u32;
  if (n <= 30) {
    // Logger reads must not fault: only touch heap/image (0x40000000-0xE0000000), both
    // committed when valid. Anything else (stack keys, garbage) is reported, not read.
    auto in_range = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
    uint32_t root = 0, keyw = 0;
    bool has_root = false, has_keyw = false;
    if (in_range(tree)) {
      root = GuestLoad32(base, tree);
      has_root = true;
    }
    if (in_range(key)) {
      keyw = GuestLoad32(base, key);
      has_keyw = true;
    }
    FC3_DBG_WARN("[dbg] IN  tree89218 #{} obj={:08X} tree={:08X} root={:08X}({}) key={:08X} "
                "[key]={:08X}({}) lr={:08X}",
                n, ctx.r3.u32, tree, root, has_root, key, keyw, has_keyw,
                static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82389218(ctx, base);
  return;
}

// What sub_83658240 writes to its out-param [r8]. sub_8241BBF0 shows dirty-disc when [r1+80] is 0
// after this call; the first thing the callee does is zero it, so the question is whether anything
// writes it back before returning.
REX_EXTERN(__imp__sub_83658240);
REX_HOOK_RAW(sub_83658240) {
  const uint32_t out = ctx.r8.u32;
  const int n = g_clobbers.fetch_add(1) + 1;
  if (n <= 12) {
    FC3_DBG_WARN("[dbg] IN  sub83658240 #{} r3={:08X} r4={:08X} r6={:08X} out=[{:08X}] lr={:08X}", n,
                ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, out, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_83658240(ctx, base);
  if (n <= 12) {
    FC3_DBG_WARN("[dbg] OUT sub83658240 #{} [out]={:08X} r3={:08X}", n, GuestLoad32(base, out),
                ctx.r3.u32);
  }
  return;
}
// ---------------------------------------------------------------------------------------------
// PROTOTYPE (fc3-switch, 2026-11-01): D3D_GpuWaitStep (0x83657888, docs/D3D-HOOKS-FC3.md) is the XDK D3D wait loop
// step (8x db16cyc, returns 1 while the GPU is still busy). On the PC ~10^9 calls in 60 s from 0x8365B868: one guest
// thread spins a whole core waiting for the GPU (on the Switch: 75 % of the main-thread samples). Yield the host
// thread while it is still waiting. FC3_GPUWAIT_YIELD=0 turns it off.
// ---------------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_83657888);
REX_HOOK_RAW(sub_83657888) {
  static const bool enabled = [] {
    const char* e = std::getenv("FC3_GPUWAIT_YIELD");
    return !(e && e[0] == '0');
  }();
  // A plain yield measured no CPU change on the PC (2.2 cores either way): with idle cores the OS returns at once.
  // After a short spin, sleep briefly instead; the wait is for the GPU (milliseconds), so 0.1 ms adds no visible latency.
  thread_local uint32_t spins = 0;
  __imp__sub_83657888(ctx, base);
  if (!enabled) return;
  if (ctx.r3.u32 == 1) {
    if (++spins > 64) std::this_thread::sleep_for(std::chrono::microseconds(100));
    else std::this_thread::yield();
  } else {
    spins = 0;
  }
}
