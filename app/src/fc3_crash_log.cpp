// Windows: logs fatal host exceptions that the SDK does not report (illegal instruction from a __builtin_trap in the
// generated code, divide by zero, stack overflow, ...) with the module and offset, so the crash can be symbolized
// afterwards: llvm-symbolizer --obj=fc3.exe 0x<0x140000000 + offset>. Access violations are left alone: the SDK
// handles those itself (guest memory, MMIO) and reports the unhandled ones.

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>
#include <thread>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/thread_state.h>

namespace {

// Guest 0xF90CAA20[0] becomes 0x04020000 (raw host bytes 04 02 00 00) between its
// allocation and the vec85378 loop (fc3_048.log, deterministic across runs). A targeted
// write watch finds the writer: protect the host page NOACCESS from a hook, catch the
// fault here (re-added to the front of the chain at arm time, ahead of the SDK handler),
// log the writer, unprotect, and re-arm until the dword turns bad.
// DIAGNOSTIC (2026-11-01): FC3_WATCH_GUEST / FC3_WATCH_BAD (hex, guest value) override the watched dword.
uint32_t EnvHex(const char* name, uint32_t fallback) {
  const char* v = std::getenv(name);
  return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : fallback;
}
const uint32_t kWatchGuest = EnvHex("FC3_WATCH_GUEST", 0xF90CAA20);
const uint32_t kBadRaw = __builtin_bswap32(EnvHex("FC3_WATCH_BAD", 0x04020000));
constexpr int kMaxWatchHits = 50000;  // a pagina e quente (list walks); o escritor real vem depois
constexpr int kMaxWatchLogs = 0;  // sem log por hit (janela aberta); so BAD/armed/give-up

std::atomic<uint8_t*> g_watch_page{nullptr};
std::atomic<int> g_watch_hits{0};
std::atomic<int> g_watch_logs{0};
std::atomic<bool> g_watch_done{false};
// Single-step credits: the watched page is hot on several guest threads, so several
// faults (each setting TF on its own thread) can be in flight at once. A bool loses
// track and a stray step then escapes to the OS (exit 0x80000004); a counter balances.
std::atomic<int> g_watch_stepping{0};
void* g_watch_handle = nullptr;
// Previous-hit stash for writer attribution (see above).
uintptr_t g_prev_fault = 0;
uintptr_t g_prev_pc = 0;
uint32_t g_prev_lr = 0;
int g_prev_hit = 0;

constexpr DWORD kSingleStep = 0x80000004u;  // EXCEPTION_SINGLE_STEP

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == kSingleStep) {
    // Our own single step after a watched hit: re-arm and go on.
    if (g_watch_stepping.fetch_sub(1) <= 0) {
      g_watch_stepping.fetch_add(1);
      REXLOG_CRITICAL("[watch] stray single-step with no credit; continuing search");
      rex::FlushLogging();
      return EXCEPTION_CONTINUE_SEARCH;
    }
    info->ContextRecord->EFlags &= ~DWORD(0x100);
    uint8_t* page = g_watch_page.load();
    if (page && !g_watch_done.load()) {
      DWORD old = 0;
      VirtualProtect(page, 4096, PAGE_NOACCESS, &old);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  uint8_t* page = g_watch_page.load();
  if (!page || g_watch_done.load()) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t fault = info->ExceptionRecord->ExceptionInformation[1];
  const uintptr_t start = reinterpret_cast<uintptr_t>(page);
  if (fault < start || fault >= start + 4096) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const int hit = g_watch_hits.fetch_add(1) + 1;
  const uintptr_t node = start + (kWatchGuest & 0xFFFu);
  DWORD old = 0;
  VirtualProtect(page, 4096, PAGE_READWRITE, &old);
  // Minimal open window: check both dwords FIRST (a few instructions), log only after
  // re-arming or disarming. Logging here (milliseconds) used to leave the page open for
  // other threads and the writer slipped through.
  uint32_t raw0 = 0, raw1 = 0;
  __builtin_memcpy(&raw0, reinterpret_cast<void*>(node), sizeof(raw0));
  __builtin_memcpy(&raw1, reinterpret_cast<void*>(node + 4), sizeof(raw1));
  const bool found = (raw0 == kBadRaw || raw1 == kBadRaw);
  const uintptr_t pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  uint32_t guest_lr = 0;
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts && ts->context()) {
    guest_lr = static_cast<uint32_t>(ts->context()->lr);
  }
  // Always log touches to the node itself (small volume, decisive); other page
  // traffic stays silent.
  const bool on_node = (fault >= node && fault < node + 32);
  // Stash this hit: if a LATER hit finds BAD, the writer is almost surely the faulting
  // instruction of the immediately preceding hit (same thread burst), not the observer.
  // Snapshot first: the log below must show the PREVIOUS hit, not this one.
  const uintptr_t snapshot_fault = g_prev_fault;
  const uintptr_t snapshot_pc = g_prev_pc;
  const uint32_t snapshot_lr = g_prev_lr;
  const int snapshot_hit = g_prev_hit;
  g_prev_fault = fault;
  g_prev_pc = pc;
  g_prev_lr = guest_lr;
  g_prev_hit = hit;
  const bool give_up = !found && hit >= kMaxWatchHits;
  if (found || give_up) {
    g_watch_done.store(true);
    if (g_watch_handle) {
      RemoveVectoredExceptionHandler(g_watch_handle);
      g_watch_handle = nullptr;
    }
    // Page already READWRITE from above; disarmed for good.
    if (found) {
      REXLOG_CRITICAL("[watch] writer found after {} hits; watch removed", hit);
    } else {
      REXLOG_CRITICAL("[watch] gave up after {} hits without seeing {:08X}; watch removed", hit,
                      kBadRaw);
    }
  } else {
    // Correct dance: page stays open, TF carries the re-arm to the single step that
    // fires after re-execution. (Re-protecting here would fault the re-execution itself
    // into an infinite loop.) Per-hit logging is OFF (kMaxWatchLogs=0): formatting is
    // milliseconds with the page open and the writer used to slip through it.
    g_watch_stepping.fetch_add(1);
    info->ContextRecord->EFlags |= DWORD(0x100);
  }
  if (found || on_node) {
    g_watch_logs.fetch_add(1);
    HMODULE module = nullptr;
    HMODULE prev_module = nullptr;
    char path[MAX_PATH] = "?";
    char prev_path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(pc), &module) &&
        module) {
      GetModuleFileNameA(module, path, MAX_PATH);
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(g_prev_pc), &prev_module) &&
        prev_module) {
      GetModuleFileNameA(prev_module, prev_path, MAX_PATH);
    }
    const char* name = std::strrchr(path, '\\');
    const char* prev_name = std::strrchr(prev_path, '\\');
    const bool has_prev = snapshot_hit > 0 && prev_module != nullptr;
    REXLOG_WARN("[watch] hit #{} fault={:08X} writer={}+0x{:X} guest_lr={:08X} "
                "[node+0]={:08X} [node+4]={:08X} prev=#{} {}+0x{:X} lr={:08X}{}",
                hit, static_cast<uint32_t>(fault), name ? name + 1 : path,
                pc - reinterpret_cast<uintptr_t>(module), guest_lr, raw0, raw1, snapshot_hit,
                has_prev ? (prev_name ? prev_name + 1 : prev_path) : "none",
                has_prev ? snapshot_pc - reinterpret_cast<uintptr_t>(prev_module) : 0,
                has_prev ? snapshot_lr : 0,
                found ? "  <<< BAD VALUE WRITTEN" : "");
    rex::FlushLogging();
  }
  return EXCEPTION_CONTINUE_EXECUTION;  // re-execute the access (page re-armed or disarmed above)
}

}  // namespace

