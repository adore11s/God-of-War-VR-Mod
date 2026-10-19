#pragma once
#include <cstdint>
#include "../core/mat.h"

namespace gowvr::fp {

// Hero skeleton access (STATIC, lua bindings FindJointIndex / GetWorldJointPosition / GetHeroPersistentLevel):
//   hero game object = *(0x1422c6950); skeleton = *(hero+0x238); joint count = *(i16*)(skel+0x38);
//   joint name hashes = *(u64**)(skel+0x30) (sub_1400f44c0 hash: case-folded, h=(c+h)*0x401, h^=h>>6);
//   world matrices = *(u8**)(skel+0x58) + (count + i) * 0x40 (row-vector 4x4, translation row 3).
// Cutscene / no-walk state: the game's cinematic-mode flag (Lua IsInCinematicMode -> byte 0x1422e9d2f).
bool InCinematic();
// D-212: a cutscene or a scripted scene (fp/firstperson.cpp ScriptedScene) this frame - Kratos' pose is the scene's
bool SceneAnimating();
// Player is in the boat (boarding/leaving transitions of the boat's docking state machine).
bool InBoat();
uint64_t HeroWorldPassCount();
// Render thread side of the hide lists: shrink right before the frame's render kicks, restore right after them.
void BodyHideDrawBegin();
void BodyHideDrawEnd();
void BodyHideEnsure();  // D-082: re-shrink the neck before a kick if a pass put it back
bool HideBody();  // always false: the F7 "arms only" mode was removed (owner 2026-10-19)
// The Leviathan axe is attached to the (IK) right hand right now (read by the arms: closed fist).
bool AxeInHand();
// D-153: the drawn (VR) axe as a segment grip -> head, this frame and the previous one (world); false when not recent
bool AxeHitSegment(float cur[2][3], float prev[2][3]);
int BladeHitSegments(float cur[2][2][3], float prev[2][2][3]);  // D-185: the drawn Blades (root -> tip), count 0..2
// vrmap: the player asked for the axe (recall / draw, toHand) or to put it away (the in-hand test uses the timing).
void AxeRequest(bool toHand);
// D-028 (gameplay F1 only): vrmap detected an aimed throw and pressed the throw button. Kratos' creature time scale is
// raised until the axe leaves the hand (instant release) and his body pose is held at the pre-throw pose through the
// throw animation (no body motion); the arms stay on the controllers.
void ThrowStart();
// D-063: the Blades' aimed throw input fired (vrmap); the Blades follow the game's throw for 1.3 s (arms.cpp)
void BladeThrowStart();
void BladeAimHeld();      // D-077: vrmap - L2 + R2 held with the Blades (the L2+R2 spin), every poll while held
bool BladesFreeRaw();     // D-077: an L2 Blades move is being input / plays (arms.cpp keeps it until the Blades are back)
bool HeroTimeScale(float scale, float* prev);  // Kratos' creature time scale (slot 5), game thread
// D-203: slot 5's one arbiter - owner 0 combat, 1 the throw; scale 1 (or 0) releases; the fastest wish is applied
bool HeroTimeScaleWish(int owner, float scale);
bool BladeThrowActive();
// The axe is flying / stuck somewhere (full size, far from both hands): the right grip recalls it.
bool AxeThrown();
void AxeDiag(int n);
void AttDiag(int n);             // diagnostic: axe/shield/bow pass placement in the game and rendered body frames
void BodyTestOffset(float deg);  // diagnostic: standing body yaw = game facing + deg (0 = follow the head)             // diagnostic: log the axe in-hand inputs for the next n axe passes
void SetCarriedHide(bool on);    // hide the bow (carried item) in gameplay first person (game model visibility)
void SetNeckHide(bool on);       // neck shrink list with the hidden head
// The game's "NarrativeCarry" state on the hero (story log carry).
bool NarrativeCarry();
// First person: collapse the head/face joints onto the neck anchor in every world pass (hides the head mesh).
void SetHeadCollapsed(bool on);
uint64_t JointHash(const char* name);
int FindJoint(const char* name);         // hero joint index or -1
int FindJointByHash(uint64_t hash);      // hero joint index or -1 (cached per skeleton)
uint8_t* HeroObject();                   // the hero's skinned game object (skeleton at +0x238) or null
bool IsKratosSoundOwner(uint8_t* obj);   // D-162: Kratos / his axe, shield, Blades (not thrown) - sound emitter owners
bool JointWorld(int index, Mat4* out);
// Hero joint world matrix computed from the animation's LOCAL transforms (sub_140591550): never contains the mod's
// world-cache edits (arm IK, head collapse). For context checks that must not see our own output.
bool AnimatedJoint(int index, Mat4* out);   // hero joint world matrix (racy while animation jobs run)
// Hero joint as of the last COMPLETED world pass (head, root, neck, shoulders, wrists; published by the pass hook).
bool StableJoint(uint64_t hash, Mat4* out);
void DumpJoints();
// Game thread after animation (View::Update of view A): per-frame joint edits in the world cache.
void BodyPostAnim();
void ArmTest(float dy);  // diagnostic: lift the right hand subtree
int JointParent(int i);
bool IsDescendant(int j, int ancestor);
Mat4* WorldCache(int i);
// Game thread, every frame: Kratos' rendered facing. active = gameplay first person: eases toward the view yaw (the
// game's own facing while walking); inactive: eases back to the game's facing and releases. Applied by turning the
// root matrix of the hero's local->world passes (the game rebuilds obj+0x60 every frame).
void BodyFollowYaw(float viewYaw, bool active, bool aiming);
// The hero object's pose as the game built it / as the pose was built with our facing (position, yaw), from the last
// world pass. The rendered body turns about the eye pivot set here (object frame: right, back).
bool GameRoot(float pos[3], float* yaw);
bool RenderRoot(float pos[3], float* yaw);
void SetBodyPivot(float right, float back);
// D-114: gameplay first person - the drawn body moved by this (game object frame: right, back, metres) so its eye stays
// under the view whatever the stance; 0, 0 = none
void SetBodyEyeShift(float right, float back);
void BodyPivot(float* right, float* back);  // the eye pivot in the game object's frame (right, back)
float ViewYaw();
// a snap turn: the drawn body turns by `delta` (radians, the view yaw's sense) at once (mutate.cpp CamSync)
void BodySnapYaw(float delta);
bool HeroMoving();                          // D-072: Kratos (drawn) moved within the last ~20 frames                            // the latest F1 view yaw (back-row convention, like the game root yaw)
// diagnostic (read-only): where does the hero object keep its facing? Logs every float triplet / quaternion in the
// hero object and the blocks it points to that match the root joint's axes, position or yaw.
void FacingScan();
void AxeFind();
void AxeTrace(int n);  // diagnostic: log the next n axe/hero passes  // diagnostic (read-only): objects near the right wrist
// diagnostic: write the hero object's yaw (current + delta) for 2 s and log whether Kratos' root follows/keeps it
void YawTest(float deltaDeg);
void ScanObjects();
void NearObjects();
const uint8_t* PassLocals(uint8_t* skel);  // the local pose the current pass (this thread) was fed
void PassLocalStats();                     // diagnostic
void PassInfo(uintptr_t* caller, int* flag);  // diagnostic: the current pass's caller (rva) and flag (this thread)
void FlyDiag(int n);       // diagnostic: flying Blades copies vs the hands
int CurrentWeapon();       // D-043: 0 unknown, 1 axe, 2 blades, 3 bare (the game's weapon mode)
void WaitHeroFrame();      // D-042: r1.cpp, before kick 1 (eye 0): the frame's hero pass is in the world cache
void EyeSync(bool on);      // D-042 on/off (A/B)
void FastRelease(bool on);  // D-059 A/B
void SatStrict(bool on);    // D-061 variant A/B
void SatLock(bool on);      // D-061 A/B: axe effects on the drawn axe with a learned offset (on) or distance match (off)
void GlueBlend(bool on);    // D-060 A/B: carried-item glue blended (on) or hard-switched (off)
void FxJump(const char* mode, const char* tag); // diagnostic: mod-made discontinuities on effect objects
void CopyFix(bool on);     // D-057 A/B
void CopyScan(int n);       // diagnostic
void CopyStats();          // diagnostic
void FlyMove(float m);     // diagnostic
void FxFix(bool on);       // D-057 A/B: effects forced onto the drawn pose
void FxStats();             // diagnostic
void WflFix(bool on);      // D-057 A/B: effects read the edited pose
void WflStats();            // diagnostic
void DrawTrace(int n);      // diagnostic: per-frame Blades vs hands trace
void BladeObjFix(bool on);  // D-054 A/B
void BladeObjDiag();        // diagnostic "bladeobjdiag"
void PassOrder(int n);      // diagnostic "passorder N"
void ChainDiag(int n);
void HandJoints(int n);
void BladeJoints();
void ChainSync(bool on);
void ChainDump();
void ChainMode(int m);
void AxeGem(bool on);
void FlameBlade(bool on);
void WflAxe(bool on);       // D-075 A/B "wflaxe on|off"
void WflAxeStats();         // "wflaxestats"
void WeaponTrace(int n);    // D-079 diagnostic "wtrace N": axe / right Blade vs the IK wrist per frame
void TsDump(int n);         // D-080 diagnostic "tsdump N": Kratos' time-scale slots per frame (hit pauses)   // D-074 A/B "flameblade on|off"       // D-073 A/B "axegem on|off"      // "chainmode 1|2" (1 = v4 default, 2 = v5 experimental)           // diagnostic "chaindump"    // D-070 A/B "chainsync on|off"         // diagnostic "bladejoints"     // diagnostic "handjoints N": joints near the hands vs the wrists while moving
void AxeFxRefresh(bool on); // D-069 A/B "axefxrefresh on|off"      // diagnostic "chaindiag N" (Blades objects' shape while moving)
void FxLeadSet(float k);    // D-067 "fxlead <k>": effects led by k x the hero's per-frame movement
void EnsureBlades(bool on); // D-052 A/B
void CarryGuard(bool on);  // D-052 A/B
void DrawCheck(const char* mode, const char* tag); // diagnostic: Blades vs hands at each frame's draw
void EyeProbeStart(int n);// diagnostic: hero wrists at the two eyes' render moments
void TalismanTrace(int n);  // D-229g diagnostic "taltrace N"
void EyeProbe(int stage);   // r1.cpp: 0 before kick 1, 1 after kick 1, 2 after between-eyes, 3 after kick 2
void StanceComp(bool on);                 // D-040: standing body turned so the chest faces the view (A/B)
void AttJitter(int n);                   // diagnostic: shield / axe placement jitter relative to the IK hand
void SkelNearStart(int passes);  // diagnostic: skeletons whose passes put them near Kratos (found by pass)
void PhysJoints();   // diagnostic "physjoints"  // diagnostic "nearobjs"   // diagnostic: every game object with a skeleton; marks the hero's
void FindSkeleton();  // diagnostic: scan the hero object for a pointer to its skeleton-bearing game object                       // log every hero joint: index, hash, candidate name, world position

}  // namespace gowvr::fp
