#pragma once
#include <cstdint>
#include "../core/mat.h"

namespace gowvr::fp {

// Game thread, once per frame (after the first-person base is known): publish the arm plan. handWorld = controller grip
// poses in game world space (rows X,Y,Z,pos), handValid per hand (0 left, 1 right). active = gameplay first person.
void ArmsUpdate(bool active, const Mat4* handWorld, const bool* handValid);
// Any thread, inside every local->world pass on a skeleton: apply the published plan if it is the hero's skeleton.
void ArmsApply(uint8_t* skel);
// Latest IK result for a hand (0 left, 1 right): wrist and elbow world matrices; false while that arm is authored.
bool ArmsIkResult(int hand, Mat4* wrist, Mat4* elbow);
// D-213: the arms' motion-control weight right now (1 = controllers, 0 = animation; eased arm_blend_ms)
float ArmsIkWeight();
void ArmsEnable(bool on);
void ArmsCurlTest(float v);  // diagnostic
void ArmsAttachOld(bool on);
void ArmsAttachMove(bool on);
void ArmsLocal(bool on);
void ArmsJitter(int passes);
void ArmsReach(int n);
bool ArmsFramePending();
void ArmsBladeDiag(int n);
void ArmsBladeDiagAll(bool on);
void ArmsShrink(int a, int b);
void ArmsAttackFilter(bool on);
void ArmsAttackFull(bool on);
void ArmsBladeAttackPose(bool on);
void ArmsGripReset();            // diagnostic "gripreset": forget the Blades' learned grips (re-created at the next draw)
void ArmsGripSettleV2(bool on);  // D-078 A/B "gripsettle new|old"
void ArmsBladeFlip(int hand);  // D-087: toggle that hand's Blade upside down (animated flip); 0 left, 1 right
void ArmsBladeFlipReset();     // D-087: both Blades back to the normal grip at once
bool ArmsBladeFlipped();       // D-087: a Blade is flipped or flipping
bool ArmsBladesFree();  // D-077: the Blades are on the game's L2 move (free), until they are back in its hands  // D-074 A/B "bladeattack on|off"    // D-053 A/B  // D-051 A/B
void ArmsBladeCheck(const char* mode, const char* tag);  // diagnostic: Blades vs IK hands per test segment
// D-048: animated -> motion-controlled wrist correction of the hand whose animated wrist is nearest to pos (< 1.6 m)
bool ArmsHandCorrection(const float* pos, Mat4* delta);  // diagnostic: shrink hero joints a..b (-1 = off)
void ArmsFistCenter(bool on);  // A/B: Blades handle moved through the fist center  // diagnostic: attach joints near the animated wrists  // D-042: the latest arm plan has not reached the hero's world cache yet
bool ArmsStanceYaw(float* yaw);  // D-040: shoulder-line yaw minus the body's yaw (radians), while the arms are on         // diagnostic: bone lengths / distance to the target per arm   // diagnostic: pass-to-pass arm pose change relative to the hand       // diagnostic: D-038 rebuild from locals on/off (A/B)  // diagnostic: attach joints follow the IK hands (D-034) on/off
void ArmsAttachDiag(int n);    // diagnostic: per-pass wrist / attach joint distances  // diagnostic: also move joint-4 children that have children (pre-D-035 behaviour)
// Hand angle corrections for the current headset family, saved in gowvr.ini: pitch (F5/F6), yaw (Ctrl+F5/F6) and roll
// (Shift+F5/F6), -/+5 degrees per press.
void ArmsAdjustPitch(float deltaDeg);
void ArmsAdjustYaw(float deltaDeg);
void ArmsAdjustRoll(float deltaDeg);

}  // namespace gowvr::fp
