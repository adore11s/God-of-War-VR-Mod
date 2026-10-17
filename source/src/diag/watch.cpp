#include "watch.h"
#include "../core/log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>

namespace gowvr::diag {
namespace {

struct Slot {
    uintptr_t addr = 0;
    int len = 0;
    bool rw = false;
    bool active = false;
};
Slot g_slots[4];

struct Hit {
    std::atomic<uintptr_t> rip{0};
    std::atomic<uint32_t> slot{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint32_t> tid{0};      // first thread seen
    std::atomic<uint32_t> tidOther{0}; // any different thread seen
};
constexpr int kMaxHits = 512;
Hit g_hits[kMaxHits];
std::atomic<uint64_t> g_overflow{0};
PVOID g_veh = nullptr;

void Record(uint32_t slot, uintptr_t rip) {
    DWORD tid = GetCurrentThreadId();
    size_t h = (rip * 0x9E3779B97F4A7C15ull) >> 55;  // 9 bits -> 0..511
    for (int i = 0; i < kMaxHits; ++i) {
        Hit& e = g_hits[(h + i) % kMaxHits];
        uintptr_t cur = e.rip.load();
        if (cur == 0) {
            uintptr_t expected = 0;
            if (!e.rip.compare_exchange_strong(expected, rip) && expected != rip) continue;
            e.slot = slot;
            e.tid = tid;
        } else if (cur != rip) {
            continue;
        }
        e.count.fetch_add(1);
        if (e.tid.load() != tid) e.tidOther = tid;
        return;
    }
    g_overflow.fetch_add(1);
}

DWORD64 Dr7Bits();

// D-080: optional call stacks per trap (watchstacks on): captured into a fixed ring (no logging / allocation in the
// exception handler), printed by WatchReport. RtlCaptureStackBackTrace unwinds through KiUserExceptionDispatcher into the
// writing code's callers.
constexpr int kStacks = 64, kFrames = 20;
struct StackRec { std::atomic<uint32_t> ready{0}; uintptr_t rip = 0; uint16_t n = 0; void* fr[kFrames]; };
StackRec g_stacks[kStacks];
std::atomic<uint32_t> g_stackNext{0};
std::atomic<bool> g_stackOn{false};
std::atomic<int> g_immediate{0};  // D-222
// module+offset for an address (GoW.exe / VERSION.dll / other), into buf
void Where(uintptr_t a, char* buf, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a,
                           &m) && m) {
        char name[MAX_PATH];
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, '\\');
        sprintf_s(buf, n, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
    } else {
        sprintf_s(buf, n, "%p", (void*)a);
    }
}
void CaptureStack(uintptr_t rip) {
    const uint32_t i = g_stackNext.fetch_add(1);
    if (i >= (uint32_t)kStacks) return;
    StackRec& s = g_stacks[i];
    s.rip = rip;
    s.n = RtlCaptureStackBackTrace(0, kFrames, s.fr, nullptr);
    s.ready = 1;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    // Any DR0-3 hit is ours (the game sets no hardware breakpoints). A hit on a slot that was disarmed while this
    // trap was in flight is stale: swallow it, otherwise it is unhandled and kills the game.
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    for (uint32_t i = 0; i < 4; ++i)
        if ((dr6 & (1ull << i)) && g_slots[i].active) {
            Record(i, (uintptr_t)c->Rip);  // trap: Rip is the instruction AFTER the access
            if (g_stackOn.load()) CaptureStack((uintptr_t)c->Rip);
            if (g_immediate.fetch_sub(1) > 0) {  // D-222
                void* fr[16];
                const USHORT nf = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
                char line[1400], w[96];
                Where((uintptr_t)c->Rip, w, sizeof(w));
                int len = sprintf_s(line, "watch: HIT slot %u addr %p written by the instruction before %s (thread %lu) | "
                                    "rax %llx rcx %llx rdx %llx r8 %llx | stack:", i, (void*)g_slots[i].addr, w,
                                    GetCurrentThreadId(), c->Rax, c->Rcx, c->Rdx, c->R8);
                for (USHORT f = 0; f < nf && len < (int)sizeof(line) - 100; ++f) {
                    Where((uintptr_t)fr[f], w, sizeof(w));
                    len += sprintf_s(line + len, sizeof(line) - len, " %s", w);
                }
                Log("%s", line);
            }
        }
    // The context we resume from is written back by NtContinue, debug registers included. Make it the current
    // desired state, or a disarm that raced with this trap would be undone for this thread.
    c->Dr0 = g_slots[0].active ? g_slots[0].addr : 0;
    c->Dr1 = g_slots[1].active ? g_slots[1].addr : 0;
    c->Dr2 = g_slots[2].active ? g_slots[2].addr : 0;
    c->Dr3 = g_slots[3].active ? g_slots[3].addr : 0;
    c->Dr7 = Dr7Bits();
    c->Dr6 = 0;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD64 Dr7Bits() {
    DWORD64 dr7 = 0;
    for (int i = 0; i < 4; ++i) {
        if (!g_slots[i].active) continue;
        DWORD64 rw = g_slots[i].rw ? 3 : 1;
        DWORD64 len = g_slots[i].len == 1 ? 0 : g_slots[i].len == 2 ? 1 : g_slots[i].len == 8 ? 2 : 3;
        dr7 |= 1ull << (2 * i);
        dr7 |= rw << (16 + 4 * i);
        dr7 |= len << (18 + 4 * i);
    }
    return dr7;
}

int ApplyToAllThreads() {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof(te)};
    int applied = 0;
    DWORD64 dr7 = Dr7Bits();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = g_slots[0].active ? g_slots[0].addr : 0;
                c.Dr1 = g_slots[1].active ? g_slots[1].addr : 0;
                c.Dr2 = g_slots[2].active ? g_slots[2].addr : 0;
                c.Dr3 = g_slots[3].active ? g_slots[3].addr : 0;
                c.Dr7 = dr7;
                if (SetThreadContext(t, &c)) ++applied;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return applied;
}

}  // namespace