// Arms the node watch from a hook (base = arena host base). Called when the allocator
// returns the deterministic node address; safe to call every boot (one-shot per process).
void Fc3ArmNodeWatch(uint8_t* host_base, uint32_t guest_addr) {
  if (guest_addr != kWatchGuest || g_watch_page.load() || g_watch_done.load()) {
    return;
  }
  // Same translation the generated code uses (REX_PHYS_HOST_OFFSET in the PCH): guest
  // addresses at or above 0xE0000000 go through a +0x1000 host offset, so the watched
  // host page must include it or the watch sits one page off and sees only noise.
  const uintptr_t host_off = (guest_addr >= 0xE0000000u) ? 0x1000u : 0u;
  uint8_t* page = host_base + (guest_addr & ~0xFFFu) + host_off;
  DWORD old = 0;
  if (!VirtualProtect(page, 4096, PAGE_NOACCESS, &old)) {
    static std::atomic<int> fails{0};
    if (fails.fetch_add(1) < 4) REXLOG_CRITICAL("[watch] VirtualProtect failed for {:08X}", guest_addr);
    return;
  }
  g_watch_page.store(page);
  // Late registration puts this ahead of the SDK's own AV handler.
  g_watch_handle = AddVectoredExceptionHandler(1, WatchHandler);
  REXLOG_WARN("[watch] armed for guest {:08X} (host page {:p})", guest_addr,
              static_cast<void*>(page));
  rex::FlushLogging();
}

