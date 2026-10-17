#include "detour.h"
#include "log.h"
#include "addr.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace gowvr {
namespace {

struct Installed {
    const HookSite* site;
    uint8_t* target;
    uint8_t* block;  // [relay 16][trampoline 32]
};
std::vector<Installed> g_installed;
// D-110: installs come from several threads (DllMain, the render thread's device creation, the command thread, the
// game thread) - one at a time; g_installed is shared
std::mutex g_mu;
std::atomic<bool> g_early{false};
// D-220b: the blocks' addresses for DetourOwnsAddress (written under g_mu, read lock-free; a page each)
std::atomic<uintptr_t> g_blocks[128];
std::atomic<int> g_nBlocks{0};
void NoteBlock(const uint8_t* b) {
    const int i = g_nBlocks.load();
    if (i < 128) { g_blocks[i] = (uintptr_t)b; g_nBlocks = i + 1; }
}  // DllMain: no other thread can run game code - patch without suspending

// Allocate one page within +-1.5 GB of `near`, so a rel32 jump from the target reaches it.
uint8_t* AllocNear(uint8_t* nearAddr) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = (uintptr_t)nearAddr > 0x60000000ull ? (uintptr_t)nearAddr - 0x60000000ull : gran;
    const uintptr_t hi = (uintptr_t)nearAddr + 0x60000000ull;
    for (uintptr_t a = ((uintptr_t)nearAddr & ~(gran - 1)) - gran; a > lo; a -= gran)
        if (void* p = VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
    for (uintptr_t a = ((uintptr_t)nearAddr & ~(gran - 1)) + gran; a < hi; a += gran)
        if (void* p = VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
    return nullptr;
}

void WriteAbsJmp(uint8_t* at, const void* dest) {
    at[0] = 0xFF;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &dest, 8);
}

// Suspends every other thread; returns false (and resumes all) if any is executing inside [lo, hi).
bool SuspendOthers(std::vector<HANDLE>& out, uintptr_t lo, uintptr_t hi) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    // D-110: no heap allocation once threads are suspended (one of them may hold the heap lock)
    out.reserve(1024);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof(te)};
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); continue; }
        out.push_back(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c) && c.Rip >= lo && c.Rip < hi) clear = false;
    }
    CloseHandle(snap);
    return clear;
}

void ResumeAll(std::vector<HANDLE>& ts) {
    for (HANDLE t : ts) {
        ResumeThread(t);
        CloseHandle(t);
    }
    ts.clear();
}

// D-187: the site on this build (GameVa: recorded, or found by signature) and its guard - on another build a byte the
// signature leaves open (a moved call / RIP displacement) may differ; every other guard byte must match
uint8_t* SiteTarget(const HookSite& site) {
    uint8_t* t = (uint8_t*)GameVa(site.staticVa);
    if (!t) Log("detour: %s not found on this build -> not installed", site.name);
    return t;
}
bool GuardOk(const HookSite& site, const uint8_t* t) {
    for (int i = 0; i < (int)sizeof(site.guard); ++i)
        if (t[i] != site.guard[i] && !AddrGuardMayDiffer(site.staticVa, i)) return false;
    return true;
}

}  // namespace

bool DetourOwnsAddress(uintptr_t a) {
    const int n = g_nBlocks.load();
    for (int i = 0; i < n; ++i) {
        const uintptr_t b = g_blocks[i].load();
        if (a >= b && a < b + 4096) return true;
    }
    return false;
}

