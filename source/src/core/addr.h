#pragma once
#include <cstdint>

namespace gowvr {

// D-187 (owner: "make the mod work for any build of the game - Steam, Epic, unverified builds"): every GoW.exe address the
// mod uses is recorded for the known build (0x140000000-based, as written in the source) and as a byte signature
// (tools/re/sigs.py -> core/addrsigs.inc). The known build uses the recorded addresses; any other build finds each one
// by its signature (one pass over .text) and the mod runs only when EVERY one was found unambiguously.

// DllMain, before any hook: knownBuild = the PE header matches the recorded build. Logs the outcome.
void AddrInit(bool knownBuild);
// true when the mod may run: the known build, or every signature resolved to exactly one place
bool AddrUsable();
// the runtime address of a recorded one (0 = not resolved - only on another build, for an address outside the table)
uintptr_t GameVa(uint64_t staticVa);
// another build: may the hook guard at `target` differ from the recorded bytes at position i (a moving field)?
bool AddrGuardMayDiffer(uint64_t staticVa, int i);
bool AddrKnownBuild();
// tools/addrtest only: the signature scan over a mapped GoW.exe image; differences from the recorded addresses (-1 = not all found)
int AddrSelfTest(uintptr_t imageBase);

}  // namespace gowvr
