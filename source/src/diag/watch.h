#pragma once
#include <cstdint>

namespace gowvr::diag {

// Hardware data watchpoints (DR0-DR3) on every thread of the process, with a vectored handler that records the
// unique instruction pointers that touched the address. Diagnostic only: the handler never alters program state.
// len: 1, 2, 4 or 8. rw: false = writes only, true = reads+writes. Re-applied to new threads while active.
bool WatchArm(int slot, uintptr_t addr, int len, bool rw);
void WatchDisarmAll();
// Logs "watch: slot=.. rip=GoW.exe+RVA hits=.. threads=.." rows and clears the table.
void WatchReport();
void WatchInstallHandler();
void WatchStacks(bool on);  // D-080: also capture a call stack per trap (first 64), printed by WatchReport
// Re-apply armed slots to threads created since arming (call periodically).
void WatchRefreshThreads();
// D-222: the next n traps are logged at once from the handler (instruction + call stack as module offsets) - for a
// write that is followed by a crash within milliseconds
void WatchImmediate(int n);

}  // namespace gowvr::diag
