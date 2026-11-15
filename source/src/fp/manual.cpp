// D-307 manual climbing / D-308 manual rowing (fp/manual.h). Static RE and the design are in docs/DECISION_LOG.md.
#include "manual.h"
#include "arms.h"
#include "body.h"
#include "combat.h"
#include "firstperson.h"
#include "movelog.h"
#include "walls.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../input/vrmap.h"
#include "../stereo/mutate.h"
#include "../vr/xr.h"
#include <windows.h>
#include <Xinput.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gowvr::fp {
namespace {

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return sqrtf(Dot(a, a)); }
V3 Norm(V3 a) { const float l = Len(a); return l > 1e-6f ? a * (1.f / l) : V3{0.f, 0.f, 0.f}; }
V3 Pos(const Mat4& m) { return {m.m[3][0], m.m[3][1], m.m[3][2]}; }
V3 RowOf(const Mat4& m, int r) { return {m.m[r][0], m.m[r][1], m.m[r][2]}; }
V3 Flat(V3 a) { return {a.x, 0.f, a.z}; }
float Clamp1(float v) { return v < -1.f ? -1.f : v > 1.f ? 1.f : v; }
float Cfg(const char* k, float d) { return config::GetFloat(k, d); }
SHORT AxisS(float v) { return (SHORT)lrintf(Clamp1(v) * 32767.f); }

constexpr uint64_t kWristHash[2] = {0x6de47b0b82506234ull, 0x3626a7d4c72e93aeull};  // arms.cpp kArms: left / right wrist
constexpr uint64_t kShoulderHash[2] = {0x05c2b5ca793d4bf2ull, 0xe783988515993319ull};  // arms.cpp kArms: left / right shoulder
constexpr float kPalm = 0.07f;  // wrist joint -> palm along the hand's +Y (arms.cpp g_wristBack)
constexpr uint64_t kTraversalListVa = 0x1411be9b0ull;  // the per-creature traversal records (sentinel = this address)
constexpr uint64_t kHandleResolveVa = 0x140549ab0ull;  // the game's object-handle lookup (OnActiveTraversePath uses it)

// ---- climbing kinds from Kratos' animation names (r_heroa00: navRopeVertical*, navClimbWall*, navClimbWallFace*,
// navClimbOverhang*). Enter = the move that puts him on (played fast); Other = what plays by itself (exits to the top,
// corners, the HOD jump poses / gap jumps - owner: "keep it as it is")
enum class Climb { None, Rope, Wall, Ledge, Enter, Other };
const char* ClimbName(Climb c) {
    switch (c) {
        case Climb::Rope: return "rope / chain";
        case Climb::Wall: return "wall (up / down / sideways)";
        case Climb::Ledge: return "ledge (sideways)";
        case Climb::Enter: return "getting on";
        case Climb::Other: return "a climbing move of the game's own";
        default: return "none";
    }
}
Climb ClassifyAnim(const char* a) {
    if (!a || !a[0]) return Climb::None;
    char s[96];
    size_t i = 0;
    for (; a[i] && a[i] != '#' && i < sizeof(s) - 1; ++i) s[i] = (char)tolower((unsigned char)a[i]);
    s[i] = 0;
    auto starts = [&](const char* p) { return strncmp(s, p, strlen(p)) == 0; };
    auto has = [&](const char* p) { return strstr(s, p) != nullptr; };
    const bool enter = has("enter") && !has("hod") && !has("jump") && !has("traverse");
    if (starts("navropevertical")) {
        if (enter) return Climb::Enter;
        if (has("exit")) return Climb::Other;
        return Climb::Rope;  // idle, up / down (start, loop, stop), the slide
    }
    if (starts("navclimbwall") && has("face")) {
        if (enter) return Climb::Enter;
        if (has("exit") || has("trans")) return Climb::Other;
        return Climb::Ledge;  // navClimbWallFaceL/R (start, loop, stop), idles
    }
    if (starts("navclimbwall") || starts("navclimboverhang") || starts("navclimbenter")) {
        if (enter) return Climb::Enter;
        if (has("exit") || has("hod") || has("trans") || has("45") || has("90") || has("peekover") || has("traverse") ||
            has("jump"))
            return Climb::Other;
        return Climb::Wall;  // up / down / left / right / diagonals (start, loop, stop), idles, the overhang
    }
    if (starts("navclimb")) return Climb::Other;  // navClimbTrans*, navClimbTraverseJump*, navClimbHammerLeap
    return Climb::None;
}

// D-307h (owner: "while climbing, when going round weird angles - facing a north wall and needing to get to the east wall -
// Kratos' climbing animation played instead of my manual climbing"; "the starting positions came back mid-climb, e.g.
// when my body turned 45 degrees"; owner log: navClimbWallHODUpEnter / navClimbWallHODDownEnter between wall sections
// - dozens a climb, each handing the arms to the game and starting the climb again with the starting holds - and
// navClimbWall90LeftInside round an inside corner): the game's own moves INSIDE a climb - the section changes (HOD),
// the corner turns (90 / 45), navClimbTrans* - keep the climb by hand going; exits, jumps, traverse jumps and the
// peek-over stay the game's
bool ClimbTransfer(const char* a, bool* corner) {
    if (!a || !a[0]) return false;
    char s[96];
    size_t i = 0;
    for (; a[i] && a[i] != '#' && i < sizeof(s) - 1; ++i) s[i] = (char)tolower((unsigned char)a[i]);
    s[i] = 0;
    auto starts = [&](const char* p) { return strncmp(s, p, strlen(p)) == 0; };
    auto has = [&](const char* p) { return strstr(s, p) != nullptr; };
    if (!(starts("navclimbwall") || starts("navclimboverhang") || starts("navclimbtrans"))) return false;
    if (has("exit") || has("jump") || has("traverse") || has("peekover")) return false;
    const bool turn = has("90") || has("45");
    if (!(turn || has("hod") || has("trans"))) return false;
    if (corner) *corner = turn;
    return true;
}

