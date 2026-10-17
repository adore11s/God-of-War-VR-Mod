#pragma once

namespace gowvr::diag {

// Starts the developer command channel thread (see cmd.cpp for the file protocol).
void StartCommandChannel();
// Any thread (the VR settings panel): VR on / off as F9 does it - carried out by the command thread within 0.1 s
void RequestVr(bool on);

}  // namespace gowvr::diag
