#pragma once
#include "../core/mat.h"

namespace gowvr::fp {

// First-person camera from Kratos' eyes (owner spec 2026-09-28):
//   F1 = first person during gameplay (motion controls come later), F3 = first person during cutscenes (no controls).
//   Gameplay vs cutscene = "can the player walk" = the game's cinematic-mode flag (0 = gameplay).
// Game thread, at View::Update: returns the camera->world base the head-tracked eyes are composed onto, or false to
// keep the game's own camera (third person). Also hides/shows the head joint so the camera is not inside the skull.
bool FirstPersonBase(const Mat4& gameCam, Mat4* base);
void FpEye(float out[3]);  // the last first-person base position (the view before the headset pose)
void SetGameplayFP(bool on);
void SetCutsceneFP(bool on);
bool GameplayFP();
bool CutsceneFP();
void SetEyeOffset(float forward, float up);  // metres from the head joint, in the head's frame
void FpStatus();
// 0 third person, 1 gameplay first person, 2 cutscene first person (as of the last FirstPersonBase call)
int FpMode();
// F1/F3 view distance in front of Kratos' eyes (metres, gowvr.ini fp_forward, default 0.20).
void SetViewForward(float metres);
void SetViewRight(float metres);  // D-067: "fpright <m>", gowvr.ini fp_right
void SetViewDown(float metres);   // D-079: "fpdown <m>", gowvr.ini fp_down
float ViewForward();  // the current fp_forward (metres)
// Diagnostic: first-person base from the body (eye point, character facing) regardless of mode.
bool DiagBodyBase(Mat4* base);
// D-220f: a render guard met a dead / unbuilt pass (crashlog.cpp). In a cutscene with F3 on, F3 steps aside (the game's
// own cutscene camera) until that cutscene ends - the game never builds such passes for its own camera. Any thread.
void SuspendCutsceneFP(const char* why);
// D-257: Kratos is at a shop (the vendor's moves) - any thread
bool InShop();

}  // namespace gowvr::fp