bool DetourInstall(const HookSite& site, void* hook, void** original) {
    std::lock_guard<std::mutex> lk(g_mu);  // D-110
    uint8_t* target = SiteTarget(site);
    if (!target) return false;
    if (!GuardOk(site, target)) {
        Log("detour: %s guard mismatch at %p -> not installed", site.name, target);
        return false;
    }
    uint8_t* block = AllocNear(target);
    if (!block) {
        Log("detour: %s no near memory", site.name);
        return false;
    }
    uint8_t* relay = block;        // jmp [rip] -> hook
    uint8_t* tramp = block + 16;   // stolen bytes; jmp [rip] -> target + stolen
    WriteAbsJmp(relay, hook);
    memcpy(tramp, target, site.stolen);
    WriteAbsJmp(tramp + site.stolen, target + site.stolen);
    FlushInstructionCache(GetCurrentProcess(), block, 64);

    std::vector<HANDLE> threads;
    for (int attempt = 0; !g_early.load(); ++attempt) {
        if (SuspendOthers(threads, (uintptr_t)target, (uintptr_t)target + site.stolen)) break;
        ResumeAll(threads);
        if (attempt == 20) {
            Log("detour: %s a thread kept executing the patch window -> not installed", site.name);
            VirtualFree(block, 0, MEM_RELEASE);
            return false;
        }
        Sleep(1);
    }
    *original = tramp;
    DWORD old;
    VirtualProtect(target, site.stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    int32_t rel = (int32_t)(relay - (target + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy(target, patch, site.stolen);
    VirtualProtect(target, site.stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, site.stolen);
    ResumeAll(threads);
    g_installed.push_back({&site, target, block});
    NoteBlock(block);
    Log("detour: %s installed at %p (stolen %d, relay %p)", site.name, target, site.stolen, relay);
    return true;
}

bool DetourCallSite(const HookSite& site, void* fn) {
    std::lock_guard<std::mutex> lk(g_mu);  // D-110
    uint8_t* target = SiteTarget(site);
    if (!target) return false;
    if (site.stolen < 5 || !GuardOk(site, target)) {
        Log("detour: call site %s guard mismatch at %p -> not installed", site.name, target);
        return false;
    }
    uint8_t* block = AllocNear(target);
    if (!block) return false;
    WriteAbsJmp(block, fn);
    FlushInstructionCache(GetCurrentProcess(), block, 16);
    std::vector<HANDLE> threads;
    for (int attempt = 0; !g_early.load(); ++attempt) {
        if (SuspendOthers(threads, (uintptr_t)target, (uintptr_t)target + site.stolen)) break;
        ResumeAll(threads);
        if (attempt == 20) {
            Log("detour: call site %s busy -> not installed", site.name);
            VirtualFree(block, 0, MEM_RELEASE);  // D-110 (was leaked)
            return false;
        }
        Sleep(1);
    }
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE8;
    int32_t rel = (int32_t)(block - (target + 5));
    memcpy(patch + 1, &rel, 4);
    DWORD old;
    VirtualProtect(target, site.stolen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, patch, site.stolen);
    VirtualProtect(target, site.stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, site.stolen);
    ResumeAll(threads);
    g_installed.push_back({&site, target, block});
    NoteBlock(block);
    Log("detour: call site %s patched at %p (relay %p)", site.name, target, block);
    return true;
}

bool DetourCaseCall(const HookSite& site, int callOffset, const uint8_t* call, int callLen, void* fn) {
    std::lock_guard<std::mutex> lk(g_mu);  // D-110
    uint8_t* target = SiteTarget(site);
    if (!target) return false;
    if (site.stolen < 5 || site.stolen > callOffset || callOffset > 64 || callLen < 2 ||
        !GuardOk(site, target) || memcmp(target + callOffset, call, callLen) != 0 ||
        target[callOffset + callLen] != 0xE9) {
        Log("detour: case %s guard mismatch at %p -> not installed", site.name, target);
        return false;
    }
    int32_t rel;
    memcpy(&rel, target + callOffset + callLen + 1, 4);
    uint8_t* cont = target + callOffset + callLen + 5 + rel;  // where the case's jmp goes
    uint8_t* block = AllocNear(target);
    if (!block) return false;
    uint8_t* p = block;
    memcpy(p, target, callOffset);  // the argument setup
    p += callOffset;
    p[0] = 0xFF;  // call qword ptr [rip+2]
    p[1] = 0x15;
    const int32_t d = 2;
    memcpy(p + 2, &d, 4);
    p[6] = 0xEB;  // jmp short over the pointer
    p[7] = 0x08;
    memcpy(p + 8, &fn, 8);
    p += 16;
    WriteAbsJmp(p, cont);
    FlushInstructionCache(GetCurrentProcess(), block, 128);
    std::vector<HANDLE> threads;
    for (int attempt = 0; !g_early.load(); ++attempt) {
        if (SuspendOthers(threads, (uintptr_t)target, (uintptr_t)target + site.stolen)) break;
        ResumeAll(threads);
        if (attempt == 20) {
            Log("detour: case %s busy -> not installed", site.name);
            VirtualFree(block, 0, MEM_RELEASE);  // D-110 (was leaked)
            return false;
        }
        Sleep(1);
    }
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t r = (int32_t)(block - (target + 5));
    memcpy(patch + 1, &r, 4);
    DWORD old;
    VirtualProtect(target, site.stolen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, patch, site.stolen);
    VirtualProtect(target, site.stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, site.stolen);
    ResumeAll(threads);
    g_installed.push_back({&site, target, block});
    NoteBlock(block);
    Log("detour: case %s patched at %p (stub %p, continues at %p)", site.name, target, block, cont);
    return true;
}

bool DetourStub(const HookSite& site, const uint8_t* code, int len) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint8_t* target = SiteTarget(site);
    if (!target) return false;
    if (site.stolen < 5 || site.stolen > 32 || len < 1 || len > 4096 - 32 || !GuardOk(site, target)) {
        Log("detour: stub %s guard mismatch at %p -> not installed", site.name, target);
        return false;
    }
    uint8_t* block = AllocNear(target);
    if (!block) return false;
    memcpy(block, code, len);
    WriteAbsJmp(block + len, target + site.stolen);
    // D-220b: unwind data = the site function's own codes (version 1, no chain), flags 0 (its handler's RVA belongs to
    // GoW.exe's base), prolog size 0 (the stub is past the prolog: every code applies); at block + 0x800, the table entry
    // at + 0x900 covering the stub and its jmp back
    bool unwindOk = false;
    {
        DWORD64 imgBase = 0;
        const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry((DWORD64)target, &imgBase, nullptr);
        const uint8_t* ui = rf ? (const uint8_t*)(imgBase + rf->UnwindData) : nullptr;
        // D-220d: a split-off fragment's info is CHAINED to its parent's (the unwinder applies the fragment's codes, then
        // the parent's whole): the codes of every link are concatenated into one unchained info
        uint8_t codes[0x200];
        int nCodes = 0, frame = 0;
        bool good = ui != nullptr;
        for (int link = 0; good && link < 8; ++link) {
            if ((ui[0] & 7) != 1 || nCodes + ui[2] > 255) { good = false; break; }
            memcpy(codes + nCodes * 2, ui + 4, (size_t)ui[2] * 2);
            nCodes += ui[2];
            if (!frame) frame = ui[3];
            if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
            const auto* parent = (const RUNTIME_FUNCTION*)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
            ui = (const uint8_t*)(imgBase + parent->UnwindData);
            if (link == 7) good = false;
        }
        if (good && 4 + nCodes * 2 + 2 <= 0x100) {
            uint8_t* u = block + 0x800;
            u[0] = 1;                 // version 1, flags 0
            u[1] = 0;                 // prolog size 0
            u[2] = (uint8_t)nCodes;   // code count
            u[3] = (uint8_t)frame;    // frame register / offset
            memcpy(u + 4, codes, (size_t)nCodes * 2);
            if (nCodes & 1) { u[4 + nCodes * 2] = 0; u[5 + nCodes * 2] = 0; }  // even slot count
            auto* tab = (RUNTIME_FUNCTION*)(block + 0x900);
            tab->BeginAddress = 0;
            tab->EndAddress = (DWORD)(len + 14);
            tab->UnwindData = 0x800;
            unwindOk = RtlAddFunctionTable(tab, 1, (DWORD64)block) != FALSE;
        }
        if (!unwindOk) Log("detour: stub %s - no unwind data registered (the site's could not be copied)", site.name);
    }
    FlushInstructionCache(GetCurrentProcess(), block, len + 16);
    std::vector<HANDLE> threads;
    for (int attempt = 0; !g_early.load(); ++attempt) {
        if (SuspendOthers(threads, (uintptr_t)target, (uintptr_t)target + site.stolen)) break;
        ResumeAll(threads);
        if (attempt == 20) {
            Log("detour: stub %s busy -> not installed", site.name);
            VirtualFree(block, 0, MEM_RELEASE);
            return false;
        }
        Sleep(1);
    }
    uint8_t patch[32];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t r = (int32_t)(block - (target + 5));
    memcpy(patch + 1, &r, 4);
    DWORD old;
    VirtualProtect(target, site.stolen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, patch, site.stolen);
    VirtualProtect(target, site.stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, site.stolen);
    ResumeAll(threads);
    g_installed.push_back({&site, target, block});
    NoteBlock(block);
    Log("detour: stub %s patched at %p (stub %p, back at %p)", site.name, target, block, target + site.stolen);
    return true;
}

bool DetourRemove(const HookSite& site) {
    std::lock_guard<std::mutex> lk(g_mu);  // D-110
    for (size_t i = 0; i < g_installed.size(); ++i) {
        if (g_installed[i].site != &site) continue;
        Installed in = g_installed[i];
        std::vector<HANDLE> threads;
        SuspendOthers(threads, 0, 0);
        DWORD old;
        VirtualProtect(in.target, site.stolen, PAGE_EXECUTE_READWRITE, &old);
        memcpy(in.target, site.guard, site.stolen);
        VirtualProtect(in.target, site.stolen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), in.target, site.stolen);
        ResumeAll(threads);
        // The relay/trampoline block is leaked on purpose: a thread may still be returning through it.
        g_installed.erase(g_installed.begin() + i);
        Log("detour: %s removed", site.name);
        return true;
    }
    return false;
}

void DetourEarlyInstall(bool on) {
    g_early = on;
    Log("detour: %s", on ? "early install (DllMain: no thread suspension)" : "normal install (threads suspended while patching)");
}

}  // namespace gowvr
