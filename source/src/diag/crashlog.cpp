// D-099 crash log (owner: "find out why it crashed"; the 2026-10-04 crash was an access violation at GoW.exe+0x9a0d61,
// sub_1409a0cd0 - a draw's texture table read while recording render commands - known only from the Windows event log,
// without the bad address, the registers or the mod's state). A vectored handler logs the first exceptions whose
// instruction is inside GoW.exe: code, instruction RVA, the address read / written, every general register, the
// return addresses on the stack, the thread, the first-person mode and where the main thread was in the stereo
// frame (R1KickPhase). It never handles anything (EXCEPTION_CONTINUE_SEARCH): the game / its crash reporter see the
// exception as before. "first-chance": an exception the game catches itself would also be logged (at most 6 lines).
#include "crashlog.h"
#include "../core/log.h"
#include "../core/detour.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "watch.h"
#include "../core/patch.h"
#include "../core/target.h"
#include "../fp/body.h"
#include "../fp/firstperson.h"
#include "../input/vpad.h"
#include "../stereo/r1.h"
#include "../vr/xr.h"
#include <initializer_list>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <intrin.h>

#ifndef GOWVR_VERSION
#define GOWVR_VERSION "dev"
#endif

#pragma intrinsic(_ReturnAddress)

namespace gowvr::diag {
namespace {

std::atomic<int> g_logged{0}, g_loggedMod{0};
uintptr_t g_base = 0, g_size = 0;
uintptr_t g_modBase = 0, g_modSize = 0;  // D-108: the mod's own DLL (its crash 2026-10-04 23:35 was not logged)

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    const bool inGame = g_base && rip >= g_base && rip < g_base + g_size;
    const bool inMod = g_modBase && rip >= g_modBase && rip < g_modBase + g_modSize;
    // D-220b: the mod's detour blocks next to GoW.exe (stubs / relays) - a fault there was in no module (owner crash
    // 2026-10-15 21:12 in the D-220 stub left no report); offsets are given from the block's page
    const bool inStub = !inGame && !inMod && DetourOwnsAddress(rip);
    // D-117: the D3D11 runtime too (owner crash 2026-10-05 19:24 in d3d11.dll+0x193fe4 left no report: neither the
    // log line nor the top-level filter saw it) - its first few, with the GoW.exe / mod return addresses on the stack
    static uintptr_t d3dBase = 0, d3dSize = 0;
    if (!d3dBase) {
        if (HMODULE h = GetModuleHandleW(L"d3d11.dll")) {
            const auto* dd = (const IMAGE_DOS_HEADER*)h;
            d3dSize = ((const IMAGE_NT_HEADERS*)((const uint8_t*)h + dd->e_lfanew))->OptionalHeader.SizeOfImage;
            d3dBase = (uintptr_t)h;
        }
    }
    const bool inD3d = !inGame && !inMod && !inStub && d3dBase && rip >= d3dBase && rip < d3dBase + d3dSize;
    if (!inGame && !inMod && !inD3d && !inStub) return EXCEPTION_CONTINUE_SEARCH;  // drivers / Windows: not here
    static std::atomic<int> loggedD3d{0};
    // (the mod's own guarded reads - __try in probes / SEH readers - raise first-chance exceptions too: own budget)
    if ((inD3d ? loggedD3d : inMod ? g_loggedMod : g_logged).fetch_add(1) >= (inD3d ? 3 : inMod ? 4 : 6))
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t modB = inStub ? (rip & ~(uintptr_t)0xfff) : inD3d ? d3dBase : inMod ? g_modBase : g_base;
    const CONTEXT* c = ep->ContextRecord;
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    const bool av = code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2;
    Log("CRASH (first-chance) 0x%08lx at %s+0x%llx (VA 0x%llx) %s 0x%llx | thread %lu | first-person mode %d, stereo "
        "kick phase %d (1 eye 0, 2 between, 3 eye 1, 4 other kick work, 0 outside)%s",
        code, inStub ? "a mod detour stub" : inD3d ? "d3d11.dll" : inMod ? "VERSION.dll (mod)" : "GoW.exe",
        (unsigned long long)(rip - modB),
        (unsigned long long)(inStub ? rip : inD3d ? rip - d3dBase + 0x180000000ull : inMod ? rip - g_modBase + 0x180000000ull
                                                                                 : rip - g_base + 0x140000000ull),
        av ? (info[0] == 0 ? "reading" : info[0] == 1 ? "writing" : "executing") : "-", av ? (unsigned long long)info[1] : 0ull,
        GetCurrentThreadId(), fp::FpMode(), stereo::R1KickPhase(),
        inMod ? " - may be a guarded read the mod handles itself" : "");
    Log("CRASH regs rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx,
        c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    Log("CRASH regs r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx", c->R8, c->R9, c->R10, c->R11,
        c->R12, c->R13, c->R14, c->R15);
    // return addresses: stack values pointing into GoW.exe (the first 12 within 512 qwords)
    char buf[600];
    int n = 0, found = 0;
    const uintptr_t* sp = (const uintptr_t*)c->Rsp;
    for (int i = 0; i < 512 && found < 12; ++i) {
        uintptr_t v = 0;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v > g_base + 0x1000 && v < g_base + g_size) {
            n += sprintf_s(buf + n, sizeof(buf) - n, " +0x%llx", (unsigned long long)(v - g_base));
            ++found;
        } else if (v > g_modBase + 0x1000 && v < g_modBase + g_modSize) {
            n += sprintf_s(buf + n, sizeof(buf) - n, " mod+0x%llx", (unsigned long long)(v - g_modBase));
            ++found;
        }
    }
    Log("CRASH stack (GoW.exe / mod return candidates):%s", found ? buf : " none");
    return EXCEPTION_CONTINUE_SEARCH;
}

// D-105 guard: sub_1409a0cd0(recorder, stageDesc, srvTable) records a draw's 16-slot shader-resource binding
// (command 0xff0f) when the stage descriptor says "type 2"; it reads the table without a null check. Crashes 2026-10-04
// 02:52 (D-099) and 22:44 (crash log: rbx = r15 + 4, r15 = 0 -> the table was NULL, not freed) - both in a cutscene a
// few seconds after F3 (cutscene first person) was switched on: a draw item whose material pass (+0xa8) has no table
// while its shader wants one. A null table now skips that one bind (that draw keeps the previous draw's textures for
// one frame) and is logged, instead of an access violation.
using PFN_BindSrvTable = void(__fastcall*)(int64_t recorder, char* stage, int64_t table);
PFN_BindSrvTable o_BindSrvTable = nullptr;
std::atomic<uint64_t> g_nullTables{0};
void __fastcall hk_BindSrvTable(int64_t recorder, char* stage, int64_t table) {
    if (table == 0 && stage && *stage == 2) {
        const uint64_t n = ++g_nullTables;
        if (n <= 5 || n % 1000 == 0)
            Log("guard: draw with a NULL texture table skipped (#%llu) - caller GoW+0x%llx, recorder %p, first-person mode %d, "
                "cinematic %d, stereo kick phase %d",
                (unsigned long long)n, (unsigned long long)((uintptr_t)_ReturnAddress() - g_base), (void*)recorder,
                fp::FpMode(), fp::InCinematic() ? 1 : 0, stereo::R1KickPhase());
        return;
    }
    o_BindSrvTable(recorder, stage, table);
}
const HookSite kBindSrvTable = {"BindSrvTable_guard", 0x1409a0cd0, 5,  // mov r11,rsp (3) + push r14 (2)
                                {0x4c, 0x8b, 0xdc, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x58, 0x02, 0x00, 0x00, 0x48, 0x8b}};

// D-109: the 2026-10-05 crash went one field further: after two D-105 skips, sub_1401789f0 + 0x358 read the same draw
// item's pass +0xb8 table (r12 + 0x28 = param_3[5] -> [rbx], rbx = 0) - the item's material pass is not built at all
// (+0xa8 and +0xb8 null), again in a cutscene with F3 on. sub_1401789f0 dereferences pass+0xb8 unconditionally, so in
// the game's own frames no drawn item has it null. sub_1405b55b0(state, item, cache) sets up a draw item and returns
// whether its draw is recorded (caller sub_14060f070: `if (sub_1405b55b0(..)) sub_1405b53d0(..)`): an item whose pass
// (*item) has no +0xb8 table is now skipped whole - not drawn that frame - and logged.
std::atomic<uint64_t> g_unbuiltPassCalls{0};  // D-220: counted by the frame-job stub (below)
using PFN_DrawState = uint64_t(__fastcall*)(int64_t* state, int64_t* item, int64_t cache);
PFN_DrawState o_DrawState = nullptr;
std::atomic<uint64_t> g_emptyPasses{0};
uint64_t __fastcall hk_DrawState(int64_t* state, int64_t* item, int64_t cache) {
    // (D-220e: no memory query here - D-220d's per-item VirtualQuery made 400 ms frames)
    const uint8_t* pass = item ? (const uint8_t*)item[0] : nullptr;
    {  // D-220: the frame job's skipped calls on unbuilt pass objects (counted by its stub), reported from here
        static uint64_t seen = 0;
        const uint64_t c = g_unbuiltPassCalls.load(std::memory_order_relaxed);
        if (c != seen && (seen < 5 || c - seen >= 1000 || c <= 5)) {
            seen = c;
            Log("guard: frame job call on an unbuilt pass object skipped (%llu so far) - first-person mode %d, cinematic %d "
                "(D-220)", (unsigned long long)c, fp::FpMode(), fp::InCinematic() ? 1 : 0);
        }
    }
    if (pass && *(const int64_t*)(pass + 0xb8) == 0) {
        const uint64_t n = ++g_emptyPasses;
        fp::SuspendCutsceneFP("a draw item with an unbuilt material pass");
        if (n <= 5 || n % 1000 == 0)
            Log("guard: draw item with an unbuilt material pass skipped (#%llu) - pass %p (+0xa8 %p), first-person mode %d, "
                "cinematic %d, stereo kick phase %d",
                (unsigned long long)n, (void*)pass, *(void* const*)(pass + 0xa8), fp::FpMode(), fp::InCinematic() ? 1 : 0,
                stereo::R1KickPhase());
        return 0;
    }
    return o_DrawState(state, item, cache);
}
const HookSite kDrawState = {"DrawState_guard", 0x1405b55b0, 5,  // mov [rsp+10h], rbx (5)
                             {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55}};

// D-220 (owner: "why does it crash?"; crashes 2026-10-15 21:00:43 and 21:03:15, both in a cutscene with F3 on, each a few
// ms after "draw item with an unbuilt material pass skipped"): GoW.exe+0x4b9f46, the frame job sub_1404b9e80's first
// loop - for each of the frame's pass requests (count 0x142d24d04, 0x88-byte entries from 0x142590b78) it calls
// `entry(0xb0 table at [0x142d4e298])+0xa8 -> vfunc +0x10` with no check; rcx = a pass object whose vtable is 0 (rax 0,
// reading 0x10) - the same unbuilt pass the draw guard (D-109) skips, met one step earlier. The 17 bytes at 0x1404b9f38
// (mov rcx,[rcx+0xa8] / mov rax,[rcx] / lea rdx,[rbx-0x7c] / call [rax+0x10]) become a stub doing the same with two null
// checks: an unbuilt pass object is not called that frame (counted, logged from the draw guard)
const HookSite kPassJob = {"PassJob_guard", 0x1404b9f38, 17,
                           {0x48, 0x8b, 0x89, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x01, 0x48, 0x8d, 0x53, 0x84, 0xff, 0x50}};
// D-220b (owner: "crash" again, 21:12:24; Windows: fault at 0x7ff748c90018 = this stub + 0x18, the `call [rax+0x10]`):
// the pass object's vtable was not 0 but garbage - the +0xa8 entry points at FREED memory. The stub now hands the call
// to SafePassCall, which makes it only for a live game object: a plausible pointer (D-220e: no memory query), its
// vtable inside GoW.exe's image and the called function inside it too (every engine vtable and method lives there);
// otherwise that one call is skipped that frame and counted. Registers: the stub runs where the game made the call - same rsp (aligned, its shadow space),
// rcx / rdx as the game set them; rax / rcx / rdx / r8 - r11 are dead after the call (the loop reloads them)
uintptr_t g_imgLo = 0, g_imgHi = 0;
// D-220e (owner: "laggy as hell"; owner log: 386 .. 466 ms frames with D-220d): no memory queries on these paths at all -
// D-220c's cached VirtualQuery per object (and D-220d's per draw item) cost 30 .. 400 ms a frame. Every dead object seen
// (D-220 / 220b / 220d crashes) was READABLE with a zero or garbage vtable, so the vtable / method range test is what
// catches them; the pointer itself only has to be a plausible, aligned user-mode address
inline bool PlausiblePtr(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    return a >= 0x10000 && a < 0x00007fffffff0000ull && (a & 7) == 0;
}
void __fastcall SafePassCall(void* obj, void* arg) {
    using Fn = void(__fastcall*)(void*, void*);
    void* const* vt = PlausiblePtr(obj) ? *(void* const* const*)obj : nullptr;
    const uintptr_t v = (uintptr_t)vt;
    Fn fn = v >= g_imgLo && v + 0x18 <= g_imgHi ? (Fn)vt[2] : nullptr;
    if (!fn || (uintptr_t)fn < g_imgLo || (uintptr_t)fn >= g_imgHi) {
        fp::SuspendCutsceneFP("a dead pass object in the frame job");
        const uint64_t c = ++g_unbuiltPassCalls;
        if (c <= 5) {
            // D-220f evidence: the request's pass index (int at the list entry = arg + 0x7c) against the pass table
            // (0x142d4e298, 0xb0 per type, count 0x142d27a8c = 128): an index past the table would read whatever follows
            const int idx = (uintptr_t)arg > 0x10000 ? *(const int*)((const char*)arg + 0x7c) : -1;
            void* const* tabp = (void* const*)GameVa(0x142d4e298ull);
            const int* cntp = (const int*)GameVa(0x142d27a8cull);
            Log("guard: frame job: pass object %p not live (vtable %p) - its call skipped (#%llu, D-220) | request pass index "
                "%d of %d, table %p", obj, (void*)vt, (unsigned long long)c, idx, cntp ? *cntp : -1, tabp ? *tabp : nullptr);
        }
        return;
    }
    fn(obj, arg);
}
// D-220d (owner crash 2026-10-15 21:21:45 - the game froze in its own crash reporter): after the frame job's calls were
// skipped (D-220b, "not live" x5), the same dead pass objects (0x7ff4bf37f660 / f980 / f990) were in the frame's draw
// commands: sub_14060f070's loop over them calls `[r10+0x20] -> vfunc +0x20` (GoW+0x60f23d) and `-> vfunc +0x18`
// (+0x60f256) for two command types - four threads faulted. Both calls go through SafeVcall (live object only, as
// SafePassCall); a skipped +0x18 call also skips the command's follow-up (sub_1405b5ba0) - straight to the loop's next
using Vcall4 = void*(__fastcall*)(void*, void*, void*, void*);
Vcall4 LiveMethod(void* obj, int slot) {
    void* const* vt = PlausiblePtr(obj) ? *(void* const* const*)obj : nullptr;
    const uintptr_t v = (uintptr_t)vt;
    if (v < g_imgLo || v + (slot + 1) * 8 > g_imgHi) return nullptr;
    const uintptr_t f = (uintptr_t)vt[slot];
    return f >= g_imgLo && f < g_imgHi ? (Vcall4)f : nullptr;
}
std::atomic<uint64_t> g_deadCmds{0};
bool SkipDead(void* obj, int slot) {
    fp::SuspendCutsceneFP("a draw command on a dead object");
    const uint64_t n = ++g_deadCmds;
    if (n <= 5 || n % 1000 == 0)
        Log("guard: draw command on a dead object %p (method %d) skipped (#%llu, D-220d)", obj, slot, (unsigned long long)n);
    return false;
}
bool __fastcall SafeVcall20(void* obj, void* a, void* b, void* c) {
    Vcall4 f = LiveMethod(obj, 4);
    if (!f) return SkipDead(obj, 4);
    f(obj, a, b, c);
    return true;
}
bool __fastcall SafeVcall18(void* obj, void* a, void* b, void* c) {
    Vcall4 f = LiveMethod(obj, 3);
    if (!f) return SkipDead(obj, 3);
    f(obj, a, b, c);
    return true;
}
const HookSite kCmdA = {"DrawCmd20_guard", 0x14060f229, 23,
                        {0x49, 0x8b, 0x4a, 0x20, 0x48, 0x8d, 0x54, 0x24, 0x20, 0x4d, 0x8b, 0x4a, 0x18, 0x4d, 0x8b, 0x42}};
const HookSite kCmdB = {"DrawCmd18_guard", 0x14060f242, 23,
                        {0x49, 0x8b, 0x4a, 0x20, 0x48, 0x8d, 0x54, 0x24, 0x20, 0x4d, 0x8b, 0x4a, 0x18, 0x4d, 0x8b, 0x42}};
// D-220g (owner crash 21:37:23 in THIRD person, F3 off all session: the dead objects are the game's own pass objects for
// pass types 51, 52 and 1 - "request pass index 51 of 128" - made once at startup (render_passes_015.c), yet freed and
// their memory reused for floats mid-cutscene while the pass table still points at them). To find WHAT frees them: once
// per frame (game thread) the 128 table entries are compared with the last frame - an object pointer that changes or a
// vtable that leaves GoW.exe is logged once per entry with its class (RTTI), the frame, the cutscene / first-person state
// and the stereo kick phase. Cost: 128 pointer reads a frame
const char* RttiName(void* const* vt) {
    // x64 MSVC: vtable[-1] = complete object locator; +0x0c type descriptor RVA; the descriptor's name at +0x10
    const uintptr_t col = (uintptr_t)vt[-1];
    if (col < g_imgLo || col + 0x18 > g_imgHi) return "?";
    const uint32_t tdRva = *(const uint32_t*)(col + 0x0c);
    const uintptr_t td = g_imgLo + tdRva;
    if (td + 0x20 > g_imgHi) return "?";
    return (const char*)(td + 0x10);
}
}  // namespace (D-220g: PassTableWatch is exported)
void PassTableWatch() {
    if (!g_imgLo) return;
    static uint8_t* const* tabp = (uint8_t* const*)GameVa(0x142d4e298ull);
    static const int* cntp = (const int*)GameVa(0x142d27a8cull);
    if (!tabp || !cntp || !*tabp) return;
    const int n = *cntp < 128 ? *cntp : 128;
    struct Seen { void* obj; void* vt; bool dead; };
    static Seen seen[128];
    static bool first = true;
    static uint64_t frame = 0;
    ++frame;
    for (int i = 0; i < n; ++i) {
        void* obj = *(void* const*)(*tabp + (size_t)i * 0xb0 + 0xa8);
        void* const* vt = PlausiblePtr(obj) ? *(void* const* const*)obj : nullptr;
        const bool live = vt && (uintptr_t)vt >= g_imgLo && (uintptr_t)vt < g_imgHi;
        Seen& s = seen[i];
        if (first) { s = {obj, (void*)vt, !live}; continue; }
        if (obj != s.obj) {
            Log("passwatch: frame %llu pass %d object %p -> %p (%s) | cutscene %d, first-person mode %d, kick phase %d (D-220g)",
                (unsigned long long)frame, i, s.obj, obj, live ? RttiName(vt) : "not live", fp::InCinematic() ? 1 : 0,
                fp::FpMode(), stereo::R1KickPhase());
            s = {obj, (void*)vt, !live};
        } else if (!live && !s.dead) {
            Log("passwatch: frame %llu pass %d object %p DIED - vtable %p -> %p (was %s) | cutscene %d, first-person mode %d, "
                "kick phase %d (D-220g)", (unsigned long long)frame, i, obj, s.vt, (void*)vt,
                s.vt ? RttiName((void* const*)s.vt) : "?", fp::InCinematic() ? 1 : 0, fp::FpMode(), stereo::R1KickPhase());
            s.dead = true;
        }
    }
    // D-222: hardware write watchpoints on the vtables of the objects that die in the VR cutscene crash (PassParticles 51,
    // PassParticlesSync 52, PassClear 1): nothing legitimately writes a live object's vtable, so the first trap is the
    // corrupting write - logged at once with its instruction and call stack. New threads get them every ~5 s
    {
        static bool armed = false;
        static uint64_t lastRefresh = 0;
        static const bool watchOn = config::GetFloat("passwatch_watchpoints", 0.f) > 0.5f;  // D-225: off - D-223/224 fixed it
        if (!first && !armed && watchOn) {
            armed = true;
            const int ids[3] = {51, 52, 1};
            int slot = 0;
            for (int id : ids)
                if (id < n && seen[id].obj && !seen[id].dead)
                    WatchArm(slot++, (uintptr_t)seen[id].obj, 8, false);
            WatchImmediate(8);
            Log("passwatch: %d write watchpoints armed on the particle / clear pass objects (D-222)", slot);
        } else if (armed && frame - lastRefresh > 300) {
            lastRefresh = frame;
            WatchRefreshThreads();
        }
    }
    if (first) {
        first = false;
        char line[4096];
        int len = sprintf_s(line, "passwatch: %d pass objects watched (D-220g):", n);
        for (int i = 0; i < n && len < (int)sizeof(line) - 80; ++i) {
            if (!seen[i].obj) continue;
            const bool live = seen[i].vt && !seen[i].dead;
            len += sprintf_s(line + len, sizeof(line) - len, " %d=%s", i, live ? RttiName((void* const*)seen[i].vt) : "-");
        }
        Log("%s", line);
    }
}
namespace {

bool InstallDrawCmdGuards() {
    static const uint8_t kSetup[] = {0x49, 0x8b, 0x4a, 0x20,         // mov rcx, [r10+0x20]   (as the game)
                                     0x48, 0x8d, 0x54, 0x24, 0x20,   // lea rdx, [rsp+0x20]
                                     0x4d, 0x8b, 0x4a, 0x18,         // mov r9, [r10+0x18]
                                     0x4d, 0x8b, 0x42, 0x10};        // mov r8, [r10+0x10]
    auto Build = [](uint8_t* s, void* fn) {
        int n = 0;
        memcpy(s, kSetup, sizeof(kSetup));
        n += sizeof(kSetup);
        s[n++] = 0x48; s[n++] = 0xb8;  // mov rax, fn
        for (int i = 0; i < 8; ++i) s[n++] = (uint8_t)((uint64_t)fn >> (8 * i));
        s[n++] = 0xff; s[n++] = 0xd0;  // call rax
        return n;
    };
    uint8_t a[64], b[64];
    int na = Build(a, (void*)&SafeVcall20);
    a[na++] = 0x90;  // nop (D-220b: the return address must not be the jmp back); then back at +0x240 = jmp to the loop
    int nb = Build(b, (void*)&SafeVcall18);
    // skipped: straight to the loop's next command (+0x266 = the site + 0x24) instead of the follow-up call
    b[nb++] = 0x84; b[nb++] = 0xc0;  // test al, al
    b[nb++] = 0x75; b[nb++] = 0x0e;  // jnz -> over the jmp below (DetourStub's jmp back to +0x259 follows)
    uint8_t* siteB = (uint8_t*)GameVa(kCmdB.staticVa);
    if (!siteB) return false;
    const uint64_t next = (uint64_t)(siteB + 0x24);
    b[nb++] = 0xff; b[nb++] = 0x25; b[nb++] = 0; b[nb++] = 0; b[nb++] = 0; b[nb++] = 0;  // jmp [rip] -> next
    for (int i = 0; i < 8; ++i) b[nb++] = (uint8_t)(next >> (8 * i));
    const bool okA = DetourStub(kCmdA, a, na), okB = DetourStub(kCmdB, b, nb);
    return okA && okB;
}

bool InstallPassJobGuard() {
    const auto* base = (const uint8_t*)GetModuleHandleW(nullptr);
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    g_imgLo = (uintptr_t)base;
    g_imgHi = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    uint8_t s[32];
    int n = 0;
    auto B = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) s[n++] = x; };
    B({0x48, 0x8b, 0x89, 0xa8, 0x00, 0x00, 0x00});  //  0 mov rcx, [rcx+0xa8]   (as the game)
    B({0x48, 0x8d, 0x53, 0x84});                    //  7 lea rdx, [rbx-0x7c]    (as the game)
    B({0x48, 0xb8});                                // 11 mov rax, SafePassCall
    const uint64_t fnp = (uint64_t)&SafePassCall;
    for (int i = 0; i < 8; ++i) s[n++] = (uint8_t)(fnp >> (8 * i));
    B({0xff, 0xd0});                                // 21 call rax
    B({0x90});                                      // 23 nop - the return address must not be the jmp back: Windows'
                                                    //    unwinder takes `jmp [rip]` at a return address for an epilog
    // 24: (DetourStub appends the jmp back to 0x1404b9f49)
    if (n != 24) return false;
    return DetourStub(kPassJob, s, n);
}

