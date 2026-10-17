#pragma once
#include <cstdint>

namespace gowvr {

// A hook site validated offline by tools/re/hookgen.py for the one supported GoW.exe hash: `stolen` bytes are whole
// instructions with no RIP-relative operand or branch, and `guard` is the first 16 live bytes that must match.
struct HookSite {
    const char* name;
    uint64_t staticVa;  // 0x140000000-based
    int stolen;
    uint8_t guard[16];
};

// Installs `hook` at the site; *original receives a trampoline that runs the stolen bytes and continues the original.
// Fails closed (returns false, patches nothing) on guard mismatch, allocation failure, or a thread executing inside
// the patched bytes.
bool DetourInstall(const HookSite& site, void* hook, void** original);
bool DetourRemove(const HookSite& site);

// Replaces an indirect call instruction at the site (e.g. `call qword ptr [r10+0x180]`, `stolen` = its length >= 5)
// with `call rel32` to a relay that jumps to `fn`, padding with NOPs. `fn` must have the callee's exact signature and
// forward to the original target itself. Same guard/fail-closed rules as DetourInstall.
bool DetourCallSite(const HookSite& site, void* fn);

// For a switch case "<argument setup><indirect call><jmp rel32>" whose call is too short for DetourCallSite: the case's
// first whole instructions (`site.stolen` >= 5 bytes, inside the setup) become a jmp to a stub that runs the setup
// (copied - it must be position-independent), calls `fn` (the callee's exact signature, forwarding itself) and jumps
// where the case's jmp goes. A thread returning from the original callee lands on the untouched jmp. `call` = the
// expected call bytes at `callOffset` (verified with the guard and the jmp after it).
bool DetourCaseCall(const HookSite& site, int callOffset, const uint8_t* call, int callLen, void* fn);

// D-220: replaces the site's first `site.stolen` bytes (>= 5, whole instructions) with a jmp to a copy of `code` (`len`
// bytes, position-independent, written for the registers / stack at the site) followed by a jmp back to
// target + stolen. The stub must do whatever the replaced instructions did (or skip it) and fall through at its end.
// Same guard / fail-closed rules as DetourInstall.
// D-220b: the stub runs inside the site function's frame (it is jumped to, not called), so it is registered with that
// function's unwind codes (looked up in the running GoW.exe, handler dropped, all codes applied) - without them Windows
// cannot walk the stack through it and a fault there or below kills the process with no crash handler run.
bool DetourStub(const HookSite& site, const uint8_t* code, int len);
// D-220b: true for an address inside one of the mod's detour blocks (relays / trampolines / stubs) - lock-free, for the
// crash handler (a fault there was in neither GoW.exe nor the mod's DLL and left no report)
bool DetourOwnsAddress(uintptr_t a);

// D-110 (code audit M5): DllMain only. version.dll is a static import of GoW.exe, so while the loader runs its DllMain
// no thread can be executing game code yet, and suspending threads (loader workers that may hold the heap lock) under
// the loader lock could hang the launch. While on, installs patch without suspending other threads.
void DetourEarlyInstall(bool on);

}  // namespace gowvr