void WatchInstallHandler() {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
}

void WatchImmediate(int n) { g_immediate = n; }

void WatchStacks(bool on) {
    for (auto& s : g_stacks) s.ready = 0;
    g_stackNext = 0;
    g_stackOn = on;
    Log("watch: call stacks per trap %s (first %d)", on ? "ON" : "off", kStacks);
}

bool WatchArm(int slot, uintptr_t addr, int len, bool rw) {
    if (slot < 0 || slot > 3 || (len != 1 && len != 2 && len != 4 && len != 8) || (addr % len)) {
        Log("watch: rejected slot=%d addr=%p len=%d (alignment/len)", slot, (void*)addr, len);
        return false;
    }
    WatchInstallHandler();
    g_slots[slot] = {addr, len, rw, true};
    int n = ApplyToAllThreads();
    Log("watch: armed slot=%d addr=%p len=%d mode=%s threads=%d", slot, (void*)addr, len, rw ? "rw" : "w", n);
    return true;
}

void WatchRefreshThreads() {
    for (auto& s : g_slots)
        if (s.active) { ApplyToAllThreads(); return; }
}

void WatchDisarmAll() {
    for (auto& s : g_slots) s = {};
    int n = ApplyToAllThreads();
    Log("watch: disarmed all (threads=%d)", n);
    // The vectored handler stays installed for the life of the process so late in-flight traps are swallowed.
}

void WatchReport() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    int rows = 0;
    for (auto& e : g_hits) {
        uintptr_t rip = e.rip.load();
        if (!rip) continue;
        char where[160];
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)rip, &m) && m) {
            char name[MAX_PATH];
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\');
            sprintf_s(where, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(rip - (uintptr_t)m));
        } else {
            sprintf_s(where, "%p", (void*)rip);
        }
        Log("watch: slot=%u ripAfter=%s staticVA=0x%llx hits=%llu tid=%u otherTid=%u", e.slot.load(), where,
            (unsigned long long)(rip - base + 0x140000000ull), (unsigned long long)e.count.load(), e.tid.load(),
            e.tidOther.load());
        e.rip = 0;
        e.count = 0;
        e.tid = 0;
        e.tidOther = 0;
        ++rows;
    }
    Log("watch: report rows=%d overflow=%llu", rows, (unsigned long long)g_overflow.exchange(0));
    // D-080: the captured call stacks (static VAs; "-" = outside GoW.exe)
    const uint32_t ns = g_stackNext.load() < (uint32_t)kStacks ? g_stackNext.load() : (uint32_t)kStacks;
    for (uint32_t i = 0; i < ns; ++i) {
        const StackRec& s = g_stacks[i];
        if (!s.ready.load()) continue;
        char b[600];
        int o = 0;
        for (uint16_t k = 0; k < s.n && o < 560; ++k) {
            const uintptr_t a = (uintptr_t)s.fr[k];
            o += (a >= base && a < base + 0x6000000) ? sprintf_s(b + o, sizeof(b) - o, " %llx", (unsigned long long)(a - base + 0x140000000ull))
                                                      : sprintf_s(b + o, sizeof(b) - o, " -");
        }
        Log("watch: stack #%u writer %llx |%s", i, (unsigned long long)(s.rip - base + 0x140000000ull), b);
    }
    for (auto& s : g_stacks) s.ready = 0;
    g_stackNext = 0;
}

}  // namespace gowvr::diag
