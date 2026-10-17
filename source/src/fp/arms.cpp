// VR arms (VRIK-style) on Kratos' own skeleton. Playbook ch.02: drive the engine's skeleton upstream of skinning (the
// world pass post-hook in body.cpp), hang the arms off the animated body (shoulders stay animated), two-bone solve with a
// deterministic elbow pole, keep the rig's bone lengths, let every child follow its parent's delta, yield in cutscenes.
//
// Rig facts (LIVE, 1099-joint Kratos): right arm clavicle 324 -> shoulder 418 -> elbow 743 -> wrist 789; left 325 ->
// 423 -> 752 -> 801 (joint names by hash below). Hand frame: fingers along +Y, thumb on -Z, palm toward +X (right) /
// -X (left). OpenXR grip frame: +X right, +Y up, fingers along -Z  =>  wrist rows = (-gripX, -gripZ, -gripY).
#include "arms.h"
#include "body.h"
#include "combat.h"
#include "../core/log.h"
#include "../core/config.h"
#include "../vr/xr.h"
#include "../input/vpad.h"
#include <algorithm>
#include <string>
#include <windows.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include <vector>

namespace gowvr::fp {
namespace {

struct ArmDef { uint64_t shoulder, elbow, wrist; };
constexpr ArmDef kArms[2] = {
    {0x05c2b5ca793d4bf2ull, 0xa46a6b3097288180ull, 0x6de47b0b82506234ull},  // left  423 / 752 / 801
    {0xe783988515993319ull, 0x21ead304ebcdad0bull, 0x3626a7d4c72e93aeull},  // right 418 / 743 / 789
};

struct V3 { float x, y, z; };
V3 Row(const Mat4& m, int r) { return {m.m[r][0], m.m[r][1], m.m[r][2]}; }
void SetRow(Mat4& m, int r, V3 v) { m.m[r][0] = v.x; m.m[r][1] = v.y; m.m[r][2] = v.z; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return sqrtf(Dot(a, a)); }
V3 Norm(V3 a) { float l = Len(a); return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 0}; }

// Row-vector rotation taking unit v onto unit u (Rodrigues, transposed for row vectors).
Mat4 RotBetween(V3 v, V3 u) {
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
    V3 k = Cross(v, u);
    float s = Len(k), c = Dot(v, u);
    if (s < 1e-6f) return r;  // parallel (anti-parallel is clamped away by the solver)
    k = k * (1.f / s);
    float t = 1.f - c;
    float col[3][3] = {
        {c + k.x * k.x * t, k.x * k.y * t - k.z * s, k.x * k.z * t + k.y * s},
        {k.y * k.x * t + k.z * s, c + k.y * k.y * t, k.y * k.z * t - k.x * s},
        {k.z * k.x * t - k.y * s, k.z * k.y * t + k.x * s, c + k.z * k.z * t},
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = col[j][i];
    return r;
}

// Rotate a world matrix's basis by row-vector rotation R, keep its position.
void RotateBasis(Mat4& w, const Mat4& R) {
    V3 t = Row(w, 3);
    Mat4 out = Mul(w, R);
    SetRow(out, 3, t);
    out.m[3][3] = 1.f;
    w = out;
}

// Row-vector rotation of `angle` radians about unit axis k.
Mat4 AxisAngle(V3 k, float angle) {
    Mat4 r{};
    float s = sinf(angle), c = cosf(angle), t = 1.f - c;
    float col[3][3] = {
        {c + k.x * k.x * t, k.x * k.y * t - k.z * s, k.x * k.z * t + k.y * s},
        {k.y * k.x * t + k.z * s, c + k.y * k.y * t, k.y * k.z * t - k.x * s},
        {k.z * k.x * t - k.y * s, k.z * k.y * t + k.x * s, c + k.z * k.z * t},
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = col[j][i];
    r.m[3][3] = 1.f;
    return r;
}

V3 Perp(V3 v, V3 axis) { return Norm(v - axis * Dot(v, axis)); }

struct Finger {
    int joint[4] = {-1, -1, -1, -1};  // base (metacarpal, follows the hand) + up to 3 bending joints
    int count = 0;                    // bending joints
    float maxDeg[3] = {0, 0, 0};
    int input = 0;                    // 0 grip (middle/ring/pinky), 1 trigger (index), 2 thumb
};

struct ArmPlan {
    bool on = false;
    int shoulder = -1, elbow = -1, wrist = -1;
    float side = 1.f;          // +1 right hand, -1 left (palm toward +X / -X of the hand frame)
    std::vector<int> subtree;  // shoulder subtree, increasing index (parents first)
    Mat4 target{};             // desired wrist world matrix (rows X,Y,Z + position)
    V3 pole{0, -1, 0};         // preferred elbow direction (world, from the BODY)
    Finger fingers[5];
    int fingerCount = 0;
    float curl[3] = {0, 0, 0};  // grip, trigger, thumb (0 open .. 1 fist)
    bool authoredFingers = false;  // D-049: fingers keep the animation's grip (Blades)
    // Shoulders: the clavicle is held at a fixed pose relative to the chest (no
    // animation shrug/bob under the player's arms) and slides toward a target beyond the arm's reach, so the hand
    // reaches the controller now that the body no longer leans.
    int clav = -1, chest = -1;  // the shoulder's parent (clavicle) and its parent
    std::vector<int> clavSub;   // clavicle + everything under it, increasing index
    bool pin = false;
    Mat4 pinLocal{};            // clavicle relative to the chest
    float slideMax = 0.f;       // metres (0 = no slide; needs the pin: an absolute target cannot build up through leaks)
    // Clean arm pose (elbow relative to shoulder, wrist relative to elbow) sampled with the clavicle: the arm bones are
    // built ABSOLUTELY from bone direction + elbow hinge, and the forearm twist is measured against this pose. Built
    // from the previous pass's cache instead, the twist fed back on our own output and flipped at 180 deg (owner:
    // "when i turn the forearms too much they switch and show inverted").
    bool frames = false;
    Mat4 le{}, lw{};
};
struct Plan {
    uint8_t* skel = nullptr;
    ArmPlan arm[2];
    // shoulder capture: the animation's own clavicle pose, sampled while the arms have been authored for a while
    bool capture[2] = {false, false};
    int capClav[2] = {-1, -1}, capChest[2] = {-1, -1};
    int capShoulder[2] = {-1, -1}, capElbow[2] = {-1, -1}, capWrist[2] = {-1, -1};
    uint64_t seq = 0;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Plan g_plan;
std::atomic<uint64_t> g_planSeq{0}, g_appliedSeq{0};
bool g_enabled = true;
// Motion-control weight of the arms (1 = controllers, 0 = the game's animation). Eased (arm_blend_ms) instead of
// switched when an F1 ANIMATE move takes the arms (combat.cpp): the arms snapped between IK and animation.
std::atomic<float> g_ikWeight{1.f};
float g_wristBack = 0.07f;   // grip origin (palm) -> wrist joint, along the fingers
float g_rollShare = 0.6f;
float g_curlTest = -1.f;     // diagnostic override of every curl (curltest <0..1>, <0 = off)    // share of the hand's roll taken by the forearm (the rest stays at the wrist)
uint64_t g_solves = 0;

std::vector<int> Subtree(uint8_t* skel, int root) {
    std::vector<int> out;
    int16_t n = *(int16_t*)(skel + 0x38);
    for (int j = 0; j < n; ++j)
        if (IsDescendant(j, root)) out.push_back(j);
    return out;
}

SRWLOCK g_outLock = SRWLOCK_INIT;
struct IkOut { bool valid = false; Mat4 wrist{}, elbow{}, target{}; };
IkOut g_out[2];

// Pinned clavicle pose per hand (relative to its parent), sampled from the ANIMATION while the arms are authored: our
// world-cache edits leak into later passes (sub_1405908a0 recomputes a joint only when its flags ask for it, otherwise
// the cache keeps what we wrote), so the pose must be taken while nothing of ours is in the cache.
SRWLOCK g_pinLock = SRWLOCK_INIT;
struct Pin { uint8_t* skel = nullptr; bool valid = false; Mat4 local{}, le{}, lw{}; uint64_t samples = 0; };
Pin g_pin[2];

void Orthonormalize(Mat4& m) {
    V3 x = Norm(Row(m, 0)), y = Row(m, 1), z0 = Row(m, 2);
    y = Norm(y - x * Dot(y, x));
    V3 z = Cross(x, y);
    if (Dot(z, z0) < 0.f) z = z * -1.f;  // keep the rig's handedness
    SetRow(m, 0, x);
    SetRow(m, 1, y);
    SetRow(m, 2, z);
}

void CapturePin(uint8_t* skel, int h, int clav, int chest, int sh, int el, int wr) {
    int16_t n = *(int16_t*)(skel + 0x38);
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    if (clav < 0 || chest < 0 || sh < 0 || el < 0 || wr < 0) return;
    if (clav >= n || chest >= n || sh >= n || el >= n || wr >= n) return;
    auto Wc = [&](int j) { return *(const Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40); };
    const Mat4 C = Wc(clav), P = Wc(chest), Sm = Wc(sh), Em = Wc(el), Wm = Wc(wr);
    if (Len(Row(C, 0)) < 0.9f || Len(Row(P, 0)) < 0.9f || Len(Row(Sm, 0)) < 0.9f || Len(Row(Em, 0)) < 0.9f) return;
    Mat4 L = Mul(C, RigidInverse(P));
    if (Len(Row(L, 3)) > 1.f) return;  // a clavicle sits well within a metre of the chest
    const Mat4 LE = Mul(Em, RigidInverse(Sm)), LW = Mul(Wm, RigidInverse(Em));
    AcquireSRWLockExclusive(&g_pinLock);
    Pin& p = g_pin[h];
    if (p.skel != skel || !p.valid) {
        p.skel = skel;
        p.local = L;
        p.le = LE;
        p.lw = LW;
        p.valid = true;
        p.samples = 1;
    } else {  // running average over ~a second of passes: breathing / walk cycles average out
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) {
                p.local.m[r][c] += (L.m[r][c] - p.local.m[r][c]) * 0.005f;
                p.le.m[r][c] += (LE.m[r][c] - p.le.m[r][c]) * 0.005f;
                p.lw.m[r][c] += (LW.m[r][c] - p.lw.m[r][c]) * 0.005f;
            }
        Orthonormalize(p.local);
        Orthonormalize(p.le);
        Orthonormalize(p.lw);
        ++p.samples;
    }
    ReleaseSRWLockExclusive(&g_pinLock);
}

std::atomic<bool> g_armsLocal{true};
std::atomic<int> g_jitter{0};
std::atomic<int> g_reachDiag{0};
// D-040: the axe stance twists the torso (right shoulder back): LIVE armreach with symmetric simulated hands 0.8 m in
// front: left shoulder->target 0.59 m, right 0.87 m -> the right arm fell 0.30 m short (owner: "i cant extend my right
// arm as much as the left"). Turning the clavicles by the ~45 deg stance twist tore the shoulder armor (harness shots);
// instead the stance twist is measured here and the standing body is turned by it (BodyFollowYaw), so the chest faces
// the view. Published for body.cpp: shoulder-line yaw minus the body's yaw.
std::atomic<float> g_stanceYaw{0.f};
std::atomic<bool> g_stanceValid{false};
std::atomic<uint64_t> g_fingerHolds[2]{}, g_curlJumps[2]{};  // diagnostics (armjitter summary)

// D-038: arm joints the pass never recomputes (forearm helpers 784 right / 796 left, 907; LIVE armjitter: they jumped
// 0.5 .. 2.5 m relative to the elbow pass after pass, in the old code too). Nothing of the game refreshes them in most
// passes, so "cache x old parent^-1 x new parent" fed our previous write back in and drifted without bound. Their offset
// to the parent is learned from the GAME's placement (the cache differs from what we wrote last) and held otherwise.
struct PlacedJoint { int j = -1; Mat4 rel{}, written{}; bool haveRel = false, haveWritten = false; };
SRWLOCK g_placedLock = SRWLOCK_INIT;
std::vector<PlacedJoint> g_placed;
static Mat4 PlacedWorld(int j, const Mat4& cur, const Mat4& parentPre, const Mat4& parentNew) {
    AcquireSRWLockExclusive(&g_placedLock);
    PlacedJoint* p = nullptr;
    for (auto& q : g_placed) if (q.j == j) { p = &q; break; }
    if (!p) { g_placed.push_back({}); p = &g_placed.back(); p->j = j; }
    if (!p->haveWritten || memcmp(&cur, &p->written, sizeof(Mat4)) != 0) {  // the game placed it: learn the offset
        const Mat4 rel = Mul(cur, RigidInverse(parentPre));
        if (Len(Row(rel, 3)) < 0.5f) { p->rel = rel; p->haveRel = true; }  // sane (bone-length) offsets only
    }
    const Mat4 w = p->haveRel ? Mul(p->rel, parentNew) : cur;
    p->written = w;
    p->haveWritten = true;
    ReleaseSRWLockExclusive(&g_placedLock);
    return w;
}

// D-088 v2 (owner: "the axe and Blades leave my hands a few mm / cm - only after an attack when I start moving; that
// start-moving animation displaces them"): the held weapons are fixed to the IK WRIST (axe: learned grip x wrist; Blades:
// learned grip x wrist), but the hand around them is not: the finger bases (metacarpals) - and with the Blades every
// finger joint (D-049: the game's own grip) - are rebuilt from the CURRENT animation's locals. An animation that holds
// the hand differently (the run start after an attack, blended in while the combat hold fades) moved the fist around
// the handle. While a hand holds the axe / the Blades its finger joints keep the hand pose learned in a plain idle with
// that weapon (where the grips themselves were learned), whatever the animation does.
struct HandPose { int weapon = 0; int n = 0; int j[24]; Mat4 loc[24]; bool have = false; };
static HandPose g_handPose[2];
static SRWLOCK g_handPoseLock = SRWLOCK_INIT;
static std::atomic<bool> g_handPoseOn{true};
static int HandWeapon(float side) {  // 1 axe, 2 Blades, 0 none - in this hand
    if (CurrentWeapon() == 2) return 2;
    return side > 0.f && AxeInHand() ? 1 : 0;
}
void SolveArm(uint8_t* skel, const ArmPlan& a) {
    int16_t n = *(int16_t*)(skel + 0x38);
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    auto W = [&](int j) -> Mat4& { return *(Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40); };
    // D-038: the animation's LOCAL matrices (first n of the block; world = local x parent world, sub_1405908a0). A pass
    // recomputes a joint only when its flags ask for it, so the cache can hold THIS joint from the previous pass (our
    // edit) next to a freshly recomputed parent: every "old child x old parent^-1 x new parent" built from the cache was
    // garbage in such passes (owner: forearm and fingers glitch standing and moving). Children are rebuilt from locals.
    const uint8_t* locs = PassLocals(skel);
    auto L = [&](int j) -> const Mat4& { return *(const Mat4*)(locs + (size_t)j * 0x40); };
    // ...except joints the pass never recomputes ((flags & 0x43) != 0x43, sub_1405908a0): another system places them and
    // their local is meaningless (LIVE physjoints: forearm helpers 784 / 907 right, 796 left; up to 0.83 m off). Those
    // keep following their parent's delta as before.
    const uint8_t* fl = *(const uint8_t**)(skel + 0x60);
    const bool local = g_armsLocal.load();  // command "armslocal off" = pre-D-038 (parent deltas from the cache), A/B
    auto Fresh = [&](int j) { return local && (!fl || (fl[j] & 0x43) == 0x43); };
    if (a.pin && a.clav >= 0 && a.chest >= 0 && !a.clavSub.empty()) {
        // clavicle: pinned pose on the current chest (absolute, so nothing builds up through the cache), slid toward a
        // target beyond the arm's reach (up to 0.30 m, reach = upper + lower - 0.018 m); its subtree follows
        Mat4 C1 = Mul(a.pinLocal, W(a.chest));
        const V3 sAt = JointParent(a.shoulder) == a.clav ? Row(Mul(L(a.shoulder), C1), 3)
                                                         : Row(Mul(W(a.shoulder), Mul(RigidInverse(W(a.clav)), C1)), 3);
        const float la0 = Len(Row(W(a.elbow), 3) - Row(W(a.shoulder), 3)), lb0 = Len(Row(W(a.wrist), 3) - Row(W(a.elbow), 3));
        const V3 toT = Row(a.target, 3) - sAt;
        const float dist = Len(toT), reach = la0 + lb0 - 0.018f;
        if (a.slideMax > 0.f && dist > reach && dist > 1e-4f) {
            const V3 sh = toT * (fminf(dist - reach, a.slideMax) / dist);
            C1.m[3][0] += sh.x; C1.m[3][1] += sh.y; C1.m[3][2] += sh.z;
        }
        // absolute: the clavicle, then every joint under it from its local on its (already rebuilt) parent
        // (clavSub is in increasing index order and parents precede their children in this rig)
        thread_local std::vector<std::pair<int, std::pair<Mat4, Mat4>>> pre;  // non-recomputed: (joint, parent) pre-edit
        pre.clear();
        for (int j : a.clavSub)
            if (j != a.clav && !Fresh(j)) pre.push_back({j, {W(j), W(JointParent(j))}});
        for (int j : a.clavSub) {
            if (j == a.clav) { W(j) = C1; continue; }
            if (Fresh(j)) { W(j) = Mul(L(j), W(JointParent(j))); continue; }
            for (auto& r : pre)
                if (r.first == j) {
                    W(j) = local ? PlacedWorld(j, r.second.first, r.second.second, W(JointParent(j)))
                                 : Mul(Mul(r.second.first, RigidInverse(r.second.second)), W(JointParent(j)));
                    break;
                }
        }
    }
    Mat4 sOld = W(a.shoulder), eOld = W(a.elbow), wOld = W(a.wrist);
    V3 S = Row(sOld, 3), E = Row(eOld, 3), Wp = Row(wOld, 3);
    float la = Len(E - S), lb = Len(Wp - E);
    if (g_reachDiag.load() > 0) {  // diagnostic "armreach N": why one arm reaches less far than the other
        --g_reachDiag;
        const V3 T0 = Row(a.target, 3), ch = Row(W(a.chest), 3), cl = a.clav >= 0 ? Row(W(a.clav), 3) : V3{0, 0, 0};
        Log("armreach: %s upper %.3f lower %.3f reach %.3f | shoulder->target %.3f (short by %.3f) | clavicle->chest %.3f "
            "shoulder->chest %.3f | pin %d | stance twist %.1f deg", a.side > 0.f ? "R" : "L", la, lb, la + lb,
            Len(T0 - S), fmaxf(Len(T0 - S) - (la + lb - 0.005f), 0.f), Len(cl - ch), Len(S - ch), (int)a.pin,
            g_stanceYaw.load() * 57.2958f);
    }
    V3 T = Row(a.target, 3);
    V3 d = T - S;
    float dist = Len(d);
    float dmin = fabsf(la - lb) + 0.01f, dmax = la + lb - 0.005f;
    float dc = dist < dmin ? dmin : dist > dmax ? dmax : dist;
    V3 nrm = Norm(d);
    if (Len(nrm) < 0.5f) return;
    // elbow on the circle around S->T, towards the body-derived pole
    float x = (la * la - lb * lb + dc * dc) / (2.f * dc);
    float r = sqrtf(fmaxf(la * la - x * x, 0.f));
    V3 p = a.pole - nrm * Dot(a.pole, nrm);
    if (Len(p) < 1e-4f) p = Cross(nrm, V3{0, 0, 1});
    p = Norm(p);
    V3 En = S + nrm * x + p * r;
    V3 Wn = S + nrm * dc;
    Mat4 sNew = sOld, eNew = eOld;
    bool built = false;
    if (a.frames) {
        // ABSOLUTE bone frames (bone axis along the bone, the pole fixes the roll): from the clean
        // pose, each bone's axis toward its child and the elbow hinge in that bone's frame, placed on the solved
        // directions with the solve's bend normal. Nothing is read from the previous pass's (possibly our own) rotation.
        const V3 aS = Norm(Row(a.le, 3)), aE = Norm(Row(a.lw, 3));
        const V3 f0 = Norm(Row(a.le, 0) * aE.x + Row(a.le, 1) * aE.y + Row(a.le, 2) * aE.z);  // forearm, shoulder frame
        V3 nS = Cross(aS, f0);
        const V3 u = Norm(En - S), f = Norm(Wn - En);
        const V3 nW = Norm(Cross(p, nrm));  // bend normal of the solve (= cross(u, f) direction), also for a straight arm
        if (Len(nS) > 0.12f && Len(u) > 0.5f && Len(f) > 0.5f && Len(nW) > 0.5f) {  // clean pose bent by > ~7 deg
            nS = Norm(nS - aS * Dot(nS, aS));
            // the hinge in the elbow frame (rows of le = the elbow's axes in the shoulder frame)
            V3 nE{Dot(nS, Row(a.le, 0)), Dot(nS, Row(a.le, 1)), Dot(nS, Row(a.le, 2))};
            nE = Norm(nE - aE * Dot(nE, aE));
            // mirrored bone frames (det < 0) flip every cross product: checked offline, scratchpad test_place.py
            const float sg = Dot(Row(sOld, 0), Cross(Row(sOld, 1), Row(sOld, 2))) < 0.f ? -1.f : 1.f;
            auto place = [&](V3 aL, V3 nL, V3 dirW, V3 nWo, V3 pos) {
                const V3 bL = Cross(aL, nL), nWp = Norm(nWo - dirW * Dot(nWo, dirW));
                const V3 nImg = nWp * sg, bImg = Cross(dirW, nImg) * sg;
                const V3 A[3] = {aL, nL, bL}, B[3] = {dirW, nImg, bImg};
                Mat4 m{};  // row i = world image of local axis i = sum_k A[k][i] * B[k]
                for (int i = 0; i < 3; ++i) {
                    const V3 e{i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f};
                    SetRow(m, i, B[0] * Dot(e, A[0]) + B[1] * Dot(e, A[1]) + B[2] * Dot(e, A[2]));
                }
                SetRow(m, 3, pos);
                m.m[3][3] = 1.f;
                return m;
            };
            sNew = place(aS, nS, u, nW, S);
            eNew = place(aE, nE, f, nW, En);
            // forearm twist: the hand's rotation about the forearm relative to the clean wrist on this forearm; a share
            // goes on the elbow bone (the rest stays at the wrist), unwrapped and held within a human range
            const Mat4 wu = Mul(a.lw, eNew);
            int k = 0;
            float best = 2.f;
            for (int i = 0; i < 3; ++i) {
                const float dd = fabsf(Dot(Row(wu, i), f));
                if (dd < best) { best = dd; k = i; }
            }
            const V3 pu = Perp(Row(wu, k), f), pt = Perp(Row(a.target, k), f);
            if (Len(pu) > 0.5f && Len(pt) > 0.5f) {
                float tw = atan2f(Dot(Cross(pu, pt), f), Dot(pu, pt));
                static float lastTw[2] = {0.f, 0.f};
                float& l = lastTw[a.side > 0.f ? 1 : 0];
                float dr = tw - l;
                while (dr > 3.14159265f) dr -= 6.2831853f;
                while (dr < -3.14159265f) dr += 6.2831853f;
                tw = fmaxf(-2.6f, fminf(2.6f, l + dr));
                l = tw;
                RotateBasis(eNew, AxisAngle(f, tw * g_rollShare));
            }
            built = true;
        }
    }
    if (!built) {  // no clean pose yet: aim the animated bones (older path)
        RotateBasis(sNew, RotBetween(Norm(E - S), Norm(En - S)));
        eNew = Mul(Mul(eOld, RigidInverse(sOld)), sNew);
        V3 fwdNow = Norm(Row(Mul(Mul(wOld, RigidInverse(sOld)), sNew), 3) - En);
        RotateBasis(eNew, RotBetween(fwdNow, Norm(Wn - En)));
        SetRow(eNew, 3, En);
        V3 fA = Norm(Wp - E), fN = Norm(Wn - En);
        V3 tA = Perp(Row(wOld, 2) * -1.f, fA);
        V3 c{Dot(tA, Row(eOld, 0)), Dot(tA, Row(eOld, 1)), Dot(tA, Row(eOld, 2))};
        V3 pred = Perp(Row(eNew, 0) * c.x + Row(eNew, 1) * c.y + Row(eNew, 2) * c.z, fN);
        V3 tT = Perp(Row(a.target, 2) * -1.f, fN);
        if (Len(pred) > 0.5f && Len(tT) > 0.5f) {
            float roll = atan2f(Dot(Cross(pred, tT), fN), Dot(pred, tT));
            static float last[2] = {0.f, 0.f};
            float& l = last[a.side > 0.f ? 1 : 0];
            float dr = roll - l;
            while (dr > 3.14159265f) dr -= 6.2831853f;
            while (dr < -3.14159265f) dr += 6.2831853f;
            roll = fmaxf(-2.9f, fminf(2.9f, l + dr));
            l = roll;
            RotateBasis(eNew, AxisAngle(fN, roll * g_rollShare));
        }
    }
    float twistRoll = 0.f;
    V3 twistAxis{0, 0, 0};
    Mat4 wNew = a.target;
    SetRow(wNew, 3, Wn);
    // D-088 v2: while this hand holds a weapon, its finger joints use the hand pose learned in a plain idle (HandPose)
    int fj[24];
    int nf = 0;
    for (int f = 0; f < a.fingerCount; ++f)
        for (int k = 0; k <= a.fingers[f].count && nf < 24; ++k)
            if (a.fingers[f].joint[k] >= 0 && a.fingers[f].joint[k] < n) fj[nf++] = a.fingers[f].joint[k];
    Mat4 frozenLoc[24];
    bool useFrozen = false;
    if (const int hw = g_handPoseOn.load() ? HandWeapon(a.side) : 0; hw && nf > 0 && local) {
        const bool learn = CombatHeroIdle() && CombatHoldWeight() <= 0.001f && !ArmsBladesFree() && !HeroMoving();
        AcquireSRWLockExclusive(&g_handPoseLock);
        HandPose& hp = g_handPose[a.side > 0.f ? 1 : 0];
        if (learn) {
            const bool first = !hp.have || hp.weapon != hw || hp.n != nf;
            if (first) {
                hp.weapon = hw;
                hp.n = nf;
                hp.have = true;
                Log("arms: %s hand pose with the %s learned (idle) - kept while it holds it", a.side > 0.f ? "right" : "left",
                    hw == 1 ? "axe" : "Blades");
            }
            for (int i = 0; i < nf; ++i) {
                hp.j[i] = fj[i];
                const Mat4& cur = L(fj[i]);
                if (first) hp.loc[i] = cur;
                else
                    for (int rr = 0; rr < 4; ++rr)
                        for (int c = 0; c < 3; ++c) hp.loc[i].m[rr][c] += (cur.m[rr][c] - hp.loc[i].m[rr][c]) * 0.05f;
            }
        }
        if (hp.have && hp.weapon == hw && hp.n == nf) {
            for (int i = 0; i < nf; ++i) frozenLoc[i] = hp.loc[i];
            useFrozen = true;
        }
        ReleaseSRWLockExclusive(&g_handPoseLock);
    }
    auto LH = [&](int j) -> const Mat4& {  // a hand joint's local: the learned weapon-holding pose while one is held
        if (useFrozen)
            for (int i = 0; i < nf; ++i)
                if (fj[i] == j) return frozenLoc[i];
        return L(j);
    };
    // write, propagating each parent's delta to every other joint of the arm (twist helpers, fingers, held items)
    std::vector<std::pair<int, Mat4>> oldNew;
    oldNew.reserve(a.subtree.size());
    for (int j : a.subtree) {
        Mat4 old = W(j);
        if (j == a.shoulder) W(j) = sNew;
        else if (j == a.elbow) W(j) = eNew;
        else if (j == a.wrist) W(j) = wNew;
        else {
            int par = JointParent(j);
            for (auto& on : oldNew)
                if (on.first == par) {  // D-038: from the local, not the cache (placed-elsewhere joints: parent delta)
                    W(j) = Fresh(j) ? Mul(LH(j), W(par))
                         : local    ? PlacedWorld(j, old, on.second, W(par))
                                    : Mul(Mul(old, RigidInverse(on.second)), W(par));
                    break;
                }
        }
        oldNew.push_back({j, old});
    }
    {   // publish for attached objects (axe on the right wrist, shield on the left forearm)
        AcquireSRWLockExclusive(&g_outLock);
        IkOut& o = g_out[a.side > 0.f ? 1 : 0];
        o.valid = true;
        o.wrist = W(a.wrist);
        o.elbow = W(a.elbow);
        o.target = a.target;
        ReleaseSRWLockExclusive(&g_outLock);
    }
    if (twistRoll != 0.f) {  // twist helpers: direct children of the elbow along the forearm (not the wrist)
        V3 eO = Row(eOld, 3), wO = Row(wOld, 3);
        V3 fo = wO - eO;
        float l2 = Dot(fo, fo);
        for (auto& on : oldNew) {
            int j = on.first;
            if (j == a.wrist || JointParent(j) != a.elbow || l2 < 1e-6f) continue;
            float t = Dot(Row(on.second, 3) - eO, fo) / l2;
            if (t < 0.05f || t > 1.1f) continue;  // not a forearm twist bone
            RotateBasis(W(j), AxisAngle(twistAxis, twistRoll * fminf(t, 1.f)));
        }
    }
    // fingers: absolute pose from the (hand-carried) metacarpal: straight at 0, fist at 1. Every finger bone runs along
    // its joint's +Y; bending turns Y toward -Z about the joint's X, same sign on both hands (MEASURED from the game's
    // own animated grip, tools/re/handaxis.py; visual check tools/re/handviz.py. Bending about Z was sideways.)
    auto oldOf = [&](int j) -> const Mat4* {
        for (auto& on : oldNew) if (on.first == j) return &on.second;
        return nullptr;
    };
    // D-049: holding the Blades the fingers keep the game's own grip (rebuilt from the animation's locals above), like
    // the Blades themselves: hand and handle come from the same authored pose, so the handle is in the fist. The
    // procedural fist left the handles in front of / beside the fingers (owner; harness side view).
    for (int f = 0; f < a.fingerCount && !a.authoredFingers; ++f) {
        const Finger& fi = a.fingers[f];
        Mat4 prev = W(fi.joint[0]);
        const Mat4* prevOld = oldOf(fi.joint[0]);
        for (int k = 1; k <= fi.count; ++k) {
            int j = fi.joint[k];
            const Mat4* o = oldOf(j);
            if (!o || !prevOld) break;
            float len = Len(Row(L(j), 3));  // bone length from the local (D-038)
            if (JointParent(j) != fi.joint[k - 1]) len = Len(Row(*o, 3) - Row(*prevOld, 3));
            float phi = (4.f + (fi.maxDeg[k - 1] - 4.f) * a.curl[fi.input]) * 0.0174533f;  // slight relaxed bend
            float s = sinf(phi), c = cosf(phi);
            V3 Y = Row(prev, 1), Z = Row(prev, 2);
            Mat4 m = prev;
            SetRow(m, 1, Y * c - Z * s);
            SetRow(m, 2, Z * c + Y * s);
            SetRow(m, 3, Row(prev, 3) + Y * len);
            W(j) = m;
            prev = m;
            prevOld = o;
        }
    }
}

// Finger chains below the wrist: bases are wrist children with a chain; thumb = the 2-joint chain; the other four are
// ordered from the thumb side (-Z of the hand) = index, middle, ring, pinky.
int BuildFingers(uint8_t* skel, int wrist, Finger* out) {
    int16_t n = *(int16_t*)(skel + 0x38);
    Mat4 w{};
    if (!JointWorld(wrist, &w)) return 0;
    struct C { int base; std::vector<int> chain; float z; };
    std::vector<C> cands;
    for (int b = 0; b < n; ++b) {
        if (JointParent(b) != wrist) continue;
        std::vector<int> chain;
        int cur = b;
        for (int guard = 0; guard < 4; ++guard) {
            int next = -1;
            for (int j = cur + 1; j < n; ++j)
                if (JointParent(j) == cur) { next = j; break; }
            if (next < 0) break;
            chain.push_back(next);
            cur = next;
        }
        if (chain.size() < 2) continue;
        Mat4 bm{};
        JointWorld(b, &bm);
        cands.push_back({b, chain, Dot(Row(bm, 3) - Row(w, 3), Row(w, 2))});
    }
    int count = 0;
    for (auto& c : cands)
        if (c.chain.size() == 2 && count < 5) {  // thumb
            Finger& f = out[count++];
            f.joint[0] = c.base; f.joint[1] = c.chain[0]; f.joint[2] = c.chain[1]; f.count = 2;
            f.maxDeg[0] = 30.f; f.maxDeg[1] = 45.f;
            f.input = 2;
        }
    std::vector<C> four;
    for (auto& c : cands) if (c.chain.size() >= 3) four.push_back(c);
    std::sort(four.begin(), four.end(), [](const C& x, const C& y) { return x.z < y.z; });
    for (size_t i = 0; i < four.size() && count < 5; ++i) {
        Finger& f = out[count++];
        f.joint[0] = four[i].base;
        for (int k = 0; k < 3; ++k) f.joint[k + 1] = four[i].chain[k];
        f.count = 3;
        f.maxDeg[0] = 80.f; f.maxDeg[1] = 95.f; f.maxDeg[2] = 65.f;
        f.input = i == 0 ? 1 : 0;  // index follows the trigger, the rest the grip
    }
    return count;
}

// Controller-specific wrist pitch (owner report: PSVR2 hand bent like Count Dooku's hilt). Degrees, positive = the
// hand's pointing direction rotated up about the grip's X axis. Per headset family, stored in gowvr.ini.
// Yaw (about the grip's up axis) and roll (about its pointing axis) complete a per-character 6-DoF hand offset
//; mirrored for the left hand so one value adjusts both hands symmetrically.
float g_pitchDeg = 0.f, g_yawDeg = 0.f, g_rollDeg = 0.f;
const char* g_pitchKey = "wristpitch_other";
const char* g_family = "other";
std::string FamilyKey(const char* what) { return std::string(what) + "_" + g_family; }

}  // namespace

float ArmsIkWeight() { return g_ikWeight.load(); }

bool ArmsIkResult(int hand, Mat4* wrist, Mat4* elbow) {
    bool on;
    AcquireSRWLockShared(&g_lock);
    on = g_plan.arm[hand].on;
    ReleaseSRWLockShared(&g_lock);
    if (!on) return false;
    AcquireSRWLockShared(&g_outLock);
    bool ok = g_out[hand].valid;
    Mat4 lastWrist{}, lastTarget{};
    if (ok) { lastWrist = g_out[hand].wrist; lastTarget = g_out[hand].target; *elbow = g_out[hand].elbow; }
    ReleaseSRWLockShared(&g_outLock);
    if (!ok) return false;
    // This frame's wrist = this frame's target (the object pass may run before this frame's arm solve: using the last
    // solve lagged a frame -> the axe wobbled in the fist), shifted by the last solve's reach clamp.
    Mat4 t;
    AcquireSRWLockShared(&g_lock);
    t = g_plan.arm[hand].target;
    ReleaseSRWLockShared(&g_lock);
    for (int j = 0; j < 3; ++j) t.m[3][j] += lastWrist.m[3][j] - lastTarget.m[3][j];
    *wrist = t;
    return true;
}

// D-034: Kratos' held weapons ride on attach joints that are children of joint 4 (the root), not of the hands - LIVE:
// JOMeleeWeapon2 = 226 (right, axe / right blade), JOMeleeWeapon1 = 227 (left), siblings 202..241. The animation puts
// them in the animated hands; with the arm IK on, held Blades floated where the animated hand would be (owner: "the
// chains/blades" misaligned). Every such attach joint that sat in a hand before the IK is moved rigidly with that hand.
// D-035: joint 4 also parents skeleton bones - 202 is the pelvis (parent of the spine/legs, e.g. JOBack1 271) and 243.
// A hand hanging at the side is within 0.35 m of the pelvis, so D-034 dragged it along ("lower body moves weirdly and
// stretches", owner). Only leaf joints (no children) are attach points; command "attachold on" restores D-034 (test).
static std::vector<int> g_attachJoints;
static std::vector<int> g_attachParents;  // joint-4 children that have children: never moved
static std::atomic<uint8_t*> g_attachSkel{nullptr};
static SRWLOCK g_attachLock = SRWLOCK_INIT;
static std::atomic<bool> g_attachOld{false};
static std::atomic<uint64_t> g_heldSig[2]{};
static std::atomic<int> g_heldLogs{0};
void ArmsAttachOld(bool on) {
    g_attachOld = on;
    g_heldLogs = 0;
    Log("arms: attach joints with children %s (attachold)", on ? "MOVED with the hands (D-034 behaviour)" : "kept");
}
static std::atomic<bool> g_attachMove{true};   // command "attachmove on|off" (A/B test of D-034)
struct BladeGrip {
    int j = -1, hand = 0; float w = 1.f; Mat4 grip{}; ULONGLONG since = 0;
    int rotMiss = 0;  // D-071: consecutive idle passes on the grip's spot but > 25 deg off it
    float settleMs = 0.f, missMs = 0.f; ULONGLONG lastLearn = 0;  // D-078: learnable (standing idle) time only
    Mat4 acc{}, cand{}; bool accHave = false; int candN = 0;  // D-051: attack pose filter (offset to the animated wrist)
    Mat4 bogusRel{}; bool bogusHave = false;                    // D-051: where the bogus passes put it (chest frame)
};
static std::vector<BladeGrip> g_bladeGrip;  // D-045, guarded by g_gripLock
static std::atomic<uint64_t> g_bogusRejected{0}, g_rawFar{0}, g_rawAll{0};
static std::atomic<bool> g_attackFull{true};  // D-053 A/B: command "attackfull on|off"
// D-074: default OFF (owner: Blades / chains flicker and misalign during attacks; LIVE: steady in the fists with it off)
static std::atomic<bool> g_bladeAttackPose{false};  // A/B "bladeattack on|off"
static std::atomic<bool> g_bladesFreeEff{false};    // D-077: the Blades are on the game's L2 move (until back in its hands)
bool ArmsBladesFree() { return g_bladesFreeEff.load(); }
// D-087 (owner: "press a controller's thumbstick: Kratos flips that Blade and then holds it upside down - left stick only
// the left Blade"): per hand an angle about the hand's palm normal (IK wrist row 0, "x outward"; row 2 = up along the
// handle) through the handle in the fist, animated 0 -> 180 deg (reversed) and on to 360 = 0 (normal again) over
// blade_flip_ms with a smoothstep. Applied to that hand's Blade points only (the Blade objects ride on them; the
// forearm chains stay on the forearm), while the Blades sit in the fists (not on the game's L2 moves).
static std::atomic<float> g_flipFrom[2]{0.f, 0.f}, g_flipTo[2]{0.f, 0.f};
static std::atomic<long long> g_flipT0[2]{0, 0};
static double QpcMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static float FlipAngleDeg(int h) {
    static const float dur = config::GetFloat("blade_flip_ms", 320.f);
    const float from = g_flipFrom[h].load(), to = g_flipTo[h].load();
    if (from == to) return to;
    const float t = dur > 1.f ? (float)((QpcMs() - (double)g_flipT0[h].load()) / dur) : 1.f;
    if (t >= 1.f) return to;
    const float s = t <= 0.f ? 0.f : t * t * (3.f - 2.f * t);
    return from + (to - from) * s;
}
void ArmsBladeFlip(int hand) {
    if (hand < 0 || hand > 1) return;
    // D-087 v2 (owner: "the second press should not go on round, it should go back the same way - the faster route"):
    // flipping back turns the Blade back along the same path (180 -> 0) instead of on to 360; a press mid-flip reverses
    const float cur = FlipAngleDeg(hand);
    const bool toReversed = g_flipTo[hand].load() < 90.f;  // where it was heading: normal -> now upside down, and back
    const float to = toReversed ? 180.f : 0.f;
    g_flipFrom[hand] = cur;
    g_flipTo[hand] = to;
    g_flipT0[hand] = (long long)QpcMs();
    Log("arms: %s Blade flipped -> %s", hand ? "right" : "left", toReversed ? "upside down" : "normal grip");
}
void ArmsBladeFlipReset() {
    for (int h = 0; h < 2; ++h) { g_flipFrom[h] = 0.f; g_flipTo[h] = 0.f; }
}
bool ArmsBladeFlipped() {
    for (int h = 0; h < 2; ++h)
        if (fabsf(fmodf(FlipAngleDeg(h), 360.f)) > 0.5f) return true;
    return false;
}
// row-vector rotation by `deg` about `axis` (unit) through `pivot`: p' = p x M
static Mat4 RotAbout(V3 axis, float deg, V3 pivot) {
    const float a = deg * 0.0174533f, c = cosf(a), s = sinf(a), t = 1.f - c;
    const float x = axis.x, y = axis.y, z = axis.z;
    const float R[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                           {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                           {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};  // column convention
    Mat4 M{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) M.m[i][j] = R[j][i];
    const V3 pr{pivot.x * M.m[0][0] + pivot.y * M.m[1][0] + pivot.z * M.m[2][0],
                pivot.x * M.m[0][1] + pivot.y * M.m[1][1] + pivot.z * M.m[2][1],
                pivot.x * M.m[0][2] + pivot.y * M.m[1][2] + pivot.z * M.m[2][2]};
    M.m[3][0] = pivot.x - pr.x; M.m[3][1] = pivot.y - pr.y; M.m[3][2] = pivot.z - pr.z; M.m[3][3] = 1.f;
    return M;
}
static std::atomic<bool> g_settleV2{true};  // D-078 A/B "gripsettle new|old"
void ArmsGripSettleV2(bool on) { g_settleV2 = on; Log("arms: Blades grip settling %s", on ? "NEW (D-078, standing time)" : "OLD (D-071/D-072)"); }
void ArmsBladeAttackPose(bool on) { g_bladeAttackPose = on; Log("arms: Blades take the game's attack pose %s", on ? "ON" : "OFF (stay in the fists)"); }
void ArmsAttackFull(bool on) { g_attackFull = on; Log("arms: Blades follow the full attack animation %s", on ? "ON" : "OFF"); }
static std::atomic<bool> g_atkFilter{false};  // measured at draw time: no difference (drawcheck) -> off by default  // D-051 A/B: command "atkfilter on|off"
void ArmsAttackFilter(bool on) { g_atkFilter = on; Log("arms: Blades attack-pose filter %s", on ? "ON" : "OFF"); }
static SRWLOCK g_gripLock = SRWLOCK_INIT;
void ArmsGripReset() {
    AcquireSRWLockExclusive(&g_gripLock);
    const size_t n = g_bladeGrip.size();
    g_bladeGrip.clear();
    ReleaseSRWLockExclusive(&g_gripLock);
    Log("arms: Blades grips forgotten (%zu joints) - re-created at the next draw", n);
}
static std::atomic<int> g_bladeDiag{0};
static std::atomic<int> g_shrinkA{-1}, g_shrinkB{-1};
// diagnostic "bladecheck ref|seg|off <tag>": Blades weapon points (225/227 right, 226/228 left) in the IK wrist frame
static std::atomic<bool> g_bcOn{false};
thread_local int t_heldDbg[2][24];
thread_local int t_nHeldDbg[2] = {0, 0};
static int g_bcGoodLogged = 0;
static SRWLOCK g_bcLock = SRWLOCK_INIT;
static int g_bcIdx[4] = {-1, -1, -1, -1};
static V3 g_bcRef[4];
static bool g_bcHaveRef[4] = {};
static float g_bcMaxDev[4], g_bcSumDev[4], g_bcMaxDist[4], g_bcMinScale[4];
static int g_bcBad[4], g_bcN[4], g_bcJumps[4];
static V3 g_bcPrev[4];
static bool g_bcHavePrev[4] = {};
static uint64_t g_bcPasses = 0;
static void BcResetSeg() {
    for (int k = 0; k < 4; ++k) {
        g_bcMaxDev[k] = g_bcSumDev[k] = g_bcMaxDist[k] = 0.f; g_bcMinScale[k] = 9.f; g_bcBad[k] = g_bcN[k] = g_bcJumps[k] = 0;
    }
    g_bcPasses = 0;
}
void ArmsBladeCheck(const char* mode, const char* tag) {
    AcquireSRWLockExclusive(&g_bcLock);
    if (!strcmp(mode, "ref")) {  // new reference (from the next pass) + new segment
        for (int k = 0; k < 4; ++k) g_bcHaveRef[k] = false;
        BcResetSeg();
        g_bcOn = true;
        Log("bladecheck: reference reset (%s)", tag);
    } else if (!strcmp(mode, "seg")) {  // report the running segment, start the next one
        static const char* nm[4] = {"R225", "L226", "R227", "L228"};
        char line[512];
        const uint64_t rf = g_rawFar.exchange(0), ra = g_rawAll.exchange(0);
        int o = sprintf_s(line, "bladecheck: [%s] passes %llu weapon %d bogus-rejected %llu raw-far %llu/%llu |", tag,
                          (unsigned long long)g_bcPasses, CurrentWeapon(), (unsigned long long)g_bogusRejected.exchange(0),
                          (unsigned long long)rf, (unsigned long long)ra);
        for (int k = 0; k < 4; ++k)
            o += sprintf_s(line + o, sizeof(line) - o, " %s maxdev %.3f avg %.3f bad %d/%d dist<=%.2f jumps %d |", nm[k],
                           g_bcMaxDev[k], g_bcN[k] ? g_bcSumDev[k] / g_bcN[k] : 0.f, g_bcBad[k], g_bcN[k], g_bcMaxDist[k],
                           g_bcJumps[k]);
        Log("%s", line);
        BcResetSeg();
        g_bcOn = true;
    } else {
        g_bcOn = false;
    }
    ReleaseSRWLockExclusive(&g_bcLock);
}
// D-048: per hand, the animated wrist and the correction animated wrist -> motion-controlled wrist of the last pass
static SRWLOCK g_handDeltaLock = SRWLOCK_INIT;
static Mat4 g_handAnim[2], g_handDelta[2];
static ULONGLONG g_handDeltaFrame[2] = {0, 0};
bool ArmsHandCorrection(const float* pos, Mat4* delta) {
    AcquireSRWLockShared(&g_handDeltaLock);
    const ULONGLONG now = GetTickCount64();
    int best = -1;
    float bd = 1.6f * 1.6f;
    for (int h = 0; h < 2; ++h) {
        if (!g_handDeltaFrame[h] || now - g_handDeltaFrame[h] > 200) continue;
        const float dx = pos[0] - g_handAnim[h].m[3][0], dy = pos[1] - g_handAnim[h].m[3][1], dz = pos[2] - g_handAnim[h].m[3][2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd) { bd = d2; best = h; }
    }
    if (best >= 0) *delta = g_handDelta[best];
    ReleaseSRWLockShared(&g_handDeltaLock);
    return best >= 0;
}
static std::atomic<bool> g_bladeDiagAll{false}; // "bladediag N all": every attach joint, any distance
// D-049: the handle shift (D-046/047) is off by default; the fingers keep the game's grip instead. Command
// "fistcenter on|off" switches between the two (on = procedural fist + handle shift).
static std::atomic<bool> g_fistCenter{false};
std::atomic<bool> g_bladesAuthoredFingers{true};
void ArmsFistCenter(bool on) {
    g_fistCenter = on;
    g_bladesAuthoredFingers = !on;
    Log("arms: Blades %s", on ? "procedural fist + handle through the fist (D-047)" : "fingers keep the game's grip (D-049)");
}
void ArmsShrink(int a, int b) { g_shrinkA = a; g_shrinkB = b; Log("shrinkj: joints %d..%d shrunk (after the arm edits)", a, b); }
void ArmsBladeDiagAll(bool on) { g_bladeDiagAll = on; }
void ArmsBladeDiag(int n) { g_bladeDiag = n; Log("bladediag: next %d hero passes", n); }
static std::atomic<int> g_attachDiag{0};      // command "attachdiag <passes>": per-pass wrist / attach joint positions
void ArmsAttachMove(bool on) { g_attachMove = on; Log("arms: attach joints follow the hands %s", on ? "ON" : "OFF"); }
void ArmsAttachDiag(int n) { g_attachDiag = n; Log("attachdiag: next %d hero passes", n); }
static void FindAttachJoints(uint8_t* skel) {
    if (skel == g_attachSkel.load()) return;
    AcquireSRWLockExclusive(&g_attachLock);
    if (skel == g_attachSkel.load()) { ReleaseSRWLockExclusive(&g_attachLock); return; }
    g_attachJoints.clear();
    g_attachParents.clear();
    const int j4 = FindJointByHash(0x169277bfd5da3f44ull);
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (j4 >= 0) {
        std::vector<char> hasKids((size_t)n, 0);
        for (int j = 0; j < n; ++j) {
            const int p = JointParent(j);
            if (p >= 0 && p < n) hasKids[(size_t)p] = 1;
        }
        for (int j = 0; j < n; ++j)
            if (JointParent(j) == j4) (hasKids[(size_t)j] ? g_attachParents : g_attachJoints).push_back(j);
    }
    g_attachSkel = skel;
    std::string parents;
    for (int j : g_attachParents) parents += " " + std::to_string(j);
    ReleaseSRWLockExclusive(&g_attachLock);
    Log("arms: %zu attach joints under joint %d follow the motion-controlled hands when held; skeleton bones never "
        "moved:%s", g_attachJoints.size(), j4, parents.c_str());
}

// D-038: world of joint j from the animation's locals alone (root param x locals down the chain), never from the cache,
// which may still hold our previous pass's edit of that joint (see SolveArm).
static Mat4 AnimWorldOf(uint8_t* mats, const uint8_t* locs, int n, int j) {
    int chain[64];
    int k = 0, c = j;
    while (c > 0 && k < 64) { chain[k++] = c; c = JointParent(c); }
    Mat4 M = *(const Mat4*)(mats + ((size_t)(uint16_t)n + (c < 0 ? 0 : c)) * 0x40);
    for (int i = k - 1; i >= 0; --i) M = Mul(*(const Mat4*)(locs + (size_t)chain[i] * 0x40), M);
    return M;
}

void ArmsApply(uint8_t* skel) {
    AcquireSRWLockShared(&g_lock);
    if (skel && skel == g_plan.skel) FindAttachJoints(skel);
    // D-038: attach joints (weapons, the forearm chains) held by a hand, decided on the ANIMATED pose (locals) so every
    // pass of a frame decides the same (LIVE owner log: the held sets alternated between two passes every frame, the
    // left hand even taking the right weapon joint 226); each joint goes to the nearer hand only.
    struct HeldJ { int j; Mat4 anim; float w; };
    HeldJ heldBy[2][24];
    int nHeldBy[2] = {0, 0};
    Mat4 wristAnim[2];
    bool haveWristAnim[2] = {false, false};
    if (skel && skel == g_plan.skel) {
        const int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        const uint8_t* locs = PassLocals(skel);
        for (int h = 0; h < 2; ++h)
            if (g_plan.arm[h].on && g_plan.arm[h].wrist >= 0 && g_plan.arm[h].wrist < n) {
                wristAnim[h] = AnimWorldOf(mats, locs, n, g_plan.arm[h].wrist);
                haveWristAnim[h] = true;
            }
        if (g_attachMove.load() && (haveWristAnim[0] || haveWristAnim[1])) {
            AcquireSRWLockShared(&g_attachLock);
            // D-041: only real held-weapon joints. LIVE (harness, moving hands): the left hand also took the Blades'
            // holster points (228/239/240/...) and the right weapon point 226 - the sheathed Blades and a second axe head
            // floated around the left forearm. Allowed: the forearm chains (two hash families, 10 joints each, on their
            // own forearm) and the weapon points JOMeleeWeapon2 (right) / JOMeleeWeapon1 (left) when in that hand.
            const uint64_t* hashes = *(const uint64_t**)(skel + 0x30);
            // D-044: the Blades hang on weapon/attach points that sit in the ANIMATED hands. LIVE bladediag (Blades,
            // Bare and Axe mode alike): 226 (JOMeleeWeapon2) and 228/240 at the LEFT animated wrist, 227
            // (JOMeleeWeapon1) and 225 at the RIGHT one - D-041 had the sides swapped, so held Blades never followed
            // the motion-controlled hands (owner: "they float around and stop moving"). Only in the game's Blades mode
            // (fp::CurrentWeapon; in Axe mode these points carried stray meshes - D-041), each to the hand it is in.
            const bool bladesMode = CurrentWeapon() == 2;
            // D-045: a FIXED grip per Blades joint, like the axe's learned grip. Using the animated offset to the wrist
            // every pass copied the animation onto the hands: the idle bobbed the Blades "a few pixels" (owner) and the
            // walk animation swung / uncoiled them away from the wrist, where they were then dropped (LIVE bladediag:
            // chains 205-224 left the 0.4 m zone while walking). Each joint's offset (anim x wrist^-1) is eased in
            // while the Blades settle in the hands, frozen after 1 s, and kept until the Blades are put away.
            const ULONGLONG nowMs = GetTickCount64();
            AcquireSRWLockExclusive(&g_gripLock);
            // D-050: the grip is kept across put-away / draw / weapon switches (LIVE bladesuite: re-learning it at every
            // draw froze mid-draw-animation poses, 0.2-0.8 m off); it only matters in Blades mode
            // D-056: LIVE drawtrace: while the combat hold fades out after an attack (weight 0.95 -> 0.14, ~250 ms) the pass
            // locals are a mix of the frozen and the live pose and the Blades followed it 0.25-0.48 m off the hands
            // ("misaligns for a second, then corrects"). The attack rule applies only at full hold; the grip learns only
            // with no hold at all.
            const float holdW = CombatHoldWeight();
            // D-077 (owner: L2+R1 "after the animation 2 s misaligned, floating to the right"; L2+R2 hold "when the blade
            // gets close to the fist it sticks like glue"): LIVE the L2 moves play on an upper-body layer - L2+R2 hold showed
            // only idle, L2+R1 only aimwalk - so the anim-name rule missed them. The Blades are "free" (the game's own pose,
            // no grip) from the input (BladesFreeRaw: L2+R2 held, the throw gesture) or the move names, and stay free until
            // the game's Blade points are back at its animated wrists (the closest per 30 ms window: ~44 % of the passes put
            // them 0.7 m off even idle - D-050) for 80 ms, after having been out; then the fists take them at once
            {
                static bool eff = false, sawOut = false;
                static ULONGLONG rawEnd = 0, winT = 0, backSince = 0, outSince = 0;
                static float winMin = 1e9f;
                static int jp[2] = {-2, -2};
                if (jp[0] == -2) { jp[0] = FindJointByHash(0x08eae59373302b22ull); jp[1] = FindJointByHash(0x6fc442db7837895aull); }
                const ULONGLONG t = GetTickCount64();
                const bool raw = bladesMode && BladesFreeRaw();
                if (raw) {
                    if (!eff) { eff = true; sawOut = false; outSince = 0; Log("arms: Blades free (the game's L2 move)"); }
                    rawEnd = 0;
                    backSince = 0;
                }
                if (eff) {
                    float dOut = 0.f;  // the farther of the two Blade points from its nearest animated wrist
                    for (int b = 0; b < 2; ++b) {
                        if (jp[b] < 0 || jp[b] >= n) continue;
                        const Mat4 P = AnimWorldOf(mats, locs, n, jp[b]);
                        float best = 1e9f;
                        for (int h = 0; h < 2; ++h) {
                            if (!haveWristAnim[h]) continue;
                            const float dx = P.m[3][0] - wristAnim[h].m[3][0], dy = P.m[3][1] - wristAnim[h].m[3][1],
                                        dz = P.m[3][2] - wristAnim[h].m[3][2];
                            best = fminf(best, sqrtf(dx * dx + dy * dy + dz * dz));
                        }
                        if (best < 1e8f) dOut = fmaxf(dOut, best);
                    }
                    winMin = fminf(winMin, dOut);
                    if (t - winT >= 30) {
                        if (winMin > 0.35f) { if (!outSince) outSince = t; if (t - outSince >= 60) sawOut = true; }
                        else outSince = 0;
                        if (!raw) {
                            if (!rawEnd) rawEnd = t;
                            if (winMin < 0.2f) { if (!backSince) backSince = t; }
                            else backSince = 0;
                            const bool back = backSince && t - backSince >= 80;
                            if ((sawOut && back) || (!sawOut && t - rawEnd > 700) || t - rawEnd > 4000) {
                                eff = false;
                                Log("arms: Blades back in the fists (%s, %llu ms after the move)", sawOut && back ? "returned" :
                                    t - rawEnd > 4000 ? "timeout" : "never left", (unsigned long long)(t - rawEnd));
                            }
                        }
                        winT = t;
                        winMin = 1e9f;
                    }
                }
                g_bladesFreeEff = eff;
            }
            const bool bladesFree = g_bladesFreeEff.load();
            // D-074 "bladeattack on|off": off = in held attacks the Blades stay in the fists (no game attack pose), so the
            // chain pins never jump with the alternating bogus passes (owner: "do an attack, you will see it flicker")
            const bool attacking = holdW >= 0.999f && g_bladeAttackPose.load();
            const bool learnOk = holdW <= 0.001f && !bladesFree;
            const int chestJ = g_plan.arm[0].chest >= 0 && g_plan.arm[0].chest < n ? g_plan.arm[0].chest : -1;
            const Mat4 chestAnim = chestJ >= 0 ? AnimWorldOf(mats, locs, n, chestJ) : Mat4{};
            auto consider = [&](int j) {
                if (bladesMode && bladesFree && hashes && j < n) {
                    const uint64_t fam = hashes[j] >> 16;
                    const bool forearm = fam == (0xb6bb38650c362623ull >> 16) || fam == (0x2af813186d6c1e78ull >> 16);
                    if (!forearm) {  // Blade points / hanging chain: the game's pose stays (no grip created either)
                        for (auto& g : g_bladeGrip)
                            if (g.j == j) g.accHave = false;
                        return;
                    }
                }
                if (bladesMode) {
                    for (auto& g : g_bladeGrip)
                        if (g.j == j) {
                            if (!haveWristAnim[g.hand]) return;
                            // D-048: thrown on the chain (attack animation puts it > 0.25 m from the animated hand):
                            // follow the game's throw relative to our hand instead of pinning it in the fist
                            // D-050: only during an attack move. LIVE bladecheck: in ~44 % of the hero's (normal) passes
                            // the game's locals put these points 0.67-0.75 m from the hand even standing idle; following
                            // them made the Blades jump out of the fists every other pass
                            const Mat4 Acur = AnimWorldOf(mats, locs, n, j);
                            if (attacking) {
                                // D-051: during attacks the game's pose is followed (throws), but the alternating bogus
                                // passes exist here too (~0.7 m jump for one pass, then back): a jump > 15 cm from the
                                // last accepted offset only counts once 3 passes in a row agree on it
                                // D-051: the bogus passes put the point at a FIXED spot on the body (learned below
                                // while idle, relative to the chest); during attacks a pass landing there is rejected
                                // (the last accepted pose is kept), every other pass - the real throw - is followed
                                const Mat4 off = Mul(Acur, RigidInverse(wristAnim[g.hand]));
                                bool bogus = false;
                                if (g_atkFilter.load() && g.bogusHave && chestJ >= 0) {
                                    const V3 bp = Row(Mul(g.bogusRel, chestAnim), 3);
                                    bogus = Len(Row(Acur, 3) - bp) < 0.12f;
                                }
                                if (bogus) ++g_bogusRejected;
                                if (!bogus || !g.accHave) { g.acc = off; g.accHave = true; }
                                // D-053: during attacks EVERY Blades joint (chain links included) takes the game's pose on
                                // our hand - chains and the thrown Blade stay one piece (owner: Blade floating beside the
                                // hand while the chain hangs from the fist); before, only joints > 0.25 m out did
                                if (g_attackFull.load() || Len(Row(g.acc, 3)) > 0.25f) {  // the accepted pose on our hand
                                    if (nHeldBy[g.hand] < 24)
                                        heldBy[g.hand][nHeldBy[g.hand]++] = {j, Mul(g.acc, wristAnim[g.hand]), 1.f};
                                    return;
                                }
                            } else {
                                g.accHave = false;
                                g.candN = 0;
                                // idle: a pass far from the hand is a bogus one -> remember where on the body it lands
                                if (Len(Row(Acur, 3) - Row(wristAnim[g.hand], 3)) > 0.3f) ++g_rawFar;
                                ++g_rawAll;
                                if (chestJ >= 0 && Len(Row(Acur, 3) - Row(wristAnim[g.hand], 3)) > 0.3f) {
                                    const Mat4 rel = Mul(Acur, RigidInverse(chestAnim));
                                    if (!g.bogusHave) { g.bogusRel = rel; g.bogusHave = true; }
                                    else for (int r = 0; r < 4; ++r)
                                        for (int c = 0; c < 3; ++c) g.bogusRel.m[r][c] += (rel.m[r][c] - g.bogusRel.m[r][c]) * 0.05f;
                                }
                            }
                            // D-050: learn only from passes that agree with the grip (within 12 cm; the alternating 0.7 m
                            // passes and draw / put-away poses never count): quickly for 1 s after it was created, then
                            // very slowly (~2 s time constant: no idle bob on the hands)
                            // D-072 (owner: "you made the right blade position bad again"): only while Kratos stands
                            // still - the walk animation turns the Blades in the hands, and ~1 s of walking re-settled
                            // the grip onto the tilted walking pose
                            if (learnOk && !HeroMoving()) {
                                const Mat4 cur = Mul(Acur, RigidInverse(wristAnim[g.hand]));
                                // D-063: and the rotation must agree too (within ~25 deg): position-only, a Blade turned
                                // 90 deg in the fist (throw / aim poses) was slowly learned - owner: "the right blade is
                                // tilted, laying flat, like it was moved 90 degrees"
                                const float rotDot = (Row(cur, 0).x * Row(g.grip, 0).x + Row(cur, 0).y * Row(g.grip, 0).y +
                                                      Row(cur, 0).z * Row(g.grip, 0).z +
                                                      Row(cur, 1).x * Row(g.grip, 1).x + Row(cur, 1).y * Row(g.grip, 1).y +
                                                      Row(cur, 1).z * Row(g.grip, 1).z) * 0.5f;  // mean axis cosine
                                // D-071 (owner: "you broke the right blade position"): the rotation gate only after the
                                // settling phase - the grip is first taken mid-draw (LIVE: the right Blade lay across
                                // the view, never corrected because every good pass was > 25 deg from that first grip)
                                const float dPos = Len(Row(cur, 3) - Row(g.grip, 3));
                                if (g_settleV2.load()) {
                                    // D-078 (owner: "you misaligned the right blade again like before"): LIVE r77 - the
                                    // game's weapon mode turns Blades AT the switch press (axe -> Blades): the grips were
                                    // created 10 ms after it, from the sheathed Blades near the hand; the draw then put the
                                    // Blade > 12 cm from that grip, so neither the 2.5 s settling nor the D-071 re-settle
                                    // ever took the real one (right Blade on its side, floating above the fist, for good).
                                    // Now the grip learns only in a plain idle (no stance, not aiming - L2 turns the Blade
                                    // in the fist on an upper-body layer under the same idle name), with the joint at the
                                    // hand (< 0.32 m: the bogus passes are ~0.7 m); the settling counts that time only and
                                    // takes any such pose; afterwards a grip that disagrees (spot > 12 cm or > 25 deg) for
                                    // 1 s of it settles again
                                    const float step = g.lastLearn && nowMs > g.lastLearn ? (float)std::min<ULONGLONG>(nowMs - g.lastLearn, 50) : 0.f;
                                    g.lastLearn = nowMs;
                                    const bool idle = CombatHeroIdle() && input::GameLeftTrigger() < 0.15f;
                                    if (idle && Len(Row(cur, 3)) < 0.32f) {
                                        const bool settling = g.settleMs < 2500.f;
                                        g.settleMs += step;
                                        const bool agree = dPos < 0.12f && rotDot > 0.9f;
                                        if (!settling && !agree) {
                                            g.missMs += step;
                                            if (g.missMs >= 1000.f) {
                                                g.settleMs = 0.f;
                                                g.missMs = 0.f;
                                                Log("arms: Blades joint %d grip re-settling (%.2f m / %.0f deg off for 1 s of idle)", g.j, dPos,
                                                    acosf(fminf(fmaxf(rotDot, -1.f), 1.f)) * 57.2958f);
                                            }
                                        } else if (agree) {
                                            g.missMs = 0.f;
                                        }
                                        if (settling || agree) {
                                            const float k = settling ? 0.1f : 0.005f;
                                            for (int r = 0; r < 4; ++r)
                                                for (int c = 0; c < 3; ++c) g.grip.m[r][c] += (cur.m[r][c] - g.grip.m[r][c]) * k;
                                            Orthonormalize(g.grip);
                                        }
                                    }
                                } else {
                                const bool settling = nowMs - g.since < 2500;
                                const bool posOk = dPos < 0.12f;
                                // a grip stuck at a wrong rotation recovers: ~1 s of idle passes that all agree on the
                                // spot but not the rotation -> settle again (fast learning for 2.5 s)
                                if (posOk && !settling && rotDot <= 0.9f) {
                                    if (++g.rotMiss >= 90) {
                                        g.since = nowMs;
                                        g.rotMiss = 0;
                                        Log("arms: Blades joint %d grip rotation re-settling (it disagreed for 90 passes)", g.j);
                                    }
                                } else if (rotDot > 0.9f) {
                                    g.rotMiss = 0;
                                }
                                if (posOk && (settling || rotDot > 0.9f)) {
                                    const float k = nowMs - g.since < 2500 ? 0.1f : 0.005f;
                                    for (int r = 0; r < 4; ++r)
                                        for (int c = 0; c < 3; ++c) g.grip.m[r][c] += (cur.m[r][c] - g.grip.m[r][c]) * k;
                                    Orthonormalize(g.grip);
                                }
                                }
                            } else {
                                g.lastLearn = 0;  // D-078: walking / attacking time does not count as settling
                            }
                            if (nHeldBy[g.hand] < 24)
                                heldBy[g.hand][nHeldBy[g.hand]++] = {j, Mul(g.grip, wristAnim[g.hand]), g.w};
                            return;
                        }
                }
                if (j >= n || !hashes) return;
                const uint64_t hs = hashes[j], fam = hs >> 16;
                const bool chain = fam == (0xb6bb38650c362623ull >> 16) || fam == (0x2af813186d6c1e78ull >> 16) ||
                                   hs == 0xa112bfa7b2a4c278ull || hs == 0x0b69651d25912452ull;
                const bool bladePoint = bladesMode && (hs == 0xb41408d8ad410271ull || hs == 0xb41408d8ad411e00ull ||
                                                       hs == 0x08eae59373302b22ull || hs == 0x6fc442db7837895aull ||
                                                       hs == 0x64d3729a5b874c38ull);
                const int weaponHand = bladePoint ? 2 : -1;  // 2 = whichever hand it is in
                if (!chain && weaponHand < 0) return;
                const Mat4 A = AnimWorldOf(mats, locs, n, j);
                float best = 0.35f * 0.35f;
                int hb = -1;
                for (int h = 0; h < 2; ++h) {
                    if (!haveWristAnim[h]) continue;
                    const float dx = A.m[3][0] - wristAnim[h].m[3][0], dy = A.m[3][1] - wristAnim[h].m[3][1],
                                dz = A.m[3][2] - wristAnim[h].m[3][2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best) { best = d2; hb = h; }
                }
                if (weaponHand >= 0 && (hb < 0 || best > 0.18f * 0.18f)) return;  // weapon point: only when in a hand
                // chains run from the wrist up to the back, where the sheathed Blades hang on their far end: full follow
                // at the wrist, fading to the game's placement 0.30 m away (moved rigidly, the far end dragged the
                // sheathed Blade around the forearm - harness burst)
                const float d = sqrtf(best);
                const float wgt = weaponHand >= 0 ? 1.f : fminf(fmaxf((0.30f - d) / (0.30f - 0.12f), 0.f), 1.f);
                if (hb >= 0 && wgt > 0.f && nHeldBy[hb] < 24) heldBy[hb][nHeldBy[hb]++] = {j, A, wgt};
                if (bladesMode && hb >= 0 && wgt > 0.f && g_bladeGrip.size() < 64) {  // start this joint's fixed grip
                    BladeGrip g;
                    g.j = j;
                    g.hand = hb;
                    g.w = wgt;
                    g.grip = Mul(A, RigidInverse(wristAnim[hb]));
                    g.since = nowMs;
                    g_bladeGrip.push_back(g);
                }
            };
            for (int j : g_attachJoints) consider(j);
            if (g_attachOld.load())
                for (int j : g_attachParents) consider(j);
            {
                static size_t logged = 0;
                if (g_bladeGrip.size() != logged) {
                    logged = g_bladeGrip.size();
                    Log("arms: Blades grip: %zu joints held with a fixed grip", logged);
                }
            }
            ReleaseSRWLockExclusive(&g_gripLock);
            if (g_bladeDiag.load() > 0 && hashes) {  // diagnostic "bladediag N": which attach joints sit in the hands
                --g_bladeDiag;
                std::string s[2];
                for (int j : g_attachJoints) {
                    const Mat4 A = AnimWorldOf(mats, locs, n, j);
                    for (int h = 0; h < 2; ++h) {
                        if (!haveWristAnim[h]) continue;
                        const float d = Len(Row(A, 3) - Row(wristAnim[h], 3));
                        if (d < (g_bladeDiagAll.load() ? 9.f : 0.40f)) {
                            char b[64];
                            sprintf_s(b, " %d(%04llx)%.2f", j, (unsigned long long)(hashes[j] & 0xffff), d);
                            s[h] += b;
                        }
                    }
                }
                Log("bladediag: weapon %d | left wrist:%s | right wrist:%s", CurrentWeapon(), s[0].c_str(), s[1].c_str());
            }
            ReleaseSRWLockShared(&g_attachLock);
        }
    }
    // D-040: stance twist of the shoulder line relative to the body's facing (rendered root), from the pinned shoulders
    // the IK starts from; BodyFollowYaw turns the standing body by it so the CHEST faces the view (arms reach evenly)
    if (skel && skel == g_plan.skel && !(g_plan.arm[0].on && g_plan.arm[1].on && g_plan.arm[0].pin && g_plan.arm[1].pin))
        g_stanceValid = false;  // arms not motion controlled (third person, ANIMATE moves): no stale stance turn
    if (skel && skel == g_plan.skel && g_plan.arm[0].on && g_plan.arm[1].on && g_plan.arm[0].pin && g_plan.arm[1].pin &&
        g_plan.arm[0].chest >= 0 && g_plan.arm[0].chest == g_plan.arm[1].chest) {
        const int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        const uint8_t* locs = PassLocals(skel);
        auto Wc = [&](int j) -> const Mat4& { return *(const Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40); };
        const Mat4 chest = Wc(g_plan.arm[0].chest);
        V3 s[2];
        for (int h = 0; h < 2; ++h) {
            const ArmPlan& a = g_plan.arm[h];
            s[h] = Row(Mul(*(const Mat4*)(locs + (size_t)a.shoulder * 0x40), Mul(a.pinLocal, chest)), 3);
        }
        const V3 rx = Row(Wc(0), 0), v = s[1] - s[0];  // body's right (rendered root); left -> right shoulder
        if (v.x * v.x + v.z * v.z > 0.01f && rx.x * rx.x + rx.z * rx.z > 0.25f) {
            float st = atan2f(-v.z, v.x) - atan2f(-rx.z, rx.x);  // yaw of the shoulder line minus the body's yaw
            while (st > 3.14159265f) st -= 6.2831853f;
            while (st < -3.14159265f) st += 6.2831853f;
            g_stanceYaw = st;
            g_stanceValid = true;
        }
    }    if (skel && skel == g_plan.skel)
        for (int h = 0; h < 2; ++h) {
            if (g_plan.capture[h])
                CapturePin(skel, h, g_plan.capClav[h], g_plan.capChest[h], g_plan.capShoulder[h], g_plan.capElbow[h],
                           g_plan.capWrist[h]);  // before any edit
            if (g_plan.arm[h].on) {
                const float w = g_ikWeight.load();
                const ArmPlan& a = g_plan.arm[h];
                const int16_t nAll = *(int16_t*)(skel + 0x38);
                uint8_t* matsAll = *(uint8_t**)(skel + 0x58);
                auto WA = [&](int j) -> Mat4& { return *(Mat4*)(matsAll + ((size_t)(uint16_t)nAll + j) * 0x40); };
                const int nHeld = nHeldBy[h];
                t_nHeldDbg[h] = nHeld;
                for (int q = 0; q < nHeld; ++q) t_heldDbg[h][q] = heldBy[h][q].j;
                if (g_attachDiag.load() > 0 && a.wrist >= 0) {
                    if (h == 1) --g_attachDiag;
                    // cache vs locals: how many arm joints hold something else than local x parent this pass (our old edit)
                    int stale = 0;
                    float worst = 0.f;
                    const uint8_t* fl = *(const uint8_t**)(skel + 0x60);
                    for (int j : a.clavSub) {
                        if (j == a.clav || (fl && (fl[j] & 0x43) != 0x43)) continue;  // placed elsewhere: not comparable
                        const Mat4 f = Mul(*(const Mat4*)(PassLocals(skel) + (size_t)j * 0x40), WA(JointParent(j)));
                        const float dx = f.m[3][0] - WA(j).m[3][0], dy = f.m[3][1] - WA(j).m[3][1], dz = f.m[3][2] - WA(j).m[3][2];
                        const float e = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (e > 0.01f) ++stale;
                        worst = fmaxf(worst, e);
                    }
                    std::string s;
                    for (int k = 0; k < nHeld; ++k) s += " " + std::to_string(heldBy[h][k].j);
                    Log("attachdiag: t%u %s arm joints not local x parent: %d (worst %.3f m) | held:%s", GetCurrentThreadId(),
                        h ? "R" : "L", stale, worst, nHeld ? s.c_str() : " none");
                }
                {   // diagnostic: log the held set when it changes (first 60 changes after start / attachold)
                    uint64_t sig = (uint64_t)nHeld;
                    for (int k = 0; k < nHeld; ++k) sig = sig * 1315423911ull + (uint64_t)heldBy[h][k].j;
                    if (g_heldSig[h].exchange(sig) != sig && g_heldLogs.fetch_add(1) < 60) {
                        std::string s;
                        for (int k = 0; k < nHeld; ++k) s += " " + std::to_string(heldBy[h][k].j);
                        Log("arms: %s hand moves attach joints:%s", h ? "right" : "left", nHeld ? s.c_str() : " none");
                    }
                }
                if (w >= 0.999f || a.clavSub.empty()) {
                    SolveArm(skel, a);
                } else {  // partial: mix the IK result with the animated arm (blend in/out of ANIMATE moves)
                    const int16_t n = *(int16_t*)(skel + 0x38);
                    uint8_t* mats = *(uint8_t**)(skel + 0x58);
                    auto W = [&](int j) -> Mat4& { return *(Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40); };
                    thread_local std::vector<Mat4> before;
                    before.resize(a.clavSub.size());
                    // the animated arm from the locals (D-038: the cache may hold our previous pass)
                    const uint8_t* fl = *(const uint8_t**)(skel + 0x60);
                    for (size_t k = 0; k < a.clavSub.size(); ++k) {
                        const int j = a.clavSub[k];
                        if (!fl || (fl[j] & 0x43) == 0x43)  // parents come first (clavicle: on the chest); placed-elsewhere kept
                            W(j) = Mul(*(const Mat4*)(PassLocals(skel) + (size_t)j * 0x40), W(JointParent(j)));
                        before[k] = W(j);
                    }
                    SolveArm(skel, a);
                    for (size_t k = 0; k < a.clavSub.size(); ++k) {
                        Mat4& m = W(a.clavSub[k]);
                        const Mat4& b = before[k];
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 3; ++c) m.m[r][c] = b.m[r][c] + (m.m[r][c] - b.m[r][c]) * w;
                        for (int r = 0; r < 3; ++r) {  // keep the axes' length (skinning scale)
                            const float lb = sqrtf(b.m[r][0] * b.m[r][0] + b.m[r][1] * b.m[r][1] + b.m[r][2] * b.m[r][2]);
                            const float lm = sqrtf(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
                            if (lm > 1e-6f) for (int c = 0; c < 3; ++c) m.m[r][c] *= lb / lm;
                        }
                        OrthonormalizeKeepScale(m);  // D-090: a blend of two rotations is sheared - made a rotation again
                    }
                }
                if (a.wrist >= 0 && haveWristAnim[h]) {  // D-048: hand correction for thrown Blades (body.cpp)
                    AcquireSRWLockExclusive(&g_handDeltaLock);
                    g_handAnim[h] = wristAnim[h];
                    g_handDelta[h] = Mul(AffineInverse(wristAnim[h]), WA(a.wrist));  // D-090: true inverse
                    g_handDeltaFrame[h] = GetTickCount64();
                    ReleaseSRWLockExclusive(&g_handDeltaLock);
                }
                if (g_jitter.load() > 0 && a.wrist >= 0) {
                    // diagnostic "armjitter N": every arm joint's position in the IK wrist frame (fingers) or the elbow
                    // frame (forearm), pass to pass. Still hands: any change is a glitch (max, and which joint).
                    static std::vector<V3> last[2];
                    static float worst[2] = {0.f, 0.f};
                    static int worstJ[2] = {-1, -1}, bad[2] = {0, 0}, cnt[2] = {0, 0};
                    static float part[2][3] = {};  // worst change: hand (wrist subtree), forearm (elbow, not hand), upper arm
                    static int partJ[2][3] = {{-1, -1, -1}, {-1, -1, -1}}, partBad[2][3] = {};
                    const Mat4 wi = RigidInverse(WA(a.wrist)), ei = RigidInverse(WA(a.elbow));
                    std::vector<V3>& lv = last[h];
                    const bool have = lv.size() == a.subtree.size();
                    lv.resize(a.subtree.size());
                    {   // the game's own data: hand joints relative to the wrist from the pass's locals alone (vanilla)
                        static std::vector<V3> vlast[2];
                        static float vworst[2] = {0.f, 0.f};
                        static int vbad[2] = {0, 0}, vJ[2] = {-1, -1};
                        const uint8_t* locs = PassLocals(skel);
                        std::vector<V3>& vl = vlast[h];
                        const bool vh = vl.size() == a.subtree.size();
                        vl.resize(a.subtree.size());
                        float pw = 0.f;
                        for (size_t k = 0; k < a.subtree.size(); ++k) {
                            const int j = a.subtree[k];
                            if (!IsDescendant(j, a.wrist)) { vl[k] = V3{0, 0, 0}; continue; }
                            Mat4 M = *(const Mat4*)(locs + (size_t)j * 0x40);
                            for (int c = JointParent(j); c >= 0 && c != a.wrist; c = JointParent(c))
                                M = Mul(M, *(const Mat4*)(locs + (size_t)c * 0x40));
                            const V3 p = Row(M, 3);
                            if (vh) {
                                const float d = Len(p - vl[k]);
                                if (d > vworst[h]) { vworst[h] = d; vJ[h] = j; }
                                pw = fmaxf(pw, d);
                            }
                            vl[k] = p;
                        }
                        if (vh && pw > 0.01f) ++vbad[h];
                        if (h == 1 && g_jitter.load() == 1)
                            for (int i = 0; i < 2; ++i) {
                                Log("armjitter: %s GAME DATA hand joints (locals only, no mod) %d passes > 1 cm (worst %.3f m j%d)",
                                    i ? "R" : "L", vbad[i], vworst[i], vJ[i]);
                                vbad[i] = 0; vworst[i] = 0.f; vJ[i] = -1; vlast[i].clear();
                            }
                    }
                    float passWorst = 0.f, passPart[3] = {0.f, 0.f, 0.f};
                    for (size_t k = 0; k < a.subtree.size(); ++k) {
                        const int j = a.subtree[k];
                        const bool hand = IsDescendant(j, a.wrist) || j == a.wrist;
                        const int pc = hand ? 0 : (IsDescendant(j, a.elbow) || j == a.elbow) ? 1 : 2;
                        const V3 p = Row(Mul(WA(j), hand ? wi : ei), 3);
                        if (have) {
                            const float d = Len(p - lv[k]);
                            if (d > worst[h]) { worst[h] = d; worstJ[h] = j; }
                            if (d > part[h][pc]) { part[h][pc] = d; partJ[h][pc] = j; }
                            passWorst = fmaxf(passWorst, d);
                            passPart[pc] = fmaxf(passPart[pc], d);
                        }
                        lv[k] = p;
                    }
                    if (have && passWorst > 0.01f) ++bad[h];
                    for (int pc = 0; pc < 3; ++pc) if (have && passPart[pc] > 0.01f) ++partBad[h][pc];
                    ++cnt[h];
                    if (h == 1 && g_jitter.fetch_sub(1) == 1) {
                        Log("armjitter: left %d/%d passes moved > 1 cm (worst %.3f m joint %d) | right %d/%d (worst %.3f m joint %d) | "
                            "local rebuild %d", bad[0], cnt[0], worst[0], worstJ[0], bad[1], cnt[1], worst[1], worstJ[1],
                            (int)g_armsLocal.load());
                        Log("armjitter: finger-sensing dropouts held L %llu R %llu, curl input jumps > 0.25 L %llu R %llu",
                            (unsigned long long)g_fingerHolds[0].load(), (unsigned long long)g_fingerHolds[1].load(),
                            (unsigned long long)g_curlJumps[0].load(), (unsigned long long)g_curlJumps[1].load());
                        for (int i = 0; i < 2; ++i)
                            Log("armjitter: %s hand %d passes > 1 cm (worst %.3f m j%d) | forearm %d (%.3f m j%d) | upper arm %d "
                                "(%.3f m j%d)", i ? "R" : "L", partBad[i][0], part[i][0], partJ[i][0], partBad[i][1], part[i][1],
                                partJ[i][1], partBad[i][2], part[i][2], partJ[i][2]);
                        for (int i = 0; i < 2; ++i) {
                            worst[i] = 0.f; worstJ[i] = -1; bad[i] = cnt[i] = 0; last[i].clear();
                            for (int pc = 0; pc < 3; ++pc) { part[i][pc] = 0.f; partJ[i][pc] = -1; partBad[i][pc] = 0; }
                        }
                    }
                }
                if (nHeld && a.wrist >= 0 && haveWristAnim[h]) {  // absolute: animated offset to the wrist x the IK wrist
                    const Mat4 D = Mul(AffineInverse(wristAnim[h]), WA(a.wrist));  // D-090: true inverse (a sheared animated wrist)
                    // D-046: the Blades' handle through the CENTER OF OUR FIST. The game's grip is relative to its own
                    // animated hand pose, not our closed fist (owner screenshot: handles beside the fists). The handle axis
                    // (the weapon point's axis most along the hand's thumb-up row 2) is moved sideways so it passes
                    // through the centroid of the long fingers' middle joints; the same shift for the hand's chain.
                    V3 shift{0, 0, 0};
                    V3 fistHole{0, 0, 0}, handleAx{0, 0, 0};  // D-087 v3: the flip pivots on the fist hole (handle axis)
                    bool haveFistHole = false;
                    // D-087 v3 (owner screenshot: the flipped RIGHT Blade lay sideways, its handle across the fist): v2 only
                    // knew the fist hole with "fistcenter on" (off by default, D-049) and otherwise took the handle axis
                    // from the JOMeleeWeapon point's axis most along the wrist's row 2 - not the Blade's axis on the right
                    // hand, so the 180 deg turn was about an axis ~45 deg off the handle (the handle ends sideways). Now
                    // measured from the DRAWN fist whenever the Blades are held: the knuckles of each curled finger lie on
                    // a circle around the handle - its centre is the fist hole, its plane's normal the handle axis
                    if (CurrentWeapon() == 2 && a.fingerCount > 0) {
                        V3 hc{0, 0, 0}, hn{0, 0, 0};
                        int hk = 0;
                        for (int f = 0; f < a.fingerCount; ++f) {
                            const Finger& fi = a.fingers[f];
                            if (fi.count != 3 || fi.joint[1] < 0 || fi.joint[2] < 0 || fi.joint[3] < 0) continue;
                            const V3 A = Row(WA(fi.joint[1]), 3), B = Row(WA(fi.joint[2]), 3), C = Row(WA(fi.joint[3]), 3);
                            const V3 u = A - C, v = B - C, n = Cross(u, v);
                            const float n2 = Dot(n, n);
                            if (n2 < 1e-10f) continue;
                            const V3 cc = C + Cross(v * Dot(u, u) - u * Dot(v, v), n) * (1.f / (2.f * n2));
                            if (Len(cc - B) > 0.08f) continue;  // nearly straight finger: no fist circle
                            const V3 nn = Norm(n);
                            hn = hn + (hk > 0 && Dot(nn, hn) < 0.f ? nn * -1.f : nn);
                            hc = hc + cc;
                            ++hk;
                        }
                        if (hk >= 3 && Len(hn) > 0.5f) {
                            fistHole = hc * (1.f / (float)hk);
                            handleAx = Norm(hn);
                            haveFistHole = true;
                        }
                    }
                    if (CurrentWeapon() == 2 && a.fingerCount > 0 && g_fistCenter.load()) {
                        const uint64_t* hs = *(const uint64_t**)(skel + 0x30);
                        int wk = -1;
                        for (int k = 0; k < nHeld; ++k) {
                            const uint64_t x = hs ? hs[heldBy[h][k].j] : 0;
                            if (x == 0xb41408d8ad410271ull || x == 0xb41408d8ad411e00ull) { wk = k; break; }
                        }
                        // D-047: the hole of the fist = the center of the circle through each long finger's three
                        // knuckles (MCP, PIP, DIP), averaged. D-046 used the PIP knuckles = the FRONT of the fist
                        // (owner: "more forward", and off sideways).
                        V3 c{0, 0, 0};
                        int nc = 0;
                        for (int f = 0; f < a.fingerCount; ++f) {
                            const Finger& fi = a.fingers[f];
                            if (fi.count != 3 || fi.joint[1] < 0 || fi.joint[2] < 0 || fi.joint[3] < 0) continue;
                            const V3 A = Row(WA(fi.joint[1]), 3), B = Row(WA(fi.joint[2]), 3), C = Row(WA(fi.joint[3]), 3);
                            const V3 u = A - C, v = B - C, n = Cross(u, v);
                            const float n2 = Dot(n, n);
                            if (n2 < 1e-10f) continue;  // straight finger: no circle
                            const V3 cc = C + Cross(v * Dot(u, u) - u * Dot(v, v), n) * (1.f / (2.f * n2));
                            if (Len(cc - B) > 0.08f) continue;  // nearly straight: circle too large
                            c = c + cc;
                            ++nc;
                        }
                        if (wk >= 0 && nc >= 3) {
                            c = c * (1.f / (float)nc);
                            // owner fine-tuning in the hand frame (x outward, y along the fingers, z up), gowvr.ini
                            static const float tx = config::GetFloat("blades_grip_x", 0.f),
                                               ty = config::GetFloat("blades_grip_y", 0.f),
                                               tz = config::GetFloat("blades_grip_z", 0.f);
                            const Mat4& Wm = WA(a.wrist);
                            c = c + Row(Wm, 0) * (tx * a.side) + Row(Wm, 1) * ty + Row(Wm, 2) * tz;
                            const Mat4 wm = Mul(heldBy[h][wk].anim, D);
                            const V3 up = Row(WA(a.wrist), 2);
                            int best = 0;
                            float bd = 0.f;
                            for (int r = 0; r < 3; ++r) {
                                const float d = fabsf(Dot(Norm(Row(wm, r)), up));
                                if (d > bd) { bd = d; best = r; }
                            }
                            const V3 ax = Norm(Row(wm, best)), v = c - Row(wm, 3);
                            shift = v - ax * Dot(v, ax);
                            if (Len(shift) > 0.12f) shift = shift * (0.12f / Len(shift));  // sanity clamp
                            fistHole = c;  // (fistcenter on: the shifted handle passes through this tuned centre)
                            static std::atomic<int> logged{0};
                            if (logged.fetch_add(1) < 4) {
                                const Mat4 wi = RigidInverse(Wm);
                                const V3 hp = Row(Mul(wm, wi), 3), hc = Row(Mul(Mat4{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{c.x,c.y,c.z,1}}}, wi), 3);
                                Log("arms: Blades handle through the %s fist: moved %.3f m (axis row %d, |dot| %.2f) | wrist frame "
                                    "(x, fingers, up): handle (%.3f %.3f %.3f) fist hole (%.3f %.3f %.3f)", h ? "right" : "left",
                                    Len(shift), best, bd, hp.x, hp.y, hp.z, hc.x, hc.y, hc.z);
                            }
                        }
                    }
                    // D-087: this hand's Blade flip (the Blade points turn about the palm normal through the handle)
                    const float flipDeg = CurrentWeapon() == 2 ? fmodf(FlipAngleDeg(h), 360.f) : 0.f;
                    Mat4 flipM{};
                    bool flipOn = false;
                    const uint64_t* fhs = *(const uint64_t**)(skel + 0x30);
                    auto isBladePoint = [&](int j) {
                        const uint64_t x = fhs ? fhs[j] : 0;
                        return x == 0xb41408d8ad410271ull || x == 0xb41408d8ad411e00ull || x == 0x08eae59373302b22ull ||
                               x == 0x6fc442db7837895aull || x == 0x64d3729a5b874c38ull;
                    };
                    if (fabsf(flipDeg) > 0.01f) {
                        int pk = -1;  // pivot: the handle point (JOMeleeWeapon1 / 2) in this hand, else any Blade point
                        for (int k = 0; k < nHeld && pk < 0; ++k) {
                            const uint64_t x = fhs ? fhs[heldBy[h][k].j] : 0;
                            if (x == 0xb41408d8ad410271ull || x == 0xb41408d8ad411e00ull) pk = k;
                        }
                        for (int k = 0; k < nHeld && pk < 0; ++k)
                            if (isBladePoint(heldBy[h][k].j)) pk = k;
                        if (pk >= 0) {
                            const Mat4 pm = Mul(heldBy[h][pk].anim, D);
                            // D-087 v2 (owner: "the right stick flip is not placed right, the left one is"): v1 pivoted on
                            // the hand's JOMeleeWeapon point and spun about the wrist's palm normal as it was - the two
                            // hands' points / frames are not mirror images, so the right Blade landed off the fist. Now the
                            // pivot is the hole of the fist (the handle passes through it, D-047) and the axis is the palm
                            // normal made exactly perpendicular to the Blade's handle axis (so 180 deg turns the Blade end
                            // for end through the fist on either hand)
                            V3 hAx = haveFistHole ? handleAx : V3{0, 0, 0};
                            if (Len(hAx) < 0.5f) {  // no fist hole this pass: the point's axis most along the hand's up
                                const V3 up = Row(WA(a.wrist), 2);
                                float bd = 0.f;
                                for (int r = 0; r < 3; ++r) {
                                    const float d = fabsf(Dot(Norm(Row(pm, r)), up));
                                    if (d > bd) { bd = d; hAx = Norm(Row(pm, r)); }
                                }
                            }
                            const V3 pivot = haveFistHole ? fistHole : Row(pm, 3) + shift;
                            // v3: the turn axis is perpendicular to the handle and to the wrist -> fist direction (the
                            // Blade swings over the knuckles, in the plane of handle and hand - on the left hand that is
                            // v1's palm normal, which the owner found right); fallback: the palm normal made perpendicular
                            V3 axis{0, 0, 0};
                            if (haveFistHole) axis = Norm(Cross(hAx, Norm(fistHole - Row(WA(a.wrist), 3))));
                            if (Len(axis) < 0.5f) {
                                axis = Norm(Row(WA(a.wrist), 0));
                                axis = Norm(axis - hAx * Dot(axis, hAx));
                            }
                            if (Len(axis) < 0.5f) axis = Norm(Cross(hAx, Row(WA(a.wrist), 1)));
                            if (Len(axis) > 0.5f) { flipM = RotAbout(axis, flipDeg, pivot); flipOn = true; }
                        }
                    }
                    for (int k = 0; k < nHeld; ++k) {
                        const HeldJ& hj = heldBy[h][k];
                        Mat4 m = Mul(hj.anim, D);
                        m.m[3][0] += shift.x; m.m[3][1] += shift.y; m.m[3][2] += shift.z;
                        if (flipOn && isBladePoint(hj.j)) m = Mul(m, flipM);
                        if (hj.w < 0.999f) {  // partial: blend with the game's placement, axes renormalised
                            for (int r = 0; r < 4; ++r)
                                for (int c = 0; c < 3; ++c) m.m[r][c] = hj.anim.m[r][c] + (m.m[r][c] - hj.anim.m[r][c]) * hj.w;
                            for (int r = 0; r < 3; ++r) {
                                const float lb = Len(Row(hj.anim, r)), lm = Len(Row(m, r));
                                if (lm > 1e-6f) for (int c = 0; c < 3; ++c) m.m[r][c] *= lb / lm;
                            }
                            OrthonormalizeKeepScale(m);  // D-090: no shear from the blend
                        }
                        WA(hj.j) = m;
                    }
                }
            }
        }
    if (skel && skel == g_plan.skel && g_bcOn.load()) {  // diagnostic "bladecheck": Blades joints vs the IK hands
        const int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        const uint64_t* hs = *(const uint64_t**)(skel + 0x30);
        auto WW = [&](int j) -> const Mat4& { return *(const Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40); };
        AcquireSRWLockExclusive(&g_bcLock);
        if (g_bcIdx[0] < 0 && hs)
            for (int j = 0; j < n; ++j) {
                const uint64_t x = hs[j];
                if (x == 0x08eae59373302b22ull) g_bcIdx[0] = j;       // 225 (right)
                if (x == 0xb41408d8ad410271ull) g_bcIdx[1] = j;       // 226 (left)
                if (x == 0xb41408d8ad411e00ull) g_bcIdx[2] = j;       // 227 (right)
                if (x == 0x6fc442db7837895aull) g_bcIdx[3] = j;       // 228 (left)
            }
        static const int handOf[4] = {1, 0, 1, 0};
        ++g_bcPasses;
        for (int k = 0; k < 4; ++k) {
            const int j = g_bcIdx[k], h = handOf[k];
            if (j < 0 || j >= n || !g_plan.arm[h].on || g_plan.arm[h].wrist < 0) continue;
            const Mat4 rel = Mul(WW(j), RigidInverse(WW(g_plan.arm[h].wrist)));
            const V3 p = Row(rel, 3);
            const float sc = Len(Row(WW(j), 0));
            if (!g_bcHaveRef[k]) { g_bcRef[k] = p; g_bcHaveRef[k] = true; }
            const float dev = Len(p - g_bcRef[k]), dist = Len(p);
            g_bcMaxDev[k] = fmaxf(g_bcMaxDev[k], dev);
            g_bcSumDev[k] += dev;
            g_bcMaxDist[k] = fmaxf(g_bcMaxDist[k], dist);
            g_bcMinScale[k] = fminf(g_bcMinScale[k], sc);
            if (dev > 0.03f) ++g_bcBad[k];
            ++g_bcN[k];
            if (g_bcHavePrev[k] && Len(p - g_bcPrev[k]) > 0.2f) ++g_bcJumps[k];  // pass-to-pass jump (flicker)
            g_bcPrev[k] = p;
            g_bcHavePrev[k] = true;
            static int detail = 0;
            if (dev > 0.1f && detail < 16) {
                ++detail;
                const uint8_t* locs = PassLocals(skel);
                const float ad = Len(Row(AnimWorldOf(mats, locs, n, j), 3) - Row(AnimWorldOf(mats, locs, n, g_plan.arm[h].wrist), 3));
                int held = -1;
                for (int q = 0; q < t_nHeldDbg[h]; ++q) if (t_heldDbg[h][q] == j) held = q;
                uintptr_t caller = 0;
                int flag = 0;
                PassInfo(&caller, &flag);
                const uint8_t* fl = *(const uint8_t**)(skel + 0x60);
                Log("bladecheck: BAD pass t%u %d dev %.3f dist %.3f | animated dist to its wrist %.3f | in held list %d (n %d) "
                    "| grips %zu | weapon %d | caller +0x%llx flag %d jointflags %02x skelflags %llx locals %s",
                    GetCurrentThreadId(), j, dev, dist, ad, held, t_nHeldDbg[h], g_bladeGrip.size(), CurrentWeapon(),
                    (unsigned long long)caller, flag, fl ? fl[j] : 0, (unsigned long long)*(uint64_t*)(skel + 0x68),
                    locs == mats ? "skel+0x58" : "other");
            } else if (dev < 0.02f && detail < 16 && g_bcGoodLogged < 4) {
                ++g_bcGoodLogged;
                uintptr_t caller = 0;
                int flag = 0;
                PassInfo(&caller, &flag);
                Log("bladecheck: good pass t%u %d dev %.3f | caller +0x%llx flag %d skelflags %llx", GetCurrentThreadId(), j, dev,
                    (unsigned long long)caller, flag, (unsigned long long)*(uint64_t*)(skel + 0x68));
            }
        }
        ReleaseSRWLockExclusive(&g_bcLock);
    }
    if (skel && skel == g_plan.skel && g_shrinkA.load() >= 0) {  // diagnostic "shrinkj a b": which mesh is on a..b
        const int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        for (int j = g_shrinkA.load(); j <= g_shrinkB.load() && j < n; ++j) {
            Mat4& m = *(Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) m.m[r][c] *= 0.001f;
        }
    }
    if (skel && skel == g_plan.skel) {  // D-042: this plan is now in the hero's world cache
        uint64_t cur = g_appliedSeq.load();
        while (cur < g_plan.seq && !g_appliedSeq.compare_exchange_weak(cur, g_plan.seq)) {}
    }
    ReleaseSRWLockShared(&g_lock);
}

void ArmsSelectController() {
    static std::string last;
    std::string sys = vr::XrSystemName();
    if (sys.empty() || sys == last) return;
    last = sys;
    std::string low = sys;
    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    const bool psvr2 = low.find("playstation") != std::string::npos || low.find("psvr") != std::string::npos ||
                       low.find("sony") != std::string::npos || low.find("vr2") != std::string::npos;
    const bool quest = low.find("quest") != std::string::npos || low.find("oculus") != std::string::npos ||
                       low.find("meta") != std::string::npos;
    g_pitchKey = psvr2 ? "wristpitch_psvr2" : quest ? "wristpitch_quest" : "wristpitch_other";
    g_family = psvr2 ? "psvr2" : quest ? "quest" : "other";
    g_pitchDeg = config::GetFloat(g_pitchKey, psvr2 ? 35.f : 0.f);
    g_yawDeg = config::GetFloat(FamilyKey("wristyaw").c_str(), 0.f);
    g_rollDeg = config::GetFloat(FamilyKey("wristroll").c_str(), 0.f);
    Log("arms: headset '%s' -> %s, wrist pitch %.0f yaw %.0f roll %.0f deg (F5/F6, Ctrl+F5/F6, Shift+F5/F6 adjust, saved "
        "to gowvr.ini)", sys.c_str(), g_family, g_pitchDeg, g_yawDeg, g_rollDeg);
}

void ArmsAdjustPitch(float deltaDeg) {
    g_pitchDeg += deltaDeg;
    config::SetFloat(g_pitchKey, g_pitchDeg);
    Log("arms: %s = %.0f deg", g_pitchKey, g_pitchDeg);
}

void ArmsAdjustYaw(float deltaDeg) {
    g_yawDeg += deltaDeg;
    config::SetFloat(FamilyKey("wristyaw").c_str(), g_yawDeg);
    Log("arms: wristyaw_%s = %.0f deg", g_family, g_yawDeg);
}

void ArmsAdjustRoll(float deltaDeg) {
    g_rollDeg += deltaDeg;
    config::SetFloat(FamilyKey("wristroll").c_str(), g_rollDeg);
    Log("arms: wristroll_%s = %.0f deg", g_family, g_rollDeg);
}

void ArmsUpdate(bool active, const Mat4* handWorld /*[2] or null*/, const bool* handValid) {
    ArmsSelectController();
    Plan p;
    uint8_t* obj = HeroObject();
    p.skel = obj ? *(uint8_t**)(obj + 0x238) : nullptr;
    static const bool pinOn = config::GetFloat("shoulder_pin", 1.f) > 0.5f;
    static const float slideMax = config::GetFloat("shoulder_slide_max", 0.25f);
    static const bool armFrames = config::GetFloat("arm_frames", 1.f) > 0.5f;  // absolute arm bone frames (D-014)
    static int authoredFrames = 0;  // frames since the arms were last motion controlled (our edits have washed out)
    {
        static ULONGLONG lastT = 0;
        const ULONGLONG t = GetTickCount64();
        const float dt = lastT ? fminf((float)(t - lastT), 100.f) : 11.f;
        lastT = t;
        static const float blendMs = config::GetFloat("arm_blend_ms", 180.f);
        // D-097: an interaction with both hands on something (doors, chests, carrying the boar / Atreus, the ogre ride ...)
        // hands both arms back to the animation through the same blend as ANIMATE moves
        const bool ctxBoth = ContextHandAuthored(0) && ContextHandAuthored(1);
        const float target = CombatArmsFromAnimation() || ctxBoth ? 0.f : 1.f, w = g_ikWeight.load();
        const float step = dt / fmaxf(blendMs, 1.f);
        g_ikWeight = target > w ? fminf(target, w + step) : fmaxf(target, w - step);
    }
    const bool controlled = active && g_enabled && p.skel && handWorld && g_ikWeight.load() > 0.01f;
    authoredFrames = controlled ? 0 : authoredFrames + 1;
    static uint8_t* shoulderSkel = nullptr;
    static int clavOf[2] = {-1, -1}, chestOf[2] = {-1, -1};
    static std::vector<int> clavSubOf[2];
    if (p.skel && p.skel != shoulderSkel) {
        shoulderSkel = p.skel;
        for (int h = 0; h < 2; ++h) {
            int s = FindJointByHash(kArms[h].shoulder);
            clavOf[h] = s >= 0 ? JointParent(s) : -1;
            chestOf[h] = clavOf[h] >= 0 ? JointParent(clavOf[h]) : -1;
            clavSubOf[h].clear();
            if (clavOf[h] >= 0) {
                clavSubOf[h].push_back(clavOf[h]);
                std::vector<int> sub = Subtree(p.skel, clavOf[h]);
                for (int j : sub) if (j != clavOf[h]) clavSubOf[h].push_back(j);
            }
            int order = 0;  // D-038 rebuilds children from locals in list order: every parent must come first
            for (size_t k = 1; k < clavSubOf[h].size(); ++k)
                if (JointParent(clavSubOf[h][k]) > clavSubOf[h][k]) ++order;
            Log("arms: %s shoulder %d clavicle %d chest %d (%zu joints move with the clavicle; %d listed before their "
                "parent)", h ? "right" : "left", s, clavOf[h], chestOf[h], clavSubOf[h].size(), order);
        }
    }
    Pin pins[2];
    AcquireSRWLockShared(&g_pinLock);
    pins[0] = g_pin[0];
    pins[1] = g_pin[1];
    ReleaseSRWLockShared(&g_pinLock);
    for (int h = 0; h < 2; ++h) {
        const bool have = pins[h].valid && pins[h].skel == p.skel;
        // sample the animation's shoulder while authored for a while; the very first time also at the start of control
        p.capture[h] = pinOn && p.skel && clavOf[h] >= 0 && chestOf[h] >= 0 && (authoredFrames > 60 || !have);
        p.capClav[h] = clavOf[h];
        p.capChest[h] = chestOf[h];
        p.capShoulder[h] = FindJointByHash(kArms[h].shoulder);
        p.capElbow[h] = FindJointByHash(kArms[h].elbow);
        p.capWrist[h] = FindJointByHash(kArms[h].wrist);
        static bool logged[2] = {false, false};
        if (have && controlled && !logged[h]) {
            logged[h] = true;
            Log("arms: %s clavicle pinned (%llu samples of the animation), slide up to %.2f m", h ? "right" : "left",
                (unsigned long long)pins[h].samples, slideMax);
        }
    }
    if (controlled) {
        Mat4 root{};
        int neckJ = FindJointByHash(0xf14d9a553fdad7ccull);
        bool haveRoot = StableJoint(0xfe57caacf699afe4ull, &root) || JointWorld(0, &root);
        vr::InputSnapshot in = vr::XrInput();
        for (int h = 0; h < 2; ++h) {
            if (!handValid[h]) continue;
            ArmPlan& a = p.arm[h];
            a.shoulder = FindJointByHash(kArms[h].shoulder);
            a.elbow = FindJointByHash(kArms[h].elbow);
            a.wrist = FindJointByHash(kArms[h].wrist);
            if (a.shoulder < 0 || a.elbow < 0 || a.wrist < 0) continue;
            // context: while the animated left hand is up at the shoulder (carrying the log), the left arm stays authored
            // The wrist/neck come from the animation's LOCAL transforms: the published world pose can already hold our
            // own IK arm, which fed back (raise the hand -> "carrying" -> arm drops -> "free" -> ... = jitter every
            // frame, owner report). Debounced: a state change needs 8 consecutive frames.
            Mat4 wa{}, na{};
            static bool carry = false;
            if (h == 0) {
                // The story log carry is the game's own "NarrativeCarry" state (sub_1405041e0(hero, hash), as the
                // game's animation code asks it). Wrist-height guesses kept seeing our own IK arm (owner: jitter).
                (void)wa; (void)na; (void)neckJ;
                bool raw = NarrativeCarry();
                static int streak = 0;
                streak = raw != carry ? streak + 1 : 0;
                if (streak >= 8) {
                    carry = raw;
                    streak = 0;
                    Log("arms: left hand %s (NarrativeCarry)", carry ? "CARRYING (authored)" : "free (motion controlled)");
                }
                if (carry) continue;
            }
            // D-097: one hand on something (a crystal in the left hand, the axe dragged in the snow by the right, a
            // throwable in the right): that hand follows the animation, the other stays on its controller
            if (ContextHandAuthored(h)) continue;
            static std::vector<int> cache[2];
            static Finger fcache[2][5];
            static int fcount[2] = {0, 0};
            static uint8_t* cacheSkel[2] = {nullptr, nullptr};
            if (cacheSkel[h] != p.skel) {
                cache[h] = Subtree(p.skel, a.shoulder);
                fcount[h] = BuildFingers(p.skel, a.wrist, fcache[h]);
                cacheSkel[h] = p.skel;
                Log("arms: %s hand, %d fingers", h ? "right" : "left", fcount[h]);
            }
            a.subtree = cache[h];
            a.fingerCount = fcount[h];
            for (int f = 0; f < fcount[h]; ++f) a.fingers[f] = fcache[h][f];
            a.side = h == 1 ? 1.f : -1.f;
            a.clav = clavOf[h];
            a.chest = chestOf[h];
            a.pin = pinOn && pins[h].valid && pins[h].skel == p.skel && a.clav >= 0 && a.chest >= 0;
            if (a.pin) {
                a.clavSub = clavSubOf[h];
                a.pinLocal = pins[h].local;
                a.slideMax = fmaxf(slideMax, 0.f);
                a.frames = armFrames;
                a.le = pins[h].le;
                a.lw = pins[h].lw;
            }
            const vr::HandState& hs = in.hand[h];
            // owner: the fist closes as soon as the finger is on the grip, without pressing it, on any controller:
            //  - finger sensing (PSVR2 touch sensors, Index capacitive grip): the runtime's hand skeleton curl
            //  - controllers without it (Quest Touch has no grip touch sensor): a light squeeze is already a fist
            static const float gripFull = fmaxf(config::GetFloat("grip_fist_full", 0.35f), 0.1f);
            a.curl[0] = fminf(fmaxf((hs.squeeze - 0.05f) / (gripFull - 0.05f), 0.f), 1.f);
            a.curl[1] = fminf(fmaxf((hs.trigger - 0.05f) / 0.9f, 0.f), 1.f);
            // D-038: finger sensing drops out for single frames (hand-tracking joints not valid) - the fingers then snapped
            // open and back. The last valid reading is held for 0.4 s.
            static float heldCurl[2][5] = {};
            static ULONGLONG heldAt[2] = {0, 0};
            const ULONGLONG nowMs = GetTickCount64();
            const float* fc = nullptr;
            if (hs.fingersValid) {
                memcpy(heldCurl[h], hs.fingerCurl, sizeof(heldCurl[h]));
                heldAt[h] = nowMs;
                fc = hs.fingerCurl;
            } else if (heldAt[h] && nowMs - heldAt[h] < 400) {
                fc = heldCurl[h];
                ++g_fingerHolds[h];
            }
            if (fc) {
                const float grip = (fc[2] + fc[3] + fc[4]) / 3.f;
                a.curl[0] = fmaxf(a.curl[0], fminf(grip / 0.7f, 1.f));  // resting on the grip reads ~0.7 curl
                a.curl[1] = fmaxf(a.curl[1], fc[1]);
            }
            a.curl[1] = fmaxf(a.curl[1], a.curl[0]);  // the index closes with the grip too: no pointing hand (owner)
            a.curl[2] = fmaxf(a.curl[0], a.curl[1] * 0.6f);
            // holding the axe: the fingers keep the game's own grip animation around the handle (VRIK: held
            // weapons use the authored grip); they follow the wrist with their animated pose
            if (h == 1 && AxeInHand()) a.curl[0] = a.curl[1] = a.curl[2] = 1.f;  // closed fist (owner preferred)
            // D-044: both fists closed around the Blades while the game is in Blades mode (owner: open hands on them)
            if (CurrentWeapon() == 2) a.curl[0] = a.curl[1] = a.curl[2] = 1.f;
            a.authoredFingers = CurrentWeapon() == 2 && g_bladesAuthoredFingers.load();
            if (g_curlTest >= 0.f) a.curl[0] = a.curl[1] = a.curl[2] = g_curlTest;
            {   // D-038: fingers move at most ~12 curl units per second (a fist in ~80 ms): no single-frame snaps
                static float shown[2][3] = {};
                static ULONGLONG last[2] = {0, 0};
                const float dt = last[h] ? fminf((float)(nowMs - last[h]) * 0.001f, 0.1f) : 1.f;
                last[h] = nowMs;
                for (int c = 0; c < 3; ++c) {
                    const float d = a.curl[c] - shown[h][c], step = 12.f * dt;
                    if (fabsf(d) > 0.25f) ++g_curlJumps[h];
                    shown[h][c] += fmaxf(-step, fminf(step, d));
                    a.curl[c] = shown[h][c];
                }
            }
            // controller grip, pitched / turned / rolled per headset family, -> hand frame, pulled back to the wrist
            Mat4 g = handWorld[h];
            if (g_pitchDeg != 0.f) {
                float pr = -g_pitchDeg * 0.0174533f, c = cosf(pr), s = sinf(pr);  // direction fixed (owner: was worse)
                V3 Y = Row(g, 1), Z = Row(g, 2);
                SetRow(g, 1, Y * c + Z * s);
                SetRow(g, 2, Z * c - Y * s);
            }
            if (g_yawDeg != 0.f) {  // about the grip's up axis; mirrored on the left hand
                float r = g_yawDeg * a.side * 0.0174533f, c = cosf(r), s = sinf(r);
                V3 X = Row(g, 0), Z = Row(g, 2);
                SetRow(g, 0, X * c - Z * s);
                SetRow(g, 2, Z * c + X * s);
            }
            if (g_rollDeg != 0.f) {  // about the grip's pointing axis; mirrored on the left hand
                float r = g_rollDeg * a.side * 0.0174533f, c = cosf(r), s = sinf(r);
                V3 X = Row(g, 0), Y = Row(g, 1);
                SetRow(g, 0, X * c + Y * s);
                SetRow(g, 1, Y * c - X * s);
            }
            Mat4 t{};
            SetRow(t, 0, Row(g, 0) * -1.f);
            SetRow(t, 1, Row(g, 2) * -1.f);
            SetRow(t, 2, Row(g, 1) * -1.f);
            SetRow(t, 3, Row(g, 3) - Row(t, 1) * g_wristBack);
            t.m[3][3] = 1.f;
            a.target = t;
            // elbow pole from the BODY (playbook: the hand's rotation must not swing the elbow): down, out, a bit back
            V3 right = haveRoot ? Row(root, 0) : V3{1, 0, 0};
            V3 back = haveRoot ? Row(root, 2) : V3{0, 0, 1};
            a.pole = Norm(V3{0, -1, 0} + right * (0.45f * a.side) + back * 0.25f);
            a.on = true;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_plan = std::move(p);
    g_plan.seq = ++g_planSeq;  // D-042: the renderer waits for a hero pass that applied this plan before eye 0
    ReleaseSRWLockExclusive(&g_lock);
    if ((g_plan.arm[0].on || g_plan.arm[1].on) && (++g_solves <= 3 || g_solves % 900 == 0))
        Log("arms: frame %llu left=%d right=%d curlR(g%.2f t%.2f)", (unsigned long long)g_solves, g_plan.arm[0].on,
            g_plan.arm[1].on, g_plan.arm[1].curl[0], g_plan.arm[1].curl[1]);
}

bool ArmsFramePending() {
    AcquireSRWLockShared(&g_lock);
    const bool on = g_plan.skel && (g_plan.arm[0].on || g_plan.arm[1].on);
    const uint64_t s = g_plan.seq;
    ReleaseSRWLockShared(&g_lock);
    return on && g_appliedSeq.load() < s;
}

bool ArmsStanceYaw(float* yaw) {
    if (!g_stanceValid.load()) return false;
    *yaw = g_stanceYaw.load();
    return true;
}
void ArmsReach(int n) { g_reachDiag = n; Log("armreach: next %d arm solves", n); }
void ArmsJitter(int passes) { g_jitter = passes; Log("armjitter: next %d right-arm passes", passes); }
void ArmsLocal(bool on) { g_armsLocal = on; Log("arms: rebuild from animation locals (D-038) %s", on ? "ON" : "OFF (old)"); }
void ArmsCurlTest(float v) { g_curlTest = v; Log("arms: curl test %.2f", v); }

void ArmsEnable(bool on) {
    g_enabled = on;
    Log("arms: motion-controlled arms %s", on ? "on" : "off");
}

}  // namespace gowvr::fp
