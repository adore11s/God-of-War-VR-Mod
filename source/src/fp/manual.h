#pragma once
#include <cstdint>
#include "../core/mat.h"
struct _XINPUT_GAMEPAD;
namespace gowvr::vr { struct InputSnapshot; }
namespace gowvr::fp {
// D-307 manual climbing / D-308 manual rowing: VR settings rows (gowvr.ini manual_climb / manual_row, default off).
// Gameplay first person with motion controls only.
//
// Climbing (static RE, docs/DECISION_LOG.md D-307): the game keeps one traversal record per creature in the list at
// 0x1411be9b0 (node+0x30 = the creature, +0x38 = the handle of the climb path it is on - LuaInteract::
// OnActiveTraversePath, +0x40 / +0x48 = the best climb choice nearby and its type name, +0x50 .. +0x8c = that choice's
// frame: left, up, forward, position - LuaInteract::GetBestTraversePathChoiceLeft / Up / Forward / Position). A climb is
// entered with the Evade button (Cross / A - the prompt code sub_14073fa40 shows it for "Interact" on a climb path) and
// moved along with the left stick; the game has no "let go" (SCR_ClimbDetach is a name without code).
//
// Game thread, every gameplay-F1 view update, with the controllers' grip poses in the game world (before the wall clamp
// and the arm plan; hands are valid only while the arms are motion controlled): pins gripping hands on the climbing
// surface, turns their pulls into the stick, sends the entry press, learns / places the paddle and turns paddle strokes
// into the boat's stick. wallsSkip[h]: the hand is held on a climbing surface - no wall push-back for it.
// head = the headset's pose in the world (the view: centre x base) - D-307b: the climbing body follows it
void ManualHands(Mat4 hands[2], const bool valid[2], bool wallsSkip[2], const Mat4& base, const Mat4& head);
void ManualIdle();         // not in gameplay F1 this frame
bool ManualClimbActive();  // climbing by hand: arms on the controllers, body held straight, the stick from the pulls
bool ManualRowActive();    // rowing by hand: arms on the controllers in the boat, body held seated, the paddle in them
float ManualPoseWeight();  // 0..1 the held body pose (body.cpp FreezeLocal)
int ManualPoseKind();      // 1 climbing (the standing pose), 2 rowing (the seated pose taken at the start)
bool ManualStandingCalm(); // Kratos stands still on the ground (the standing pose may be taken)
bool ManualRowCaptureReq();  // the seated pose is to be taken from the next hero pass (cleared by the call)
float ManualHook(int h);   // 0..1 fingers hooked over a ledge
float ManualFist(int h);   // 0..1 closed fist (the rope, a hold on the wall, the paddle)
// D-307b: climbing by hand - the drawn body's world offset toward the head (body.cpp, the hero pass); false = none
bool ManualBodyOffset(float out[3]);
// D-307c: climbing by hand - the view pushed back from the rope / wall (world; firstperson.cpp adds it to the eye point)
bool ManualViewPush(float out[3]);
// D-308d: this hand's grip pose is built by the mod (the fist round the paddle's shaft): arms.cpp applies no per-headset
// hand angle to it
bool ManualHandExact(int h);
// pad thread (vrmap): the stick / buttons of manual climbing and rowing laid over the mapped pad
void ManualPad(const vr::InputSnapshot& s, _XINPUT_GAMEPAD* g);
// D-308: the paddle object's world pass (body.cpp, any thread): out = its root re-placed between the drawn hands
bool ManualPaddlePlace(uint8_t* skel, const float* root, float* out);
}  // namespace gowvr::fp
