#pragma once
#include "../vr/xr.h"
#include "../core/mat.h"

namespace gowvr::stereo {

// Game thread, before kick 1: take the latest located OpenXR views, set both eye cameras (mutate.cpp SetEyePoses) and
// report the views + rendered FoV the frame is drawn with (for the submitted layer). False: no valid views yet.
bool HeadcamPrepare();
// Game thread, before kick 1: arm the eye cameras computed by the last HeadcamPrepare; `used` = views + rendered FoV.
bool HeadcamArm(vr::EyeViews* used);
// Head-centred camera (eye midpoint, same orientation) + frustum of the last prepare, for view A at View::Update.
bool HeadcamCenter(Mat4* center, float* tx, float* ty, bool* level);
void HeadcamOverscan(float marginDeg, int predictFrames);
// Controller grip poses in game world space for this frame's base (same chain as the eyes: pose relative to the
// re-center origin, scaled, composed onto `base`). valid[h] false if that hand is not tracked.
void HeadcamHandsWorld(const Mat4& base, Mat4 out[2], bool valid[2]);
void HeadcamHandSim(bool on);
void HeadcamHandSimLock(bool on);    // diagnostic: simulated hands ignore the fplook head turn
void HeadcamHandSimMove(float amp);
void HeadcamHandSimFwd(float m);     // diagnostic: simulated hands' distance in front of the eyes  // diagnostic: simulated hands move / roll (0 = still)
void FpLook(float pitchDeg, float yawDeg, float back, float up);  // diagnostic head-space view offset (harness shots)
void HeadcamHandSimRoll(float deg);
void HeadcamRecenter();
// Increments on every performed re-center (first person recalibrates its eye point then).
int HeadcamRecenterCount();
// First person: the head position stays on Kratos' head (HMD rotation only); hands keep their offset from the head.
void HeadcamLockPosition(bool on);
void HeadcamHmdFov(bool on);
void HeadcamConfig(float scale, bool level, float testYawDeg = 0.f, float motion = 0.f);

}  // namespace gowvr::stereo
