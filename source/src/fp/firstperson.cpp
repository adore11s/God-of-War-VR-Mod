// First-person camera (see firstperson.h). Joints by name hash (body.cpp):
//   head af26b97f3448d61a (joint 243 on Kratos' 529-joint rig), neck f14d9a553fdad7cc, spine 75bab1a8dd0cb354.
// Orientation policy (playbook ch.01 "yield to authored cameras" + comfort): the head bone supplies POSITION only;
// yaw comes from the game camera in gameplay (right stick still turns, stick-forward walks where you face) and from
// the character's facing in cutscenes; pitch/roll come only from the headset.
#include "firstperson.h"
#include "body.h"
#include "combat.h"
#include "movelog.h"
#include "../stereo/headcam.h"
#include "../stereo/eyes.h"
#include "../core/log.h"
#include "../core/config.h"
#include <windows.h>
#include <atomic>
#include <cmath>

namespace gowvr::fp {
namespace {

constexpr uint64_t kHeadHash = 0xaf26b97f3448d61aull;
constexpr uint64_t kHeadParentHash = 0xf2648c102f472469ull;  // joint 411 on Kratos
std::atomic<bool> g_gameplay{false}, g_cutscene{false};
float g_eyeFwd = 0.f, g_eyeUp = 0.f;  // extra adjustment on top of the fitted eye offset
float g_fwd = -100.f;                  // view forward of the eye point, metres (gowvr.ini fp_forward)
float g_right = -100.f;                // view right of the eye point, metres (gowvr.ini fp_right, D-067)
float g_down = -100.f;                 // view below the eye point, metres (gowvr.ini fp_down, D-079)
bool g_headHidden = false;
int g_lastMode = -1;  // 0 third person, 1 gameplay FP, 2 cutscene FP
int calibMode = -1;   // gameplay FP eye calibration: recalibrate whenever gameplay first person (re)starts
uint64_t g_frames = 0;

uint8_t* Base() { return (uint8_t*)GetModuleHandleW(nullptr); }
using PFN_SetJointVisible = void(__fastcall*)(uint8_t* skel, uint64_t joint, char visible);  // sub_140591990

void SetHeadVisible(bool visible) {
    // The engine's joint visibility (sub_140591990) only stops a joint updating - the face mesh stayed, frozen where
    // F3 was pressed, and stretched as the body moved (owner report). The body pass collapses the head joints instead.
    if (!HeroObject()) return;
    SetHeadCollapsed(!visible);
    static int logged = 0;
    if (logged++ < 4) Log("fp: head %s (collapse)", visible ? "shown" : "hidden");
    g_headHidden = !visible;
}

// The world cache holds the anchor COLLAPSED while the head is hidden (body.cpp); only the published copy is the real
// pose. A cache read with a shrunken rotation is a failed lookup, not a camera.
bool Unscaled(const Mat4& m) {
    return m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2] > 0.25f;
}

// Yaw-only frame (rows X, Y=up, Z=back) from a "back" direction.
Mat4 YawFrame(float bx, float bz, const float* pos) {
    float n = sqrtf(bx * bx + bz * bz);
    if (n < 1e-4f) { bx = 0.f; bz = 1.f; } else { bx /= n; bz /= n; }
    Mat4 m{};
    m.m[0][0] = bz;  m.m[0][2] = -bx;
    m.m[1][1] = 1.f;
    m.m[2][0] = bx;  m.m[2][2] = bz;
    m.m[3][0] = pos[0]; m.m[3][1] = pos[1]; m.m[3][2] = pos[2];
    m.m[3][3] = 1.f;
    return m;
}

}  // namespace

static float g_lastEye[3] = {0.f, 0.f, 0.f};
void FpEye(float out[3]) { out[0] = g_lastEye[0]; out[1] = g_lastEye[1]; out[2] = g_lastEye[2]; }

// D-184 (owner: "the Baldur (the Stranger) fight is a bit of a mess - the mod mistakes cutscenes for F1 mode, and F1 can't
// do the view where the head is so well; it should have been F3 cutscene"; owner log: the fight's scripted sequences
// csBaldurFromHere2ThereEnter02 / Tumble / HitWall / StatueSmash / RoofSlam / ShieldSmash / AxeStuckCounter ... and
// narFor...Cine all ran with the game's cinematic flag 0x1422e9d2f = 0, so F1's fixed eye point + body-follows-the-head
// model was used through choreographed moves). Scripted scenes count as cutscenes for the F1 / F3 choice: the boss's
// sequences (csBaldur*), the narrative moves (nar*) and anything named *Cine, held 0.5 s after the last such frame (the
// sequences chain with a few frames of ordinary moves between them). Not every "cs" move: csDefBlockParry01 (a parry),
// csDraugr00BareRageKillRip (a finisher) are ordinary combat. D-192: csMMA* is the mounted brawl on Baldur (it was
// wrongly counted as ordinary combat here). gowvr.ini scripted_as_cutscene 0 = off.
static bool ScriptedName(const char* a) {
    if (!a || !a[0]) return false;
    // D-192: + csMMA* - the mounted brawl on Baldur (csMMAKratosIdleL, csMMABaldurChokeEnterL / HitL, R1 / R2 prompts)
    if (_strnicmp(a, "csbaldur", 8) == 0 || _strnicmp(a, "csmma", 5) == 0 || _strnicmp(a, "nar", 3) == 0) return true;
    // D-206: Kratos boosting Atreus onto a ledge (navBuddyBoostSon*) - a scripted pair move
    if (_strnicmp(a, "navbuddyboost", 13) == 0) return true;
    // D-207: the points of interest's scripted moves (poi* - e.g. poiRivBrokSonHideKratos000Enter after the shop)
    if (_strnicmp(a, "poi", 3) == 0) return true;
    char low[96];
    size_t i = 0;
    for (; a[i] && i + 1 < sizeof(low); ++i) low[i] = (char)tolower((unsigned char)a[i]);
    low[i] = 0;
    // D-195 (owner: "before free roam Kratos falls to his knees - that should be F3, not F1; or from the moment he gets the
    // axe make it F3"): the wounded walk after the Baldur fight from navPostBaldurWoundedWalkPhase0AcquireAxe on
    // (Phase1 dragging the axe, Phase2 .., the fall to his knees); Phase0 before the axe stays F1
    if (strstr(low, "postbaldurwounded") && (strstr(low, "acquireaxe") || !strstr(low, "phase0"))) return true;
    // D-210 (owner: "the animation where I kill the ogre should be in F3, not F1"): the ogre (troll) sync kill -
    // csragegrabtroll00enter, csRageBeatdownTroll00Idle, csRageBeatdownTroll00Finisher. Any cs* move on a troll and the
    // csRage* grab / beatdown family; the draugr rage kills (csDraugr00BareRage*) stay ordinary combat
    if (low[0] == 'c' && low[1] == 's' && (strstr(low, "troll") || strncmp(low, "csrage", 6) == 0)) return true;
    // D-227 (owner: "the little cinematic in the hands of the big giant shouldn't be in F1 but in F3", "at the very end
    // Kratos with his shield up and Atreus behind him should be in F3"; owner log: phase0_csTransitionToPhase1 / _Loop and
    // phase1_csbaldurentrance ran in F1 as sync moves): a boss fight's scripted phase moves (phase<N>_cs*), and the
    // blizzard's shield moves csMaMoSnowBlindBlockEnter / Idle (csMaMo*; the walk navSnowBlindShuffle* stays F1)
    if (strncmp(low, "phase", 5) == 0) {
        const char* p = low + 5;
        while (*p >= '0' && *p <= '9') ++p;
        if (p > low + 5 && p[0] == '_' && p[1] == 'c' && p[2] == 's') return true;
    }
    if (strncmp(low, "csmamo", 6) == 0) return true;
    return strstr(low, "cine") != nullptr;
}
static bool ScriptedScene() {
    static const bool on = config::GetFloat("scripted_as_cutscene", 1.f) > 0.5f;
    if (!on) return false;
    static ULONGLONG lastSeen = 0;
    static bool logged = false;
    char prim[96];
    HeroPrimaryAnim(prim, sizeof(prim));
    const char* act = ActiveMoveAnim();
    const ULONGLONG now = GetTickCount64();
    const bool now1 = ScriptedName(act) || ScriptedName(prim);
    if (now1) {
        if (!logged || now - lastSeen > 2000)
            Log("fp: scripted scene '%s' - treated as a cutscene for F1 / F3", ScriptedName(act) ? act : prim);
        logged = true;
        lastSeen = now;
    }
    return lastSeen && now - lastSeen < 500;
}