// ---- the creature's traversal record (static RE, see manual.h) ---------------------------------------------------------
struct TravInfo {
    bool found = false, onPath = false, choice = false, byObject = false;
    char type[48] = "";
    V3 left{}, up{}, fwd{}, pos{};
};
using PFN_Resolve = void*(__fastcall*)(uint64_t* handle);
void ReadTraversalSeh(uint8_t* obj, TravInfo* t) {
    __try {
        uint8_t* cr = *(uint8_t**)(obj + 0x220);
        uint8_t* head = (uint8_t*)GameVa(kTraversalListVa);
        uint8_t* node = *(uint8_t**)head;
        for (int i = 0; i < 512 && node && node != head; ++i, node = *(uint8_t**)node) {
            uint8_t* who = *(uint8_t**)(node + 0x30);
            if (who != cr && who != obj) continue;
            t->found = true;
            t->byObject = who == obj;
            t->onPath = ((PFN_Resolve)GameVa(kHandleResolveVa))((uint64_t*)(node + 0x38)) != nullptr;
            void* cand = *(void**)(node + 0x40);
            const char* nm = *(const char**)(node + 0x48);
            if (cand && nm) {
                size_t k = 0;
                for (; k + 1 < sizeof(t->type) && nm[k] && (unsigned char)nm[k] >= 0x20 && (unsigned char)nm[k] < 0x7f; ++k)
                    t->type[k] = nm[k];
                t->type[k] = 0;
                const float* f = (const float*)(node + 0x50);
                t->left = {f[0], f[1], f[2]};
                t->up = {f[4], f[5], f[6]};
                t->fwd = {f[8], f[9], f[10]};
                t->pos = {f[12], f[13], f[14]};
                t->choice = t->pos.x == t->pos.x && fabsf(t->pos.x) + fabsf(t->pos.y) + fabsf(t->pos.z) > 1.f;
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t->found = false;
    }
}
bool ReadTraversal(TravInfo* t) {
    uint8_t* obj = HeroObject();
    if (!obj) return false;
    ReadTraversalSeh(obj, t);
    return t->found;
}

// the game's animated palm (the hold its own animation puts the hand on) - the animation's locals, not the drawn arm
bool AnimPalm(int h, V3* out) {
    static int idx[2] = {-1, -1};
    static uint8_t* skelSeen = nullptr;
    uint8_t* obj = HeroObject();
    uint8_t* sk = obj ? *(uint8_t**)(obj + 0x238) : nullptr;
    if (sk != skelSeen) { skelSeen = sk; idx[0] = idx[1] = -1; }
    if (idx[h] < 0) idx[h] = FindJointByHash(kWristHash[h]);
    Mat4 w{};
    if (idx[h] < 0 || !AnimatedJoint(idx[h], &w)) return false;
    *out = Pos(w) + Norm(RowOf(w, 1)) * kPalm;
    return out->x == out->x;
}
bool DrawnPalm(int h, V3* out) {  // the published (last completed pass) wrist: the animation while the arms are authored
    Mat4 w{};
    if (!StableJoint(kWristHash[h], &w)) return false;
    *out = Pos(w) + Norm(RowOf(w, 1)) * kPalm;
    return out->x == out->x;
}

// ---- shared state ---------------------------------------------------------------------------------------------------
std::atomic<bool> g_climbActive{false}, g_rowActive{false}, g_rowMode{false}, g_standCalm{false}, g_rowCapture{false};
std::atomic<float> g_poseW{0.f};
std::atomic<int> g_poseKind{0};
std::atomic<float> g_hook[2] = {0.f, 0.f}, g_fist[2] = {0.f, 0.f};
std::atomic<float> g_climbX{0.f}, g_climbY{0.f}, g_rowX{0.f}, g_rowY{0.f};
std::atomic<ULONGLONG> g_entryPulseUntil{0};
std::atomic<int> g_rowSide{1};  // +1 paddle on the right, -1 on the left (Y flips it)
std::atomic<bool> g_exact[2] = {false, false};    // D-308d: this hand's grip is built by the mod (arms: no per-headset turn)
// D-308d: Kratos' fist from his own hand bones (gowvr_logs/joints_kratos.txt, fingers curled as arms.cpp curls them -
// tools/re/_handgrip.py), in each WRIST joint's frame: the tunnel the fingers close round (pinky -> index side), the
// back of the hand, and the tunnel's centre (where a bar held in the fist runs)
const V3 kFistTunnel[2] = {{0.003f, 0.158f, -0.987f}, {0.000f, 0.076f, -0.997f}};
const V3 kFistBack[2] = {{1.000f, 0.000f, 0.004f}, {-1.000f, 0.000f, 0.000f}};
const V3 kFistCentre[2] = {{-0.032f, 0.096f, 0.016f}, {0.029f, 0.099f, 0.012f}};
// D-308d: a grip's place on the paddle's shaft line (its own Z axis; row_shaft_on_z 0 = the palm point itself)
V3 ShaftPoint(V3 g) {
    static const bool onZ = Cfg("row_shaft_on_z", 1.f) > 0.5f;
    return onZ ? V3{0.f, 0.f, g.z} : g;
}
// The wrist frame (rows X, Y, Z) that puts hand h's fist round a bar: its tunnel along `thumbward` (the bar's direction
// toward the thumb), the back of the hand toward `back` (made square to the bar)
void FistOnBar(int h, V3 thumbward, V3 back, V3 rows[3]) {
    const V3 a = Norm(kFistTunnel[h]);
    const V3 b = Norm(kFistBack[h] - a * Dot(kFistBack[h], a));
    const V3 d = Cross(a, b);
    const V3 A = Norm(thumbward);
    V3 B = back - A * Dot(back, A);
    if (Len(B) < 1e-3f) B = Cross(A, fabsf(A.y) < 0.9f ? V3{0.f, 1.f, 0.f} : V3{1.f, 0.f, 0.f});
    B = Norm(B);
    const V3 D = Cross(A, B);
    const V3 e[3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    for (int i = 0; i < 3; ++i) rows[i] = A * Dot(e[i], a) + B * Dot(e[i], b) + D * Dot(e[i], d);
}

// the paddle: grips learned from the game's own animation (paddle-root frame), the drawn hands for its pass
SRWLOCK g_padLock = SRWLOCK_INIT;
// D-308c: the grips start as the clean ones the owner's first boat learned (D-308 log: spread 0.004 m) - the paddle is
// always in the hands; a learn at the boat's idle only refines them (the D-308b learn ran while the game rowed: spread
// 0.37 m, rejected - and with no grips the paddle was left where the game put it, "stable in one position")
bool g_padLearned = true, g_faceLearned = false;
// D-308d: the top (T-grip) hand at +0.73 along the paddle's Z, the blade-side hand near its root - the blade is past the
// root end (owner: "the thick side of the paddle is on the left, but the motion I feel only on the right side")
V3 g_gripUp{-0.005f, 0.027f, 0.734f}, g_gripLow{-0.047f, 0.012f, -0.090f};  // top / blade-side palm, paddle root frame
V3 g_faceL{1.f, 0.f, 0.f};   // D-308b: the direction (across the shaft) the game keeps toward the boat's bow, paddle frame
float g_bladeTip = 0.9f;     // metres from the lower grip to the blade's end, along the shaft
// D-308b: the paddle fixed in the top fist (the axe's way): that hand, its controller palm (fallback for the IK fist),
// the shaft's direction toward the other hand (smoothed) and the face direction in the world
int g_drawTop = 0;
V3 g_drawTopP{}, g_drawDir{}, g_drawFace{};
bool g_drawValid = false;

// ---- climbing -------------------------------------------------------------------------------------------------------
struct ClimbState {
    Climb kind = Climb::None;
    bool engaged = false;
    bool pin[2] = {false, false}, pinPrev[2] = {false, false}, zone[2] = {false, false}, gripPrev[2] = {false, false};
    V3 pinP[2]{}, lastHand[2]{};
    bool haveLast[2] = {false, false};
    V3 lastBase{}, lastRoot{};
    bool haveBase = false, haveRoot = false;
    float req[2] = {0.f, 0.f};  // metres still to climb: up, right
    bool everGripped = false, descending = false, descendChecked = false;
    ULONGLONG noGripSince = 0;
    bool haveSurf = false;
    V3 surfP{};             // a point on the surface: Kratos' animated palms (his holds), smoothed
    V3 wallIn{0.f, 0.f, 1.f};  // horizontal, pointing into the wall
    bool haveChoiceIn = false;
    V3 choiceIn{};          // the game's climb choice's forward (made horizontal, into the wall) - kept from the entry
    char choiceType[48] = "";
    float pulled = 0.f, consumed = 0.f;
    ULONGLONG engagedAt = 0;
    // D-307b: the starting holds (both hands put on at head height when the climb starts; a grip press lets that hand go)
    bool autoHold[2] = {false, false}, needAuto = false, holdGripPrev[2] = {false, false}, waitRelease[2] = {false, false};
    bool fromEnter = false;    // D-307h: the game's getting-on move played since the last climb (the starting holds)
    bool inTransfer = false;   // D-307h: a game move inside the climb is playing
    bool noGrab = false;       // D-307h: round a corner - no new holds until it ends
    ULONGLONG stillSince = 0;  // the held hands have not pulled since
    ULONGLONG rootStillSince = 0;  // the game has not moved Kratos since
    // D-307b: how far the game coasts on after the stick is let go (its stop moves), measured per kind (rope / wall /
    // ledge, kept across climbs) - the stick lets go that much early
    float coast[3] = {0.f, 0.f, 0.f};
    bool releaseOpen = false;
    V3 releaseRoot{};
    float releaseDir[2] = {0.f, 0.f};
    float lastStick[2] = {0.f, 0.f};
    V3 bodyOff{};              // D-307b: the drawn body moved toward the head (world, smoothed)
    V3 push{};                 // D-307c: the view (and with it the body) pushed back from the rope / wall (world)
    V3 backV{};                // D-307d: the rope's fixed back-off inside `push` (outside the rope's offset cap)
    float ropeAhead = 0.f;     // D-307e: the rope's line in front of the head along Kratos' facing (logged)
    V3 bodyBack{};             // D-307f: the drawn body (not the view) further back on a rope (world, eased)
    float clearDist = 0.f;     // D-307c: the head's distance from the surface without the push (logged)
    float lastStickY = 0.f;    // D-307c: our last stick, for the "the game's own move" diagnostic
} g_c;
std::atomic<float> g_viewPush[3] = {0.f, 0.f, 0.f};
std::atomic<bool> g_viewPushOn{false};
std::atomic<float> g_padStick[2] = {0.f, 0.f};  // the player's own left stick (pad thread), for the diagnostic
std::atomic<float> g_bodyOff[3] = {0.f, 0.f, 0.f};
std::atomic<bool> g_bodyOffOn{false};

bool g_enterBoost = false;  // the getting-on time boost is wished (HeroTimeScaleWish owner 2)
// D-307g: where the chain / rope the player sees is, against the line through Kratos' animated palms - forward (along
// his climbing facing, + toward the chain's far side) and to his right - learned from the player's own grabs (gowvr.ini
// climb_rope_line_f / _r; the owner's estimate -0.08 = 8 cm toward his back to start with)
float g_lineF = 0.f, g_lineR = 0.f;
bool g_lineLoaded = false, g_lineDirty = false;
int g_lineGrabs = 0;
void RopeLineLoad() {
    if (g_lineLoaded) return;
    g_lineLoaded = true;
    // D-307j (owner, D-307i build: "ok good, but move the climbing path where Kratos can grab on the rope a bit more
    // backwards, like 2 cm"): -0.37 (the learned -0.35 + 0.02), and the learning off (it only ran into its limit)
    g_lineF = Cfg("climb_rope_line_f", -0.37f);
    g_lineR = Cfg("climb_rope_line_r", 0.f);
}
void RopeLineSave() {
    if (!g_lineDirty) return;
    g_lineDirty = false;
    config::SetFloats({{"climb_rope_line_f", g_lineF}, {"climb_rope_line_r", g_lineR}});
    Log("manual climb: the rope / chain grab line saved - %.2f m %s and %.2f m %s of Kratos' palms (%d grabs this climb)",
        fabsf(g_lineF), g_lineF < 0.f ? "toward his back" : "toward the chain's far side", fabsf(g_lineR),
        g_lineR < 0.f ? "to his left" : "to his right", g_lineGrabs);
    g_lineGrabs = 0;
}
// the hold of one climb (hands, pulls, letting go); the grip edges and the game's offered climb outlive it
void ClimbReset() {
    RopeLineSave();  // D-307g
    for (int h = 0; h < 2; ++h) {
        g_c.pin[h] = g_c.pinPrev[h] = g_c.zone[h] = g_c.haveLast[h] = false;
        g_hook[h] = 0.f;
        g_fist[h] = 0.f;
    }
    g_c.haveBase = g_c.haveRoot = g_c.haveSurf = g_c.engaged = false;
    g_c.req[0] = g_c.req[1] = 0.f;
    g_c.everGripped = g_c.descending = g_c.descendChecked = false;
    g_c.noGripSince = 0;
    g_c.pulled = g_c.consumed = 0.f;
    g_c.autoHold[0] = g_c.autoHold[1] = g_c.needAuto = false;
    g_c.waitRelease[0] = g_c.waitRelease[1] = false;
    g_c.stillSince = g_c.rootStillSince = 0;
    g_c.releaseOpen = false;
    g_c.lastStick[0] = g_c.lastStick[1] = 0.f;
    // (the body's offset toward the head is not reset here: it eases back to none after the climb, ClimbTick)
    g_climbActive = false;
    g_climbX = g_climbY = 0.f;
}

void ClimbTick(Mat4 hands[2], const bool valid[2], bool wallsSkip[2], const Mat4& base, const Mat4& head, float dt,
               ULONGLONG now, const char* prim, const char* moveAnim) {
    const bool opt = Cfg("manual_climb", 0.f) > 0.5f && MotionControls() && !InBoat();
    Climb kind = ClassifyAnim(prim);
    if (kind == Climb::None) kind = ClassifyAnim(moveAnim);
    if (kind == Climb::Enter && !g_c.engaged) g_c.fromEnter = true;  // D-307h (not a move inside the climb)
    // D-307h: a game move inside the climb by hand keeps it going (the surface kind of the climb it belongs to)
    {
        bool corner = false;
        const bool tr = opt && g_c.engaged && (kind == Climb::Other || kind == Climb::Enter) &&
                        (ClimbTransfer(prim, &corner) || ClimbTransfer(moveAnim, &corner));
        if (tr) {
            if (!g_c.inTransfer) {
                g_c.inTransfer = true;
                static int logs = 0;
                if (logs++ < 200)
                    Log("manual climb: the game's move '%s' inside the climb - the hands stay yours%s (D-307h)", prim,
                        corner ? ", the holds let go round the corner" : "");
                if (corner) {
                    for (int h = 0; h < 2; ++h) { g_c.pin[h] = g_c.autoHold[h] = false; g_c.waitRelease[h] = false; }
                    g_c.noGrab = true;
                    g_c.haveSurf = false;  // the next wall's surface taken anew
                }
            }
            kind = g_c.kind;
        } else if (g_c.inTransfer) {
            g_c.inTransfer = false;
            g_c.noGrab = false;
        }
    }
    if (kind != g_c.kind) {
        static int logs = 0;
        if (opt && logs++ < 200) Log("manual climb: %s ('%s')", ClimbName(kind), prim);
    }
    // getting on: the game's own entry move, played fast (owner: "it doesn't play the animation of Kratos getting onto
    // the wall or rope - it just instantly gets them on it"; the time boost arbiter D-203, owner 2)
    {
        const bool want = opt && kind == Climb::Enter;
        if (want != g_enterBoost) {
            const float sp = fminf(fmaxf(Cfg("climb_enter_speed", 4.f), 1.f), 10.f);
            if (HeroTimeScaleWish(2, want ? sp : 0.f)) g_enterBoost = want;
            if (want) Log("manual climb: getting on at %.1fx speed", sp);
        }
    }
    float gp[3], gy = 0.f;
    const bool haveRoot = GameRoot(gp, &gy);
    const V3 root{gp[0], gp[1], gp[2]};
    const V3 fwd{-sinf(gy), 0.f, -cosf(gy)}, right{cosf(gy), 0.f, -sinf(gy)};
    const V3 basePos = Pos(base);
    TravInfo t;
    const bool travOk = opt && ReadTraversal(&t);  // (nothing read with the option off)
    // the climb the game offers right now (its prompt): kept as the surface's orientation for the climb that follows
    if (travOk && t.choice && kind == Climb::None) {
        V3 in = Norm(Flat(t.fwd));
        if (Len(in) > 0.5f) {
            if (Dot(in, fwd) < 0.f) in = in * -1.f;
            g_c.choiceIn = in;
            g_c.haveChoiceIn = true;
        }
        if (strcmp(t.type, g_c.choiceType) != 0) {
            memcpy(g_c.choiceType, t.type, sizeof(t.type));
            static int logs = 0;
            if (logs++ < 120)
                Log("manual climb: the game offers '%s' at (%.2f %.2f %.2f), %.2f m from Kratos, forward (%.2f %.2f %.2f) up "
                    "(%.2f %.2f %.2f)%s", t.type, t.pos.x, t.pos.y, t.pos.z, Len(t.pos - root), t.fwd.x, t.fwd.y, t.fwd.z,
                    t.up.x, t.up.y, t.up.z, t.byObject ? " (record keyed by the object)" : "");
        }
    } else if (travOk && !t.choice && kind == Climb::None && g_c.choiceType[0]) {
        g_c.choiceType[0] = 0;
    }

    // ---- on the ground: a grip reaching for the climb the game offers -> its climb button (Evade: Cross / A)
    if (opt && kind == Climb::None && !CombatBusy()) {
        static ULONGLONG cool = 0;
        static const float reach = fmaxf(Cfg("climb_entry_reach_m", 1.2f), 0.3f);
        for (int h = 0; h < 2; ++h) {
            const bool grip = input::GripHeld(h);
            const bool edge = grip && !g_c.gripPrev[h];
            g_c.gripPrev[h] = grip;
            if (!edge || !valid[h] || now < cool) continue;
            const V3 hp = Pos(hands[h]);
            const float ahead = Dot(hp - basePos, fwd);
            if (ahead < 0.2f) continue;  // a grip at the body (shield, fists, the over-shoulder grab) is not a reach
            const float off = travOk && t.choice ? Len(Flat(hp - t.pos)) : 99.f;
            static int logs = 0;
            if (travOk && t.choice && off < reach) {
                g_entryPulseUntil = now + 150;
                cool = now + 1000;
                vr::XrHaptic(h, 0.45f, 0.05f);
                if (logs++ < 120)
                    Log("manual climb: %s hand gripped %.2f m from the offered '%s' -> the game's climb button", h ? "right" : "left",
                        off, t.type);
            } else if (logs++ < 120) {
                Log("manual climb: %s hand gripped ahead (%.2f m) - %s", h ? "right" : "left", ahead,
                    !travOk ? "no traversal record for Kratos" : !t.choice ? "the game offers no climb here"
                                                                            : "too far from the offered climb");
                if (travOk && t.choice) Log("manual climb:   (the offered '%s' is %.2f m from the hand, reach %.2f)", t.type, off, reach);
            }
        }
    } else {
        for (int h = 0; h < 2; ++h) g_c.gripPrev[h] = input::GripHeld(h);
    }

    const bool engage = opt && haveRoot && (kind == Climb::Rope || kind == Climb::Wall || kind == Climb::Ledge);
    if (!engage) {
        if (g_c.engaged) {
            // D-307c (owner log: navRopeVerticalExitDown 0.2 - 2.5 s after a starting hold was let go, our stick at 0):
            // what was sent when a move of the game's own took over
            Log("manual climb: hands back to the game (%s, '%s') - our stick up %.2f, the player's stick %.2f %.2f, the "
                "drawn body %.2f m from the game's (view pushed back %.2f m)", ClimbName(kind), prim, g_c.lastStickY,
                g_padStick[0].load(), g_padStick[1].load(), Len(g_c.bodyOff + g_c.push), Len(g_c.push));
            ClimbReset();
        }
        g_climbActive = false;
        g_climbX = g_climbY = 0.f;
        g_c.kind = kind;
        if (g_bodyOffOn.load() || g_viewPushOn.load()) {  // the body and the view ease back (0.1 / 0.2 s)
            g_c.bodyOff = g_c.bodyOff * expf(-dt / 0.1f);
            g_c.push = g_c.push * expf(-dt / 0.2f);
            g_c.backV = g_c.backV * expf(-dt / 0.2f);
            g_c.bodyBack = g_c.bodyBack * expf(-dt / 0.2f);
            if (Len(g_c.bodyOff) < 0.005f && Len(g_c.push) < 0.005f && Len(g_c.bodyBack) < 0.005f) {
                g_c.bodyOff = g_c.push = g_c.backV = g_c.bodyBack = V3{0.f, 0.f, 0.f};
                g_bodyOffOn = g_viewPushOn = false;
            }
            const V3 o = g_c.bodyOff + g_c.push + g_c.bodyBack;
            g_bodyOff[0] = o.x; g_bodyOff[1] = o.y; g_bodyOff[2] = o.z;
            g_viewPush[0] = g_c.push.x; g_viewPush[1] = g_c.push.y; g_viewPush[2] = g_c.push.z;
        }
        return;
    }
    if (!g_c.engaged) {
        g_c.engaged = true;
        g_c.engagedAt = now;
        g_c.haveSurf = false;
        // D-307h (owner: "the starting animation of both hands holding on should only appear when you get on the wall,
        // not during wall climbing"): only right after the game's getting-on move
        // D-307j (owner: "remove the starting positions for both climbing walls and ropes"): off (climb_start_holds 1 = on)
        g_c.needAuto = Cfg("climb_start_holds", 0.f) > 0.5f && g_c.fromEnter;
        g_c.fromEnter = false;
        for (int h = 0; h < 2; ++h) g_c.holdGripPrev[h] = input::GripHeld(h);
        Log("manual climb: ON - %s; %s, grip to hold, pull to climb", ClimbName(kind),
            g_c.needAuto ? "both hands start on it at head height (a grip press lets a hand go)"
                         : "no starting holds (not just got on) - a held grip takes hold");
    }
    g_c.kind = kind;
    g_climbActive = true;
    const V3 headP = Pos(head);

    // ---- the surface: Kratos' own animated holds (his palms) give where it is; the game's offered choice (or his
    // facing) which way it faces
    g_c.wallIn = g_c.haveChoiceIn && Dot(g_c.choiceIn, fwd) > 0.5f ? g_c.choiceIn : fwd;
    V3 pa[2];
    const bool pl = AnimPalm(0, &pa[0]), pr = AnimPalm(1, &pa[1]);
    if (pl || pr) {
        V3 c = pl && pr ? (pa[0] + pa[1]) * 0.5f : pl ? pa[0] : pa[1];
        // D-307f (owner: "move the 'grab here' line for manual climbing 8 cm backwards - right now it doesn't correspond
        // with the physical chain"): on a rope / chain the line through Kratos' animated palms is moved
        // climb_rope_line_back_m (0.08) toward his back (against his climbing facing, D-307e) - the holds, the grab zone,
        // the starting holds and the distance the view keeps all use the moved line
        // D-307g: the offset is now learned from the player's grabs (starting from the owner's 8 cm)
        if (kind == Climb::Rope) {
            RopeLineLoad();
            const V3 f = Norm(Flat(g_c.wallIn)), rg{-f.z, 0.f, f.x};
            c = c + f * g_lineF + rg * g_lineR;
        }
        // D-307c: on a rope the animated palms swing round it hand over hand - its line (x / z) smoothed 0.6 s, its
        // height 0.15 s; walls / ledges 0.15 s as before
        const float k = g_c.haveSurf ? 1.f - expf(-dt / 0.15f) : 1.f;
        const float kh = g_c.haveSurf ? 1.f - expf(-dt / (kind == Climb::Rope ? 0.6f : 0.15f)) : 1.f;
        g_c.surfP.x += (c.x - g_c.surfP.x) * kh;
        g_c.surfP.y += (c.y - g_c.surfP.y) * k;
        g_c.surfP.z += (c.z - g_c.surfP.z) * kh;
        g_c.haveSurf = true;
    }
    if (!g_c.haveSurf) { g_climbX = g_climbY = 0.f; return; }

    // where a point near `p` is held on this kind of surface
    auto onSurface = [&](V3 p) {
        if (kind == Climb::Rope) return V3{g_c.surfP.x, p.y, g_c.surfP.z};
        V3 q = p - g_c.wallIn * Dot(p - g_c.surfP, g_c.wallIn);
        if (kind == Climb::Ledge) q.y = g_c.surfP.y;
        return q;
    };
    // D-307b (owner: "make a starting posture when you start climbing - both hands holding at about the height of my head
    // (rope: one up, one a tiny bit below), then I can ungrip and go on; if I press grip on either, it lets go"): both
    // hands are put on the surface at head height when the climb starts; a grip PRESS on such a hand lets it go, after
    // which that hand holds only while its grip is held, as before
    if (g_c.needAuto) {
        g_c.needAuto = false;
        for (int h = 0; h < 2; ++h) {
            const float sideH = h ? 1.f : -1.f;
            V3 p = headP + right * (0.22f * sideH);
            if (kind == Climb::Rope) p = V3{headP.x, headP.y + (h ? 0.12f : -0.05f), headP.z};
            else p.y = headP.y + 0.05f;
            g_c.pinP[h] = onSurface(p);
            g_c.pin[h] = g_c.autoHold[h] = true;
            g_c.pinPrev[h] = false;
        }
        Log("manual climb: both hands start on the %s at head height (%.2f)", kind == Climb::Rope ? "rope" : kind == Climb::Ledge
            ? "ledge" : "wall", headP.y);
    }

    // ---- hands: held where they took hold on the surface, free otherwise
    static const float ropeGrab = Cfg("rope_grab_m", 0.25f), wallGrab = Cfg("wall_grab_m", 0.15f),
                       ledgeGrab = Cfg("ledge_grab_m", 0.15f), slip = fmaxf(Cfg("climb_slip_m", 0.45f), 0.15f),
                       span = Cfg("climb_span_m", 1.1f);
    V3 pull{0.f, 0.f, 0.f};
    int pulling = 0;
    const V3 dBase = g_c.haveBase ? basePos - g_c.lastBase : V3{0.f, 0.f, 0.f};
    for (int h = 0; h < 2; ++h) {
        const bool grip = input::GripHeld(h);
        const bool press = grip && !g_c.holdGripPrev[h];
        g_c.holdGripPrev[h] = grip;
        if (!valid[h]) { if (!g_c.autoHold[h]) g_c.pin[h] = false; g_c.haveLast[h] = false; continue; }
        const V3 hp = Pos(hands[h]);
        if (g_c.autoHold[h] && press) {  // the starting hold let go (that press grabs nothing - wait for the release)
            g_c.autoHold[h] = g_c.pin[h] = false;
            g_c.waitRelease[h] = true;
            vr::XrHaptic(h, 0.2f, 0.03f);
            Log("manual climb: %s hand lets go of its starting hold", h ? "right" : "left");
        }
        if (!grip) g_c.waitRelease[h] = false;
        // where this hand would hold, and whether it is on a grabbable part
        const V3 proj = onSurface(hp);
        bool zone = false;
        if (kind == Climb::Rope) {
            zone = Len(Flat(hp - g_c.surfP)) < ropeGrab && fabsf(hp.y - g_c.surfP.y) < span;
        } else {
            const float into = Dot(hp - g_c.surfP, g_c.wallIn);
            const V3 along = proj - g_c.surfP;
            zone = kind == Climb::Ledge ? into > -wallGrab && fabsf(hp.y - g_c.surfP.y) < ledgeGrab && Len(Flat(along)) < span
                                        : into > -wallGrab && Len(along) < span;
        }
        if (zone && !g_c.zone[h] && !g_c.pin[h]) vr::XrHaptic(h, 0.12f, 0.02f);  // a hold within reach
        g_c.zone[h] = zone;
        if (!g_c.autoHold[h]) {
            // a held grip takes hold as soon as the hand is on a grabbable part (pressed there, or moved there held)
            if (grip && !g_c.pin[h] && zone && !g_c.waitRelease[h] && !g_c.noGrab) {
                g_c.pin[h] = true;
                g_c.pinP[h] = proj;
                // D-307g (owner, D-307f build: "the rope climbing is still the same"; owner log: grabs 0.02 - 0.22 m from
                // the line before and after its 8 cm move - the player grabs the chain they see, the line is elsewhere):
                // on a rope / chain the hold is where the hand closed (no snap onto the line), and the line moves 30 %
                // of the way toward each grab, along Kratos' facing and to his side (at most 0.35 m back / 0.25 m
                // forward / 0.25 m sideways), saved at the climb's end
                if (kind == Climb::Rope) {
                    g_c.pinP[h] = hp;
                    const V3 f = Norm(Flat(g_c.wallIn)), rg{-f.z, 0.f, f.x};
                    const V3 e = Flat(hp - g_c.surfP);
                    const float ef = Dot(e, f), er = Dot(e, rg);
                    static const float learn = fminf(fmaxf(Cfg("climb_rope_line_learn", 0.f), 0.f), 1.f);  // D-307j: off
                    g_lineF = fminf(fmaxf(g_lineF + learn * ef, -0.5f), 0.25f);
                    g_lineR = fminf(fmaxf(g_lineR + learn * er, -0.25f), 0.25f);
                    if (learn > 0.f) g_lineDirty = true;
                    ++g_lineGrabs;
                    static int glogs = 0;
                    if (glogs++ < 200)
                        Log("manual climb:   the grab %.2f m %s / %.2f m %s of the grab line -> the line now %.2f m %s, %.2f m "
                            "%s of Kratos' palms (D-307g)", fabsf(ef), ef < 0.f ? "toward his back" : "toward the far side",
                            fabsf(er), er < 0.f ? "left" : "right", fabsf(g_lineF), g_lineF < 0.f ? "back" : "forward",
                            fabsf(g_lineR), g_lineR < 0.f ? "left" : "right");
                }
                g_c.everGripped = true;
                vr::XrHaptic(h, 0.55f, 0.05f);
                // D-307c (owner: "when you ungrip one hand and grip again anywhere with it, the other hand lets go by itself
                // - no pressing ungrip twice"): the other hand's starting hold is let go (a grip held on it now waits
                // for its release, so it does not grab again at once)
                const int o = 1 - h;
                if (g_c.autoHold[o]) {
                    g_c.autoHold[o] = g_c.pin[o] = false;
                    g_c.waitRelease[o] = input::GripHeld(o);
                    vr::XrHaptic(o, 0.2f, 0.03f);
                    Log("manual climb: %s hand's starting hold let go (the %s hand took hold)", o ? "right" : "left",
                        h ? "right" : "left");
                }
                static int logs = 0;
                if (logs++ < 200)
                    Log("manual climb: %s hand holds the %s (%.2f m from Kratos' hold line)", h ? "right" : "left",
                        kind == Climb::Rope ? "rope" : kind == Climb::Ledge ? "ledge" : "wall", Len(proj - hp));
            } else if (!grip && g_c.pin[h]) {
                g_c.pin[h] = false;
                vr::XrHaptic(h, 0.15f, 0.02f);
            }
        }
        if (g_c.pin[h]) {
            // D-307i (owner: "on top of the chain with the starting position, moving down with the joystick should move
            // the arms with me - now the arms and body stay floating upstairs even though I'm at the end"): the starting
            // holds, and any hold while the player's own stick moves Kratos, ride along with the view's base (the game's
            // movement) instead of staying where they were put
            {
                const bool physStick = fabsf(g_padStick[0].load()) > 0.35f || fabsf(g_padStick[1].load()) > 0.35f;
                if (g_c.autoHold[h] || physStick) g_c.pinP[h] = g_c.pinP[h] + dBase;
            }
            const V3 off = hp - g_c.pinP[h];
            const float d = Len(off);
            if (!g_c.autoHold[h] && d > slip) {  // the hand pulled far from its hold: the hold slides along
                g_c.pinP[h] = g_c.pinP[h] + off * (1.f - slip / d);
                if (kind == Climb::Rope) { g_c.pinP[h].x = hp.x - off.x; g_c.pinP[h].z = hp.z - off.z; }  // D-307g: along the rope
                if (kind == Climb::Ledge) g_c.pinP[h].y = g_c.surfP.y;
            }
            // only a hand the player is gripping pulls (the starting holds just hold)
            if (!g_c.autoHold[h] && g_c.pinPrev[h] && g_c.haveLast[h]) {
                pull = pull + ((hp - g_c.lastHand[h]) - dBase);
                ++pulling;
            }
            hands[h].m[3][0] = g_c.pinP[h].x;
            hands[h].m[3][1] = g_c.pinP[h].y;
            hands[h].m[3][2] = g_c.pinP[h].z;
            wallsSkip[h] = true;
        }
        g_c.lastHand[h] = hp;
        g_c.haveLast[h] = true;
        g_c.pinPrev[h] = g_c.pin[h];
        g_fist[h] = g_c.pin[h] && kind != Climb::Ledge ? (kind == Climb::Rope ? 1.f : 0.9f) : 0.f;
        g_hook[h] = g_c.pin[h] && kind == Climb::Ledge ? 1.f : 0.f;
    }
    g_c.lastBase = basePos;
    g_c.haveBase = true;
    // ---- pulls -> how far the body still has to move; the game's own movement uses it up
    // D-307b (owner: "staying / stopping in the middle of climbing makes my whole body and view go up and down nonstop";
    // owner log: pulled 2.3 - 2.6 m in 5 s, the game moved 3.0 - 5.8 m): the game's climb moves overshoot (its stop moves
    // carry on), and that overshoot used to become a request the OTHER way - climb back, overshoot, climb back ... Now only
    // pulls make a request; the game's movement only uses it up (never past zero), and a request left when the held hands
    // have been still for climb_settle_ms (250) is dropped
    static const float reqMax = fmaxf(Cfg("climb_request_max", 0.5f), 0.1f);
    static const ULONGLONG settleMs = (ULONGLONG)fmaxf(Cfg("climb_settle_ms", 250.f), 0.f);
    if (pulling > 0) {
        pull = pull * (1.f / (float)pulling);
        g_c.req[0] -= pull.y;
        g_c.req[1] -= Dot(pull, right);
        g_c.pulled += Len(pull);
        if (Len(pull) / fmaxf(dt, 1e-3f) > 0.08f) g_c.stillSince = 0;
        else if (!g_c.stillSince) g_c.stillSince = now;
    } else {
        g_c.stillSince = g_c.stillSince ? g_c.stillSince : now;
    }
    if (g_c.haveRoot) {
        const V3 rd = root - g_c.lastRoot;
        if (Len(rd) > 0.002f) g_c.rootStillSince = 0;
        else if (!g_c.rootStillSince) g_c.rootStillSince = now;
        if (Len(rd) < 0.5f) {  // (a jump of the root is a teleport / a new path, not climbing)
            const float use[2] = {rd.y, Dot(rd, right)};
            for (int k = 0; k < 2; ++k) {
                const float before = g_c.req[k], after = before - use[k];
                g_c.req[k] = before > 0.f ? fmaxf(after, 0.f) : before < 0.f ? fminf(after, 0.f) : 0.f;
            }
            g_c.consumed += Len(rd);
        }
    }
    g_c.lastRoot = root;
    g_c.haveRoot = true;
    // a leftover the game does not take (it stopped: the top / bottom of the climb, a blocked side) is dropped once the
    // held hands are still too - never while the game is still climbing toward it (that would cut a pull short)
    if (g_c.stillSince && now - g_c.stillSince >= settleMs && g_c.rootStillSince && now - g_c.rootStillSince >= 400)
        g_c.req[0] = g_c.req[1] = 0.f;
    if (kind == Climb::Rope) g_c.req[1] = 0.f;
    for (float& r : g_c.req) r = fminf(fmaxf(r, -reqMax), reqMax);
    // ---- letting go: the game has no fall from a wall or rope (SCR_ClimbDetach is a name without code), so both hands
    // off (after having held on) for climb_release_ms with ground below = the game's own climb down at full speed until
    // Kratos steps off at the bottom; any grip stops it (owner: "they fall back to the ground, from where they were")
    if (g_c.pin[0] || g_c.pin[1]) {
        g_c.noGripSince = 0;
        if (g_c.descending) Log("manual climb: holding on again - no more climbing down");
        g_c.descending = false;
        g_c.descendChecked = false;
    } else if (g_c.everGripped) {
        if (!g_c.noGripSince) g_c.noGripSince = now;
        static const ULONGLONG releaseMs = (ULONGLONG)fmaxf(Cfg("climb_release_ms", 500.f), 100.f);
        if (!g_c.descendChecked && now - g_c.noGripSince >= releaseMs) {
            g_c.descendChecked = true;
            static const float maxDrop = fmaxf(Cfg("climb_drop_max", 15.f), 1.f);
            const float a[3] = {root.x, root.y + 0.3f, root.z}, b[3] = {root.x, root.y - maxDrop, root.z};
            float hit[3];
            const bool ground = WallsRay(a, b, hit);
            // D-307k (owner: "remove that thing that makes you drop down slowly when you stop gripping the wall or rope"):
            // off - letting go leaves Kratos where he is (gowvr.ini climb_release_descend 1 brings the climb down back)
            static const bool descendOn = Cfg("climb_release_descend", 0.f) > 0.5f;
            g_c.descending = ground && descendOn;
            if (descendOn) {
                Log("manual climb: let go - %s", ground ? "climbing down to the ground" : "no ground below within reach: holding on");
                if (ground) Log("manual climb:   ground %.2f m below Kratos", root.y - hit[1]);
            }
        }
    }
    // ---- the stick: what is left to climb, less the distance the game coasts on after a stop (learned: the root's
    // travel along the last stick direction from the moment the stick went back to centre until it stood still); a
    // simulation of these rules against games that coast 0 - 0.15 m (scratch, D-307b) brought 1.20 m of pulls from
    // 1.88 / 1.54 m of climbing down to 1.46 / 1.31 m, with no stick reversals
    static const float full = fmaxf(Cfg("climb_full_m", 0.15f), 0.02f), dead = Cfg("climb_dead_m", 0.03f);
    static const float sx = Cfg("climb_sign_x", 1.f) < 0.f ? -1.f : 1.f, sy = Cfg("climb_sign_y", 1.f) < 0.f ? -1.f : 1.f;
    const int ck = kind == Climb::Rope ? 0 : kind == Climb::Wall ? 1 : 2;
    float x = 0.f, y = 0.f;
    {
        const float mag = sqrtf(g_c.req[0] * g_c.req[0] + g_c.req[1] * g_c.req[1]);
        const float eff = mag - g_c.coast[ck];
        if (eff > dead && mag > 1e-4f) {
            const float k = fminf(eff / full, 1.f) / mag;
            x = Clamp1(g_c.req[1] * k) * sx;
            y = Clamp1(g_c.req[0] * k) * sy;
        }
    }
    if (g_c.descending) { x = 0.f; y = -sy; }
    {   // learn the coast
        const bool wasOn = g_c.lastStick[0] != 0.f || g_c.lastStick[1] != 0.f, isOn = x != 0.f || y != 0.f;
        if (wasOn && !isOn && !g_c.descending) {
            const float l = sqrtf(g_c.lastStick[0] * g_c.lastStick[0] + g_c.lastStick[1] * g_c.lastStick[1]);
            g_c.releaseOpen = true;
            g_c.releaseRoot = root;
            g_c.releaseDir[0] = g_c.lastStick[1] * sy / l;  // up
            g_c.releaseDir[1] = g_c.lastStick[0] * sx / l;  // right
        }
        if (isOn) g_c.releaseOpen = false;
        if (g_c.releaseOpen && g_c.rootStillSince && now - g_c.rootStillSince >= 150) {
            g_c.releaseOpen = false;
            const V3 d = root - g_c.releaseRoot;
            const float over = d.y * g_c.releaseDir[0] + Dot(d, right) * g_c.releaseDir[1];
            if (over > -0.05f && over < 0.6f) {
                const float prev = g_c.coast[ck];
                g_c.coast[ck] = fminf(fmaxf(prev + (over - prev) * 0.5f, 0.f), 0.3f);
                static int logs = 0;
                if (logs++ < 30)
                    Log("manual climb: the game coasted %.2f m after the stick let go (%s) - the stick now lets go %.2f m early",
                        over, ClimbName(kind), g_c.coast[ck]);
            }
        }
        g_c.lastStick[0] = x;
        g_c.lastStick[1] = y;
    }
    g_climbX = x;
    g_climbY = y;
    g_c.lastStickY = y;
    // ---- D-307b (owner: "the body is not where my head is - it's stuck near the wall while climbing; I can't hold with
    // one hand and move my whole body around freely"): the drawn body moves so its eye follows the head (the headset's
    // offset from the view's base, smoothed 0.08 s), at most climb_body_follow_m (0.6) sideways, 0.5 down / 0.35 up, and
    // never so far that a held hand would be out of the arm's reach (shoulder ~0.13 m under the eye, 0.2 m to the side,
    // reach 0.62 m - Kratos' arm 0.58 + palm, tools/re/_manualviz.py)
    // ---- D-307c (owner: "rope climbing puts my view and body way too close to the wall again; on walls move the body a
    // tiny bit more away from the wall"; owner log: on the rope the body went 0.32 m toward the head = toward the rope,
    // whose climb faces -z): the head is kept at least climb_rope_clear_m (0.32) from the rope's line /
    // climb_wall_clear_m (0.35) from the wall, plus climb_wall_back_m (0.06) on walls / ledges: the VIEW is pushed back
    // by what is missing (firstperson.cpp adds it to the eye point) and the body, which follows the head, goes with it.
    // Measured on the head without the push already in the view (no feedback); smoothed 0.2 s
    {
        const V3 raw = headP - g_c.push;
        float dist;
        V3 away;
        if (kind == Climb::Rope) {
            const V3 d = Flat(raw - g_c.surfP);
            dist = Len(d);
            // D-307e (owner, D-307d build: "the chain movement you made the body go forward - I wanted it to go
            // backwards"): the push followed the line from Kratos' animated palms to the head, whose direction depends on
            // which side of the palms the head is (and flipped as the head crossed it) - on the chain it went forward.
            // On a rope / chain it now always points to Kratos' BACK (against his climbing facing, the game's offered
            // climb's forward); the distance rule is D-307c's. The palms' line along his facing is logged (+ = in front)
            const V3 fl = Norm(Flat(g_c.wallIn));
            away = fl * -1.f;
            g_c.ropeAhead = Dot(g_c.surfP - raw, fl);
        } else {
            dist = Dot(g_c.surfP - raw, g_c.wallIn);
            away = g_c.wallIn * -1.f;
        }
        // D-307d (owner, manual climbing on: "on the rope the only issue is you have to move the whole body 5 - 7 cm
        // backwards - Kratos' normal rope animation has his body literally on the rope"): on a rope always
        // climb_rope_back_m (0.06) more, the view and the drawn body together (kept as backV so the rope's offset cap
        // below never eats it) - D-307e: toward Kratos' back. Walls / ledges unchanged
        static const float ropeClear = Cfg("climb_rope_clear_m", 0.32f), wallClear = Cfg("climb_wall_clear_m", 0.35f),
                           wallBack = Cfg("climb_wall_back_m", 0.06f),
                           // D-307i (owner: "during rope climbing the view is still too close - 10 cm backwards, and the
                           // whole body 10 cm backwards"): 0.16 (was 0.06) - the view and the drawn body together
                           ropeBack = Cfg("climb_rope_back_m", 0.16f);
        const float len = kind == Climb::Rope ? fmaxf(ropeClear - dist, 0.f) + ropeBack : fmaxf(wallClear - dist, 0.f) + wallBack;
        const V3 target = Flat(away) * fminf(len, 0.6f);
        const float k = 1.f - expf(-dt / 0.2f);
        g_c.push = g_c.push + (target - g_c.push) * k;
        const V3 backT = kind == Climb::Rope ? Flat(away) * fminf(ropeBack, fminf(len, 0.6f)) : V3{0.f, 0.f, 0.f};
        g_c.backV = g_c.backV + (backT - g_c.backV) * k;
        g_c.clearDist = dist;
        g_viewPush[0] = g_c.push.x; g_viewPush[1] = g_c.push.y; g_viewPush[2] = g_c.push.z;
        g_viewPushOn = true;
    }
    {
        static const float follow = Cfg("climb_body_follow_m", 0.6f);
        V3 want = headP - basePos;
        V3 hz = Flat(want);
        if (Len(hz) > follow) hz = hz * (follow / Len(hz));
        want = V3{hz.x, fminf(fmaxf(want.y, -0.5f), 0.35f), hz.z};
        for (int it = 0; it < 2; ++it)
            for (int h = 0; h < 2; ++h) {
                if (!g_c.pin[h]) continue;
                const V3 sh = basePos + want + right * (h ? 0.2f : -0.2f) + V3{0.f, -0.13f, 0.f};
                const V3 d = g_c.pinP[h] - sh;
                const float dl = Len(d);
                if (dl > 0.62f) want = want + d * ((dl - 0.62f) / dl);
            }
        if (Cfg("climb_body_follow", 1.f) < 0.5f) want = V3{0.f, 0.f, 0.f};
        g_c.bodyOff = g_c.bodyOff + (want - g_c.bodyOff) * (1.f - expf(-dt / 0.08f));
        // D-307f (owner: "you need to move Kratos' body like 10 cm backwards" - on the rope / chain): the drawn body only
        // (the view stays) climb_rope_body_back_m (0.10) toward his back, eased 0.2 s, on top of D-307d / e's back-off of
        // view and body together; outside the rope's offset cap like that one. Less only when a held hand would be out
        // of the arm's reach (0.62 m from the shoulder): the body may come forward that much
        {
            static const float bodyBackM = Cfg("climb_rope_body_back_m", 0.10f);
            V3 bt = kind == Climb::Rope ? Norm(Flat(g_c.wallIn)) * -bodyBackM : V3{0.f, 0.f, 0.f};
            for (int h = 0; h < 2 && kind == Climb::Rope; ++h) {
                if (!g_c.pin[h]) continue;
                const V3 sh = basePos + g_c.bodyOff + g_c.push + bt + right * (h ? 0.2f : -0.2f) + V3{0.f, -0.13f, 0.f};
                const float over = Len(g_c.pinP[h] - sh) - 0.62f;
                if (over > 0.f) bt = bt * fmaxf(0.f, 1.f - over / fmaxf(bodyBackM, 0.01f));
            }
            g_c.bodyBack = g_c.bodyBack + (bt - g_c.bodyBack) * (1.f - expf(-dt / 0.2f));
        }
        // the drawn body from the game's root = the follow + the view's push; on a rope at most climb_rope_offset_m
        // (0.3) sideways - the game's rope reads Kratos' drawn wrists (sub_1406a5930 / sub_1406a5e80: JOLeftWrist1 /
        // JORightWrist1) and left the rope on its own with the body 0.32 m off (owner log)
        V3 o = g_c.bodyOff + g_c.push;
        if (kind == Climb::Rope) {
            // D-307g: 0.5 (was 0.3) - owner log: the player's head 0.40 - 0.52 m back from the game's eye on the chain,
            // the body held 0.1 - 0.2 m in front of the view by the cap (the rope exit it was made for, D-307c, was the
            // player's own stick, D-307d)
            static const float ropeMax = Cfg("climb_rope_offset_m", 0.5f);
            const V3 hzo = Flat(o - g_c.backV);  // D-307d: the cap is on the follow + the clearance, not the back-off
            if (Len(hzo) > ropeMax) { const V3 c = hzo * (ropeMax / Len(hzo)) + Flat(g_c.backV); o.x = c.x; o.z = c.z; }
        }
        o = o + g_c.bodyBack;  // D-307f
        g_bodyOff[0] = o.x;
        g_bodyOff[1] = o.y;
        g_bodyOff[2] = o.z;
        g_bodyOffOn = true;
    }
    static ULONGLONG tLog = 0;
    if (now - tLog >= 5000) {
        tLog = now;
        Log("manual climb: %s, holding L %d%s R %d%s, to climb up %.2f right %.2f m, stick %.2f %.2f, pulled %.2f m, the game "
            "moved %.2f m (last 5 s), body toward the head (%.2f %.2f %.2f), the head %.2f m from the %s%s, view pushed back "
            "%.2f m (%.2f %.2f)%s",
            ClimbName(kind), (int)g_c.pin[0], g_c.autoHold[0] ? " (start)" : "", (int)g_c.pin[1], g_c.autoHold[1] ? " (start)" : "",
            g_c.req[0], g_c.req[1], x, y, g_c.pulled, g_c.consumed, g_c.bodyOff.x, g_c.bodyOff.y, g_c.bodyOff.z, g_c.clearDist,
            kind == Climb::Rope ? "rope" : "wall",
            kind == Climb::Rope ? (g_c.ropeAhead >= 0.f ? " (in front of it)" : " (PAST it)") : "", Len(g_c.push), g_c.push.x,
            g_c.push.z, g_c.descending ? ", climbing down (let go)" : "");
        if (kind == Climb::Rope)
            Log("manual climb:   the grab line %.2f m %s / %.2f m %s of Kratos' palms; the view and body pushed back %.2f m "
                "(%.2f of it the fixed back-off, D-307i), the drawn body %.2f m further back than the view (D-307f / g)",
                fabsf(g_lineF), g_lineF < 0.f ? "back" : "forward", fabsf(g_lineR), g_lineR < 0.f ? "left" : "right",
                Len(g_c.push), Len(g_c.backV), Len(g_c.bodyBack));
        g_c.pulled = g_c.consumed = 0.f;
    }
}

// ---- rowing ---------------------------------------------------------------------------------------------------------
struct RowState {
    bool seated = false, active = false, learnDone = false;
    ULONGLONG seatedAt = 0;
    V3 lastBlade{}, lastBase{};
    V3 lastBladeLocal{};  // D-308h: the blade in the view base's frame (turns with the boat)
    bool haveLast = false, inWater = false;
    float drive = 0.f, turn = 0.f;  // the boat's command: forward (-1..1), turn right (-1..1)
    int strokes = 0;
    float strokeLen = 0.f, minBlade = 1e9f, maxBlade = -1e9f;
    ULONGLONG rudderSince = 0;
    bool ruddering = false;
    V3 lowBack[2]{};          // D-308e: the far hand's back-of-hand direction (eased)
    float dipWide = 0.f;      // D-308f: how wide this dip's stroke went (0 close .. 1 a wide sweep)
    float bladeSpeed = 0.f;   // D-308h: the blade middle's speed against the boat (smoothed 0.2 s)
    ULONGLONG stillSince = 0; // D-308h: the paddle held still since
    bool stopped = false;     // D-308h: held still row_stop_still_ms - the boat stops (unused since D-315)
    V3 strokeDir{};           // D-315: this stroke's direction in the view base's frame (a reversal = a new stroke)
    bool haveStrokeDir = false;
    bool haveLowBack[2] = {false, false};
    V3 dir{};  // the shaft's direction (smoothed)
    bool haveDir = false;
    // D-308c: the hands locked on the paddle (their turn relative to it, taken when rowing starts / at a Y flip)
    float lockRot[2][3][3] = {};
    bool haveLock = false;
    int lockSide = 0;
    float dipBack = 0.f;  // metres the blade moved back in this dip
    bool dipBoosted = false;
    V3 lastPadRoot{};
    bool havePadRoot = false;
    // learning
    V3 sumUp{}, sumLow{}, sumUp2{}, sumLow2{}, sumFace{};
    int n = 0;
} g_r;

bool ReadPaddleSeh(uint8_t* skel, Mat4* root, V3* tipLocal, int* nj) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(skel + 0x38);
        const float* m = *(const float* const*)(skel + 0x58);
        if (!m || n < 1 || n > 64) return false;
        const float* w = m + (size_t)n * 16;
        memcpy(root, w, sizeof(Mat4));
        *nj = n;
        const Mat4 inv = AffineInverse(*root);
        float best = 0.f;
        *tipLocal = {0.f, 0.f, 0.f};
        for (int j = 1; j < n; ++j) {
            const float* p = w + (size_t)j * 16 + 12;
            Mat4 t{};
            t.m[3][0] = p[0]; t.m[3][1] = p[1]; t.m[3][2] = p[2]; t.m[3][3] = 1.f;
            const V3 l = Pos(Mul(t, inv));
            const float d = Len(l);
            if (d < 3.f && d > best) { best = d; *tipLocal = l; }
        }
        return fabsf(root->m[3][0]) + fabsf(root->m[3][1]) + fabsf(root->m[3][2]) > 1.f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int PaddleJointsLocal(uint8_t* skel, const Mat4& inv, float out[][3], int maxN) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(skel + 0x38);
        const float* w = *(const float* const*)(skel + 0x58) + (size_t)n * 16;
        int k = 0;
        for (int j = 0; j < n && k < maxN; ++j, ++k) {
            const float* p = w + (size_t)j * 16 + 12;
            for (int c = 0; c < 3; ++c)
                out[k][c] = p[0] * inv.m[0][c] + p[1] * inv.m[1][c] + p[2] * inv.m[2][c] + inv.m[3][c];
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
V3 ToLocal(V3 p, const Mat4& inv) {
    Mat4 t{};
    t.m[3][0] = p.x; t.m[3][1] = p.y; t.m[3][2] = p.z; t.m[3][3] = 1.f;
    return Pos(Mul(t, inv));
}
V3 ToWorld(V3 p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

void PaddleLoadSaved() {
    static bool done = false;
    if (done) return;
    done = true;
    // (formats 1 / 2 - D-308 / D-308b - had the grips' roles the wrong way round or no face direction: learned again)
    if (Cfg("row_learned", 0.f) < 2.5f) return;
    AcquireSRWLockExclusive(&g_padLock);
    g_gripUp = {Cfg("row_grip_up_x", 0.f), Cfg("row_grip_up_y", 0.f), Cfg("row_grip_up_z", 0.f)};
    g_gripLow = {Cfg("row_grip_low_x", 0.f), Cfg("row_grip_low_y", 0.f), Cfg("row_grip_low_z", 0.f)};
    g_faceL = {Cfg("row_face_x", 1.f), Cfg("row_face_y", 0.f), Cfg("row_face_z", 0.f)};
    g_bladeTip = Cfg("row_blade_m", 0.9f);
    g_faceLearned = Len(g_gripLow - g_gripUp) > 0.4f && Len(g_gripLow - g_gripUp) < 1.3f && Len(g_faceL) > 0.5f;
    if (!g_faceLearned) {  // unusable: the built-in grips (D-308c / D-308d)
        g_gripUp = {-0.005f, 0.027f, 0.734f};
        g_gripLow = {-0.047f, 0.012f, -0.090f};
        g_faceL = {1.f, 0.f, 0.f};
    }
    g_padLearned = true;
    ReleaseSRWLockExclusive(&g_padLock);
    Log("manual row: paddle grips from gowvr.ini - upper (%.3f %.3f %.3f) lower (%.3f %.3f %.3f), toward the bow (%.2f %.2f "
        "%.2f) in the paddle's frame, blade end %.2f m past the lower hand%s", g_gripUp.x, g_gripUp.y, g_gripUp.z, g_gripLow.x,
        g_gripLow.y, g_gripLow.z, g_faceL.x, g_faceL.y, g_faceL.z, g_bladeTip, g_faceLearned ? "" : " - unusable, the built-in grips meanwhile");
}

// learn where the game's own animation holds the paddle (arms authored: the published wrists are the animation's)
void PaddleLearn(const char* lp) {
    uint8_t* sk = PaddleSkeleton();
    Mat4 root{};
    V3 tipL{};
    int nj = 0;
    V3 pl, pr;
    if (!sk || !ReadPaddleSeh(sk, &root, &tipL, &nj) || !DrawnPalm(0, &pl) || !DrawnPalm(1, &pr)) return;
    // D-308c: only at the boat's idle with the paddle still (the attachment follows the hands a frame late, so a moving
    // paddle - the game rowing - gives wrong grips)
    {
        const V3 o{root.m[3][0], root.m[3][1], root.m[3][2]};
        const bool still = g_r.havePadRoot && Len(o - g_r.lastPadRoot) < 0.005f;
        g_r.lastPadRoot = o;
        g_r.havePadRoot = true;
        if (strncmp(lp, "navboatidle", 11) != 0 || !still) return;
    }
    const Mat4 inv = AffineInverse(root);
    const V3 l = ToLocal(pl, inv), r = ToLocal(pr, inv);
    if (Len(l) > 2.5f || Len(r) > 2.5f || Len(l - r) < 0.15f) return;  // not held in both hands right now
    // D-308d: the blade-side hand is the one nearer the paddle's root (the blade is past the root end - owner's test of
    // D-308c, where the height rule had put the blade at the top hand)
    const bool leftLow = Len(l) < Len(r);
    const V3 up = leftLow ? r : l, low = leftLow ? l : r;
    // D-308b: the boat's bow in the paddle's frame, across the shaft: how the game turns the blade to the boat
    {
        float gp[3], gy = 0.f;
        if (!GameRoot(gp, &gy)) return;
        const V3 o{root.m[3][0], root.m[3][1], root.m[3][2]};
        const V3 fl = ToLocal(o + V3{-sinf(gy), 0.f, -cosf(gy)}, inv) - ToLocal(o, inv);
        const V3 ax = Norm(low - up);
        const V3 f = Norm(fl - ax * Dot(fl, ax));
        if (Len(f) < 0.5f) return;
        g_r.sumFace = g_r.sumFace + f;
    }
    {   // the paddle's joints, once (its shape: where the blade is)
        static bool logged = false;
        if (!logged) {
            logged = true;
            char b[600];
            int o = 0;
            __declspec(align(16)) float jl[64][3];
            const int nl = PaddleJointsLocal(sk, inv, jl, 64);
            for (int j = 0; j < nl && o < (int)sizeof(b) - 40; ++j)
                o += sprintf_s(b + o, sizeof(b) - o, " %d(%.2f %.2f %.2f)", j, jl[j][0], jl[j][1], jl[j][2]);
            Log("manual row: paddle joints in its root's frame:%s", nl ? b : " (not read)");
        }
    }
    g_r.sumUp = g_r.sumUp + up;
    g_r.sumLow = g_r.sumLow + low;
    g_r.sumUp2 = g_r.sumUp2 + V3{up.x * up.x, up.y * up.y, up.z * up.z};
    g_r.sumLow2 = g_r.sumLow2 + V3{low.x * low.x, low.y * low.y, low.z * low.z};
    if (++g_r.n < 90) return;
    const float n = (float)g_r.n;
    const V3 mu = g_r.sumUp * (1.f / n), ml = g_r.sumLow * (1.f / n);
    auto spread = [&](V3 s2, V3 m) {
        const V3 v{s2.x / n - m.x * m.x, s2.y / n - m.y * m.y, s2.z / n - m.z * m.z};
        return sqrtf(fmaxf(v.x, 0.f) + fmaxf(v.y, 0.f) + fmaxf(v.z, 0.f));
    };
    const float su = spread(g_r.sumUp2, mu), sl = spread(g_r.sumLow2, ml);
    // the blade's end: the far joint's distance past the lower grip along the shaft, when the paddle has one
    const V3 axis = Norm(ml - mu);
    float tip = Cfg("row_blade_m", 0.9f);
    if (Len(tipL) > 0.3f && Dot(tipL - ml, axis) > 0.2f) tip = Dot(tipL - ml, axis);
    Log("manual row: paddle grips learned from the game's animation (%d samples, spread %.3f / %.3f m, %d paddle joints): "
        "upper (%.3f %.3f %.3f) lower (%.3f %.3f %.3f), hands %.2f m apart, blade end %.2f m past the lower hand%s", g_r.n, su,
        sl, nj, mu.x, mu.y, mu.z, ml.x, ml.y, ml.z, Len(ml - mu), tip, Len(tipL) > 0.3f ? "" : " (no far joint: row_blade_m)");
    const V3 face = Norm(g_r.sumFace - axis * Dot(g_r.sumFace, axis));
    Log("manual row:   toward the bow in the paddle's frame (%.2f %.2f %.2f)", face.x, face.y, face.z);
    g_r.n = 0;
    g_r.sumUp = g_r.sumLow = g_r.sumUp2 = g_r.sumLow2 = g_r.sumFace = V3{0.f, 0.f, 0.f};
    if (su > 0.05f || sl > 0.05f) { Log("manual row:   the hands moved on the paddle while learning - learning again"); return; }
    if (Len(face) < 0.5f) { Log("manual row:   no steady bow direction - learning again"); return; }
    AcquireSRWLockExclusive(&g_padLock);
    g_gripUp = mu;
    g_gripLow = ml;
    g_faceL = face;
    g_bladeTip = tip;
    g_padLearned = g_faceLearned = true;
    ReleaseSRWLockExclusive(&g_padLock);
    config::SetFloats({{"row_grip_up_x", mu.x}, {"row_grip_up_y", mu.y}, {"row_grip_up_z", mu.z}, {"row_grip_low_x", ml.x},
                       {"row_grip_low_y", ml.y}, {"row_grip_low_z", ml.z}, {"row_face_x", face.x}, {"row_face_y", face.y},
                       {"row_face_z", face.z}, {"row_blade_m", tip}, {"row_learned", 3.f}});
    g_r.learnDone = true;
}

void RowTick(Mat4 hands[2], const bool valid[2], const Mat4& base, float dt, ULONGLONG now, const char* prim) {
    const bool opt = Cfg("manual_row", 0.f) > 0.5f && MotionControls();
    g_rowMode = opt && InBoat();
    PaddleLoadSaved();
    char lp[96];
    size_t ci = 0;
    for (; prim && prim[ci] && prim[ci] != '#' && ci < sizeof(lp) - 1; ++ci) lp[ci] = (char)tolower((unsigned char)prim[ci]);
    lp[ci] = 0;
    // D-308b: the game's own row moves (navBoatRowStartRFEnter / navBoatRowSlowRF / navBoatRowRFExit, owner log) are
    // not boarding - they turned rowing by hand off ("OFF (boarding / leaving)")
    const bool rowMove = strncmp(lp, "navboatrow", 10) == 0;
    const bool boarding = !rowMove && (strstr(lp, "enter") || strstr(lp, "exit") || strstr(lp, "dock") ||
                                       strstr(lp, "pushoff") || strstr(lp, "pluck") || strstr(lp, "shuffle"));
    // seated: the boat's idle has played since boarding (navBoatIdle*); kept while rowing (the row moves' names are not
    // in the data the mod has) until a boarding / leaving / docking move or the boat's own "left" state
    if (!InBoat() || boarding) g_r.seated = false;
    else if (strncmp(lp, "navboatidle", 11) == 0 && !g_r.seated) { g_r.seated = true; g_r.seatedAt = now; }
    if (!opt || !g_r.seated) {
        if (g_r.active) {
            Log("manual row: OFF (%s) - the fists open", !opt ? "option / motion controls off or left the boat" : "boarding / leaving");
            // D-308h (owner: "after getting off the boat my fingers stay curled from the manual rowing"): the paddle fists
            // (g_fist 1, set every rowing frame) were never put back - the arms read ManualFist always
            for (int h = 0; h < 2; ++h) { g_fist[h] = 0.f; g_hook[h] = 0.f; }
        }
        g_r.stillSince = 0;
        g_r.stopped = false;
        g_r.bladeSpeed = 0.f;
        g_r.active = false;
        g_rowActive = false;
        g_rowX = g_rowY = 0.f;
        g_r.drive = g_r.turn = 0.f;
        g_r.haveLast = false;
        g_r.haveDir = false;
        AcquireSRWLockExclusive(&g_padLock);
        g_drawValid = false;
        ReleaseSRWLockExclusive(&g_padLock);
        g_r.haveLock = false;
        g_exact[0] = g_exact[1] = false;
        if (InBoat() && g_r.seated && !g_faceLearned) PaddleLearn(lp);  // arms authored: learn from the game's grip
        return;
    }
    // the first row_learn_ms (1.5 s) in the seat the arms stay the game's while the grips are refined (once - kept in
    // gowvr.ini); the built-in grips are used when the game's paddle never sits still in that time
    if (!g_faceLearned && now - g_r.seatedAt < (ULONGLONG)fmaxf(Cfg("row_learn_ms", 1500.f), 0.f)) {
        PaddleLearn(lp);
        return;
    }
    if (!g_r.active) {
        g_r.active = true;
        g_r.haveLock = false;
        g_rowCapture = true;  // the seated pose from the next hero pass
        Log("manual row: ON - paddle on the %s (Y flips it), held in both hands%s", g_rowSide.load() > 0 ? "right" : "left",
            g_faceLearned ? "" : " (the built-in grips)");
    }
    g_rowActive = true;
    if (!valid[0] || !valid[1]) { g_rowX = g_rowY = 0.f; g_exact[0] = g_exact[1] = false; return; }
    const int side = g_rowSide.load() >= 0 ? 1 : -1;
    const int low = side > 0 ? 1 : 0, top = 1 - low;
    for (int h = 0; h < 2; ++h) { g_fist[h] = 1.f; g_hook[h] = 0.f; }
    // the boat's frame: Kratos sits facing the bow
    float gp[3], gy = 0.f;
    if (!GameRoot(gp, &gy)) return;
    const V3 root{gp[0], gp[1], gp[2]};
    const V3 fwd{-sinf(gy), 0.f, -cosf(gy)}, right{cosf(gy), 0.f, -sinf(gy)};
    const V3 basePos = Pos(base);
    // D-308b (owner: "the paddle should be stable and stay fixed at one position, like the axe"; owner log: the paddle was
    // put between the hands by their midpoint - its grips are 0.82 m apart, the hands were 0.31 m apart - and kept the
    // game's own roll from its rowing animation, so it slid and turned): the paddle is FIXED in the top fist (its upper
    // grip exactly there, like the axe's grip on the IK hand) and points at the other hand (smoothed 0.05 s); its face
    // keeps the game's own turn toward the bow. The lower hand slides along the shaft and is drawn on it
    const V3 topP = Pos(hands[top]), lowP = Pos(hands[low]);
    {
        V3 d = lowP - topP;
        if (Len(d) > 0.08f) {
            d = Norm(d);
            if (!g_r.haveDir) { g_r.dir = d; g_r.haveDir = true; }
            else g_r.dir = Norm(g_r.dir + (d - g_r.dir) * (1.f - expf(-dt / 0.08f)));
        }
    }
    if (!g_r.haveDir) { g_rowX = g_rowY = 0.f; return; }
    const V3 axis = g_r.dir;
    V3 faceW = fwd - axis * Dot(fwd, axis);
    if (Len(faceW) < 0.1f) faceW = V3{0.f, 1.f, 0.f} - axis * axis.y;
    faceW = Norm(faceW);
    AcquireSRWLockExclusive(&g_padLock);
    const float grips = Len(ShaftPoint(g_gripLow) - ShaftPoint(g_gripUp)), bladeTip = g_bladeTip;
    g_drawTop = top;
    g_drawTopP = topP;
    g_drawDir = axis;
    g_drawFace = faceW;
    g_drawValid = true;
    ReleaseSRWLockExclusive(&g_padLock);
    // D-308d (owner: "the hands get their wrists broken / twisted at the paddle flips; usually you're wrong by 90 degrees
    // forward or backward, or 30 degrees to the left"): D-308c took each hand's turn from the controllers at the flip - so
    // any controller angle at that moment became the grip. Now both hands hold the shaft the way Kratos' own fist closes
    // round a bar (FistOnBar, from his hand bones): the fist's tunnel along the shaft, thumbs toward the paddle's top, the
    // top hand's knuckles toward the bow, the lower hand's outboard. Owner: "both hands should stay stable, not one that
    // can slide": each fist's centre sits at its own fixed place on the shaft - the top fist at the top grip, the lower
    // one the paddle's grip spacing further down (the controllers only aim the shaft). Written as the controller grip
    // arms.cpp turns into the wrist target
    // (t.X = -g.X, t.Y = -g.Z, t.Z = -g.Y, t.pos = g.pos - t.Y x 0.07), with no per-headset hand angle on these hands
    {
        const V3 toTop = axis * -1.f;
        // D-308e (owner: "make the far hand closer to the body by about 10 cm"): the lower fist row_low_in (0.10 m) up the
        // shaft from the paddle's own lower grip
        static const float lowIn = Cfg("row_low_in", 0.10f);
        const float lowAt = fmaxf(grips - lowIn, 0.3f);
        const V3 at[2] = {top == 0 ? topP : topP + axis * lowAt, top == 1 ? topP : topP + axis * lowAt};
        for (int h = 0; h < 2; ++h) {
            V3 back = h == top ? fwd : right * (float)side;
            if (h != top) {
                // D-308e (owner: "the far arm's wrist can move in unnatural ways, unlike the arm close to Kratos"): its
                // knuckles were turned outboard whatever the arm did - degenerate when the shaft points outboard (the
                // paddle out to the side: a wrist flip). Now the far hand continues its arm: the hand's length (wrist ->
                // knuckles, the fist's third axis D = tunnel x back, _handgrip.py) along shoulder -> grip, made square to
                // the shaft - a straight wrist; outboard only when the arm runs along the shaft; eased 0.1 s
                Mat4 sm{};
                V3 armDir = StableJoint(kShoulderHash[h], &sm) ? at[h] - Pos(sm) : V3{0.f, 0.f, 0.f};
                armDir = armDir - toTop * Dot(armDir, toTop);
                if (Len(armDir) > 0.05f) {
                    const V3 a = Norm(kFistTunnel[h]), bb = Norm(kFistBack[h] - a * Dot(kFistBack[h], a));
                    const float ySign = Dot(V3{0.f, 1.f, 0.f}, Cross(a, bb)) < 0.f ? -1.f : 1.f;  // the hand's +Y along D or -D
                    back = Cross(Norm(armDir) * ySign, Norm(toTop));  // B = D x A
                }
                if (g_r.haveLowBack[h] && Dot(back, g_r.lowBack[h]) > -0.5f)
                    back = Norm(g_r.lowBack[h] + (Norm(back) - g_r.lowBack[h]) * (1.f - expf(-dt / 0.1f)));
                g_r.lowBack[h] = Norm(back);
                g_r.haveLowBack[h] = true;
            }
            V3 t[3];
            FistOnBar(h, toTop, back, t);
            const V3 c = kFistCentre[h];
            const V3 wrist = at[h] - (t[0] * c.x + t[1] * c.y + t[2] * c.z);
            const V3 g[4] = {t[0] * -1.f, t[2] * -1.f, t[1] * -1.f, wrist + t[1] * 0.07f};
            for (int i = 0; i < 4; ++i) { hands[h].m[i][0] = g[i].x; hands[h].m[i][1] = g[i].y; hands[h].m[i][2] = g[i].z; }
            hands[h].m[0][3] = hands[h].m[1][3] = hands[h].m[2][3] = 0.f;
            hands[h].m[3][3] = 1.f;
            g_exact[h] = true;
        }
    }
    // the water (owner: "about 10 cm under the top of the boat; from the head in first person about 0.5 m"): the seated
    // view's height minus row_water_below_eye (0.5); row_water_m adds to it. (The boat object's root, D-308, was 0.35 m
    // too low - the blade never reached it - and "goboat*" also matched the mooring ropes.)
    // D-308j (owner: "you guessed wrong where the water level is - it's more like 20 cm more down"): 0.7 (was 0.5)
    static const float belowEye = Cfg("row_water_below_eye", 0.7f);
    const float water = basePos.y - belowEye + Cfg("row_water_m", 0.f);
    // the blade's middle: the paddle's own length from the top fist (grips + the blade end - 0.2 m), whatever the hands'
    // spacing on the shaft
    const V3 blade = topP + axis * (grips + fmaxf(bladeTip - 0.2f, 0.2f));
    g_r.minBlade = fminf(g_r.minBlade, blade.y);
    g_r.maxBlade = fmaxf(g_r.maxBlade, blade.y);
    // D-315 (owner 2026-11-14: "make manual rowing actually fun - rowing in ANY direction makes you go further with the
    // motion controls, and whether you go right or left is the left joystick, like in the flat game"). Replaces D-308e ..
    // D-308k's rules (pull-back-only strokes, sweep turns, the rudder, the stop): two simple systems now -
    //  - GO (motion): the blade under the water (either side, > 0.15 m out from the boat's middle) moving through it
    //    faster than row_stroke_speed (0.3 m/s) in ANY horizontal direction - back, forward, out, in - pushes the boat
    //    ahead. Each stroke: row_stroke_boost (0.3) once it has gone row_stroke_min (0.10 m), then row_drive_per_m (0.6)
    //    per metre more, times its speed (x0.5 slow .. x1.5 fast, 0.8 m/s = x1). A stroke that turns round in the water
    //    (back then forward) is a new stroke. Push fades over row_glide_s (3 s): steady rowing = full speed, one stroke
    //    a glide. Measured in the view base's frame (the hands' own motion - a turning boat moves nothing).
    //  - TURN (stick): the player's left stick sideways = the boat's turn, as the flat game's stick (row_steer 1.0);
    //    its up / down is ignored (the speed is the rowing's).
    const bool inWater = blade.y < water && fabsf(Dot(blade - root, right)) > 0.15f;
    if (inWater && !g_r.inWater) { vr::XrHaptic(0, 0.3f, 0.04f); vr::XrHaptic(1, 0.3f, 0.04f); }  // the blade bites the water
    static const float minV = Cfg("row_stroke_speed", 0.3f), strokeMin = Cfg("row_stroke_min", 0.10f),
                       boost = Cfg("row_stroke_boost", 0.3f), kDrive = Cfg("row_drive_per_m", 0.6f),
                       steer = Cfg("row_steer", 1.0f);
    const V3 bladeLocal = ToLocal(blade, AffineInverse(base));
    if (g_r.haveLast && dt > 1e-4f && inWater) {
        V3 dl = bladeLocal - g_r.lastBladeLocal;
        dl.y = 0.f;  // horizontal only: dipping in / lifting out is not a stroke
        const float step = fminf(Len(dl), 0.1f);  // a lever jump is not a stroke
        const float v = step / dt;
        g_r.bladeSpeed += (v - g_r.bladeSpeed) * (1.f - expf(-dt / 0.2f));
        if (v > minV && step > 1e-4f) {
            const V3 dir = dl * (1.f / Len(dl));
            if (g_r.haveStrokeDir && Dot(dir, g_r.strokeDir) < -0.3f) { g_r.dipBack = 0.f; g_r.dipBoosted = false; }
            g_r.strokeDir = dir;
            g_r.haveStrokeDir = true;
            g_r.dipBack += step;
            const float speedK = fminf(fmaxf(v / 0.8f, 0.5f), 1.5f);
            if (!g_r.dipBoosted && g_r.dipBack >= strokeMin) {
                g_r.dipBoosted = true;
                g_r.drive += boost;
                ++g_r.strokes;
                vr::XrHaptic(0, 0.8f, 0.08f);
                vr::XrHaptic(1, 0.8f, 0.08f);
                static int logs = 0;
                if (logs++ < 60)
                    Log("manual row: stroke on the %s (%.1f m/s) -> push %.2f (water %.2f, blade %.2f)", side > 0 ? "right" : "left", v,
                        fminf(g_r.drive, 1.f), water, blade.y);
            } else if (g_r.dipBoosted) {
                g_r.drive += kDrive * step * speedK;
            }
            const float amp = fminf(fmaxf(v * 0.35f, 0.2f), 0.9f);  // feel the water's resistance
            vr::XrHaptic(0, amp, 0.03f);
            vr::XrHaptic(1, amp, 0.03f);
        }
    }
    if (!inWater) { g_r.dipBack = 0.f; g_r.dipBoosted = false; g_r.haveStrokeDir = false; }
    g_r.inWater = inWater;
    g_r.lastBlade = blade;
    g_r.lastBladeLocal = bladeLocal;
    g_r.lastBase = basePos;
    g_r.haveLast = true;
    static const float tauDrive = fmaxf(Cfg("row_glide_s", 3.0f), 0.1f);
    g_r.drive = fminf(fmaxf(g_r.drive, 0.f), 1.f) * expf(-dt / tauDrive);
    if (g_r.drive < 0.01f) g_r.drive = 0.f;
    // the player's left stick sideways (pad thread), past a 0.15 dead zone, rescaled to 0..1
    {
        const float sx = g_padStick[0].load();
        g_r.turn = fabsf(sx) < 0.15f ? 0.f : Clamp1((sx - copysignf(0.15f, sx)) / 0.85f * steer);
    }
    // the boat's stick: wanted direction in the world through the game camera's frame (D-064: the stick is read in the
    // camera's frame; with the boat's camera behind the boat this is forward / sideways)
    const V3 want = fwd * g_r.drive + right * g_r.turn;
    const float cy = GameCamYaw();
    const V3 camR{cosf(cy), 0.f, -sinf(cy)}, camF{-sinf(cy), 0.f, -cosf(cy)};
    float x = Dot(want, camR), y = Dot(want, camF);
    const float m = sqrtf(x * x + y * y);
    static const float rowMax = fminf(fmaxf(Cfg("row_max", 1.0f), 0.1f), 1.f);  // D-315: 1.0 (was 0.85)
    if (m > rowMax) { x *= rowMax / m; y *= rowMax / m; }
    if (m < 0.05f) x = y = 0.f;
    g_rowX = x;
    g_rowY = y;
    static ULONGLONG tLog = 0;
    if (now - tLog >= 5000) {
        tLog = now;
        float boatY = 0.f;
        const bool haveBoat = BoatRootY(&boatY);
        Log("manual row: paddle %s, %d strokes (5 s), drive %.2f turn %.2f -> stick %.2f %.2f | water %.2f (view %.2f - %.2f), "
            "blade %.2f .. %.2f (5 s), Kratos' root %.2f, boat root %s%.2f, hands %.2f m apart", side > 0 ? "right" : "left",
            g_r.strokes, g_r.drive, g_r.turn, x, y, water, basePos.y, belowEye, g_r.minBlade, g_r.maxBlade, root.y,
            haveBoat ? "" : "(none) ", boatY, Len(lowP - topP));
        g_r.strokes = 0;
        g_r.minBlade = 1e9f;
        g_r.maxBlade = -1e9f;
    }
}

}  // namespace

bool ManualClimbActive() { return g_climbActive.load(); }
bool ManualBodyOffset(float out[3]) {
    if (!g_bodyOffOn.load()) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_bodyOff[i].load();
    return true;
}
bool ManualViewPush(float out[3]) {
    if (!g_viewPushOn.load()) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_viewPush[i].load();
    return true;
}
bool ManualRowActive() { return g_rowActive.load(); }
float ManualPoseWeight() { return g_poseW.load(); }
int ManualPoseKind() { return g_poseKind.load(); }
bool ManualStandingCalm() { return g_standCalm.load(); }
bool ManualRowCaptureReq() { return g_rowCapture.exchange(false); }
// D-308h: only while climbing / rowing by hand (a value left behind never keeps the fingers curled)
float ManualHook(int h) { return h >= 0 && h < 2 && (g_climbActive.load() || g_rowActive.load()) ? g_hook[h].load() : 0.f; }
float ManualFist(int h) { return h >= 0 && h < 2 && (g_climbActive.load() || g_rowActive.load()) ? g_fist[h].load() : 0.f; }
bool ManualHandExact(int h) { return h >= 0 && h < 2 && g_rowActive.load() && g_exact[h].load(); }

void ManualHands(Mat4 hands[2], const bool valid[2], bool wallsSkip[2], const Mat4& base, const Mat4& head) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = last ? fminf((float)(now - last) * 0.001f, 0.1f) : 1.f / 90.f;
    last = now;
    char prim[96];
    HeroPrimaryAnim(prim, sizeof(prim));
    const char* moveAnim = ActiveMoveHash() ? ActiveMoveAnim() : "";
    // the standing pose for the straight body (owner: "while holding on make the body straight - the normal position"):
    // taken while Kratos stands still on the ground
    g_standCalm = Cfg("manual_climb", 0.f) > 0.5f && CombatHeroIdle() && !HeroInSyncMove() && !InBoat() && !HeroMoving() &&
                  ClassifyAnim(prim) == Climb::None;
    ClimbTick(hands, valid, wallsSkip, base, head, dt, now, prim, moveAnim ? moveAnim : "");
    RowTick(hands, valid, base, dt, now, prim);
    // the held body: standing while climbing by hand, seated while rowing by hand (eased 0.15 s)
    const int kind = g_climbActive.load() ? 1 : g_rowActive.load() ? 2 : 0;
    static int lastKind = 0;
    if (kind) lastKind = kind;
    g_poseKind = lastKind;
    const bool straight = kind == 1 ? Cfg("climb_body_straight", 1.f) > 0.5f : kind == 2 ? Cfg("row_body_still", 1.f) > 0.5f : false;
    float w = g_poseW.load();
    w += ((straight ? 1.f : 0.f) - w) * (1.f - expf(-dt / 0.15f));
    if (!straight && w < 0.01f) w = 0.f;
    g_poseW = w;
}

void ManualIdle() {
    if (g_climbActive.load() || g_c.engaged) Log("manual climb: OFF (not in first person gameplay)");
    if (g_c.engaged) ClimbReset();
    g_climbActive = false;
    g_climbX = g_climbY = 0.f;
    g_c.haveChoiceIn = false;
    g_c.bodyOff = g_c.push = g_c.backV = g_c.bodyBack = V3{0.f, 0.f, 0.f};
    g_bodyOffOn = g_viewPushOn = false;
    for (auto& o : g_bodyOff) o = 0.f;
    for (auto& o : g_viewPush) o = 0.f;
    if (g_enterBoost) { HeroTimeScaleWish(2, 0.f); g_enterBoost = false; }
    g_rowActive = false;
    g_rowMode = false;
    g_r.active = false;
    g_exact[0] = g_exact[1] = false;
    g_rowX = g_rowY = 0.f;
    g_poseW = 0.f;
    g_standCalm = false;
    for (int h = 0; h < 2; ++h) { g_hook[h] = 0.f; g_fist[h] = 0.f; }
    AcquireSRWLockExclusive(&g_padLock);
    g_drawValid = false;
    ReleaseSRWLockExclusive(&g_padLock);
}

void ManualPad(const vr::InputSnapshot& s, XINPUT_GAMEPAD* g) {
    const ULONGLONG now = GetTickCount64();
    const vr::HandState& L = s.hand[0];
    const bool phys = L.active && (fabsf(L.stick[0]) > 0.35f || fabsf(L.stick[1]) > 0.35f);  // the player's own stick wins
    g_padStick[0] = L.active ? L.stick[0] : 0.f;
    g_padStick[1] = L.active ? L.stick[1] : 0.f;

    if (now < g_entryPulseUntil.load()) g->wButtons |= XINPUT_GAMEPAD_A;
    // D-314 (owner: "during manual rowing I can't press R1 in the menu"): a menu / 2D screen keeps the game thread's last
    // climbing / rowing state (ManualIdle comes only from the gameplay view), so the R1 / L1 strip, the boat's stick and
    // the swallowed Y went on in the menu. On a 2D screen the pad is the plain mapping's alone
    if (stereo::Screen2DFrame()) return;
    if (g_climbActive.load()) {
        g->wButtons &= (WORD)~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);  // the grips hold on, no R1 / L1
        if (!phys) {
            g->sThumbLX = AxisS(g_climbX.load());
            g->sThumbLY = AxisS(g_climbY.load());
        }
    }
    static bool yPrev = false;
    const bool yNow = L.active && L.b;
    if (g_rowMode.load()) {
        // D-308 (owner: "press Y / triangle in the boat and the paddle flips - that tells which side they row on")
        if (yNow && !yPrev) {
            const int side = -g_rowSide.load();
            g_rowSide = side;
            vr::XrHaptic(0, 0.4f, 0.05f);
            vr::XrHaptic(1, 0.4f, 0.05f);
            Log("manual row: paddle flipped to the %s", side > 0 ? "right" : "left");
        }
        g->wButtons &= (WORD)~XINPUT_GAMEPAD_Y;
        if (g_rowActive.load()) {
            g->wButtons &= (WORD)~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);  // the grips hold the paddle
            // D-308d: the boat's stick is the rowing's; D-315: the player's left stick sideways is in it as the boat's
            // turn (RowTick reads g_padStick), its up / down is not
            {
                g->sThumbLX = AxisS(g_rowX.load());
                g->sThumbLY = AxisS(g_rowY.load());
            }
        }
    }
    yPrev = yNow;
}

// D-308b: the paddle's world pass - built like the axe's: its upper grip exactly in the drawn top fist (ArmsIkResult =
// this frame's wrist, so the pass never lags the hand), its shaft along the smoothed direction to the other hand, its
// face turned toward the bow as the game holds it. Nothing of the game's own (animated) paddle pose is used but its scale,
// so the paddle stays still in the fist
bool ManualPaddlePlace(uint8_t* skel, const float* root, float* out) {
    (void)skel;
    if (!g_rowActive.load() || !root) return false;
    AcquireSRWLockShared(&g_padLock);
    const bool ok = g_padLearned && g_drawValid;
    const V3 gu = g_gripUp, gl = g_gripLow, faceL = g_faceL, topP = g_drawTopP, aW = g_drawDir, faceW = g_drawFace;
    const int top = g_drawTop;
    ReleaseSRWLockShared(&g_padLock);
    if (!ok) return false;
    // D-308d: the grips are where Kratos' PALMS sit (2 - 5 cm off the shaft's middle, on different sides); the shaft itself
    // runs along the paddle's own Z (the two grips: 0.82 m apart in Z, 4 / 1.5 cm in X / Y) - the fist's centre goes on it
    const V3 guA = ShaftPoint(gu), glA = ShaftPoint(gl);
    V3 palm = topP;
    Mat4 wr{}, el{};
    if (ArmsIkResult(top, &wr, &el)) {  // D-308d: the drawn top fist's centre (where a bar in it runs, _handgrip.py)
        const V3 c = kFistCentre[top];
        palm = Pos(wr) + RowOf(wr, 0) * c.x + RowOf(wr, 1) * c.y + RowOf(wr, 2) * c.z;
    }
    const V3 aL = Norm(glA - guA);
    const V3 rL = Norm(faceL - aL * Dot(faceL, aL)), cL = Cross(aL, rL);
    const V3 rW = Norm(faceW - aW * Dot(faceW, aW)), cW = Cross(aW, rW);
    if (Len(aL) < 0.5f || Len(rL) < 0.5f || Len(aW) < 0.5f || Len(rW) < 0.5f) return false;
    // the game's scale of the paddle (row lengths of its root)
    float sc = 0.f;
    for (int i = 0; i < 3; ++i) sc += sqrtf(root[i * 4] * root[i * 4] + root[i * 4 + 1] * root[i * 4 + 1] + root[i * 4 + 2] * root[i * 4 + 2]);
    sc /= 3.f;
    if (!(sc > 0.05f && sc < 20.f)) return false;
    // local axes (aL, rL, cL) -> world (aW, rW, cW): row i of the root = sum_k L[k][i] * W[k]
    const float L[3][3] = {{aL.x, aL.y, aL.z}, {rL.x, rL.y, rL.z}, {cL.x, cL.y, cL.z}};
    const V3 W[3] = {aW, rW, cW};
    Mat4 o{};
    V3 rowv[3];
    for (int i = 0; i < 3; ++i) {
        rowv[i] = (W[0] * L[0][i] + W[1] * L[1][i] + W[2] * L[2][i]) * sc;
        o.m[i][0] = rowv[i].x; o.m[i][1] = rowv[i].y; o.m[i][2] = rowv[i].z; o.m[i][3] = 0.f;
    }
    const V3 org = palm - (rowv[0] * guA.x + rowv[1] * guA.y + rowv[2] * guA.z);
    o.m[3][0] = org.x; o.m[3][1] = org.y; o.m[3][2] = org.z; o.m[3][3] = 1.f;
    memcpy(out, &o, sizeof(o));
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 3)
        Log("manual row: paddle fixed in the %s fist (scale %.3f, grips %.2f m apart on its shaft)", top ? "right" : "left", sc,
            Len(glA - guA) * sc);
    return true;
}

}  // namespace gowvr::fp
