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

}  // namespace gowvr::stereo