// ---- D-112 crash reports ------------------------------------------------------------------------------------------
// Collected ahead of time (a crashed process should do as little as possible): PC, GPU, user name (to remove it).
char g_os[96] = "?", g_cpu[96] = "?", g_ram[32] = "?", g_gpu[192] = "not created yet", g_user[64] = "";
std::atomic<bool> g_infoDone{false};

void CollectInfo() {
    if (g_infoDone.exchange(true)) return;
    using PFN_RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto rgv = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) {
        OSVERSIONINFOW v{sizeof(v)};
        if (rgv(&v) == 0) sprintf_s(g_os, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    wchar_t cpu[96] = L"";
    DWORD sz = sizeof(cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &sz) == ERROR_SUCCESS)
        sprintf_s(g_cpu, "%ls", cpu);
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) sprintf_s(g_ram, "%.0f GB", ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    GetEnvironmentVariableA("USERNAME", g_user, sizeof(g_user));
}

// the report text (static: no heap in a crashed process)
char g_rep[128 * 1024];
size_t g_repLen = 0;
void Add(const char* fmt, ...) {
    if (g_repLen >= sizeof(g_rep) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(g_rep + g_repLen, sizeof(g_rep) - g_repLen, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_repLen = n < 0 ? sizeof(g_rep) - 1 : g_repLen + (size_t)n;
}
// a log line without the user name (paths like C:\Users\<name>\...)
void AddClean(const char* line, void*) {
    char out[400];
    size_t o = 0;
    const size_t ul = strlen(g_user);
    for (const char* p = line; *p && o < sizeof(out) - 8;) {
        if (ul > 1 && _strnicmp(p, g_user, ul) == 0) {
            memcpy(out + o, "<user>", 6);
            o += 6;
            p += ul;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    Add("  %s\r\n", out);
}

// "Module.dll+0x1234" for an address (file name only - no folders)
void AddrName(uintptr_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)a, &m) && m) {
        wchar_t p[MAX_PATH] = L"?";
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* n = wcsrchr(p, L'\\');
        sprintf_s(out, cap, "%ls+0x%llx", n ? n + 1 : p, (unsigned long long)(a - (uintptr_t)m));
    } else {
        sprintf_s(out, cap, "0x%llx", (unsigned long long)a);
    }
}

const char* ExceptionName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case 0xE06D7363: return "C++ exception";
        case 0xC0000409: return "stack buffer overrun / fail fast";
        case 0xE0000001: return "TEST REPORT (command crashreport - no crash happened)";
        default: return "exception";
    }
}

