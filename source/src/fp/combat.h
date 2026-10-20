#pragma once

namespace gowvr::fp {

// F1 combat body rules (docs/F1_COMBAT_PLAN.md section 10). Gameplay F1 only; everything is neutral otherwise.
//   HOLD    - attacks / evades (active move animation att*, defevade*, spartankick*, *delaystance*): Kratos' pose is held
//             at the pose from just before the move (FreezeLocal), arms stay on the controllers, the game still moves
//             his position (lunges / dashes); leaps measured live from the animated pose raise the body and the view
//             (vertical only).
//   ANIMATE - sync moves (LuaInteract::IsDoingSyncMove: grabs, finishers) and a few cinematic moves (rage enter / grab,
//             boulder rip): the game's full animation plays, arms follow the animation, the view follows the animated
//             head's filtered translation (never its rotation).
void CombatTick();                      // game thread, every frame (after the move reader)
float CombatHoldWeight();               // 0..1 weight of the held pose for the hero's world passes
bool CombatArmsFromAnimation();         // true while ANIMATE: the arm IK stands down
// D-244: VR settings panel "Motion controls" (gowvr.ini motion_controls, default 1, live). Off = first person with the
// controllers as a plain gamepad: no arm IK, no gesture attacks, attacks / evades play their animation (no hold, no
// speed-up, no hit boost); everything else in F1 (view, hit reactions frozen, turning, walls) stays. Any thread.
bool MotionControls();
void CombatCameraOffset(float out[3]);  // world offset added to the F1 eye base (leaps / ANIMATE)
float CombatBodyLift();                 // metres the held body is raised in the hero pass (leaps)
void CombatReportRise(float rise);      // hero pass: animated neck height minus the held pose's (metres)
void CombatEnable(bool on);             // command "combat on|off", gowvr.ini combat_f1 (default 1)
// D-062 combat rework (owner attack review, docs/F1_ATTACK_REVIEW.md); safe from any thread
bool CombatInRage();                    // Spartan Rage active (from the hero's animation names)
bool CombatRageMeter(float* value, float* max);  // D-094: the Spartan Rage meter (hero meter "Blood"); false = unread
float CombatHealthFraction();  // D-267: Kratos' health / its max (hero meter "Health"), -1 = not read
bool CombatRageReady();                 // D-094: the Spartan Rage meter is full (L3+R3 enters Rage)
// D-097 interaction context (gameplay F1; from the hero's animation names / the sync-move flag)
bool ContextHandAuthored(int h);        // this hand (0 left, 1 right) follows the animation, not the controller
bool ContextBodyLocked();               // the body keeps the game's facing (does not turn with the head)
bool ContextPlainButtons();             // right grip / trigger are plain R1 / R2 (no swing gestures)
bool ContextThrowable();                // a throwable in the hand: swings throw it without a fist
bool CombatBladesFree();                // D-073: a Blades aimed move plays - Blades follow the game's pose in the world
bool CombatSprinting();                 // the game's sprint ("navBrutalRun*") is playing
bool CombatBusy();                      // an attack / evade / kick / sync move is playing
unsigned long long CombatIdleMs();      // ms since the last attack / evade ended (0 while busy)
void CombatRequestAnimate();            // the next attack move that starts plays in full (button-triggered moves)
void CombatTrace(int frames);           // diagnostic "combattrace N"
bool CombatInStance();                  // the game's post-attack "delay stance" (pause-combo window) is playing
bool CombatHeroIdle();                  // D-078: Kratos' primary animation is a plain idle (no stance, no move)
void CombatFinisherPressed();           // D-080 / D-154: R3 sent (stun finisher / lock-on): no attack hold until it is done
void CombatFinisherCancel();            // D-154: a swing - the R3 was a lock-on, holds are back
bool CombatInOwnFollowUp();             // D-154: one of Kratos' own attack follow-ups (cs*: the axe stuck in the enemy ...)
unsigned long long CombatLastAttackStartMs();  // D-154: GetTickCount64 of the last attack move start (0 = none)
void CombatHitProbe(bool on);           // D-080 diagnostic "hitprobe on|off": stacks of the hit-data builders
void CombatTsProbe(bool on);            // D-080 diagnostic "tsprobe on|off": stacks of time-scale dips (hit pauses)
void CombatHitRadiusFist(float m);      // D-108 "hitradiusfist <m>": the same for bare-handed attacks (hit_radius_fist)
void CombatHitRadius(float m);          // D-081 "hitradius <m>": Kratos' attack sphere radius in F1 (0 = the game's own)
void CombatAttackHold(bool on);         // G1 A/B: hold the position during attacks (no lunge), command "attackhold"
// D-064: the game camera's yaw (its back row; mutate.cpp) = the frame of the game's left stick
void SetGameCamYaw(float yaw);
// D-064: forward stick push (0..1, 0 = none) the pad merge adds while the player's stick rests: a held attack's recovery
// goes into locomotion like when walking, instead of the post-attack "delay stance" (and its sound)
float CombatStickNudge();
bool CombatBlockMove();  // D-200: the axe-stuck follow-up - the left stick reaches the game centred
// D-065: while a held attack (or its recovery tail) holds the position: the held spot (x, z) - the drawn hero root is
// put exactly there (body.cpp hk_LocalToWorld); false otherwise
void CombatCtlDump();  // diagnostic "ctldump"
// D-066: while a held attack holds the facing (aimed at the view at its start): the held game yaw
bool CombatHeldYaw(float* yaw);
// D-067 spam: a new attack is queued while one plays - a held attack's rest is fast-forwarded (no effect otherwise)
void CombatFastForward();
void CombatStanceFastForward();  // D-076: a swing waits on the post-attack stance - fast-forward it
void CombatReach(float m);   // "attackreach <m>"
float CombatReachNow();
void CombatAttackAim(bool on);  // A/B "attackaim on|off"
bool CombatHeldRoot(float* x, float* z, float* y);  // y = NAN: keep the game's height (walking during the attack)
// D-077: after a held attack lets go, the held spot minus the game's position at that moment, decaying (~0.1 s) - the view
// eases onto the game's position instead of jumping (e.g. the game stopped Kratos at a ledge: 0.15 m in one frame)
bool CombatReleaseOffset(float* dx, float* dz);
bool CombatViewHeight(float gameY, float* y);  // D-084: the eye height while walking during a held attack
void CombatNudgeAmount(float a);        // command "nudge <0..1>"
// D-233: a move in which Kratos is HIT (his own hit reactions hit*, Baldur's quick hits on him) - frozen in F1, not a scene
bool IsHitReactionMove(const char* a);

}  // namespace gowvr::fp