// D-207 (owner: "in the shop make it so that even with F1 on it disables F1 temporarily while I'm in the shop"): the
// shop's moves (envVendor* - envVendorIdle01toIdle02 / Idle02 / KratosAxeUpgrade) turn gameplay first person off (third
// person) while they play, held 0.5 s past the last; F1 comes back on its own. gowvr.ini shop_third_person 0 = off
static bool ShopScene() {
    static const bool on = config::GetFloat("shop_third_person", 1.f) > 0.5f;
    if (!on) return false;
    static ULONGLONG lastSeen = 0;
    static bool logged = false;
    char prim[96];
    HeroPrimaryAnim(prim, sizeof(prim));
    const char* act = ActiveMoveAnim();
    const bool now1 = (act && _strnicmp(act, "envvendor", 9) == 0) || _strnicmp(prim, "envvendor", 9) == 0;
    const ULONGLONG now = GetTickCount64();
    if (now1) {
        if (!logged || now - lastSeen > 2000) Log("fp: at the shop - F1 off while it lasts (D-207)");
        logged = true;
        lastSeen = now;
    }
    return lastSeen && now - lastSeen < 500;
}

// Owner 2026-10-18 (VR settings panel "Finishers / executions"): with Third person the finishers play in the game's own
// camera instead of F1 / F3 - the sync kills whose move names start with "cs": the draugr finishers
// (csDraugr00BareRageGruntRunEnter / Loop / ExitThrow, csDraugr00BareRageKillRip), the troll kill (csragegrabtroll00enter,
// csRageBeatdownTroll00Idle / Finisher - D-210 counted it as a cutscene for F3). Not the other sync moves (climbing nav*,
// objects env*) nor the Baldur fight's scripted csBaldur* / csMMA* (D-184 / D-192: cutscenes), nor csDef* (parries,
// ordinary combat). Held 0.5 s past the last such frame (the kills chain a few moves). gowvr.ini finisher_third_person.
static bool FinisherName(const char* a) {
    if (!a || _strnicmp(a, "cs", 2) != 0) return false;
    // (D-227: nor the blizzard's scripted shield moves csMaMo* - cutscenes, ScriptedName)
    return _strnicmp(a, "csbaldur", 8) != 0 && _strnicmp(a, "csmma", 5) != 0 && _strnicmp(a, "csdef", 5) != 0 &&
           _strnicmp(a, "csmamo", 6) != 0;
}
static bool FinisherScene() {
    if (config::GetFloat("finisher_third_person", 0.f) < 0.5f) return false;
    static ULONGLONG lastSeen = 0;
    static bool logged = false;
    char prim[96];
    HeroPrimaryAnim(prim, sizeof(prim));
    const char* act = ActiveMoveAnim();
    auto trollKill = [](const char* a) {  // (the troll kill's moves may not all be sync moves)
        if (!FinisherName(a)) return false;
        for (const char* p = a; *p; ++p)
            if (_strnicmp(p, "troll", 5) == 0) return true;
        return false;
    };
    const bool troll = trollKill(act) || trollKill(prim);
    const bool now1 = (HeroInSyncMove() && (FinisherName(act) || FinisherName(prim))) || troll;
    const ULONGLONG now = GetTickCount64();
    if (now1) {
        if (!logged || now - lastSeen > 2000)
            Log("fp: finisher '%s' - third person while it plays (finisher_third_person)", FinisherName(act) ? act : prim);
        logged = true;
        lastSeen = now;
    }
    return lastSeen && now - lastSeen < 500;
}

static std::atomic<bool> g_sceneAnimating{false};
bool SceneAnimating() { return g_sceneAnimating.load(); }

// D-220f (owner: five crashes 2026-10-15, all in an F3 cutscene: render passes the frame uses were already freed /
// never built - the guards D-105 / D-109 / D-220 / 220b / 220d each caught one reader and the next one crashed, the last
// in the effects system on recycled memory). The earliest sign - a guard skip - came 1 s / 2.3 s / 22 ms before the
// crashes and only ever in F3 cutscenes (no third-person cutscene ever logged one): at the first one F3 is suspended
// for the rest of that cutscene (the game's own cutscene camera, in which the game never meets them); it comes back at
// the next cutscene. gowvr.ini f3_crash_suspend 0 = off
std::atomic<bool> g_f3Suspended{false};
void SuspendCutsceneFP(const char* why) {
    // D-220g: off by default - the 21:37 crash came in third person (F3 never on that session): F3 is not the cause
    static const bool on = config::GetFloat("f3_crash_suspend", 0.f) > 0.5f;
    if (!on || g_lastMode != 2 || g_f3Suspended.exchange(true)) return;
    Log("fp: F3 suspended for the rest of this cutscene - %s (the game's own cutscene camera; D-220f)", why);
}

