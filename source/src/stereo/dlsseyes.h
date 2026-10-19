#pragma once
// D-108 per-eye DLSS: the stereo double kick evaluates the game's one DLSS instance twice a frame (eye 0, eye 1), so each
// eye was reconstructed from the OTHER eye's history - smear around near objects (characters) with the eyes' parallax.
// A second DLSS instance is created next to the game's and used for eye 1.
#include <windows.h>

namespace gowvr::stereo {

// hk_GetProcAddress: a wrapper for the NGX D3D11 entry points this needs (CreateFeature / EvaluateFeature /
// ReleaseFeature / Shutdown / Shutdown1) - the original is kept; nullptr = not wrapped
FARPROC DlssEyesWrap(const char* name, FARPROC original);
void DlssEyesEnable(bool on);  // command "dlsseyes on|off", gowvr.ini dlss_per_eye (default on)
// dlss-vr: command "dlsslog N" - the next N DLSS evaluations' inputs (jitter, motion-vector scale, reset, sharpness,
// exposure) per eye, and the main RenderView projection's off-centre terms per build (does the image carry that jitter?)
void DlssLog(int evals);
void DlssFind(int evals);  // dlss-vr diagnostic "dlssfind N": the counter that steps with the jitter phase
// r1.cpp, game thread, every kick: true while the stereo view renders - the DLSS jitter phase count for VR (or back)
void DlssVrFrame(bool stereo);
// D-242 (owner: "without DLSS the VR view shimmers - make it work with and without DLSS: DLSS on = the new logic, off =
// the old"): true while the DLSS-era stereo fixes apply - the game's DLSS is on (its flag 0x143e60c04), read live every
// call (the game's graphics setting switches it). gowvr.ini dlss_fixes_auto 0 = the fixes always on (as before)
bool DlssFixesActive();
// mutate.cpp hk_RVBuild, after the main RenderView (kind 0) is built: eye 0 / 1, -1 = flat
void DlssNoteProjection(int eye, const float* proj);

}  // namespace gowvr::stereo
