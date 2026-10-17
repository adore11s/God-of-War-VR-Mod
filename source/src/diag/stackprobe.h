#pragma once
#include <cstdint>

namespace gowvr::diag {

// Installs a call-stack probe at a guarded function entry (stolen length + guard bytes from tools/re/hookgen.py).
// Logs up to `budget` distinct caller stacks. One probe per session.
bool StackProbeInstall(uint64_t staticVa, int stolen, const uint8_t guard[16], int budget);
void StackProbeRearm(int budget);
// Suspend each other thread briefly, unwind it and log its stack (hang diagnosis).
void DumpAllThreadStacks();

}  // namespace gowvr::diag