bool FirstPersonBase(const Mat4& gameCam, Mat4* base) {
    const bool cine = InCinematic() || ScriptedScene();
    g_sceneAnimating = cine;
    {  // D-220f: the suspension ends with the cutscene (1 s out of it - a cut between two scenes keeps it)
        static ULONGLONG outSince = 0;
        const ULONGLONG nowT = GetTickCount64();
        if (cine) outSince = 0;
        else if (!outSince) outSince = nowT;
        if (g_f3Suspended.load() && outSince && nowT - outSince > 1000) {
            g_f3Suspended = false;
            Log("fp: cutscene over - F3 back (D-220f)");
        }
    }
    const bool want = !FinisherScene() &&
                      (cine ? (g_cutscene.load() && !g_f3Suspended.load()) : (g_gameplay.load() && !ShopScene()));
    const int mode = !want ? 0 : (cine ? 2 : 1);
    int head = want ? FindJointByHash(kHeadHash) : -1;
    Mat4 h{};
    // Anchor = the head's PARENT joint (411). The hidden head joint stops being updated by some of the engine's world
    // passes once its visibility bit is cleared (LIVE: frozen at the value it had when F1 turned on), which froze or
    // flashed the camera (owner report). Its parent stays visible and is published from completed passes.
    const int anchor = want ? FindJointByHash(kHeadParentHash) : -1;
    bool fromPublished = false;
    bool ok = want && anchor >= 0 &&
              ((fromPublished = StableJoint(kHeadParentHash, &h)) || (JointWorld(anchor, &h) && Unscaled(h)));
    // D-206 (owner: "the last animation, in F3, my head is still too low - in my stomach / lower chest, very short, the
    // camera moved down"; owner log: "published joints - collapsed in the cache, rebuilt up their parent chain 1,
    // published as they are 8481" - D-191's rebuild mostly fails and then publishes the COLLAPSED anchor; it sits
    // between the neck base and the chest, scale 0.001, the eye offset gone): the camera never takes a shrunk anchor.
    // The last good anchor is kept relative to Kratos' root joint and, while the published one is shrunk, it is put on
    // the current root - the view follows his body, never the collapse point
    // D-209: set while the anchor is that held copy - it is a frozen pose, not a reading (see the F1 lean below)
    // D-219 (owner: "the boat in F1 was bad again - the camera too high"; owner log: in the boat the anchor came collapsed
    // nearly every frame, the body-locked eye's last real reading was 1.60 m - standing, at boarding - and D-209 kept it):
    // a copy kept on the ROOT is only right in the pose it was taken in - taken standing, it put the seated Kratos' view
    // a standing height up. The collapsed anchor is now rebuilt from THIS frame's animation: the head anchor relative to
    // its first unshrunk ancestor (the chest - the hide never touches it), both from the animation's local transforms
    // (AnimatedJoint: no head collapse), put on that ancestor as drawn. That follows any pose (seated, rowing, running),
    // so it is a reading like the anchor itself. The root copy stays the last fallback (a frozen pose: anchorHeld)
    bool anchorHeld = false;
    // D-219d (owner: "you fixed the boat but now walking in F1 feels terrible - jittery"): a rebuilt anchor (D-219 / 219c:
    // from the animation or on the chest, read at another moment of the frame than the published one) alternated with
    // published frames while walking, and since D-219 the movement lean read both - the view jumped between them every
    // frame. Rebuilt frames serve the body-locked / boat eye (their purpose) but, like held ones (D-209), are no reading
    // for the walking lean or the eye calibration: walking is as before D-219
    bool anchorRebuilt = false;
    {
        static Mat4 goodRel;
        static bool haveGoodRel = false;
        static uint64_t held = 0;
        Mat4 rt{};
        const bool haveRoot = ok && (StableJoint(0xfe57caacf699afe4ull, &rt) || JointWorld(0, &rt)) && Unscaled(rt);
        // D-219c (owner: "the height is OK but I'm too far forward in the boat"; owner log, D-219b build: rebuilt from the
        // animation 4910 times, "animated anchor shrunk" 59890 - the head's locals are shrunk too while it is hidden, so the
        // root copy still ran most frames: a standing head on the seated root, 0.26 .. 0.31 m forward): the anchor is also
        // kept relative to its first unshrunk ancestor as drawn (the chest - never hidden), learned on every good frame.
        // Head-to-chest hardly changes between standing, sitting and rowing (head-to-root does), so while collapsed the
        // anchor is put on the current chest - a reading of this frame's upper body
        int chest = anchor;
        Mat4 cw{};
        bool haveChest = false;
        for (int k = 0; ok && k < 12 && chest >= 0 && !haveChest; ++k) {
            chest = JointParent(chest);
            haveChest = chest >= 0 && JointWorld(chest, &cw) && Unscaled(cw);
        }
        // one learned offset per ancestor (the first unshrunk one is the neck base 323 at times, mostly the chest 296)
        struct ChestRel { int idx = -1; Mat4 rel{}; };
        static ChestRel chestRels[4];
        ChestRel* cr = nullptr;
        for (ChestRel& c : chestRels) if (haveChest && c.idx == chest) cr = &c;
        if (ok && haveChest && Unscaled(h)) {
            const float dx = h.m[3][0] - cw.m[3][0], dy = h.m[3][1] - cw.m[3][1], dz = h.m[3][2] - cw.m[3][2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > 0.03f && d < 0.8f) {  // a torn cache read is never the head above the chest
                if (!cr) {
                    for (ChestRel& c : chestRels) if (c.idx < 0) { cr = &c; break; }
                    if (!cr) cr = &chestRels[0];
                    cr->idx = chest;
                    Log("fp: head anchor learned on joint %d (%.2f m from it) (D-219c)", chest, d);
                }
                cr->rel = Mul(h, RigidInverse(cw));
            }
        }
        bool rebuilt = false;
        if (ok && !Unscaled(h)) {
            // D-219b (owner log, D-219 build: rebuilt 3 times, the root copy 3000+ - the neck base 323 is shrunk by the hide
            // too, D-120 folds the head between 323 and the chest 296): the reference is the ROOT (never hidden; the
            // drawn one is already read above): this frame's animated anchor relative to this frame's animated root, put
            // on the drawn root. Failures are counted by reason
            static uint64_t fNoRoot = 0, fNoAnim = 0, fShrunk = 0, fLen = 0, rb = 0, onChest = 0;
            const int rootIdx = FindJointByHash(0xfe57caacf699afe4ull);  // (cached per skeleton)
            Mat4 ha{}, ra{};
            float up = 0.f;
            if (!haveRoot || rootIdx < 0) ++fNoRoot;
            else if (!AnimatedJoint(anchor, &ha) || !AnimatedJoint(rootIdx, &ra)) ++fNoAnim;
            else if (!Unscaled(ha) || !Unscaled(ra)) ++fShrunk;
            else {
                const Mat4 rel = Mul(ha, RigidInverse(ra));
                const float len = sqrtf(rel.m[3][0] * rel.m[3][0] + rel.m[3][1] * rel.m[3][1] + rel.m[3][2] * rel.m[3][2]);
                up = rel.m[3][1];
                if (len > 0.3f && len < 2.4f) {  // a torn read (animation jobs running) is never a head
                    h = Mul(rel, rt);
                    rebuilt = true;
                    ++rb;
                } else {
                    ++fLen;
                }
            }
            // D-219c: the animation could not give it - the learned head-on-chest on the current chest
            if (!rebuilt && cr) {
                h = Mul(cr->rel, cw);
                rebuilt = true;
                ++onChest;
            }
            const uint64_t tries = rb + fNoRoot + fNoAnim + fShrunk + fLen;
            if (tries <= 3 || tries % 1800 == 0)
                Log("fp: head anchor collapsed - rebuilt from this frame's animation on the root %llu times (last %.2f m up); "
                    "failed: no root %llu, no animated joint %llu, animated anchor shrunk %llu, implausible %llu; put on the "
                    "chest %llu (D-219)",
                    (unsigned long long)rb, up, (unsigned long long)fNoRoot, (unsigned long long)fNoAnim,
                    (unsigned long long)fShrunk, (unsigned long long)fLen, (unsigned long long)onChest);
        }
        if (ok && rebuilt) {
            anchorRebuilt = true;  // a reading of this frame's pose for the body-locked eye: no hold, no root copy from it
        } else if (ok && Unscaled(h)) {
            if (haveRoot) { goodRel = Mul(h, RigidInverse(rt)); haveGoodRel = true; }
        } else if (ok) {
            if (haveRoot && haveGoodRel) {
                h = Mul(goodRel, rt);
                anchorHeld = true;
                if (++held <= 5 || held % 600 == 0)
                    Log("fp: the head anchor came collapsed - the last good one on the current root used (%llu times, D-206)",
                        (unsigned long long)held);
            } else {
                ok = false;  // (the hold-last-base path below)
            }
        }
    }
    // A single failed lookup must not drop the frame to third person (owner report: F1 flashing between first and
    // third person; both eyes of the same frames). Hold the last good base for up to ~0.5 s, count the reasons.
    static Mat4 lastBase;
    static int lastGoodAge = 1 << 20;
    static uint64_t failNoHead = 0, failWorld = 0, held = 0;
    if (want && !ok) {
        if (head < 0) ++failNoHead; else ++failWorld;
        if (lastGoodAge < 45 && mode == g_lastMode) {
            ++lastGoodAge;
            ++held;
            *base = lastBase;
            if (held <= 5 || held % 600 == 0)
                Log("fp: head lookup failed (noHead=%llu world=%llu) - holding last first-person base (%llu held)",
                    (unsigned long long)failNoHead, (unsigned long long)failWorld, (unsigned long long)held);
            return true;
        }
    }
    static uint8_t* lastHero = nullptr;
    uint8_t* hero = HeroObject();
    if (mode != g_lastMode || (ok != g_headHidden) || hero != lastHero) {  // new hero (level/save load): re-hide
        lastHero = hero;
        SetHeadVisible(!ok);
        if (mode != g_lastMode) calibMode = -1;
        if (mode != g_lastMode)
            Log("fp: mode %s (cinematic=%d head joint=%d)", mode == 0 ? "third person" : mode == 1 ? "first person (gameplay)"
                : "first person (cutscene)", cine, head);
        g_lastMode = mode;
    }
    if (mode != 1) SetBodyEyeShift(0.f, 0.f);  // D-114: the body follows the view only in gameplay first person
    if (!ok) return false;
    // eye point in the anchor's frame (LIVE fit on Kratos: eye = head + 0.09 fwd - 0.07 up = (0.002, 0.105, -0.089)
    // in joint 411's frame); fpeye adjusts forward (-Z) / up (Y) on top
    float eye[3];
    for (int j = 0; j < 3; ++j)
        eye[j] = h.m[3][j] + h.m[0][j] * 0.0018f + h.m[1][j] * (0.1053f + g_eyeUp) + h.m[2][j] * (-0.0885f - g_eyeFwd);
    if (mode == 1) {
        // VRIK / Cyberpunk-port model: the camera is the headset, the body follows it. The base eye point is
        // Kratos' eye measured relative to the game object when F1 turns on (averaged over 30 frames), not per frame:
        // steady (no animation bob), never fed by our own arm IK. NOT re-measured on a recenter (owner: height wrong
        // after a SteamVR recenter; LIVE log: the eye height re-measured at each recenter varied 1.61..1.77 m with the
        // animation). The rendered body turns about this eye point (body.cpp), so Kratos' head stays at the view
        // whichever way he faces (owner: "f1 mode makes me be on the right shoulder").
        // D-114 (owner: "with the axe in hand F1 starts good, with just fists it puts the body too much forward"): the eye
        // was measured over the first 30 frames after F1 turned on and then kept - so it depended on the stance of that
        // moment (owner logs: 0.04 .. 0.45 m in front of the object; the fists stance leans ~0.17 m further forward than
        // the relaxed axe stance, walking / turning more). Now:
        //  - the VIEW sits at a fixed eye point on the game object (fp_eye_right / _up / _back: Kratos' eye in the relaxed
        //    stance, the median of the owner's axe-in-hand starts) - the same whatever F1 was started in;
        //  - the drawn BODY follows: its eye, measured live in the object frame (filtered ~0.35 s, only while standing /
        //    walking normally - D-110), is the pivot it turns about, and it is moved so that eye stays under the view
        //    (horizontally) - in the fists stance, walking, after a weapon switch.
        static const float camOff[3] = {config::GetFloat("fp_eye_right", 0.05f), config::GetFloat("fp_eye_up", 1.68f),
                                        config::GetFloat("fp_eye_back", -0.20f)};
        static uint8_t* liveHero = nullptr;
        static bool liveHave = false;
        static float live[3] = {0.f, 0.f, 0.f};  // the drawn eye in the object frame (right, up, back), filtered
        static LARGE_INTEGER qf{}, qLast{};
        if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        LARGE_INTEGER qNow;
        QueryPerformanceCounter(&qNow);
        float dt = qLast.QuadPart ? (float)(qNow.QuadPart - qLast.QuadPart) / (float)qf.QuadPart : 0.f;
        if (dt <= 0.f || dt > 0.1f) dt = 1.f / 90.f;
        qLast = qNow;
        // D-125 (owner: "when I start moving make it move the camera or something - I can't see the body hunched over
        // when I run forward"): the view stayed at the standing eye point while the run animation bent Kratos forward
        // and down; the body was dragged back under it. Moving, the view follows the body's lean since it stood
        // (drawn eye now - drawn eye standing) and the body keeps its standing shift: weight from the game object's
        // speed, x gowvr.ini fp_move_follow (0 = off); held during attacks (D-065 / D-066: attacks never move the view);
        // 0 in the boat.
        // D-127 (owner after D-125: "in F1 when I start running Kratos' body gets hunched over forward and I can see the
        // back of Kratos' body"): the lean was read from `live`, which only updates while the drawn eye is 1.45 .. 2.1 m
        // above the root (the D-110 calibration gate) - the fists stance already stands at 1.59 m (owner log), the run
        // hunch takes the eye below 1.45: `live` froze at an upright value and the view stayed above the hunched head,
        // looking down onto the back. The lean now has its own per-frame reading `cur` (0.9 .. 2.2 m; frozen in attacks,
        // sync moves, interactions, the boat), filtered 0.06 s; the weight starts at 0.5 m/s (full at 2 m/s, eased
        // 0.15 s) and the view height follows in 0.15 s - the camera reaches the hunch while it forms.
        static const float follow = config::GetFloat("fp_move_follow", 1.f);
        static float moveW = 0.f, speedF = 0.f, viewY = 0.f, lastGp[2] = {0.f, 0.f};
        static float stand[3] = {0.f, 0.f, 0.f};  // the drawn eye while standing (object frame): the lean is cur - stand
        static float cur[3] = {0.f, 0.f, 0.f};    // D-127: the drawn eye this frame (lightly filtered, object frame)
        static bool haveLastGp = false, curHave = false;
        float view[3] = {camOff[0], camOff[1], camOff[2]};
        const float* off = view;  // eye relative to the game object, object frame (right, up, back)
        float gp[3], gy, rp[3], ry;
        if (GameRoot(gp, &gy) && RenderRoot(rp, &ry)) {
            if (calibMode != 1 || hero != liveHero) {  // F1 (re)started / new hero: no body shift until measured
                liveHave = false;
                curHave = false;
                liveHero = hero;
                moveW = 0.f;
                speedF = 0.f;
                haveLastGp = false;
                viewY = camOff[1];
                Log("fp: view at the fixed eye point (right %.3f up %.3f back %.3f); the body follows it", camOff[0], camOff[1],
                    camOff[2]);
            }
            // rendered eye relative to the rendered object in its frame (independent of the shift below)
            const float sr = sinf(ry), cr = cosf(ry);
            const float edx = eye[0] - rp[0], edy = eye[1] - rp[1], edz = eye[2] - rp[2];
            const float m[3] = {edx * cr - edz * sr, edy, edx * sr + edz * cr};  // right = (cos, 0, -sin), back = (sin, 0, cos)
            // D-110 (code audit M11): only frames where Kratos stands / walks normally count (no attack, sync move, boat,
            // climbing / interaction, eye 1.45 .. 2.1 m above the root)
            const float calibDy = edy;
            const bool plain = !HeroInSyncMove() && !InBoat() && !ContextBodyLocked() && !ContextHandAuthored(0) &&
                               !ContextHandAuthored(1);
            // D-209 (owner: "in F1 fighting, all of a sudden the camera got really low - when I move it goes very low, when
            // I stop it's okay"; owner log: the lean reading frozen at (-0.038 0.932 -0.811) - 0.93 m above the root against
            // 1.79 standing - for 30+ s while D-206 held the anchor 1800 times): while the anchor is collapsed D-206 puts
            // the last good one (kept from a crouched / attacking pose) on the root; that frozen pose passed the 0.9 m gate
            // and the movement lean pulled the view ~0.9 m down. Held frames are no reading: neither the lean nor the
            // calibration take them, and the lean eases back to none (0.15 s) while they last
            static uint64_t heldSkips = 0;
            if (anchorHeld && (++heldSkips <= 3 || heldSkips % 1800 == 0))
                Log("fp: head anchor held (D-206) - F1 lean / eye calibration not read from it (%llu frames, D-209)",
                    (unsigned long long)heldSkips);
            const bool noReading = anchorHeld || anchorRebuilt;  // D-209 / D-219d
            const bool calibPose = !noReading && !CombatBusy() && plain && calibDy > 1.45f && calibDy < 2.1f;
            if (calibPose) {
                const float k = liveHave ? 1.f - expf(-dt / 0.35f) : 1.f;
                for (int j = 0; j < 3; ++j) live[j] += (m[j] - live[j]) * k;
                if (!liveHave) {
                    Log("fp: drawn eye on the body (right %.3f up %.3f back %.3f)", live[0], live[1], live[2]);
                    for (int j = 0; j < 3; ++j) stand[j] = live[j];
                }
                liveHave = true;
                if (moveW < 0.05f) for (int j = 0; j < 3; ++j) stand[j] = live[j];  // standing: the reference
            }
            // D-127: the lean reading - every plain frame, any height a run / sprint hunch reaches
            if (!noReading && !CombatBusy() && plain && calibDy > 0.9f && calibDy < 2.2f) {
                const float k = curHave ? 1.f - expf(-dt / 0.06f) : 1.f;
                for (int j = 0; j < 3; ++j) cur[j] += (m[j] - cur[j]) * k;
                curHave = true;
            } else if (noReading && curHave && liveHave) {  // D-209: no reading - back to no lean
                const float k = 1.f - expf(-dt / 0.15f);
                for (int j = 0; j < 3; ++j) cur[j] += (stand[j] - cur[j]) * k;
            }
            if (!curHave && liveHave) for (int j = 0; j < 3; ++j) cur[j] = live[j];
            // movement weight from the game object's horizontal speed (before any attack hold replaces gp)
            {
                float sp = 0.f;
                if (haveLastGp) {
                    const float vx = gp[0] - lastGp[0], vz = gp[2] - lastGp[1];
                    sp = sqrtf(vx * vx + vz * vz) / dt;
                    if (sp > 15.f) sp = 0.f;  // a teleport / respawn, not movement
                }
                lastGp[0] = gp[0];
                lastGp[1] = gp[2];
                haveLastGp = true;
                speedF += (sp - speedF) * (1.f - expf(-dt / 0.15f));
                // D-162: climbing / chains / levers / doors ... (body locked to the game's animation, a sync move -
                // CombatBusy): the running lean was frozen at its value from before (walking into a climb kept the view
                // ahead of the eye point all the way up - owner: "the head is a bit too far forward while climbing";
                // F1 off / on reset it). In those contexts the lean eases out
                const bool locked = ContextBodyLocked();
                if (!CombatBusy() || locked) {  // held through attacks: an attack never moves the view
                    const float x = fminf(fmaxf((speedF - 0.5f) / 1.5f, 0.f), 1.f);
                    const float target = InBoat() || locked || !liveHave || !curHave
                                             ? 0.f
                                             : x * x * (3.f - 2.f * x) * fminf(fmaxf(follow, 0.f), 1.f);
                    moveW += (target - moveW) * (1.f - expf(-dt / 0.15f));
                }
                // D-126 diagnostic: while moving, the lean numbers once a second; once per F1 session after 1 s of
                // running, both eyes saved (shots/f1_run_eye0 / _eye1.png) from a worker thread (EyesShot waits for
                // the render thread)
                static ULONGLONG movingSince = 0, lastLeanLog = 0;
                static uint8_t* shotFor = nullptr;
                const ULONGLONG nowMs = GetTickCount64();
                if (moveW > 0.9f && liveHave) {
                    if (!movingSince) movingSince = nowMs;
                    if (nowMs - lastLeanLog > 1000) {
                        lastLeanLog = nowMs;
                        Log("fp: moving %.1f m/s - drawn eye now (right %.3f up %.3f back %.3f, %.2f m above the root), "
                            "standing (%.3f %.3f %.3f), view height %.3f", speedF, cur[0], cur[1], cur[2], calibDy, stand[0],
                            stand[1], stand[2], viewY);
                    }
                    if (nowMs - movingSince > 1000 && shotFor != hero) {
                        shotFor = hero;
                        Log("fp: running - saving both eyes as shots/f1_run (one per F1 session)");
                        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { stereo::EyesShot("f1_run", 2); return 0; },
                                                nullptr, 0, nullptr);
                        if (t) CloseHandle(t);
                    }
                } else {
                    movingSince = 0;
                }
                if (calibMode != 1) shotFor = nullptr;  // a new F1 session may save again
                static float loggedW = 0.f;
                if (fabsf(moveW - loggedW) > 0.5f) {
                    loggedW = moveW > 0.5f ? 1.f : 0.f;
                    Log("fp: view %s the body's lean while moving (speed %.1f m/s)", moveW > 0.5f ? "follows" : "back from",
                        speedF);
                }
            }
            // D-163 (owner after D-162: "when I enter the climbing animation it moves my Kratos body ~0.10 m forward - that's
            // why my hands are bad, it's the whole body"): the drawn body is shifted so that ITS eye sits under the view
            // (D-114) - but by the eye measured standing (`live`), never by the pose that plays. A climb holds the head
            // elsewhere relative to the root than standing does, so the climbing body stood off the view by that
            // difference (and the shoulders off the hand targets, which hang off the view). In body-locked contexts
            // (climbing, chains, levers, doors ...; not the boat) the body is placed by its CURRENT eye (this frame's
            // drawn eye, filtered 0.1 s), blended in / out over 0.15 s: the climbing head stays under the view, the arms
            // on the hands. gowvr.ini fp_locked_eye 0 = the standing eye as before
            // D-164 (owner: "in an animation I'm a bit too far back, ~0.1 m - because of the climbing fix?"; owner log: the
            // travel table, "body placed by its current eye (back -0.398; standing -0.120)"): D-163 MOVED the body under the
            // view - the table / lever / wall Kratos works on stays where the game has it, so the body (and with it the
            // player) ended up ~0.2 m back from it. Now the body stays exactly where the game put it (aligned with the
            // thing it holds) and the VIEW goes to its current eye (horizontal 0.1 s, height 0.2 s) - the same head-to-body
            // arrangement D-163 gave the climb, without moving Kratos off his prop
            static const bool lockedEyeOn = config::GetFloat("fp_locked_eye", 1.f) > 0.5f;
            static float lockEye[3] = {0.f, 0.f, 0.f}, lockW = 0.f;
            static bool lockHave = false;
            {
                // D-181 (owner: "in the boat the camera is higher and more forward than it should be - it worked before
                // today"; owner log: F1 turned on while already seated, navBoatIdleR): the boat was left out of this rule
                // and relied on the ANIMATE neck offset, which is relative to the move's START - started seated it is 0,
                // so the view sat at the STANDING eye point (1.68 m up, 0.2 m ahead) above the seated Kratos. It worked
                // only when F1 was on before boarding (the boarding move carried the head down). The boat now takes this
                // rule too - the view at the seated body's current eye - with slower smoothing (0.4 s / 0.5 s height) so
                // rowing does not bob the view
                const bool boat = InBoat();
                const bool lockedNow = lockedEyeOn && (ContextBodyLocked() || boat) && calibDy > 0.5f && calibDy < 2.5f &&
                                       (lockHave || !anchorHeld);
                if (lockedNow && !anchorHeld) {  // D-209: a held (frozen) anchor keeps the last real reading
                    const float k = lockHave ? 1.f - expf(-dt / (boat ? 0.40f : 0.10f)) : 1.f;
                    const float ky = lockHave ? 1.f - expf(-dt / (boat ? 0.50f : 0.20f)) : 1.f;
                    lockEye[0] += (m[0] - lockEye[0]) * k;
                    lockEye[1] += (m[1] - lockEye[1]) * ky;
                    lockEye[2] += (m[2] - lockEye[2]) * k;
                    lockHave = true;
                }
                lockW += ((lockedNow ? 1.f : 0.f) - lockW) * (1.f - expf(-dt / 0.15f));
                if (!lockedNow && lockW < 0.01f) { lockW = 0.f; lockHave = false; }
                static bool loggedLock = false;
                if (lockedNow != loggedLock) {
                    loggedLock = lockedNow;
                    if (lockedNow)
                        Log("fp: body-locked context - the view at the body's current eye (right %.3f up %.3f back %.3f; standing "
                            "%.3f %.3f), the body where the game puts it", lockEye[0], lockEye[1], lockEye[2], live[0], live[2]);
                }
            }
            if (liveHave) {
                // the view: the standing eye point plus the body's lean since it stood (cur - stand), by the movement
                // weight (horizontal at once, height in 0.15 s) - standing still it is exactly the fixed point (D-114)
                view[0] = camOff[0] + (cur[0] - stand[0]) * moveW;
                view[2] = camOff[2] + (cur[2] - stand[2]) * moveW;
                const float yT = camOff[1] + (cur[1] - stand[1]) * moveW;
                viewY += (yT - viewY) * (1.f - expf(-dt / 0.15f));
                view[1] = viewY;
                // the drawn eye the body is placed by: the slow standing reading at rest (no idle-bob shuffle, D-114),
                // the current one while moving; horizontal shift that puts it under the view, at most 0.5 m
                const float eR = live[0] + (cur[0] - live[0]) * moveW, eB = live[2] + (cur[2] - live[2]) * moveW;
                float sR = view[0] - eR, sB = view[2] - eB;
                if (lockHave) {  // D-164: the view to the body's current eye, the body unshifted
                    view[0] += (lockEye[0] - view[0]) * lockW;
                    view[1] += (lockEye[1] - view[1]) * lockW;
                    view[2] += (lockEye[2] - view[2]) * lockW;
                    sR *= 1.f - lockW;
                    sB *= 1.f - lockW;
                }
                const float sl = sqrtf(sR * sR + sB * sB);
                if (sl > 0.5f) { sR *= 0.5f / sl; sB *= 0.5f / sl; }
                SetBodyPivot(eR, eB);
                SetBodyEyeShift(sR, sB);
            } else if (lockHave) {  // D-164: no standing reading yet (F1 started mid-climb) - the view at the current eye
                viewY = camOff[1];
                for (int j = 0; j < 3; ++j) view[j] = camOff[j] + (lockEye[j] - camOff[j]) * lockW;
                SetBodyPivot(camOff[0], camOff[2]);
                SetBodyEyeShift(0.f, 0.f);
            } else {
                viewY = camOff[1];
                SetBodyPivot(camOff[0], camOff[2]);
                SetBodyEyeShift(0.f, 0.f);
            }
            // D-066 (owner: "every attack moves my head a little and it comes back"): the eye came from the GAME root and
            // facing, which a held attack still moves / turns for a frame; while an attack holds them the view uses the
            // held spot, height and facing
            {
                float hx, hz, hy, hyaw;
                if (CombatHeldRoot(&hx, &hz, &hy)) {
                    gp[0] = hx;
                    gp[2] = hz;
                    if (!isnan(hy)) gp[1] = hy;
                    if (CombatHeldYaw(&hyaw)) gy = hyaw;
                } else if (CombatReleaseOffset(&hx, &hz)) {  // D-077: eased onto the game's position after the hold
                    gp[0] += hx;
                    gp[2] += hz;
                }
                float vh;  // D-084: walking during a held attack - the view's height without the attack's own bob
                if (CombatViewHeight(gp[1], &vh)) gp[1] = vh;
            }
            const float sg = sinf(gy), cg = cosf(gy);
            float co[3];
            CombatCameraOffset(co);  // leaps under HOLD (vertical) / ANIMATE moves (filtered head translation)
            eye[0] = gp[0] + cg * off[0] + sg * off[2] + co[0];
            eye[1] = gp[1] + off[1] + co[1];
            eye[2] = gp[2] - sg * off[0] + cg * off[2] + co[2];
        }
        calibMode = 1;
        *base = YawFrame(gameCam.m[2][0], gameCam.m[2][2], eye);  // view yaw (detached, see mutate.cpp CamSync)
    } else {
        calibMode = -1;
        SetBodyEyeShift(0.f, 0.f);  // D-114: the body follows the view only in gameplay first person
        Mat4 root{};
        if (!StableJoint(0xfe57caacf699afe4ull, &root) && !JointWorld(0, &root)) root = h;
        // D-126 (owner: "in F3 I want it to know which way Kratos' head is facing - what he is seeing"): the cutscene
        // view turns with the HEAD (anchor 411's back axis, m[2]: the eye sits at -0.0885 along it = forward), not the
        // character root. Smoothed 0.08 s, snapped on a cut (> 60 deg); the root's facing while the head looks nearly
        // straight up / down (horizontal part < 0.3). Pitch: gowvr.ini fp_f3_pitch (0 = level, the default - forced
        // pitch in VR is the usual sickness trigger; 1 = the head's full up / down)
        static const float f3Pitch = config::GetFloat("fp_f3_pitch", 0.f);
        static float hb[2] = {0.f, 1.f}, hp = 0.f;
        float bx = h.m[2][0], by = h.m[2][1], bz = h.m[2][2];
        const float bl = sqrtf(bx * bx + by * by + bz * bz);
        if (bl > 1e-4f) { bx /= bl; by /= bl; bz /= bl; }
        // D-191 (owner, the Baldur fight's punch prompts - csbaldurroofslamuppercut03, F3 since D-184: "it's backwards, I
        // see behind me; in the animation Kratos' head looks downward"): the horizontal part of the head's back axis
        // turns round once the head tips past straight down / up (bent over a foe on the ground). The head's UP axis
        // tilts the way he faces as he looks down (and the other way as he looks up): its horizontal part, weighted by
        // how far the head tips (by), keeps the facing right up to ~128 deg of tilt. The pitch (fp_f3_pitch) still uses by
        float hx = bx, hz = bz;
        {
            float ux = h.m[1][0], uy = h.m[1][1], uz = h.m[1][2];
            const float ul = sqrtf(ux * ux + uy * uy + uz * uz);
            if (ul > 1e-4f) { ux /= ul; uz /= ul; }
            hx = bx - ux * by;
            hz = bz - uz * by;
        }
        const float hl = sqrtf(hx * hx + hz * hz);
        if (hl < 0.3f || bl <= 1e-4f) { hx = root.m[2][0]; hz = root.m[2][2]; by = 0.f; }
        bx = hx;
        bz = hz;
        {
            const float n = sqrtf(bx * bx + bz * bz);
            if (n > 1e-4f) { bx /= n; bz /= n; }
        }
        static LARGE_INTEGER qf3{}, ql3{};
        if (!qf3.QuadPart) QueryPerformanceFrequency(&qf3);
        LARGE_INTEGER qn3;
        QueryPerformanceCounter(&qn3);
        float dt3 = ql3.QuadPart ? (float)(qn3.QuadPart - ql3.QuadPart) / (float)qf3.QuadPart : 1.f;
        const bool entered = dt3 <= 0.f || dt3 > 0.25f;  // first F3 frame after a gap (F3 / a cutscene started)
        if (entered) dt3 = 1.f / 90.f;
        ql3 = qn3;
        const bool fresh = entered || hb[0] * bx + hb[1] * bz < 0.5f;  // entered / a cut (> 60 deg)
        const float k3 = fresh ? 1.f : 1.f - expf(-dt3 / 0.08f);
        hb[0] += (bx - hb[0]) * k3;
        hb[1] += (bz - hb[1]) * k3;
        // pitch of the head's forward (-back): up > 0
        const float pitch = asinf(fminf(fmaxf(-by, -1.f), 1.f)) * fminf(fmaxf(f3Pitch, 0.f), 1.f);
        hp += (pitch - hp) * k3;
        *base = YawFrame(hb[0], hb[1], eye);
        {   // D-184 diagnostic (owner: "in some F3 scenes where I actually walk with the stick the camera is ~0.5 m down, at
            // Kratos' legs"): the F3 eye under 1.2 m above Kratos' root joint - what played and where the head came from
            static ULONGLONG lastLow = 0;
            const float above = eye[1] - root.m[3][1];
            const ULONGLONG nowLow = GetTickCount64();
            if (above < 1.2f && nowLow - lastLow > 2000) {
                lastLow = nowLow;
                char prim[96];
                HeroPrimaryAnim(prim, sizeof(prim));
                Log("fp: F3 eye only %.2f m above Kratos' root (head anchor from the %s, anchor y %.2f, root y %.2f, cinematic "
                    "flag %d) - anim '%s' / '%s'", above, fromPublished ? "published pose" : "live cache", h.m[3][1],
                    root.m[3][1], InCinematic() ? 1 : 0, ActiveMoveAnim(), prim);
            }
        }
        if (hp != 0.f) {  // tilt about the base's right axis: forward' = forward cos + up sin (back' = back cos - up sin)
            const float c = cosf(hp), s = sinf(hp);
            float up[3], bk[3];
            for (int j = 0; j < 3; ++j) {
                up[j] = base->m[1][j] * c + base->m[2][j] * s;
                bk[j] = base->m[2][j] * c - base->m[1][j] * s;
            }
            for (int j = 0; j < 3; ++j) { base->m[1][j] = up[j]; base->m[2][j] = bk[j]; }
        }
    }
    // Owner: the view a little in front of the eyes (~0.25 m or a bit less) so it never overlaps Kratos' body. Along
    // the base's own forward (it looks down -Z), in F1 and F3; gowvr.ini fp_forward, command "fpfwd <m>".
    // D-104 (owner: "move the F1 / F3 camera a bit backward, like 0.25 m"): -0.05 (was 0.20)
    if (g_fwd < -99.f) g_fwd = config::GetFloat("fp_forward", 0.2f);
    for (int j = 0; j < 3; ++j) base->m[3][j] -= base->m[2][j] * g_fwd;
    // D-067 (owner: "move the default F1 head position a bit to the right"): along the base's right axis, F1 gameplay;
    // gowvr.ini fp_right, command "fpright <m>". Owner 2026-10-17: "try to make it 0 cm to the right" - 0 (was 0.04)
    if (g_right < -99.f) g_right = config::GetFloat("fp_right", 0.f);
    if (mode == 1)
        for (int j = 0; j < 3; ++j) base->m[3][j] += base->m[0][j] * g_right;
    // D-079 (owner: "make the default camera a bit more south"): lower, F1 gameplay; gowvr.ini fp_down, command "fpdown <m>"
    // D-080 (owner, after trying 5 cm lower: "make the default F1 head position way higher"): 10 cm ABOVE the eye
    if (g_down < -99.f) g_down = config::GetFloat("fp_down", -0.10f);
    if (mode == 1) base->m[3][1] -= g_down;
    lastBase = *base;
    lastGoodAge = 0;
    g_lastEye[0] = base->m[3][0]; g_lastEye[1] = base->m[3][1]; g_lastEye[2] = base->m[3][2];
    if (++g_frames <= 3 || g_frames % 1800 == 0)
        Log("fp: frame %llu eye(%.3f %.3f %.3f) head(%.3f %.3f %.3f) camBack(%.2f %.2f) rootBack(%.2f %.2f)",
            (unsigned long long)g_frames, eye[0], eye[1], eye[2], h.m[3][0], h.m[3][1], h.m[3][2], gameCam.m[2][0],
            gameCam.m[2][2], base->m[2][0], base->m[2][2]);
    return true;
}