namespace {

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_STACK_BUFFER_OVERRUN:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  const char* name = std::strrchr(path, '\\');
  REXLOG_CRITICAL("[crash] host exception {:08X} at {:016X} ({}+0x{:X}) on thread {}", uint32_t(code), pc,
                  name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module), GetCurrentThreadId());
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}


// DIAGNOSTIC (2026-11-01): host backtrace of the first access violation nobody handled. Registered late (from the
// fiber hook, after the SDK's own handlers) and last in the chain, so it only sees faults the SDK passed on.
// The stack is scanned for words inside fc3.exe (a heuristic, like scripts/sample-threads.py); symbolize the
// RVAs with llvm-symbolizer --relative-address.
LONG CALLBACK LogUnhandledAv(EXCEPTION_POINTERS* info) {
  static std::atomic<int> once{0};
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || once.fetch_add(1) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      mod + reinterpret_cast<const IMAGE_DOS_HEADER*>(mod)->e_lfanew);
  const uintptr_t end = mod + nt->OptionalHeader.SizeOfImage;
  const CONTEXT* c = info->ContextRecord;
  std::string rvas = fmt::format("{:X}", c->Rip - mod);
  const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  int found = 0;
  for (int i = 0; i < 16384 && found < 80; ++i) {
    if (reinterpret_cast<uintptr_t>(sp + i) >= reinterpret_cast<uintptr_t>(tib->StackBase)) break;
    const uintptr_t v = sp[i];
    if (v > mod + 0x1000 && v < end) {
      rvas += fmt::format(" {:X}", v - mod);
      ++found;
    }
  }
  REXLOG_CRITICAL("[crash] first unhandled AV on thread {}: rva stack {}", GetCurrentThreadId(), rvas);
  {  // DIAGNOSTIC (2026-11-01): where rip is when it is outside fc3.exe, and what was accessed.
    HMODULE rip_mod = nullptr;
    char rip_path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(c->Rip), &rip_mod) && rip_mod) {
      GetModuleFileNameA(rip_mod, rip_path, MAX_PATH);
    }
    const char* rip_name = std::strrchr(rip_path, '\\');
    const auto* er = info->ExceptionRecord;
    REXLOG_CRITICAL("[crash] AV detail: rip {:016X} = {}+0x{:X}, {} {:016X}; rax {:X} rbx {:X} rcx {:X} rdx {:X} "
                    "rsi {:X} rdi {:X} r8 {:X} r9 {:X}",
                    c->Rip, rip_name ? rip_name + 1 : rip_path, c->Rip - reinterpret_cast<uintptr_t>(rip_mod),
                    er->NumberParameters > 0 ? (er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "exec" : "read") : "?",
                    er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi,
                    c->Rdi, c->R8, c->R9);
  }
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

