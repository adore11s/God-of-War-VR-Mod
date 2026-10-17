// Stack probe: who calls an engine function, on which thread, with which first arguments. Diagnostic only; one probe
// active at a time. Unwinds with the x64 unwinder (RtlLookupFunctionEntry / RtlVirtualUnwind) starting from the
// caller frame, using the callee-saved registers captured by stackprobe_thunk.asm.
#include "stackprobe.h"
#include "../core/detour.h"
#include "../core/log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" void stack_probe_thunk();
extern "C" void* g_probeTrampoline = nullptr;

namespace gowvr::diag {
namespace {
std::atomic<int> g_budget{0};
uint64_t g_seen[64];
std::atomic<int> g_nseen{0};
HookSite* g_site = nullptr;

void Describe(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)a, &m) && m) {
        if (m == GetModuleHandleW(nullptr)) {
            sprintf_s(out, n, "0x%llx", (unsigned long long)(a - (uintptr_t)m + 0x140000000ull));
            return;
        }
        char name[MAX_PATH];
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, '\\');
        sprintf_s(out, n, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
        return;
    }
    sprintf_s(out, n, "%p", (void*)a);
}

int Unwind(uint64_t* regs, uintptr_t* frames, int max) {
    CONTEXT c{};
    uint64_t entryRsp = regs[0];
    c.Rip = *(uint64_t*)entryRsp;
    c.Rsp = entryRsp + 8;
    c.Rbx = regs[1]; c.Rbp = regs[2]; c.Rsi = regs[3]; c.Rdi = regs[4];
    c.R12 = regs[5]; c.R13 = regs[6]; c.R14 = regs[7]; c.R15 = regs[8];
    int n = 0;
    __try {
        while (n < max && c.Rip) {
            frames[n++] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe) {
                c.Rip = *(uint64_t*)c.Rsp;
                c.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
}  // namespace

extern "C" void StackProbeRecord(uint64_t* regs) {
    if (g_budget.load() <= 0) return;
    uintptr_t frames[24];
    int n = Unwind(regs, frames, 24);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) h = (h ^ frames[i]) * 1099511628211ull;
    for (int i = 0; i < g_nseen.load(); ++i)
        if (g_seen[i] == h) return;  // only distinct stacks
    int slot = g_nseen.fetch_add(1);
    if (slot >= 64) return;
    g_seen[slot] = h;
    if (--g_budget < 0) return;
    const uint64_t* args = (const uint64_t*)regs[0];  // pushed rcx, rdx, r8, r9 sit just below the entry rsp
    char line[1400];
    int len = sprintf_s(line, "stackprobe: tid=%lu rcx=%p rdx=%p r8=%p |", GetCurrentThreadId(), (void*)args[-1],
                        (void*)args[-2], (void*)args[-3]);
    for (int i = 0; i < n && len < (int)sizeof(line) - 40; ++i) {
        char d[96];
        Describe(frames[i], d, sizeof(d));
        len += sprintf_s(line + len, sizeof(line) - len, " %s", d);
    }
    Log("%s", line);
}

bool StackProbeInstall(uint64_t staticVa, int stolen, const uint8_t guard[16], int budget) {
    if (g_site) {  // one probe at a time: move it
        g_budget = 0;
        DetourRemove(*g_site);
        Log("stackprobe: removed previous probe at 0x%llx", (unsigned long long)g_site->staticVa);
        g_site = nullptr;  // the HookSite is leaked on purpose (DetourRemove keeps no reference after removal)
        Sleep(50);
    }
    auto* s = new HookSite{"StackProbe", staticVa, stolen, {}};
    memcpy(s->guard, guard, 16);
    g_budget = budget;
    g_nseen = 0;
    bool ok = DetourInstall(*s, (void*)&stack_probe_thunk, &g_probeTrampoline);
    if (ok) g_site = s;
    Log("stackprobe: install at 0x%llx stolen=%d budget=%d -> %d", (unsigned long long)staticVa, stolen, budget, ok);
    return ok;
}

void DumpAllThreadStacks() {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); continue; }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        uintptr_t frames[20];
        int nf = 0;
        if (GetThreadContext(t, &c)) {
            __try {
                while (nf < 20 && c.Rip) {
                    frames[nf++] = c.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                    if (!fe) { c.Rip = *(uint64_t*)c.Rsp; c.Rsp += 8; continue; }
                    void* hd = nullptr;
                    DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &est, nullptr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(t);
        CloseHandle(t);
        char line[1400];
        int len = sprintf_s(line, "threads: tid=%lu |", te.th32ThreadID);
        for (int i = 0; i < nf && len < (int)sizeof(line) - 40; ++i) {
            char d[96];
            Describe(frames[i], d, sizeof(d));
            len += sprintf_s(line + len, sizeof(line) - len, " %s", d);
        }
        Log("%s", line);
        ++n;
    }
    CloseHandle(snap);
    Log("threads: %d dumped", n);
}

void StackProbeRearm(int budget) {
    g_nseen = 0;
    g_budget = budget;
    Log("stackprobe: rearmed budget=%d", budget);
}

}  // namespace gowvr::diag