void SetGameplayFP(bool on) { g_gameplay = on; Log("fp: gameplay first person %s", on ? "ON" : "off"); }
void SetCutsceneFP(bool on) { g_cutscene = on; Log("fp: cutscene first person %s", on ? "ON" : "off"); }
bool GameplayFP() { return g_gameplay; }
bool CutsceneFP() { return g_cutscene; }

void SetEyeOffset(float forward, float up) {
    g_eyeFwd = forward;
    g_eyeUp = up;
    Log("fp: eye offset forward=%.3f up=%.3f", forward, up);
}

int FpMode() { return g_lastMode; }

float ViewForward() { return g_fwd < -99.f ? config::GetFloat("fp_forward", 0.2f) : g_fwd; }

void SetViewForward(float metres) {
    g_fwd = metres;
    config::SetFloat("fp_forward", metres);
    Log("fp: view %.2f m in front of the eyes", metres);
}
void SetViewRight(float metres) {
    g_right = metres;
    config::SetFloat("fp_right", metres);
    Log("fp: view %.2f m to the right of the eyes", metres);
}
void SetViewDown(float metres) {
    g_down = metres;
    config::SetFloat("fp_down", metres);
    Log("fp: view %.2f m below the eyes", metres);
}

// Diagnostic (no headset needed): Kratos' eye point with the character's facing, whatever the camera mode.
bool DiagBodyBase(Mat4* base) {
    Mat4 h{}, root{};
    int anchor = FindJointByHash(kHeadParentHash);
    if (anchor < 0 || !(StableJoint(kHeadParentHash, &h) || (JointWorld(anchor, &h) && Unscaled(h)))) return false;
    float eye[3];
    for (int j = 0; j < 3; ++j)
        eye[j] = h.m[3][j] + h.m[0][j] * 0.0018f + h.m[1][j] * (0.1053f + g_eyeUp) + h.m[2][j] * (-0.0885f - g_eyeFwd);
    if (!StableJoint(0xfe57caacf699afe4ull, &root) && !JointWorld(0, &root)) root = h;
    *base = YawFrame(root.m[2][0], root.m[2][2], eye);
    return true;
}

void FpStatus() {
    Log("fp: status gameplay=%d cutscene=%d cinematic=%d mode=%d headHidden=%d head=%d", g_gameplay.load(),
        g_cutscene.load(), InCinematic(), g_lastMode, g_headHidden, FindJointByHash(kHeadHash));
}

}  // namespace gowvr::fp
