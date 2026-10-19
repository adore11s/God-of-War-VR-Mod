#pragma once
#include <cstdint>
#include "../vr/xr.h"

struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace gowvr::stereo {

// Execute BOTH kicks' recordings: kick 1's chunk contexts are copied after it drains (EyesSteal), submitted before
// kick 2's at end-of-frame submission (call-site hooks), and each eye's final image is captured on the render thread
// (after eye 0's last context executes / at Present for eye 1).
bool EyesInstall();
void EyesSteal();                       // call between kicks, after kick 1's work is drained
void EyesOnPresent(IDXGISwapChain* sc);  // capture eye 1, before the original Present
void EyesEnable(bool on);
// Game thread, before kick 1: the views this frame's eyes are rendered with (travel with the stolen eye-0 slot).
void EyesSetFrameViews(const vr::EyeViews& v);
// Render thread, after EyesOnPresent: this frame's eye pair and the views it was rendered with. False: no pair.
bool EyesPair(ID3D11Texture2D** eye0, ID3D11Texture2D** eye1, vr::EyeViews* views);
bool EyesEnabled();
// D-108 render thread: the eye whose command streams are executing now (0 / 1), -1 = eyes off
int EyesRenderEye();
// D-241 AFR rendering (VR settings panel / gowvr.ini afr_rendering): one kick per frame, the eyes alternate. EyesAfr
// switches the capture to it (live); EyesAfrKick (game thread, before the frame's single kick) names the eye and its
// views - they travel with the frame's submission, the render thread captures that eye at Present and pairs it with the
// other eye's last image and the pose it was rendered at.
void EyesAfr(bool on);
void EyesAfrKick(int eye);
bool EyesAfrOn();
// Save both captured eye images to gowvr_logs/shots/<name>_eye0.png / _eye1.png (1/div scale). Command thread.
void EyesShot(const char* name, int div);

}  // namespace gowvr::stereo
