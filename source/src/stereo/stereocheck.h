#pragma once
// D-336 stereo check (owner: "players see laser lights / god rays coming out of light auras such as flames in Native stereo,
// even PS VR2 players, I never do - make sure stereo is the same for everyone and make detailed logging so their log says
// what didn't load"). Everything the stereo picture depends on that can differ between two PCs goes into gowvr.log:
//  - in each VR summary ("stereo check:" lines): what did not load (core/parts.h), the game build, CPU / RAM / Windows,
//    the upscaler path (DLSS / FSR / TAA and whether each eye has its own history), every game settings.ini value, every
//    gowvr.ini value and the stereo keys changed from the built-in defaults, other injected DLLs (overlays, ReShade ...),
//    SteamVR's motion smoothing, the frame's render passes by name, and a verdict listing the differences from the
//    owner-tested setup;
//  - once a minute with the VR check ("stereo health N:"): per-frame rates of every stereo step (particle jobs per eye,
//    fog history fix, lens flares, how far the render thread / GPU are behind, the wait before eye 1, frame pacing).
#include <dxgi.h>
#include <cstdint>

namespace gowvr::stereo {

// render thread, every game Present (frame/d3d_hooks.cpp): one GPU event query per frame - how many frames the GPU still
// has queued (finished queries are polled without flushing)
void StereoCheckOnPresent(IDXGISwapChain* sc);
// the GPU's queued frames at the last Present (-1 = not measured yet)
int StereoGpuQueued();
// the "stereo check" block of a VR summary (vr/xr.cpp LogSummary); `first` adds the once-a-session parts (CPU, Windows,
// DLL list, the whole gowvr.ini)
void StereoCheckLog(bool stereo, bool first);
// the "stereo health" line (vr/xr.cpp VrCheckTick, render thread): `headsetFrames` = frames handed to the headset so far,
// `displayMs` = the headset's frame period
void StereoHealthLog(int number, uint64_t headsetFrames, double displayMs);

// r1.cpp: the per-frame rates since the last call (one caller), and the frame's render passes by class name
void R1HealthText(char* out, size_t n);
int R1PassNames(char* out, size_t n);
// dlsseyes.cpp / taaeyes.cpp: their state and counters in one phrase
void DlssStatsText(char* out, size_t n);
void TaaEyesStatsText(char* out, size_t n);

}  // namespace gowvr::stereo