// Installed before main, first in the chain; it only logs and never handles.
const PVOID g_handler = AddVectoredExceptionHandler(1, LogFatalException);


// DIAGNOSTIC (2026-11-01): FC3_POLL_GUEST=<hex addr> FC3_POLL_BAD=<hex value>: a host thread polls the guest dword
// and, the moment it holds the bad value, suspends every other thread of the process and logs where each one is
// (RIP and stack words inside fc3.exe, as RVAs). Catches writers a page watch cannot (host-side copies: file
// reads, GPU resolves, XMA output) and pages too hot to watch.
void PollThread(uint8_t* host_base, uint32_t guest, uint32_t bad) {
  const uintptr_t host_off = (guest >= 0xE0000000u) ? 0x1000u : 0u;
  volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(host_base + guest + host_off);
  const uint32_t bad_raw = __builtin_bswap32(bad);
  const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      mod + reinterpret_cast<const IMAGE_DOS_HEADER*>(mod)->e_lfanew);
  const uintptr_t end = mod + nt->OptionalHeader.SizeOfImage;
  REXLOG_WARN("[poll] watching guest {:08X} for {:08X}", guest, bad);
  uint32_t last = 0;
  bool have_last = false;
  // Wait until the page is committed, then read without system calls (VirtualQuery in the loop takes the
  // address-space lock and starved the page watch's VirtualProtect).
  for (;;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(const_cast<uint32_t*>(p), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) break;
    Sleep(1);
  }
  for (;;) {
    const uint32_t v = *p;
    // Trigger only on a change TO the bad value after a good one (0 is the initial value of fresh memory too).
    const bool trigger = have_last && last != bad_raw && v == bad_raw && last != 0;
    if (!have_last || v != last) {
      if (have_last && v != bad_raw) {
        static int changes = 0;
        if (++changes <= 64) REXLOG_WARN("[poll] {:08X} = {:08X}", guest, __builtin_bswap32(v));
      }
      last = v;
      have_last = true;
    }
    if (!trigger) {
      YieldProcessor();
      continue;
    }
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    std::string out;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
      HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) != DWORD(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
          std::string line = fmt::format("\n  tid {}: rip {}", te.th32ThreadID,
                                         c.Rip >= mod && c.Rip < end ? fmt::format("{:X}", c.Rip - mod)
                                                                     : fmt::format("ext:{:X}", c.Rip));
          int found = 0;
          const auto* sp = reinterpret_cast<const uintptr_t*>(c.Rsp);
          for (int i = 0; i < 4096 && found < 24; ++i) {
            MEMORY_BASIC_INFORMATION m{};
            if ((i & 511) == 0 && (!VirtualQuery(sp + i, &m, sizeof(m)) || m.State != MEM_COMMIT)) break;
            const uintptr_t w = sp[i];
            if (w > mod + 0x1000 && w < end) {
              line += fmt::format(" {:X}", w - mod);
              ++found;
            }
          }
          if (found) out += line;
        }
        ResumeThread(h);
      }
      CloseHandle(h);
    }
    CloseHandle(snap);
    REXLOG_CRITICAL("[poll] {:08X} became {:08X}; threads (rva stacks):{}", guest, bad, out);
    rex::FlushLogging();
    return;
  }
}

}  // namespace

void Fc3ArmAvBacktrace(uint8_t* host_base) {
  static std::atomic<bool> armed{false};
  if (armed.exchange(true)) return;
  AddVectoredExceptionHandler(0, LogUnhandledAv);
  const char* g = std::getenv("FC3_POLL_GUEST");
  const char* b = std::getenv("FC3_POLL_BAD");
  if (g && b) {
    std::thread(PollThread, host_base, uint32_t(std::strtoul(g, nullptr, 16)), uint32_t(std::strtoul(b, nullptr, 16)))
        .detach();
  }
}

#endif