void WriteCrashReport(EXCEPTION_POINTERS* ep) {
    CollectInfo();
    SYSTEMTIME t;
    GetLocalTime(&t);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t rip = (uintptr_t)er->ExceptionAddress;
    char where[160];
    AddrName(rip, where, sizeof(where));
    const bool inGame = g_base && rip >= g_base && rip < g_base + g_size;
    const bool inMod = g_modBase && rip >= g_modBase && rip < g_modBase + g_modSize;
    const TargetInfo ti = InspectTarget();
    g_repLen = 0;
    Add("God of War VR mod - crash report\r\n");
    Add("Please send this file to the mod's author with a few words on what you were doing.\r\n");
    Add("It holds no personal data: no user name, no files or folders outside the game.\r\n\r\n");
    Add("When:        %04u-%02u-%02u %02u:%02u:%02u\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Add("Mod:         gowvr %s\r\n", GOWVR_VERSION);
    Add("Game:        GoW.exe stamp 0x%08x (%s)\r\n", ti.timeDateStamp, ti.headerMatch ? "the supported Steam build" : "NOT the supported build");
    Add("Windows:     %s\r\n", g_os);
    Add("CPU:         %s, %s RAM\r\n", g_cpu, g_ram);
    Add("GPU:         %s\r\n", g_gpu);
    Add("Headset:     %s\r\n", vr::XrSystemName()[0] ? vr::XrSystemName() : "(no OpenXR session yet)");
    Add("VR:          %s; first person %s; pad %s; stereo phase %d\r\n", vr::XrRunning() ? "running" : "off",
        fp::FpMode() == 1 ? "F1 (gameplay)" : fp::FpMode() == 2 ? "F3 (cutscene)" : "off", input::PadMode() ? "PlayStation" : "Xbox",
        stereo::R1KickPhase());
    Add("\r\nCrash:       0x%08lx %s at %s (%s)\r\n", er->ExceptionCode, ExceptionName(er->ExceptionCode), where,
        inMod ? "inside the VR mod" : inGame ? "inside the game" : "inside another module");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Add("             %s address 0x%llx\r\n", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
            (unsigned long long)er->ExceptionInformation[1]);
    Add("Thread:      %lu\r\n", GetCurrentThreadId());
    Add("Registers:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx\r\n", c->Rax, c->Rbx, c->Rcx,
        c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    Add("             r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\r\n", c->R8, c->R9, c->R10, c->R11,
        c->R12, c->R13, c->R14, c->R15);
    Add("Stack (return address candidates):\r\n");
    const uintptr_t* sp = (const uintptr_t*)c->Rsp;
    int found = 0;
    for (int i = 0; i < 1024 && found < 24; ++i) {
        uintptr_t v = 0;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        const bool code = (v > g_base + 0x1000 && v < g_base + g_size) || (v > g_modBase + 0x1000 && v < g_modBase + g_modSize);
        if (!code) continue;
        char n[160];
        AddrName(v, n, sizeof(n));
        Add("  %s\r\n", n);
        ++found;
    }
    Add("\r\nLast lines of the mod's log:\r\n");
    LogRecent(&AddClean, nullptr);
    // gowvr_logs\crash_reports\crash_<date>.txt next to GoW.exe
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* s = wcsrchr(path, L'\\')) *s = 0;
    wcscat_s(path, L"\\gowvr_logs");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\crash_reports");
    CreateDirectoryW(path, nullptr);
    wchar_t file[64];
    swprintf_s(file, L"\\crash_%04u-%02u-%02u_%02u-%02u-%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wcscat_s(path, file);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, g_rep, (DWORD)g_repLen, &w, nullptr);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    Log("CRASH report written: gowvr_logs\\crash_reports%ls (%s at %s)", file, ExceptionName(er->ExceptionCode), where);
}

// the top-level filter: ours first, then the game's own crash handler, then any other one that replaced ours
LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter = nullptr, g_otherFilter = nullptr;
std::atomic<int> g_reported{0};
thread_local bool t_inFilter = false;
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (t_inFilter) return EXCEPTION_CONTINUE_SEARCH;  // a chained filter that calls back into ours
    t_inFilter = true;
    if (g_reported.fetch_add(1) == 0) {
        __try {
            WriteCrashReport(ep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (LPTOP_LEVEL_EXCEPTION_FILTER g = g_gameFilter; g && g != CrashFilter) r = g(ep);
    if (r == EXCEPTION_CONTINUE_SEARCH)
        if (LPTOP_LEVEL_EXCEPTION_FILTER o = g_otherFilter; o && o != CrashFilter && o != g_gameFilter) r = o(ep);
    t_inFilter = false;
    return r;
}
// GoW.exe's own SetUnhandledExceptionFilter (its crash handler): kept behind ours instead of replacing it
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_SetUEF(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_gameFilter;
    if (f != CrashFilter) g_gameFilter = f;
    Log("crashlog: the game installed its crash handler %p - chained behind the mod's crash report", (void*)f);
    return prev;
}

}  // namespace

void CrashNoteDevice(ID3D11Device* dev) {
    if (!dev) return;
    IDXGIDevice* dx = nullptr;
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && SUCCEEDED(dx->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC d{};
        LARGE_INTEGER umd{};
        ad->GetDesc(&d);
        char drv[48] = "?";
        if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            sprintf_s(drv, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart),
                      (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
        sprintf_s(g_gpu, "%ls, %.1f GB, driver %s", d.Description, d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0), drv);
        Log("crashlog: GPU %s", g_gpu);
    }
    if (ad) ad->Release();
    if (dx) dx->Release();
}

void CrashReportTest() {
    CONTEXT c{};
    RtlCaptureContext(&c);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = 0xE0000001;
    er.ExceptionAddress = (void*)c.Rip;
    EXCEPTION_POINTERS ep{&er, &c};
    WriteCrashReport(&ep);
}

void CrashFilterKeep() {
    CollectInfo();
    // something else replaced the top-level filter: ours goes back in front, theirs is chained behind it
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev && prev != CrashFilter && prev != g_gameFilter && prev != g_otherFilter) {
        g_otherFilter = prev;
        Log("crashlog: another crash handler (%p) was installed - chained behind the mod's crash report", (void*)prev);
    }
}

void InstallCrashGuards() {
    static bool done = false;
    if (done) return;
    done = true;
    // D-224 (owner crash 2026-10-15 22:00:52 - after D-223 the VR cutscene got further, then 0x80000003 at GoW+0x4830bc:
    // the game's own "Instance buffer overflow, crashing (count %d > limit %d)" check in sub_140483040): the instance
    // buffers (2, alternating per frame, 0x68-byte instances) hold limit 0x14105ee04 = 81920 per frame; in VR both eyes
    // take instances from the same frame's buffer - twice the count. The PassSharedBuffers constructor sizes the buffers
    // as limit * 0x68 when the renderer starts (render_passes_017.c), and the check reads the same global: raised here,
    // at DLL load (before the renderer exists), the game builds bigger buffers and checks against the bigger limit by
    // itself. Only when it holds the known 81920. gowvr.ini instance_limit_scale (3; 1 = off)
    {
        const float scale = config::GetFloat("instance_limit_scale", 3.f);
        int32_t* lim = (int32_t*)GameVa(0x14105ee04ull);
        if (lim && *lim == 81920 && scale > 1.f) {
            *lim = (int32_t)(81920 * scale);
            Log("guard: render instance limit 81920 -> %d per frame (both VR eyes share a frame's buffer, D-224)", *lim);
        } else {
            Log("guard: render instance limit left at %d (D-224)", lim ? *lim : -1);
        }
    }
    const bool ok = DetourInstall(kBindSrvTable, (void*)&hk_BindSrvTable, (void**)&o_BindSrvTable);
    Log("guard: null texture-table guard (sub_1409a0cd0) %s", ok ? "installed" : "NOT installed");
    const bool ok2 = DetourInstall(kDrawState, (void*)&hk_DrawState, (void**)&o_DrawState);
    Log("guard: unbuilt material pass guard (sub_1405b55b0) %s", ok2 ? "installed" : "NOT installed");
    const bool ok3 = InstallPassJobGuard();
    Log("guard: unbuilt pass object guard (sub_1404b9e80 frame job, D-220) %s", ok3 ? "installed" : "NOT installed");
    const bool ok4 = InstallDrawCmdGuards();
    Log("guard: dead object draw command guards (sub_14060f070, D-220d) %s", ok4 ? "installed" : "NOT installed");
}

void InstallCrashLog() {
    static bool done = false;
    if (done) return;
    done = true;
    const auto* base = (const uint8_t*)GetModuleHandleW(nullptr);
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    g_base = (uintptr_t)base;
    g_size = nt->OptionalHeader.SizeOfImage;
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&CrashVeh, &self) && self) {
        const auto* mdos = (const IMAGE_DOS_HEADER*)self;
        const auto* mnt = (const IMAGE_NT_HEADERS*)((const uint8_t*)self + mdos->e_lfanew);
        g_modBase = (uintptr_t)self;
        g_modSize = mnt->OptionalHeader.SizeOfImage;
    }
    AddVectoredExceptionHandler(0, CrashVeh);
    Log("crashlog: exceptions inside GoW.exe and the mod are logged (code, address, registers, stack, mode, stereo phase)");
    // D-112: a real crash (unhandled) writes gowvr_logs\crash_reports\crash_<date>.txt; the game's own crash handler is
    // chained behind ours (its SetUnhandledExceptionFilter goes through the hook)
    SetUnhandledExceptionFilter(CrashFilter);
    void* unused = nullptr;
    const bool uef = PatchImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "SetUnhandledExceptionFilter", (void*)&hk_SetUEF, &unused);
    Log("crashlog: crash reports on (gowvr_logs\\crash_reports), game crash handler chained %d", uef);
}

}  // namespace gowvr::diag
