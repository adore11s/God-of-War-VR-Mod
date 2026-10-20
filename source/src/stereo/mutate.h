#pragma once
#include "../core/mat.h"
#include <cstddef>

namespace gowvr::stereo {

// RE-08 reversible-mutation experiment hooks (BuildRenderViewTable post-hook, View::Update post-hook).
bool InstallMutationHooks();
// seam: "table" (render view table entry of the main view) or "view" (view A object after View::Update).
// kind: "yaw" (degrees) or "offx" (metres along camera right). amount 0 = armed zero-delta control.
void MutateArm(const char* seam, const char* kind, float amount, int frames);
void MutateOff();
// Holds view A at a snapshot taken on the next View::Update (camera-still instrument for memory searches).
void Freeze(bool on);
// 64-byte camera->world of the frozen snapshot, or null.
const unsigned char* FrozenCamWorld();
// Per-eye camera for the RenderView parameter override (used by the R1 double kick around each kick).
void SetEyeCamera(bool on, float offX, float yawDeg);
// Head-tracked eyes: local-space rigid deltas per eye (row-vector convention), applied to view A's camera with pitch
// and roll removed when `level`. SetEyeIndex selects the eye for each build.
void SetEyePoses(bool on, const Mat4* eye0, const Mat4* eye1, bool level);
// tanX/tanY of the projection the eye was last rendered with (for the submitted layer's FoV).
bool EyeRenderedTan(int eye, float* tanX, float* tanY);
// Symmetric frustum for the head-tracked eye views (0,0 = the game's own projection).
void SetEyeFov(float tanX, float tanY);
// Diagnostic: log view A's camera at the next n main RenderView builds.
void CamLog(int n);
// Game thread, before kick 1 of a head-tracked frame: remember view A's original for this frame (view A write-back).
void EyeFrameBegin();
void EyeViewAWrite(bool on);
// VR: head-centred camera + VR frustum into view A at View::Update (shadows/culling follow the head).
void SetHeadCamera(bool on);
// Diagnostic: drive the arms (simulated hands) in the flat third-person view.
void ArmsDiag(bool on);
// Camera yaw sync (view detached from the game camera, camera + body steered toward the head). gowvr.ini camsync.
void CamSync(bool on);// Headset aiming: the aim view's pitch bias (degrees up), saved in gowvr.ini aim_pitch_deg.
void AimAdjustPitch(float deltaDeg);
// Diagnostic: compare the main RenderView after eye 0 and eye 1 builds for n double-kicked frames.
void RvProbe(int n);
// Which eye the next RenderView build belongs to (per-eye banked previous-camera state), and a history reset.
void SetEyeIndex(int eye);
void ResetEyeHistory();
// D-083: this frame is a flat 2D screen (VR on and the pause menu open): one plain kick with the game's own camera,
// shown as a screen in the dark (xr.cpp). Latched once per frame at view A's View::Update.
bool Screen2DFrame();
void Screen2DMode(int mode);  // 0 off, 1 pause menu (default), 2 always (test)
// D-083: the UI (2D views C / D) drawn at this fraction of the eye image around its centre (stereo frames only)
void UiScale(float s);
void UiScaleFp(float s);  // D-102: UI scale in F1 / F3 (ui_scale_fp)
// D-107: while head-aiming (+400 ms) - the game reticle's UI spot / radius and where the HUD draws it (UI uv)
bool AimReticleDisplay(float* srcU, float* srcV, float* srcR, float* dstU, float* dstV);
// The UI's extra vertical scale that undoes the headset's vertical stretch of the game's 2D UI (frame width / height in;
// 1 = none: VR eye frustum not set, or gowvr.ini ui_aspect_fix=0)
float UiVerticalFix(float frameAspect);
// The game's pause menu is open (its inPauseMenu byte, latched at view A's update) - the VR settings panel shows then
bool PauseMenuOpen();
// D-271: how many loading screens (Kratos not posed) have ended this session - 1+ = a save has been loaded
int LoadsFinished();
// VR check line (log): UI view updates scaled / skipped and why, and the flat 2D screen's state and signals
void UiDiagText(char* out, size_t n);
float UiScaleCurrent();   // D-104: the sharp HUD scale now (ui_scale / ui_scale_fp in F1 F3; "uiscale", "uiscalefp")
// D-147: the head camera (eye midpoint, camera->world) and its projection tangents as view A got them this frame; false
// when no head camera ran in the last 250 ms (any thread)
bool HeadCameraForUi(Mat4* camToWorld, float* tanX, float* tanY);
void LoadWatch(bool on);  // D-083 diagnostic: loading-screen candidate signals per frame
void LoadWatchTick();     // render thread, each Present

}  // namespace gowvr::stereo
