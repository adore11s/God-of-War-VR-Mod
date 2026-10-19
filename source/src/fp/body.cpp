#include "body.h"
#include "../core/addr.h"
#include "../core/log.h"
#include "../core/detour.h"
#include "arms.h"
#include "firstperson.h"
#include "movelog.h"
#include "combat.h"
#include "posdiag.h"
#include "../vr/xr.h"
#include "../core/config.h"
#include "../input/vpad.h"
#include "../stereo/mutate.h"
#include "../stereo/r1.h"
#include "../stereo/sharpui.h"
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <intrin.h>
#include <vector>
#include <array>
#include <windows.h>
#include <cmath>
#include <cstring>

namespace gowvr::fp {
bool PlausibleSkeleton(uint8_t* skel);
namespace {

uint8_t* Base() { return (uint8_t*)GetModuleHandleW(nullptr); }
template <class T> T& G(uint64_t va) { return *(T*)((uint8_t*)GameVa(va)); }

constexpr uint64_t kHeroPtr = 0x1422c6950;

uint8_t* g_heroObj = nullptr;  // the hero's skinned game object (auto-picked, see PickHero)

// Hero = the character-table (table 0, second set) object with a large skeleton (>= 300 joints) nearest the render
// camera, excluding rigs parked at the origin. (LIVE: Kratos = 529 joints, table0/1[0] in the opening.)
uint8_t* PickHero() {
    // The camera's follow target (camera_014: _DAT_1411bdbd8 = *(*(DAT_1422e7d50 + 8) + 8)) = Kratos' goGameObject.
    if (uint8_t* mgr = G<uint8_t*>(0x1422e7d50); mgr && !IsBadReadPtr(mgr + 8, 8)) {
        uint8_t* a = *(uint8_t**)(mgr + 8);
        if (a && !IsBadReadPtr(a + 8, 8)) {
            uint8_t* o = *(uint8_t**)(a + 8);
            if (o && !IsBadReadPtr(o, 0x240) && PlausibleSkeleton(*(uint8_t**)(o + 0x238))) return o;
        }
    }
    uint8_t* viewA = G<uint8_t*>(0x142d4e5d8);
    if (!viewA) return nullptr;
    const float* cam = (const float*)(viewA + 0x180);
    uint8_t* tab = G<uint8_t*>(0x1422a98c0);
    if (!tab || IsBadReadPtr(tab, 0x38)) return nullptr;
    uint8_t** arr = *(uint8_t***)(tab + 0x20);
    uint32_t cnt = *(uint32_t*)(tab + 0x30);
    if (!arr || cnt == 0 || cnt > 100000 || IsBadReadPtr(arr, (size_t)cnt * 8)) return nullptr;
    uint8_t* best = nullptr;
    float bestD = 1e30f;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint8_t* o = arr[i];
        if (!o || IsBadReadPtr(o, 0x240)) continue;
        uint8_t* sk = *(uint8_t**)(o + 0x238);
        if (!PlausibleSkeleton(sk) || *(int16_t*)(sk + 0x38) < 300) continue;
        const float* p = (const float*)(*(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)*(int16_t*)(sk + 0x38) * 0x40 + 0x30);
        if (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) < 0.01f) continue;
        float d = (p[0] - cam[0]) * (p[0] - cam[0]) + (p[1] - cam[1]) * (p[1] - cam[1]) + (p[2] - cam[2]) * (p[2] - cam[2]);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

// Per-frame hero lookup must be cheap and must not probe large buffers the animation threads are rewriting
// (IsBadReadPtr over the whole matrix array intermittently failed -> F1 flashed to third person). The camera follow
// target is read every call; the full scan only runs when that chain is unavailable.
bool LightSkeleton(uint8_t* skel) {
    if (!skel) return false;
    int16_t n = *(int16_t*)(skel + 0x38);
    return n >= 20 && n <= 1200 && *(uint8_t**)(skel + 0x30) && *(uint8_t**)(skel + 0x58);
}

uint8_t* Skeleton() {
    if (uint8_t* mgr = G<uint8_t*>(0x1422e7d50)) {
        uint8_t* a = *(uint8_t**)(mgr + 8);
        uint8_t* o = a ? *(uint8_t**)(a + 8) : nullptr;
        if (o && LightSkeleton(*(uint8_t**)(o + 0x238))) {
            g_heroObj = o;
            return *(uint8_t**)(o + 0x238);
        }
    }
    if (!g_heroObj || IsBadReadPtr(g_heroObj, 0x240) || !PlausibleSkeleton(*(uint8_t**)(g_heroObj + 0x238)))
        g_heroObj = PickHero();
    if (!g_heroObj) return nullptr;
    return *(uint8_t**)(g_heroObj + 0x238);
}

// Candidate joint names (SMS rigs are not documented; matched by hash, confirmed by position).
const char* const kCandidates[] = {
    "head", "Head", "neck", "Neck", "neck1", "neck2", "jaw", "Jaw", "spine", "spine1", "spine2", "spine3", "chest",
    "pelvis", "hips", "root", "Root", "COG", "cog", "l_eye", "r_eye", "eye_l", "eye_r", "lEye", "rEye", "headTop",
    "head_end", "headEnd", "l_shoulder", "r_shoulder", "l_clavicle", "r_clavicle", "l_upperarm", "r_upperarm",
    "l_arm", "r_arm", "l_forearm", "r_forearm", "l_elbow", "r_elbow", "l_hand", "r_hand", "l_wrist", "r_wrist",
    "lHand", "rHand", "hand_l", "hand_r", "L_Hand", "R_Hand", "l_hip", "r_hip", "l_knee", "r_knee", "l_foot",
    "r_foot", "l_toe", "r_toe", "weapon", "r_weapon", "l_weapon", "axe", "shield", "lShoulder", "rShoulder",
    "lElbow", "rElbow", "lWrist", "rWrist", "lHip", "rHip", "lKnee", "rKnee", "lAnkle", "rAnkle", "l_ankle",
    "r_ankle", "spineA", "spineB", "spineC", "neckA", "neckB", "headA", "camera", "cam", "attach", "prop",
    "r_prop", "l_prop", "back", "beard", "l_thumb1", "r_thumb1", "l_index1", "r_index1",
};

}  // namespace

bool InCinematic() { return G<uint8_t>(0x1422e9d2f) != 0; }  // IsInCinematicMode (sub_1407a17b0)

uint64_t JointHash(const char* name) {
    uint64_t h = 0;
    for (const uint8_t* p = (const uint8_t*)name; *p; ++p) {
        uint64_t c = (uint8_t)(*p - 0x20);
        if ((uint8_t)(*p + 0x9f) > 0x19) c = *p;  // only 'a'..'z' are folded to upper case
        h = (c + h) * 0x401;
        h ^= h >> 6;
    }
    return h;
}

uint8_t* HeroObject() {
    return Skeleton() ? g_heroObj : nullptr;
}

int FindJointByHash(uint64_t h) {
    static uint8_t* cachedSkel = nullptr;
    static uint64_t cachedHash = 0;
    static int cachedIndex = -1;
    uint8_t* skel = Skeleton();
    if (!skel) return -1;
    if (skel == cachedSkel && h == cachedHash) return cachedIndex;
    int16_t n = *(int16_t*)(skel + 0x38);
    uint64_t* hashes = *(uint64_t**)(skel + 0x30);
    int found = -1;
    if (n > 0 && hashes && !IsBadReadPtr(hashes, (size_t)n * 8))
        for (int i = 0; i < n; ++i)
            if (hashes[i] == h) { found = i; break; }
    cachedSkel = skel;
    cachedHash = h;
    cachedIndex = found;
    return found;
}

int FindJoint(const char* name) {
    uint8_t* skel = Skeleton();
    if (!skel) return -1;
    int16_t n = *(int16_t*)(skel + 0x38);
    uint64_t* hashes = *(uint64_t**)(skel + 0x30);
    if (n <= 0 || !hashes || IsBadReadPtr(hashes, (size_t)n * 8)) return -1;
    uint64_t h = JointHash(name);
    for (int i = 0; i < n; ++i)
        if (hashes[i] == h) return i;
    return -1;
}

// sub_140591550 for the mod itself: the detour's trampoline once installed (the override in hk_WorldFromLocals is for
// the game's effects only), the original address before
using PFN_WorldRaw = void(__fastcall*)(uint8_t*, float*, int);
static std::atomic<void*> g_wflTrampoline{nullptr};
static PFN_WorldRaw RawWorldFromLocals() {
    if (void* t = g_wflTrampoline.load()) return (PFN_WorldRaw)t;
    return (PFN_WorldRaw)((uint8_t*)GameVa(0x140591550ull));
}

bool JointWorld(int index, Mat4* out) {
    uint8_t* skel = Skeleton();
    if (!skel || index < 0) return false;
    int16_t n = *(int16_t*)(skel + 0x38);
    if (index >= n) return false;
    // Same path as the Lua GetWorldJoint* bindings (sub_14018fd40): cached world matrices only when valid
    // (sub_14058f1d0), otherwise computed by sub_140591550(skel, out, joint).
    using PFN_Valid = char(__fastcall*)(uint8_t*);
    using PFN_World = void(__fastcall*)(uint8_t*, float*, int);
    auto valid = (PFN_Valid)((uint8_t*)GameVa(0x14058f1d0ull));
    auto world = RawWorldFromLocals();  // D-057: never through our override
    if (valid(skel)) {
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        if (!mats) return false;
        memcpy(out, mats + ((size_t)(uint16_t)n + index) * 0x40, sizeof(Mat4));
    } else {
        alignas(16) float m[16];
        world(skel, m, index);
        memcpy(out, m, sizeof(Mat4));
    }
    return true;
}

bool AnimatedJoint(int index, Mat4* out) {
    uint8_t* skel = Skeleton();
    if (!skel || index < 0 || index >= *(int16_t*)(skel + 0x38)) return false;
    alignas(16) float m[16];
    RawWorldFromLocals()(skel, m, index);  // D-057: the true animated pose, never through our override
    memcpy(out, m, sizeof(Mat4));
    return true;
}

bool PlausibleSkeleton(uint8_t* skel) {
    if (!skel || IsBadReadPtr(skel, 0x60)) return false;
    int16_t n = *(int16_t*)(skel + 0x38);
    uint8_t* h = *(uint8_t**)(skel + 0x30);
    uint8_t* m = *(uint8_t**)(skel + 0x58);
    return n >= 20 && n <= 1200 && h && m && !IsBadReadPtr(h, (size_t)n * 8) && !IsBadReadPtr(m, (size_t)n * 0x80);
}

void FindSkeleton() {
    uint8_t* hero = G<uint8_t*>(kHeroPtr);
    if (!hero) { Log("fp: no hero"); return; }
    int found = 0;
    for (int off = 0; off < 0x4000 && found < 20; off += 8) {
        if (IsBadReadPtr(hero + off, 8)) break;
        uint8_t* p = *(uint8_t**)(hero + off);
        if ((uintptr_t)p < 0x10000 || IsBadReadPtr(p, 0x240)) continue;
        if (PlausibleSkeleton(*(uint8_t**)(p + 0x238))) {
            Log("fp: hero+0x%x -> obj %p, obj+0x238 skeleton joints=%d", off, p, *(int16_t*)(*(uint8_t**)(p + 0x238) + 0x38));
            ++found;
        }
        if (PlausibleSkeleton(p)) {
            Log("fp: hero+0x%x -> skeleton %p joints=%d", off, p, *(int16_t*)(p + 0x38));
            ++found;
        }
    }
    Log("fp: skeleton scan done, %d candidates", found);
}

// Game-object tables (sub_1404ef0b0): 64 table pointers at 0x1422a98c0; table: +0x8 objects / +0x18 count, and
// +0x20 / +0x30 for the second set.
namespace {
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const uint8_t*)p + n <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}
bool Near3(const float* a, const float* b, float tol) {
    return fabsf(a[0] - b[0]) < tol && fabsf(a[1] - b[1]) < tol && fabsf(a[2] - b[2]) < tol;
}
void ScanBlock(const uint8_t* base, size_t len, const char* tag, const Mat4& r, float yaw, int* hits) {
    if (!Readable(base, len)) return;
    const float* X = r.m[0]; const float* Z = r.m[2]; const float* P = r.m[3];
    float nX[3] = {-X[0], -X[1], -X[2]}, nZ[3] = {-Z[0], -Z[1], -Z[2]};
    float qy = sinf(yaw * 0.5f), qw = cosf(yaw * 0.5f);
    for (size_t o = 0; o + 16 <= len && *hits < 60; o += 4) {
        const float* f = (const float*)(base + o);
        const char* what = nullptr;
        if (Near3(f, X, 0.02f)) what = "rootX";
        else if (Near3(f, Z, 0.02f)) what = "rootZ";
        else if (Near3(f, nX, 0.02f)) what = "-rootX";
        else if (Near3(f, nZ, 0.02f)) what = "-rootZ";
        else if (Near3(f, P, 0.02f)) what = "rootPos";
        else if (fabsf(f[0]) < 0.02f && fabsf(f[2]) < 0.02f &&
                 ((fabsf(f[1] - qy) < 0.02f && fabsf(f[3] - qw) < 0.02f) || (fabsf(f[1] + qy) < 0.02f && fabsf(f[3] + qw) < 0.02f)))
            what = "yawQuat";
        if (what) { Log("facescan: %s %p+0x%zx %s (%.3f %.3f %.3f %.3f)", tag, base, o, what, f[0], f[1], f[2], f[3]); ++*hits; }
    }
}
}  // namespace

// yawtest: does writing the hero object's world matrix (obj+0x60, rows X right, Y up, Z back, pos) turn Kratos?
// Writes an absolute yaw for 120 frames at View::Update, then only observes for 120 more; logs object vs root yaw.
std::atomic<int> g_yawTestFrames{0};
float g_yawTestTarget = 0.f;
void YawTestTick() {
    int f = g_yawTestFrames.load();
    if (f <= 0) return;
    g_yawTestFrames = f - 1;
    uint8_t* obj = HeroObject();
    int rootJ = FindJointByHash(0xfe57caacf699afe4ull);
    if (!obj) return;
    float* m = (float*)(obj + 0x60);
    Mat4 r{};
    bool haveRoot = rootJ >= 0 && AnimatedJoint(rootJ, &r);
    if (f % 15 == 0)
        Log("yawtest: frame %d objYaw %.1f rootYaw %.1f target %.1f %s", 240 - f, atan2f(m[8], m[10]) * 57.2958f,
            haveRoot ? atan2f(r.m[2][0], r.m[2][2]) * 57.2958f : 0.f, g_yawTestTarget * 57.2958f, f > 120 ? "WRITING" : "observing");
    if (f > 120) {
        float bx = sinf(g_yawTestTarget), bz = cosf(g_yawTestTarget);
        m[0] = bz; m[1] = 0.f; m[2] = -bx;   // X (right)
        m[8] = bx; m[9] = 0.f; m[10] = bz;   // Z (back)
    }
}

// Body turns with the head (the body follows the camera, which is steered to the head, continuously). Owner: "when you look with vr headset the body doesnt turn".
// LIVE (2026-09-29 log): writing the hero object's facing (obj+0x60) does not hold - the game rebuilds it from its own
// state every frame (three writes toward 29.9 deg, the body stayed at 17.3 deg; later 94 deg behind the view). So the
// facing is applied where the pose is built: the root matrix handed to the engine's local->world pass for the hero
// (hk_LocalToWorld) is turned about the vertical axis through Kratos' position to our yaw. Every joint, the IK arms,
// the attached axe/shield and the axe's hit volumes follow the rendered body. The game thread keeps that yaw:
//   - gameplay first person, standing / aiming: eases toward the view yaw (tau body_follow_tau, 0.03 s while the right
//     stick turns, <= ~690 deg/s, deadband body_follow_deadband_deg);
//   - walking: eases toward the game's own facing (it turns Kratos toward the stick, already rotated to the view);
//   - leaving first person: eases back to the game's facing, then the override is released.
std::atomic<bool> g_bodyOverride{false};
std::atomic<float> g_bodyYaw{0.f};       // rendered facing (radians, "back" axis convention)
std::atomic<float> g_gameRootYaw{0.f};   // the game's own facing, measured in the hero's world passes
std::atomic<bool> g_gameRootYawValid{false};
std::atomic<uint8_t*> g_heroSkel{nullptr};
// The rendered body turns about Kratos' EYE, not his feet (owner: "f1 mode makes me be on the right shoulder"): his eye
// sits ~0.2 m in front of the object origin, so turning about the origin swung his head sideways away from the view
// (which stays put: the headset tracks the player's own head). Pivot = game object position + the eye offset measured
// by firstperson.cpp (object frame, right / back). Game and rendered object pose are published for that measurement.
SRWLOCK g_rootLock = SRWLOCK_INIT;
float g_gameRoot[4] = {0, 0, 0, 0}, g_renderRoot[4] = {0, 0, 0, 0};  // x, y, z, yaw
Mat4 g_gameRootM{};  // guarded by g_rootLock
Mat4 g_renderRootM{};  // D-229j: the drawn root of the last hero world pass (g_rootLock)
std::atomic<float> g_pivotRight{0.f}, g_pivotBack{0.f};
std::atomic<float> g_eyeShiftR{0.f}, g_eyeShiftB{0.f};  // D-114: drawn body moved under the view (firstperson.cpp)
std::atomic<float> g_bodyTestOffset{0.f};  // diagnostic "bodyoff <deg>": standing target = game facing + offset
std::atomic<int> g_attDiag{0};             // diagnostic "attdiag <n>": carried/attached object placement vs both frames

float WrapPiB(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

std::atomic<bool> g_heroMoving{false};  // D-072: the drawn root moved > 5 mm in the last frame (BodyPostAnim)
bool HeroMoving() { return g_heroMoving.load(); }
std::atomic<float> g_viewYawLatest{0.f};
float ViewYaw() { return g_viewYawLatest.load(); }
// D-114: the VIEW's eye point (the drawn body turns about its own eye, g_pivot*, and is moved by g_eyeShift* to sit
// under the view: pivot + shift = the view's eye) - combat.cpp turns the game's facing about it, the view must not move
void BodyPivot(float* right, float* back) {
    *right = g_pivotRight.load() + g_eyeShiftR.load();
    *back = g_pivotBack.load() + g_eyeShiftB.load();
}
std::atomic<bool> g_stanceComp{true};
void StanceComp(bool on) { g_stanceComp = on; Log("fp: body turned by the stance twist (chest faces the view) %s", on ? "ON" : "OFF"); }

// a snap turn (mutate.cpp CamSync): the drawn body turns by the same step at once, not eased after the view
std::atomic<float> g_bodySnap{0.f};
void BodySnapYaw(float delta) {
    float cur = g_bodySnap.load();
    while (!g_bodySnap.compare_exchange_weak(cur, cur + delta)) {}
}

void BodyFollowYaw(float viewYaw, bool active, bool aiming) {
    static LARGE_INTEGER freq{}, last{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.f;
    if (dt <= 0.f || dt > 0.1f) dt = 1.f / 90.f;  // first call / after a pause: one frame, no snap
    dt = fmaxf(dt, 0.001f);
    last = now;
    static bool have = false;
    static float yaw = 0.f;
    if (!g_gameRootYawValid.load()) { g_bodyOverride = false; have = false; return; }
    const float game = g_gameRootYaw.load();
    if (!active && !g_bodyOverride.load()) { have = false; return; }
    if (!have) { yaw = game; have = true; }
    if (const float sn = g_bodySnap.exchange(0.f); sn != 0.f && active) yaw = WrapPiB(yaw + sn);
    g_viewYawLatest = viewYaw;
    vr::InputSnapshot in = vr::XrInput();
    const float lx = in.hand[0].stick[0], ly = in.hand[0].stick[1];
    // D-066 (owner: "when I'm moving the body must turn where I'm looking too, just like standing"): walking no longer
    // hands the body back to the game's facing (gowvr.ini body_follow_walking=0 restores the old behaviour)
    static const bool followWalking = config::GetFloat("body_follow_walking", 1.f) > 0.5f;
    const bool walking = !followWalking && in.focused && lx * lx + ly * ly > 0.2f * 0.2f;
    static const float dead = config::GetFloat("body_follow_deadband_deg", 2.f) * 0.0174533f;
    static const float tau = fmaxf(config::GetFloat("body_follow_tau", 0.10f), 0.01f);
    float target = game, t = 0.08f;
    // D-040: the idle stances twist the torso (axe: ~45 deg, right shoulder back); standing, the body is turned by that
    // twist so the chest - not the hips - faces the view, and both arms reach equally far (owner: right arm shorter)
    static float stance = 0.f;
    float st = 0.f;
    if (!g_stanceComp.load() || !ArmsStanceYaw(&st)) st = 0.f;  // arms not motion controlled: ease back to none
    // 85 %: LIVE armreach at full compensation left the LEFT arm a little further than the right
    st = fmaxf(-1.2f, fminf(1.2f, st)) * 0.85f;
    stance += WrapPiB(st - stance) * (1.f - expf(-dt / 0.4f));  // eased: stance changes are slow, no snap
    if (active && !walking) {
        target = g_bodyTestOffset.load() != 0.f ? WrapPiB(game + g_bodyTestOffset.load())
                                                : WrapPiB(viewYaw - (g_stanceComp.load() ? stance : 0.f));
        t = in.focused && fabsf(in.hand[1].stick[0]) > 0.2f ? 0.03f : tau;
    }
    float diff = WrapPiB(target - yaw);
    const float d = active && !walking ? dead : 0.f;
    if (fabsf(diff) > d) {
        const float e = diff - (diff > 0.f ? d : -d);
        float step = e * (1.f - expf(-dt / t));
        const float maxStep = 12.f * dt;  // ~690 deg/s
        step = fmaxf(-maxStep, fminf(maxStep, step));
        yaw = WrapPiB(yaw + step);
    }
    if (!active && fabsf(WrapPiB(game - yaw)) < 0.02f) {  // back on the game's facing: release
        g_bodyOverride = false;
        have = false;
        Log("fp: body facing released to the game");
        return;
    }
    g_bodyYaw = yaw;
    if (!g_bodyOverride.exchange(true)) Log("fp: body facing follows the head (rendered root yaw)");
    static uint64_t frames = 0;
    if (++frames % 900 == 1)
        Log("fp: body follows the head: body %.1f deg, view %.1f deg, game %.1f deg, walking %d aiming %d (deadband %.1f, "
            "tau %.2f s), stance twist %.1f deg", yaw * 57.2958f, viewYaw * 57.2958f, game * 57.2958f, walking, aiming,
            dead * 57.2958f, tau, stance * 57.2958f);
}

bool GameRoot(float pos[3], float* yaw) {
    if (!g_gameRootYawValid.load()) return false;
    AcquireSRWLockShared(&g_rootLock);
    pos[0] = g_gameRoot[0]; pos[1] = g_gameRoot[1]; pos[2] = g_gameRoot[2]; *yaw = g_gameRoot[3];
    ReleaseSRWLockShared(&g_rootLock);
    return true;
}

bool RenderRoot(float pos[3], float* yaw) {
    if (!g_gameRootYawValid.load()) return false;
    AcquireSRWLockShared(&g_rootLock);
    pos[0] = g_renderRoot[0]; pos[1] = g_renderRoot[1]; pos[2] = g_renderRoot[2]; *yaw = g_renderRoot[3];
    ReleaseSRWLockShared(&g_rootLock);
    return true;
}

void SetBodyPivot(float right, float back) { g_pivotRight = right; g_pivotBack = back; }
void SetBodyEyeShift(float right, float back) { g_eyeShiftR = right; g_eyeShiftB = back; }
void BodyTestOffset(float deg) { g_bodyTestOffset = deg * 0.0174533f; Log("fp: body test offset %.1f deg", deg); }
void AttDiag(int n) { g_attDiag = n; Log("attdiag: next %d object passes", n); }

// Object root position in a body frame (x right, y up, z back) given that frame's origin and yaw.
static void BodyLocal(const float* p, const float* frame, float out[3]) {
    const float vx = p[0] - frame[0], vz = p[2] - frame[2], s = sinf(frame[3]), c = cosf(frame[3]);
    out[0] = vx * c - vz * s;
    out[1] = p[1] - frame[1];
    out[2] = vx * s + vz * c;
}
// Carried objects (bow, sheathed axe) and the turned body (owner: standing still the axe sat "to the left of me where
// it flickers" and "I see two bows"): the rendered body is turned about the eye pivot (BodyFollowYaw), but some passes
// of those objects are placed from the game's unturned facing. Their pose in the body frame is learned while both
// facings agree (walking / F1 start); a pass that matches it in the GAME frame better than in the rendered frame is
// turned with the body (same rotation, same pivot as the hero root).
// D-027: the bow and the sheathed axe are glued to Kratos' RENDERED chest joint (JOBack3) while the body is turned.
// LIVE (harness, body swept -90..+120 deg, axe sheathed): the game places them from several passes per frame that
// disagree once the body is turned (some from the game's facing, some from the turned body, some from neither): turning
// the "game-facing" ones still left the axe jumping up to 0.18 m between passes ("flickers between good spot and bad
// spot"). Now: offset object-root x chest^-1 learned while both facings agree (< 2 deg), and while they differ every
// pass gets root = offset x rendered chest (the published pose follows the turned body). One pose per frame.
constexpr uint64_t kChestHash = 0xf1bc142992438c01ull;  // JOBack3 = joint 296
// D-132: the spot is learned by a majority vote - a first spot when 10 of the last 16 samples lie within 6 cm of one,
// later a sample > 8 cm off the spot counts only when 24 of the last 32 agree on a new spot. A single pass could
// re-teach the spot before (owner D-130/131 log: the put-away axe glued 1.1-1.2 m from the game's sheathed axe = "at my
// left foot"), and the game interleaves bogus passes (Blades: ~44 %), so "N in a row" would never settle either.
struct BodyGlue {
    bool have = false; Mat4 O{}; uint64_t glued = 0;
    Mat4 hist[32]{}; int histN = 0, histHead = 0;  // recent samples that disagreed with the spot
    // D-143: where the item sat on the DRAWN body (relative to the drawn root) in the last pass that was classified as
    // the game's own placement - stray passes and attack holds keep it there (not at the idle spot)
    SRWLOCK relLock = SRWLOCK_INIT; Mat4 rel{}; ULONGLONG relAt = 0; bool haveRel = false;
};
static void GlueLearn(BodyGlue& gl, const Mat4& S, const char* what) {
    auto dist = [](const Mat4& x, const Mat4& y) {
        const float dx = x.m[3][0] - y.m[3][0], dy = x.m[3][1] - y.m[3][1], dz = x.m[3][2] - y.m[3][2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    // D-134: every sample goes into the ring (D-132 kept only the disagreeing ones: two interleaved clusters then took
    // turns winning - owner log D-133: the spot "MOVED" between two places 33 cm apart every 0.5-1.5 s = back / rib)
    gl.hist[gl.histHead] = S;
    gl.histHead = (gl.histHead + 1) % 32;
    if (gl.histN < 32) ++gl.histN;
    if (gl.have && dist(S, gl.O) < 0.08f) {  // agrees with the spot: follow it slowly
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) gl.O.m[r][c] += (S.m[r][c] - gl.O.m[r][c]) * 0.1f;
        OrthonormalizeKeepScale(gl.O);
        return;
    }
    const int window = gl.have ? 32 : 16;
    const int m = gl.histN < window ? gl.histN : window;
    int agree = 0, old = 0;
    for (int k = 0; k < m; ++k) {
        const Mat4& h = gl.hist[(gl.histHead - 1 - k + 64) % 32];
        if (dist(h, S) < 0.06f) ++agree;
        if (gl.have && dist(h, gl.O) < 0.08f) ++old;
    }
    // first spot: 10 of the last 16 agree; moving it: 24 of the last 32 agree AND the old spot is gone (<= 4 of 32)
    if (gl.have ? (agree >= 24 && old <= 4) : agree >= 10) {
        Log("fp: %s spot on the chest %s at (%.2f %.2f %.2f): %d of the last %d samples agree", what,
            gl.have ? "MOVED" : "learned", S.m[3][0], S.m[3][1], S.m[3][2], agree, m);
        gl.O = S;
        gl.have = true;
    }
}
BodyGlue g_glueAxe, g_glueBow;
// D-121: the axe was put away (not thrown) since it last left the hand - FixAttached; true at start (sheathed)
std::atomic<bool> g_axeOnBack{true};
// D-060: the glue used to switch HARD between the game's placement (facings within 2 deg) and the glued one: during
// attacks the game keeps turning Kratos across that line, so glued items flipped between two places (LIVE fxjump kinks:
// Mimir 0.4-0.77 m, a belt glow 0.12-0.64 m, frame to frame) -> stretched glow / trails (owner). Now a smooth blend
// over 1-6 deg of facing difference. Command "glueblend on|off" (A/B).
std::atomic<bool> g_glueBlend{true};
void GlueBlend(bool on) { g_glueBlend = on; Log("fp: carried-item glue blended in over 1-6 deg %s", on ? "ON" : "OFF (hard switch)"); }
static float GlueWeight(float d) {
    if (!g_glueBlend.load()) return fabsf(d) < 0.035f ? 0.f : 1.f;
    const float x = fminf(fmaxf((fabsf(d) - 0.017f) / (0.105f - 0.017f), 0.f), 1.f);
    return x * x * (3.f - 2.f * x);
}
// placement between the game's R and the glued W: positions and axes lerped, axes keep R's lengths (the sheathed axe is
// scaled 0.85)
static Mat4 BlendPlacement(const Mat4& R, const Mat4& W, float t) {
    if (t >= 0.999f) return W;
    Mat4 o = W;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) o.m[r][c] = R.m[r][c] + (W.m[r][c] - R.m[r][c]) * t;
    for (int r = 0; r < 3; ++r) {
        const float lr = sqrtf(R.m[r][0] * R.m[r][0] + R.m[r][1] * R.m[r][1] + R.m[r][2] * R.m[r][2]);
        const float lo = sqrtf(o.m[r][0] * o.m[r][0] + o.m[r][1] * o.m[r][1] + o.m[r][2] * o.m[r][2]);
        if (lo > 1e-6f) for (int c = 0; c < 3; ++c) o.m[r][c] *= lr / lo;
    }
    OrthonormalizeKeepScale(o);  // D-090: no shear from the blend
    return o;
}
// force: 0 the guard decides; 1 glued while the drawn body is > 0.3 m from the game's (D-121); 2 always (D-130: a pass
// at the hand while the axe is put away)
static bool LiveGameJoint(int j, Mat4* out);  // below: a hero joint on the GAME's body (its locals x the game's root)
static bool GlueRoot(const float* root, BodyGlue& gl, const char* what, float* out, bool learn = true, int force = 0) {
    // D-138 (owner, D-137 build: "still the same - inside my right chest; and in non-F1/F3 mode the axe goes through the
    // body"). Owner logs D-133..D-137: every learned spot swung round (chest- or root-relative alike), and D-137's counter
    // showed no stale hero root. What unifies it (D-027 already measured it): the game places these items from SEVERAL
    // passes per frame - some on the game's body, some already on our drawn (turned) body. Learning mixed both kinds (a
    // drawn-body pass measured against the game's root is rotated by the body turn), and a fixed spot cannot follow the
    // torso turning against the feet (idle stances, walk starts) - it sank into the chest. Now:
    //  - plain third person (no F1, body not turned): the game's own placement - never moved (owner); learning only
    //  - each pass is classified against the learned spot: near it in the GAME's root frame -> moved onto the drawn body
    //    (the game's own pose, follows the torso exactly); near it in the DRAWN root frame -> already right, kept
    //  - stray passes (put-away axe) and attack holds (the drawn pose is frozen) -> the learned spot on the drawn root
    //  - learning only in idle and only from classified passes; the first spot only while the bodies coincide
    const Mat4& R = *(const Mat4*)root;
    float g[4], r[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g)); memcpy(r, g_renderRoot, sizeof(r));
    ReleaseSRWLockShared(&g_rootLock);
    if (!g_gameRootYawValid.load()) return false;
    auto yawPos = [](const float* p4) {
        Mat4 m{};
        const float sy = sinf(p4[3]), cy = cosf(p4[3]);
        m.m[0][0] = cy; m.m[0][2] = -sy; m.m[1][1] = 1.f; m.m[2][0] = sy; m.m[2][2] = cy;
        m.m[3][0] = p4[0]; m.m[3][1] = p4[1]; m.m[3][2] = p4[2]; m.m[3][3] = 1.f;
        return m;
    };
    auto dist = [](const Mat4& x, const Mat4& y) {
        const float dx = x.m[3][0] - y.m[3][0], dy = x.m[3][1] - y.m[3][1], dz = x.m[3][2] - y.m[3][2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    const Mat4 G = yawPos(g), Dr = yawPos(r);
    const Mat4 S1 = Mul(R, RigidInverse(G));   // the pass relative to the game's root
    const bool onBody = sqrtf((R.m[3][0] - g[0]) * (R.m[3][0] - g[0]) + (R.m[3][1] - g[1]) * (R.m[3][1] - g[1]) +
                              (R.m[3][2] - g[2]) * (R.m[3][2] - g[2])) < 1.8f;  // not thrown / a far copy
    const bool hold = CombatHoldWeight() > 0.001f;
    const bool f1 = FpMode() == 1, turned = g_bodyOverride.load();
    if (!f1 && !turned) {  // plain third person / cutscenes: both bodies are one - learn, never move anything
        if (learn && onBody && !hold && (CombatHeroIdle() || !gl.have)) GlueLearn(gl, S1, what);
        return false;
    }
    const float d = turned ? WrapPiB(r[3] - g[3]) : 0.f;
    const float ox = r[0] - g[0], oy = r[1] - g[1], oz = r[2] - g[2];
    const float off = sqrtf(ox * ox + oy * oy + oz * oz);
    if (!gl.have) {  // first spot: only while the bodies coincide (then both frames are the same)
        if (learn && onBody && !hold && fabsf(d) < 0.05f && off < 0.05f) GlueLearn(gl, S1, what);
        if (!gl.have) return false;
    }
    const Mat4 S2 = Mul(R, RigidInverse(Dr));  // the pass relative to the drawn root
    const float e1 = dist(S1, gl.O), e2 = dist(S2, gl.O);
    const bool forced = f1 && (force == 2 || (force == 1 && off > 0.3f));
    // D-143 (owner: "hand combat glues the Leviathan axe into me, like I told you before"; owner log D-142 build: fist
    // combat in F1 = "attack hold: learned spot" with the drawn body turned -49 deg against the game's, and "stance twist
    // -51 deg"): the fist / axe stance twists Kratos' torso ~50 deg against his feet, and BodyFollowYaw turns the drawn
    // root back by that twist so the chest faces the view (D-040). The learned spot is from IDLE (no twist, relative to
    // the root): put on a stance body it lands ~0.35 m round the side - inside the hip / ribs. That spot was used for
    // every attack hold (the drawn pose frozen in the stance) and every stray pass. Now both keep the item where the last
    // pass classified as the game's own placement had it on the drawn body (the frozen pose is that same stance); the
    // learned spot only when there is no such placement yet. The axe's full-size passes are never classified (D-132: the
    // sheathed axe is shrunk; a full-size pass is the stray one at the hanging hand, ~0.45 m from the back spot)
    const float sc = sqrtf(R.m[0][0] * R.m[0][0] + R.m[0][1] * R.m[0][1] + R.m[0][2] * R.m[0][2]);
    const bool strayAxe = &gl == &g_glueAxe && sc > 0.97f;
    const bool classified = !strayAxe && fminf(e1, e2) < 0.40f;  // (the torso turns the item ~0.3 m against the spot)
    const float hw = CombatHoldWeight();
    const ULONGLONG now = GetTickCount64();
    Mat4 rel{};
    bool haveRel = false;
    AcquireSRWLockShared(&gl.relLock);
    if (gl.haveRel && now - gl.relAt < (hold ? 20000ull : 1000ull)) { rel = gl.rel; haveRel = true; }
    ReleaseSRWLockShared(&gl.relLock);
    Mat4 W;
    const char* how = "";
    if (!hold && classified) {
        const bool gameFrame = e1 <= e2;
        if (learn && CombatHeroIdle()) GlueLearn(gl, gameFrame ? S1 : S2, what);
        W = gameFrame ? Mul(S1, Dr) : R;  // the game's own pose, on the drawn body
        how = gameFrame ? "game pass moved onto the drawn body" : "pass already on the drawn body";
        if (f1) {
            AcquireSRWLockExclusive(&gl.relLock);
            gl.rel = Mul(W, RigidInverse(Dr));
            gl.relAt = now;
            gl.haveRel = true;
            ReleaseSRWLockExclusive(&gl.relLock);
        }
    } else if (forced || (hold && fminf(e1, e2) < 0.5f)) {
        if (haveRel) {
            W = Mul(rel, Dr);  // where it was on the drawn body (the held pose is the pose of that moment)
            how = hold ? "attack hold: kept where it was" : "stray pass: kept where it was";
            if (hold && hw < 0.999f && classified)  // the hold fading out: toward the game's pose as the drawn pose thaws
                W = BlendPlacement(e1 <= e2 ? Mul(S1, Dr) : R, W, hw);
        } else {
            W = Mul(gl.O, Dr);  // nothing placed recently: the learned spot
            how = hold ? "attack hold: learned spot" : "stray pass: learned spot";
        }
    } else {
        return false;  // not on Kratos (thrown, dropped, Atreus' bow ...)
    }
    if (!f1) W = BlendPlacement(R, W, GlueWeight(d));  // F1 just turned off: eased back to the game's placement
    // D-137 safety (owner's suggestion): the put-away axe never closer than axe_back_min to the torso centre
    if (f1 && &gl == &g_glueAxe) {
        static const float minBack = config::GetFloat("axe_back_min", 0.18f);
        Mat4 sr, sl;
        if (minBack > 0.f && StableJoint(0xe783988515993319ull, &sr) && StableJoint(0x05c2b5ca793d4bf2ull, &sl)) {
            float rx = sr.m[3][0] - sl.m[3][0], rz = sr.m[3][2] - sl.m[3][2];
            const float rn = sqrtf(rx * rx + rz * rz);
            if (rn > 0.05f) {
                rx /= rn; rz /= rn;
                float bx = -rz, bz = rx;  // back = right x up
                if (bx * Dr.m[2][0] + bz * Dr.m[2][2] < 0.f) { bx = -bx; bz = -bz; }  // same side as the root's back
                const float cx = 0.5f * (sr.m[3][0] + sl.m[3][0]), cz = 0.5f * (sr.m[3][2] + sl.m[3][2]);
                const float behind = (W.m[3][0] - cx) * bx + (W.m[3][2] - cz) * bz;
                if (behind < minBack) {
                    W.m[3][0] += bx * (minBack - behind);
                    W.m[3][2] += bz * (minBack - behind);
                    static std::atomic<int> pushLogs{0};
                    if (pushLogs.fetch_add(1) < 10)
                        Log("fp: sheathed axe pushed %.2f m backwards (it was %.2f m behind the torso centre; %s)",
                            minBack - behind, behind, how);
                }
            }
        }
    }
    memcpy(out, &W, sizeof(W));
    if (++gl.glued % 1800 == 1)
        Log("fp: %s on the drawn body (turned %.0f deg, %.2f m apart): %s (%llu passes)", what, d * 57.2958f, off, how,
            (unsigned long long)gl.glued);
    return true;
}

// diagnostic "skelnear <passes>": every skeleton whose world pass puts its root within 1.6 m of Kratos (found by pass,
// not by the object tables - the sheathed Blades / talisman are not in them), with its callers and its root in the
// game-facing and rendered body frames (a carried item has a steady position in one of them).
struct SkelNear {
    uint8_t* skel; int joints; uint64_t hash0; uint64_t passes; uintptr_t callers[4]; int nc;
    float g[3], r[3]; float gMin[3], gMax[3], rMin[3], rMax[3];
};
static std::atomic<int> g_skelNear{0};
static SRWLOCK g_skelNearLock = SRWLOCK_INIT;
static std::vector<SkelNear> g_skelNearList;
void SkelNearStart(int passes) {
    AcquireSRWLockExclusive(&g_skelNearLock);
    g_skelNearList.clear();
    ReleaseSRWLockExclusive(&g_skelNearLock);
    g_skelNear = passes;
    Log("skelnear: next %d passes", passes);
}
static void SkelNearSample(uint8_t* skel, const float* root, uintptr_t caller) {
    if (g_skelNear.load() <= 0 || !root || !g_gameRootYawValid.load()) return;
    float g[4], r[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g)); memcpy(r, g_renderRoot, sizeof(r));
    ReleaseSRWLockShared(&g_rootLock);
    const float dx = root[12] - r[0], dy = root[13] - r[1], dz = root[14] - r[2];
    if (dx * dx + dz * dz > 1.6f * 1.6f || fabsf(dy) > 2.5f) return;
    float lg[3], lr[3];
    BodyLocal(root + 12, g, lg);
    BodyLocal(root + 12, r, lr);
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint64_t* hashes = *(const uint64_t**)(skel + 0x30);
    const uint64_t h0 = hashes && !IsBadReadPtr(hashes, 8) ? hashes[0] : 0;
    AcquireSRWLockExclusive(&g_skelNearLock);
    SkelNear* e = nullptr;
    for (auto& s : g_skelNearList) if (s.skel == skel) { e = &s; break; }
    if (!e && g_skelNearList.size() < 64) {
        SkelNear s{};
        s.skel = skel; s.joints = n; s.hash0 = h0;
        for (int i = 0; i < 3; ++i) { s.gMin[i] = s.gMax[i] = lg[i]; s.rMin[i] = s.rMax[i] = lr[i]; }
        g_skelNearList.push_back(s);
        e = &g_skelNearList.back();
    }
    if (e) {
        ++e->passes;
        bool have = false;
        for (int i = 0; i < e->nc; ++i) if (e->callers[i] == caller) have = true;
        if (!have && e->nc < 4) e->callers[e->nc++] = caller;
        for (int i = 0; i < 3; ++i) {
            e->g[i] = lg[i]; e->r[i] = lr[i];
            e->gMin[i] = fminf(e->gMin[i], lg[i]); e->gMax[i] = fmaxf(e->gMax[i], lg[i]);
            e->rMin[i] = fminf(e->rMin[i], lr[i]); e->rMax[i] = fmaxf(e->rMax[i], lr[i]);
        }
    }
    ReleaseSRWLockExclusive(&g_skelNearLock);
    if (g_skelNear.fetch_sub(1) == 1) {
        AcquireSRWLockShared(&g_skelNearLock);
        const float bd = WrapPiB(r[3] - g[3]) * 57.2958f;
        Log("skelnear: %zu skeletons near Kratos (body turned %.1f deg from the game's facing; frames x right y up z back)",
            g_skelNearList.size(), bd);
        const uint8_t* base = (const uint8_t*)Base();
        for (auto& s : g_skelNearList) {
            char cs[96] = {};
            int o = 0;
            for (int i = 0; i < s.nc; ++i)
                o += sprintf_s(cs + o, sizeof(cs) - o, " +0x%llx", (unsigned long long)(s.callers[i] - (uintptr_t)base));
            Log("skelnear: skel %p joints %4d hash0 %016llx passes %4llu%s | game(%.2f %.2f %.2f) spread %.2f | "
                "render(%.2f %.2f %.2f) spread %.2f%s", s.skel, s.joints, (unsigned long long)s.hash0,
                (unsigned long long)s.passes, cs, s.g[0], s.g[1], s.g[2],
                fmaxf(fmaxf(s.gMax[0] - s.gMin[0], s.gMax[1] - s.gMin[1]), s.gMax[2] - s.gMin[2]), s.r[0], s.r[1], s.r[2],
                fmaxf(fmaxf(s.rMax[0] - s.rMin[0], s.rMax[1] - s.rMin[1]), s.rMax[2] - s.rMin[2]),
                s.skel == g_heroSkel.load() ? " <== HERO" : "");
        }
        ReleaseSRWLockShared(&g_skelNearLock);
    }
}

// diagnostic "flydiag N": the flying Blades copies (3-joint skeletons, hash0 65301791f418f62d, spawned by Blades
// attacks): root vs the animated (published) and the motion-controlled wrists, in the rendered and the game frame
std::atomic<int> g_flyDiag{0};
void FlyDiag(int n) { g_flyDiag = n; Log("flydiag: next %d flying-blade passes", n); }
static void FlyDiagLog(uint8_t* skel, const float* root) {
    if (g_flyDiag.load() <= 0) return;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint64_t* hs = *(const uint64_t**)(skel + 0x30);
    if (n > 4 || !hs || IsBadReadPtr(hs, 8) || hs[0] != 0x65301791f418f62dull) return;
    --g_flyDiag;
    Mat4 aw[2], iw[2], ie;
    bool ha[2], hi[2];
    const uint64_t wh[2] = {0x6de47b0b82506234ull, 0x3626a7d4c72e93aeull};
    float d[4] = {-1, -1, -1, -1};
    for (int h = 0; h < 2; ++h) {
        ha[h] = StableJoint(wh[h], &aw[h]);
        hi[h] = ArmsIkResult(h, &iw[h], &ie);
        auto D = [&](const Mat4& m) {
            const float x = root[12] - m.m[3][0], y = root[13] - m.m[3][1], z = root[14] - m.m[3][2];
            return sqrtf(x * x + y * y + z * z);
        };
        if (ha[h]) d[h] = D(aw[h]);
        if (hi[h]) d[2 + h] = D(iw[h]);
    }
    float g[4], r[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g)); memcpy(r, g_renderRoot, sizeof(r));
    ReleaseSRWLockShared(&g_rootLock);
    float lg[3], lr[3];
    BodyLocal(root + 12, g, lg);
    BodyLocal(root + 12, r, lr);
    Log("flydiag: skel %p n %d | to animated wrist L %.2f R %.2f | to IK wrist L %.2f R %.2f | game-frame (%.2f %.2f %.2f) "
        "render-frame (%.2f %.2f %.2f) body turned %.0f deg", skel, n, d[0], d[1], d[2], d[3], lg[0], lg[1], lg[2], lr[0],
        lr[1], lr[2], WrapPiB(r[3] - g[3]) * 57.2958f);
}

static void AttDiagLog(const char* what, uint8_t* skel, uintptr_t caller) {
    if (g_attDiag <= 0) return;
    --g_attDiag;
    const int16_t n = *(int16_t*)(skel + 0x38);
    const float* R = (const float*)(*(uint8_t**)(skel + 0x58) + (size_t)(uint16_t)n * 0x40);
    float g[4], r[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g)); memcpy(r, g_renderRoot, sizeof(r));
    ReleaseSRWLockShared(&g_rootLock);
    float lg[3], lr[3];
    BodyLocal(R + 12, g, lg);
    BodyLocal(R + 12, r, lr);
    const float oy = atan2f(R[8], R[10]);
    Log("attdiag: %s t%u caller +0x%llx game-frame(%.2f %.2f %.2f) render-frame(%.2f %.2f %.2f) objYaw-game %.1f "
        "objYaw-render %.1f bodyDelta %.1f", what, GetCurrentThreadId(), (unsigned long long)(caller - (uintptr_t)Base()),
        lg[0], lg[1], lg[2], lr[0], lr[1], lr[2], WrapPiB(oy - g[3]) * 57.2958f, WrapPiB(oy - r[3]) * 57.2958f,
        WrapPiB(r[3] - g[3]) * 57.2958f);
}

void YawTest(float deltaDeg) {
    uint8_t* obj = HeroObject();
    if (!obj) return;
    const float* m = (const float*)(obj + 0x60);
    g_yawTestTarget = atan2f(m[8], m[10]) + deltaDeg * 0.0174533f;
    g_yawTestFrames = 240;
    Log("yawtest: start delta %.1f deg", deltaDeg);
}

void FacingScan() {
    uint8_t* obj = HeroObject();
    int rootJ = FindJointByHash(0xfe57caacf699afe4ull);
    Mat4 r{};
    if (!obj || rootJ < 0 || !AnimatedJoint(rootJ, &r)) { Log("facescan: no hero/root"); return; }
    float yaw = atan2f(r.m[2][0], r.m[2][2]);
    Log("facescan: obj=%p root X(%.3f %.3f %.3f) Z(%.3f %.3f %.3f) pos(%.2f %.2f %.2f) yaw(back)=%.1f deg", obj, r.m[0][0],
        r.m[0][1], r.m[0][2], r.m[2][0], r.m[2][1], r.m[2][2], r.m[3][0], r.m[3][1], r.m[3][2], yaw * 57.2958f);
    int hits = 0;
    ScanBlock(obj, 0x1000, "obj", r, yaw, &hits);
    for (size_t o = 0; o < 0x1000 && hits < 60; o += 8) {
        if (!Readable(obj + o, 8)) break;
        uint8_t* p = *(uint8_t**)(obj + o);
        if ((uintptr_t)p < 0x10000 || ((uintptr_t)p & 7)) continue;
        char tag[32];
        sprintf_s(tag, "obj+0x%zx->", o);
        ScanBlock(p, 0x600, tag, r, yaw, &hits);
    }
    Log("facescan: %d hits", hits);
}

// diagnostic (read-only): skinned objects near Kratos' right wrist (the axe candidates). Distances to the wrist in the
// world cache (with our IK) and in the animation (without).
void AxeFind() {
    uint8_t* hero = HeroObject();
    uint8_t* hs = Skeleton();
    int wr = FindJointByHash(0x3626a7d4c72e93aeull);
    if (!hero || !hs || wr < 0) { Log("axefind: no hero"); return; }
    Mat4 wIk{}, wAn{};
    int16_t hn = *(int16_t*)(hs + 0x38);
    memcpy(&wIk, *(uint8_t**)(hs + 0x58) + ((size_t)(uint16_t)hn + wr) * 0x40, sizeof(Mat4));
    AnimatedJoint(wr, &wAn);
    Log("axefind: wrist ik(%.2f %.2f %.2f) anim(%.2f %.2f %.2f)", wIk.m[3][0], wIk.m[3][1], wIk.m[3][2], wAn.m[3][0],
        wAn.m[3][1], wAn.m[3][2]);
    auto dist = [](const float* a, const float* b) {
        return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    int shown = 0;
    for (int t = 0; t < 64; ++t) {
        uint8_t* tab = G<uint8_t*>(0x1422a98c0 + t * 8);
        if (!tab || IsBadReadPtr(tab, 0x38)) continue;
        for (int set = 0; set < 2; ++set) {
            uint8_t** arr = *(uint8_t***)(tab + (set ? 0x20 : 0x8));
            uint32_t cnt = *(uint32_t*)(tab + (set ? 0x30 : 0x18));
            if (!arr || cnt == 0 || cnt > 100000 || IsBadReadPtr(arr, (size_t)cnt * 8)) continue;
            for (uint32_t i = 0; i < cnt; ++i) {
                uint8_t* o = arr[i];
                if (!o || o == hero || IsBadReadPtr(o, 0x240)) continue;
                const float* op = (const float*)(o + 0x90);
                uint8_t* sk = *(uint8_t**)(o + 0x238);
                float sp[3] = {1e9f, 1e9f, 1e9f};
                int nj = 0;
                if (PlausibleSkeleton(sk)) {
                    nj = *(int16_t*)(sk + 0x38);
                    memcpy(sp, *(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)nj * 0x40 + 0x30, 12);
                }
                float d1 = fminf(dist(op, wIk.m[3]), dist(sp, wIk.m[3])), d2 = fminf(dist(op, wAn.m[3]), dist(sp, wAn.m[3]));
                if (fminf(d1, d2) > 1.0f) continue;
                Log("axefind: tab%d/%d[%u]=%p skel=%p joints=%d objPos(%.2f %.2f %.2f) skelRoot(%.2f %.2f %.2f) dIK=%.2f dAnim=%.2f",
                    t, set, i, o, nj ? sk : nullptr, nj, op[0], op[1], op[2], sp[0], sp[1], sp[2], d1, d2);
                if (++shown > 40) { Log("axefind: truncated"); return; }
            }
        }
    }
    Log("axefind: %d candidates", shown);
}

// diagnostic "nearobjs": every game object within 2.5 m of Kratos' neck (name, skeleton, joints, distance)
void NearObjects() {
    Mat4 neckM;
    if (!StableJoint(0xf14d9a553fdad7ccull, &neckM)) { Log("nearobjs: no neck"); return; }
    uint8_t* heroSkel = Skeleton();
    int n = 0;
    for (int t = 0; t < 64; ++t) {
        uint8_t* tab = G<uint8_t*>(0x1422a98c0 + t * 8);
        if (!tab || IsBadReadPtr(tab, 0x38)) continue;
        for (int set = 0; set < 2; ++set) {
            uint8_t** arr = *(uint8_t***)(tab + (set ? 0x20 : 0x8));
            uint32_t cnt = *(uint32_t*)(tab + (set ? 0x30 : 0x18));
            if (!arr || cnt == 0 || cnt > 100000 || IsBadReadPtr(arr, (size_t)cnt * 8)) continue;
            for (uint32_t i = 0; i < cnt; ++i) {
                uint8_t* o = arr[i];
                if (!o || IsBadReadPtr(o, 0x240)) continue;
                const char* nm = *(const char**)(o + 0x38);
                if (!nm || IsBadReadPtr(nm, 8)) nm = "?";
                uint8_t* sk = *(uint8_t**)(o + 0x238);
                const bool pl = PlausibleSkeleton(sk);
                const float* pos = pl ? (const float*)(*(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)*(int16_t*)(sk + 0x38) * 0x40 + 0x30)
                                      : (const float*)(o + 0x90);
                if (IsBadReadPtr(pos, 12)) continue;
                const float dx = pos[0] - neckM.m[3][0], dy = pos[1] - neckM.m[3][1], dz = pos[2] - neckM.m[3][2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > 2.5f) continue;
                Log("nearobjs: '%s' obj=%p skel=%p%s joints=%d dist %.2f m%s", nm, o, sk, pl ? "" : " (no skeleton)",
                    pl ? *(int16_t*)(sk + 0x38) : 0, d, sk == heroSkel ? " <== HERO" : "");
                ++n;
            }
        }
    }
    Log("nearobjs: %d objects", n);
}

// diagnostic "physjoints": hero joints the local->world pass does not recompute ((flags & 0x43) != 0x43, sub_1405908a0),
// i.e. placed by other systems (physics / chains); logs their distance to the parent and the nearest recomputed ancestor
void PhysJoints() {
    uint8_t* skel = Skeleton();
    if (!skel) return;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint8_t* fl = *(const uint8_t**)(skel + 0x60);
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    const uint64_t* hashes = *(const uint64_t**)(skel + 0x30);
    if (!fl || IsBadReadPtr(fl, n)) { Log("physjoints: no flags"); return; }
    auto W = [&](int j) -> const Mat4& { return *(const Mat4*)(mats + ((size_t)n + j) * 0x40); };
    int cnt = 0;
    for (int j = 1; j < n; ++j) {
        if ((fl[j] & 0x43) == 0x43) continue;
        ++cnt;
        int a = JointParent(j);
        while (a >= 0 && (fl[a] & 0x43) != 0x43) a = JointParent(a);
        const int p = JointParent(j);
        auto dist = [&](int x, int y) {
            if (x < 0 || y < 0) return -1.f;
            const float dx = W(x).m[3][0] - W(y).m[3][0], dy = W(x).m[3][1] - W(y).m[3][1], dz = W(x).m[3][2] - W(y).m[3][2];
            return sqrtf(dx * dx + dy * dy + dz * dz);
        };
        if (cnt <= 400)
            Log("physjoints: %4d flags %02x parent %4d (%.3f m) anchor %4d (%.3f m) hash %016llx", j, fl[j], p, dist(j, p), a,
                dist(j, a), (unsigned long long)hashes[j]);
    }
    Log("physjoints: %d of %d joints not recomputed by the pass", cnt, n);
}

void ScanObjects() {
    uint8_t* hero = G<uint8_t*>(kHeroPtr);
    int shown = 0;
    for (int t = 0; t < 64; ++t) {
        uint8_t* tab = G<uint8_t*>(0x1422a98c0 + t * 8);
        if (!tab || IsBadReadPtr(tab, 0x38)) continue;
        for (int set = 0; set < 2; ++set) {
            uint8_t** arr = *(uint8_t***)(tab + (set ? 0x20 : 0x8));
            uint32_t cnt = *(uint32_t*)(tab + (set ? 0x30 : 0x18));
            if (!arr || cnt == 0 || cnt > 100000 || IsBadReadPtr(arr, (size_t)cnt * 8)) continue;
            for (uint32_t i = 0; i < cnt; ++i) {
                uint8_t* o = arr[i];
                if (!o || IsBadReadPtr(o, 0x240)) continue;
                uint8_t* sk = *(uint8_t**)(o + 0x238);
                if (!PlausibleSkeleton(sk)) continue;
                uint8_t* back = *(uint8_t**)(o + 0x220);
                Mat4 root{};
                memcpy(&root, *(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)*(int16_t*)(sk + 0x38) * 0x40, sizeof(root));
                if (back == hero || shown < 60 || *(int16_t*)(sk + 0x38) >= 120)
                    Log("fp: obj tab%d/%d[%u]=%p skel=%p joints=%d back=%p%s pos(%.2f %.2f %.2f)", t, set, i, o, sk,
                        *(int16_t*)(sk + 0x38), back, back == hero ? " <== HERO" : "", root.m[3][0], root.m[3][1],
                        root.m[3][2]);
                ++shown;
            }
        }
    }
    Log("fp: object scan done, %d skinned objects (hero wrapper %p)", shown, hero);
}

// ---- post-animation joint edits (world cache, skel+0x58) --------------------------------------------------------
int JointParent(int i) {
    uint8_t* skel = Skeleton();
    if (!skel) return -1;
    return (int)(int16_t)(*(uint64_t*)(*(uint8_t**)(skel + 0x20) + (size_t)i * 8) >> 48);
}

bool IsDescendant(int j, int ancestor) {
    for (int k = j, guard = 0; k >= 0 && guard < 64; k = JointParent(k), ++guard)
        if (k == ancestor) return true;
    return false;
}

Mat4* WorldCache(int i) {
    uint8_t* skel = Skeleton();
    if (!skel) return nullptr;
    using PFN_Valid = char(__fastcall*)(uint8_t*);
    if (!((PFN_Valid)((uint8_t*)GameVa(0x14058f1d0ull)))(skel)) return nullptr;
    int16_t n = *(int16_t*)(skel + 0x38);
    if (i < 0 || i >= n) return nullptr;
    return (Mat4*)(*(uint8_t**)(skel + 0x58) + ((size_t)(uint16_t)n + i) * 0x40);
}

float g_armTestDy = 0.f;

// ---- post-hook on the engine's local->world pass (sub_1405908a0, several times per frame on job threads) ----------
// Arm edits are computed once per frame on the game thread and published as a small plan; every world pass on the
// hero's skeleton re-applies it, so the last pass before skinning carries it (a single edit at View::Update was
// overwritten - F-015).
// Boat (owner: both hands authored while in the boat). The boat's docking state machine sub_140803760(boat, newState):
// states 1/4 = boarded, 3/5 = left (the game's own BoatEntered / BoatExited telemetry uses exactly these).
extern std::atomic<uint64_t> g_frameNo;
// D-070 (owner: the loose Blades chain - pinned at the hand and the bottom of the Blade - drifts off both pins when
// walking): the chain is attachment::tChainedWeapon (links "goChainLink"), simulated in vfunc_11 (0x1408853f0), which
// reads the hand from the hero's world cache (sub_140592770 + joint this+0x50, copy at this+0x9f40, translation +0x9f70)
constexpr HookSite kChainUpdate{"ChainUpdate", 0x1408853f0, 12,
    {0x40, 0x55, 0x41, 0x56, 0x48, 0x8d, 0xac, 0x24, 0x48, 0xfd, 0xff, 0xff, 0x48, 0x81, 0xec, 0xb8}};
using PFN_ChainUpdate = void(__fastcall*)(uint8_t* self);
PFN_ChainUpdate o_ChainUpdate = nullptr;
struct ChainRec { uint8_t* self = nullptr; float simHand[3] = {}; uint64_t frame = 0; short joint = -1; uint64_t updates = 0;
                  float corr[3] = {}, applied[3] = {}; };
static ChainRec g_chainRec[4];
static SRWLOCK g_chainRecLock = SRWLOCK_INIT;
static void ChainRecordSeh(uint8_t* self, ChainRec* r) {
    __try {
        r->joint = *(short*)(self + 0x50);
        const float* t = (const float*)(self + 0x9f70);
        r->simHand[0] = t[0]; r->simHand[1] = t[1]; r->simHand[2] = t[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->joint = -1;
    }
}
static bool EnsureWorldSafe(uint8_t* skel, char force);
// LIVE chaindiag: the sim had the hand 5-9 cm BEHIND the drawn hand along the movement (0 standing) - it reads the hero
// world cache before this frame's hero pass. "chainsync" (default on): the hero pass is forced (with the mod's hand
// edits) right before the chain simulates, so its pins are this frame's drawn hand / Blade point.
// default OFF: LIVE screenshots, the pin shift (v2) made the chain fly out sideways while walking (the lag estimate came
// from +0x9f70, which the update overwrites with the chain object's own matrix - not the hand pin)
std::atomic<bool> g_chainSync{true};  // D-070 v4: ON (owner confirmed the chain test with it on)
std::atomic<int> g_chainMode{1};
void ChainMode(int m) { g_chainMode = m; Log("fp: chain fix mode %d", m); }
std::atomic<int> g_chainInFlight{0};
// v3: the game carries the chain's particles with a hero joint (sub_140884af0: joint this+0xa15c, its last position
// at this+0xa0d0, particles at this+0x6350 stride 0x20 = pos + previous pos, count this+0x9ba8; more points at
// this+0x61a0 stride 0x80, count this+0x9bac) - read before Kratos is moved for the frame. At the draw (after the hero
// pass) the same carry step is repeated with the fresh joint; +0xa0d0 is updated so the game's next step adds only
// the rest. Returns the carry distance (m), -1 if skipped.
// v4 (LIVE chaindump): the hanging links are transforms at +0x1680.. (stride 0x40, translation rows) from the hand down
// and back up to the Blade; the hand point the sim used is kept at +0x860 (copies +0x1530 / +0x1570). At the draw every
// position triplet near the hand in the link / particle blocks is moved by (drawn hand now - sim hand) and +0x860 is
// updated, so the chain hangs from this frame's hand
static float ChainShiftSeh(uint8_t* self) {
    __try {
        const short j = *(short*)(self + 0x50);
        uint8_t* hs = *(uint8_t**)(*(uint8_t**)(*(uint8_t**)(*(uint8_t**)(self + 0x28) + 0x10) + 8) + 0x238);
        if (!hs || j < 0) return -1.f;
        const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
        if (j >= hn) return -1.f;
        const float* w = (const float*)(*(uint8_t**)(hs + 0x58) + ((size_t)hn + j) * 0x40 + 0x30);
        // v5 (LIVE chainends: v4 halved the hand-end lag 11-13 -> 5-7 cm; the links lag ~2 frames, the +0x860 copy 1):
        // the shift is the hand end link's own gap to the drawn hand point, minus its standing rest offset
        // mode 1 (v4, default - owner: "the second test worked"): shift by drawn hand - the sim's hand copy (+0x860);
        // mode 2 (v5, experimental "chainmode 2"): by the hand end link's gap to the drawn hand minus its rest offset
        float* endLink = (float*)(self + 0x1a00);
        float* simHand = (float*)(self + 0x860);
        float d[3];
        float ref[3];
        if (g_chainMode.load() == 2) {
            static float rest[3] = {0.f, 0.f, 0.f};
            for (int c = 0; c < 3; ++c) d[c] = w[c] - endLink[c];
            if (!HeroMoving()) {
                for (int c = 0; c < 3; ++c) rest[c] += (d[c] - rest[c]) * 0.1f;
                return 0.f;
            }
            for (int c = 0; c < 3; ++c) { d[c] -= rest[c]; ref[c] = endLink[c]; }
        } else {
            for (int c = 0; c < 3; ++c) { d[c] = w[c] - simHand[c]; ref[c] = simHand[c]; }
        }
        const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < 1e-5f) return 0.f;
        if (dl > 0.3f) return -1.f;  // a teleport / draw: leave it to the game
        auto shiftBlock = [&](size_t from, size_t to, size_t step) {
            for (size_t o = from; o + 12 <= to; o += step) {
                float* v = (float*)(self + o);
                const float e0 = v[0] - ref[0], e1 = v[1] - ref[1], e2 = v[2] - ref[2];
                if (e0 * e0 + e1 * e1 + e2 * e2 > 1.2f * 1.2f) continue;
                v[0] += d[0]; v[1] += d[1]; v[2] += d[2];
            }
        };
        shiftBlock(0x1500, 0x1c00, 0x10);   // hand copies + link transforms (translation rows sit on 16-byte rows)
        shiftBlock(0x6180, 0x6350 + 92 * 0x20, 0x10);  // particle points / particles (pos and prev)
        if (g_chainMode.load() != 2) for (int c = 0; c < 3; ++c) simHand[c] = w[c];
        return dl;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.f;
    }
}
static float ChainCarrySeh(uint8_t* self) {
    __try {
        const short j = *(short*)(self + 0xa15c);
        if (j < 0) return -1.f;
        uint8_t* hs = *(uint8_t**)(*(uint8_t**)(*(uint8_t**)(*(uint8_t**)(self + 0x28) + 0x10) + 8) + 0x238);
        if (!hs) return -1.f;
        const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
        if (j >= hn) return -1.f;
        const float* w = (const float*)(*(uint8_t**)(hs + 0x58) + ((size_t)hn + j) * 0x40 + 0x30);
        float* last = (float*)(self + 0xa0d0);
        const float d[3] = {w[0] - last[0], w[1] - last[1], w[2] - last[2]};
        const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < 1e-5f || dl > 0.5f) return dl > 0.5f ? -1.f : 0.f;
        const int np = *(int*)(self + 0x9ba8);
        if (np < 0 || np > 400) return -1.f;
        for (int i = 0; i < np; ++i) {
            float* p = (float*)(self + 0x6350 + (size_t)i * 0x20);
            for (int c = 0; c < 3; ++c) { p[c] += d[c]; p[4 + c] += d[c]; }
        }
        const int nq = *(int*)(self + 0x9bac);
        for (int i = 0; i < nq && i < 64; ++i) {
            float* q = (float*)(self + 0x61a0 + (size_t)i * 0x80);
            for (int c = 0; c < 3; ++c) q[c] += d[c];
        }
        for (int c = 0; c < 3; ++c) last[c] = w[c];
        return dl;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.f;
    }
}
void ChainSync(bool on) { g_chainSync = on; Log("fp: Blades chain simulated on this frame's hands %s", on ? "ON" : "OFF"); }
// v2 (LIVE: forcing the hero pass changed nothing - the game has not moved Kratos for the frame yet when the chain
// simulates): the chain's pins are shifted by the lag measured at the draw (drawn hand - uncorrected sim hand, eased)
// for the duration of the sim: the hero's hand joints (this+0x50, this+0xa156) and the chain object's whole skeleton
// (the Blade end, this+8 -> +0x238), restored right after
struct PinSave { float* p; float v[3]; };
static int ShiftPinsSeh(uint8_t* self, uint8_t* hs, const float d[3], PinSave* save, int cap) {
    int n = 0;
    __try {
        const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
        uint8_t* hm = *(uint8_t**)(hs + 0x58);
        const short js[2] = {*(short*)(self + 0x50), *(short*)(self + 0xa156)};
        for (short j : js)
            if (j >= 0 && j < hn && n < cap) {
                float* t = (float*)(hm + ((size_t)hn + j) * 0x40 + 0x30);
                bool dup = false;
                for (int k = 0; k < n; ++k) dup |= save[k].p == t;
                if (dup) continue;
                save[n].p = t; memcpy(save[n].v, t, 12); ++n;
            }
        uint8_t* obj = *(uint8_t**)(self + 8);
        uint8_t* bs = obj ? *(uint8_t**)(obj + 0x238) : nullptr;
        if (bs && bs != hs) {
            const int bn = (int)(uint16_t)*(int16_t*)(bs + 0x38);
            uint8_t* bm = *(uint8_t**)(bs + 0x58);
            if (bn > 0 && bn <= 64 && bm)
                for (int j = 0; j < bn && n < cap; ++j) {
                    float* t = (float*)(bm + ((size_t)bn + j) * 0x40 + 0x30);
                    if (t[0] == 0.f && t[1] == 0.f && t[2] == 0.f) continue;  // unused joints at the origin
                    save[n].p = t; memcpy(save[n].v, t, 12); ++n;
                }
        }
        for (int k = 0; k < n; ++k) { save[k].p[0] += d[0]; save[k].p[1] += d[1]; save[k].p[2] += d[2]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (int k = 0; k < n; ++k) memcpy(save[k].p, save[k].v, 12);
        return 0;
    }
    return n;
}
static void RestorePinsSeh(PinSave* save, int n) {
    __try {
        for (int k = 0; k < n; ++k) memcpy(save[k].p, save[k].v, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void __fastcall hk_ChainUpdate(uint8_t* self) {
    if (!self) { o_ChainUpdate(self); return; }
    AcquireSRWLockExclusive(&g_chainRecLock);
    ChainRec* r = nullptr;
    int oldest = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_chainRec[i].self == self) { r = &g_chainRec[i]; break; }
        if (g_chainRec[i].frame < g_chainRec[oldest].frame) oldest = i;
    }
    if (!r) { r = &g_chainRec[oldest]; *r = ChainRec{}; r->self = self; Log("chain: chained weapon %p updating", self); }
    memset(r->applied, 0, sizeof(r->applied));
    ReleaseSRWLockExclusive(&g_chainRecLock);
    g_chainInFlight.fetch_add(1);
    o_ChainUpdate(self);
    g_chainInFlight.fetch_sub(1);
    AcquireSRWLockExclusive(&g_chainRecLock);
    ChainRecordSeh(self, r);
    r->frame = g_frameNo.load();
    ++r->updates;
    ReleaseSRWLockExclusive(&g_chainRecLock);
}
extern std::atomic<int> g_chainDiag;
extern float g_heroStepDiag[3];
// draw time, every frame: the lag of the chain sim's hand behind the drawn hand, measured on the UNCORRECTED sim hand
// (sim hand - the shift applied), eased -> the next sim's pin shift
static void ChainCorrTick() {
    if (!g_chainSync.load() || FpMode() != 1 || g_chainInFlight.load() > 0) return;
    // D-073 (owner: "during combat the chains forged together into one big chain through both hands"): during attacks /
    // throws the chain is stretched and the Blades are on the game's pose - shifting the links near the hand spliced it
    if (CombatBusy() || CombatBladesFree() || BladeThrowActive()) return;
    static double sum = 0;
    static int n = 0, skipped = 0;
    uint8_t* selves[4];
    int ns = 0;
    const uint64_t f0 = g_frameNo.load();
    AcquireSRWLockShared(&g_chainRecLock);
    for (auto& r : g_chainRec) if (r.self && f0 - r.frame <= 2) selves[ns++] = r.self;
    ReleaseSRWLockShared(&g_chainRecLock);
    for (int i = 0; i < ns; ++i) {
        const float c = ChainShiftSeh(selves[i]);
        if (c < 0.f) ++skipped; else { sum += c; ++n; }
    }
    if (n >= 600) {
        Log("chain: carried with this frame's Kratos at the draw - mean %.3f m per update (%d skipped)", sum / n, skipped);
        sum = 0; n = 0; skipped = 0;
    }
}
static void ChainCorrTickOld() {
    uint8_t* hs = g_heroSkel.load();
    if (!hs || FpMode() != 1) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    static const float ease = config::GetFloat("chain_lag_ease", 0.5f), maxLag = 0.4f;
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockExclusive(&g_chainRecLock);
    for (auto& r : g_chainRec) {
        if (!r.self || f - r.frame > 2 || r.joint < 0 || r.joint >= hn) { if (r.self) memset(r.corr, 0, sizeof(r.corr)); continue; }
        const float* w = (const float*)(hm + ((size_t)hn + r.joint) * 0x40 + 0x30);
        float lag[3];
        for (int c = 0; c < 3; ++c) lag[c] = w[c] - (r.simHand[c] - r.applied[c]);
        const float ll = sqrtf(lag[0] * lag[0] + lag[1] * lag[1] + lag[2] * lag[2]);
        if (ll > maxLag) for (int c = 0; c < 3; ++c) lag[c] = 0.f;  // a teleport / weapon change: no shift
        for (int c = 0; c < 3; ++c) r.corr[c] += (lag[c] - r.corr[c]) * ease;
    }
    ReleaseSRWLockExclusive(&g_chainRecLock);
}
// draw time: the chain's end particles vs its two drawn pins (hand point this+0x50 on the hero, Blade end this+0x520
// on the chain object's skeleton) - particles this+0x6350 + i*0x20, active i = [this+0xa140, this+0x9ba8)
static bool ChainEndsSeh(uint8_t* self, float out[4]) {  // out: |first-hand|, |last-blade|, first along, last along
    __try {
        // v4 layout (LIVE chaindump): link translations +0x1680 .. +0x1a00 stride 0x40; the end at +0x1a00 sits on the
        // hand point (this+0x50), the end at +0x1680 on the Blade's bottom (chain object joint this+0x520)
        if (true) {
            uint8_t* hs = *(uint8_t**)(*(uint8_t**)(*(uint8_t**)(*(uint8_t**)(self + 0x28) + 0x10) + 8) + 0x238);
            uint8_t* bs = *(uint8_t**)(*(uint8_t**)(self + 8) + 0x238);
            const short jh = *(short*)(self + 0x50), jb = *(short*)(self + 0x520);
            if (!hs || !bs || jh < 0 || jb < 0) return false;
            const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38), bn = (int)(uint16_t)*(int16_t*)(bs + 0x38);
            if (jh >= hn || jb >= bn) return false;
            const float* ph = (const float*)(*(uint8_t**)(hs + 0x58) + ((size_t)hn + jh) * 0x40 + 0x30);
            const float* pb = (const float*)(*(uint8_t**)(bs + 0x58) + ((size_t)bn + jb) * 0x40 + 0x30);
            const float* eh = (const float*)(self + 0x1a00);
            const float* eb = (const float*)(self + 0x1680);
            const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
            const float v1[3] = {eh[0] - ph[0], eh[1] - ph[1], eh[2] - ph[2]}, v2[3] = {eb[0] - pb[0], eb[1] - pb[1], eb[2] - pb[2]};
            out[0] = sqrtf(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
            out[1] = sqrtf(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]);
            out[2] = sl > 1e-4f ? (v1[0] * sx + v1[2] * sz) / sl : 0.f;
            out[3] = sl > 1e-4f ? (v2[0] * sx + v2[2] * sz) / sl : 0.f;
            return true;
        }
        uint8_t* hs = *(uint8_t**)(*(uint8_t**)(*(uint8_t**)(*(uint8_t**)(self + 0x28) + 0x10) + 8) + 0x238);
        uint8_t* bs = *(uint8_t**)(*(uint8_t**)(self + 8) + 0x238);
        const short jh = *(short*)(self + 0x50), jb = *(short*)(self + 0x520);
        const int i0 = *(int*)(self + 0xa140), i1 = *(int*)(self + 0x9ba8) - 1;
        if (!hs || !bs || jh < 0 || jb < 0 || i0 < 0 || i1 <= i0 || i1 > 400) return false;
        const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38), bn = (int)(uint16_t)*(int16_t*)(bs + 0x38);
        if (jh >= hn || jb >= bn) return false;
        const float* ph = (const float*)(*(uint8_t**)(hs + 0x58) + ((size_t)hn + jh) * 0x40 + 0x30);
        const float* pb = (const float*)(*(uint8_t**)(bs + 0x58) + ((size_t)bn + jb) * 0x40 + 0x30);
        const float* a = (const float*)(self + 0x6350 + (size_t)i0 * 0x20);
        const float* z = (const float*)(self + 0x6350 + (size_t)i1 * 0x20);
        // which end belongs to which pin: the nearer pairing
        const float da = (a[0]-ph[0])*(a[0]-ph[0]) + (a[1]-ph[1])*(a[1]-ph[1]) + (a[2]-ph[2])*(a[2]-ph[2]) +
                         (z[0]-pb[0])*(z[0]-pb[0]) + (z[1]-pb[1])*(z[1]-pb[1]) + (z[2]-pb[2])*(z[2]-pb[2]);
        const float db = (z[0]-ph[0])*(z[0]-ph[0]) + (z[1]-ph[1])*(z[1]-ph[1]) + (z[2]-ph[2])*(z[2]-ph[2]) +
                         (a[0]-pb[0])*(a[0]-pb[0]) + (a[1]-pb[1])*(a[1]-pb[1]) + (a[2]-pb[2])*(a[2]-pb[2]);
        const float* eh = da <= db ? a : z;
        const float* eb = da <= db ? z : a;
        const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
        float v1[3] = {eh[0] - ph[0], eh[1] - ph[1], eh[2] - ph[2]}, v2[3] = {eb[0] - pb[0], eb[1] - pb[1], eb[2] - pb[2]};
        out[0] = sqrtf(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
        out[1] = sqrtf(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]);
        out[2] = sl > 1e-4f ? (v1[0] * sx + v1[2] * sz) / sl : 0.f;
        out[3] = sl > 1e-4f ? (v2[0] * sx + v2[2] * sz) / sl : 0.f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// diagnostic "chaindump": every float triplet inside the first chained weapon (0 .. 0xa200) within 1.5 m of the drawn
// right / left Blade points (225 / 228), with its offset and distances - to map the link particles
static int ChainScanSeh(uint8_t* self, const float* p225, const float* p228, int* hits) {
    __try {
        for (size_t o = 0; o + 12 <= 0xa200; o += 4) {
            const float* v = (const float*)(self + o);
            if (!(fabsf(v[0]) < 1e5f && fabsf(v[1]) < 1e5f && fabsf(v[2]) < 1e5f)) continue;
            const float d1 = sqrtf((v[0]-p225[0])*(v[0]-p225[0]) + (v[1]-p225[1])*(v[1]-p225[1]) + (v[2]-p225[2])*(v[2]-p225[2]));
            const float d2 = sqrtf((v[0]-p228[0])*(v[0]-p228[0]) + (v[1]-p228[1])*(v[1]-p228[1]) + (v[2]-p228[2])*(v[2]-p228[2]));
            if (fminf(d1, d2) < 1.5f && (*hits)++ < 400)
                Log("chaindump: +0x%04zx (%.3f %.3f %.3f) to 225 %.3f to 228 %.3f", o, v[0], v[1], v[2], d1, d2);
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
static void ChainRawSeh(uint8_t* self) {
    __try {
        uint8_t* obj = *(uint8_t**)(self + 8);
        const float* om = (const float*)(obj + 0x60);
        uint8_t* owner = *(uint8_t**)(*(uint8_t**)(*(uint8_t**)(self + 0x28) + 0x10) + 8);
        Log("chaindump: joints +0x50=%d +0x54=%d +0xa14e=%d +0xa150=%d +0xa154=%d +0xa156=%d +0xa15c=%d +0x520=%d | chain obj %p "
            "(%.3f %.3f %.3f) owner obj %p skel %p (hero %p) | +0xa0d0 (%.3f %.3f %.3f)", *(short*)(self + 0x50),
            *(short*)(self + 0x54), *(short*)(self + 0xa14e), *(short*)(self + 0xa150), *(short*)(self + 0xa154),
            *(short*)(self + 0xa156), *(short*)(self + 0xa15c), *(short*)(self + 0x520), obj, om[12], om[13], om[14], owner,
            owner ? *(uint8_t**)(owner + 0x238) : nullptr, g_heroSkel.load(), *(float*)(self + 0xa0d0), *(float*)(self + 0xa0d4),
            *(float*)(self + 0xa0d8));
        for (int i = 0; i < 92; i += 3) {
            const float* p = (const float*)(self + 0x6350 + (size_t)i * 0x20);
            Log("chaindump: particle %2d (%.3f %.3f %.3f %.3f) prev (%.3f %.3f %.3f %.3f)", i, p[0], p[1], p[2], p[3], p[4], p[5],
                p[6], p[7]);
        }
        for (int i = 0; i < 2; ++i) {
            const float* q = (const float*)(self + 0x61a0 + (size_t)i * 0x80);
            Log("chaindump: point %d (%.3f %.3f %.3f %.3f)", i, q[0], q[1], q[2], q[3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("chaindump: raw read faulted");
    }
}
void ChainDump() {
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    const int j225 = 225, j228 = 228;
    const float* p225 = (const float*)(hm + ((size_t)hn + j225) * 0x40 + 0x30);
    const float* p228 = (const float*)(hm + ((size_t)hn + j228) * 0x40 + 0x30);
    AcquireSRWLockShared(&g_chainRecLock);
    uint8_t* self = nullptr;
    for (auto& r : g_chainRec) if (r.self && g_frameNo.load() - r.frame <= 2) { self = r.self; break; }
    ReleaseSRWLockShared(&g_chainRecLock);
    if (!self) { Log("chaindump: no live chain"); return; }
    int hits = 0;
    Log("chaindump: chain %p, 225 (%.3f %.3f %.3f), 228 (%.3f %.3f %.3f), counts +0x9ba8=%d +0x9bac=%d +0x9bb4=%d +0xa140=%d", self,
        p225[0], p225[1], p225[2], p228[0], p228[1], p228[2], *(int*)(self + 0x9ba8), *(int*)(self + 0x9bac),
        *(int*)(self + 0x9bb4), *(int*)(self + 0xa140));
    ChainScanSeh(self, p225, p228, &hits);
    Log("chaindump: %d triplets", hits);
    ChainRawSeh(self);
}
static void ChainEndsCheck() {
    if (g_chainDiag.load() <= 0) return;
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    const bool moving = sl > 0.02f;
    static double acc[2][4] = {};
    static int cnt[2] = {};
    uint8_t* selves[4];
    int ns = 0;
    const uint64_t f0 = g_frameNo.load();
    AcquireSRWLockShared(&g_chainRecLock);
    for (auto& r : g_chainRec) if (r.self && f0 - r.frame <= 2) selves[ns++] = r.self;
    ReleaseSRWLockShared(&g_chainRecLock);
    for (int i = 0; i < ns && i < 1; ++i) {  // the first chain (both behave alike)
        float o[4];
        if (!ChainEndsSeh(selves[i], o)) continue;
        const int k = moving ? 1 : 0;
        for (int c = 0; c < 4; ++c) acc[k][c] += o[c];
        ++cnt[k];
    }
    if (cnt[1] >= 60) {
        Log("chaindiag: chain ends vs pins - standing: hand %.3f m, Blade %.3f m (%d) | moving %.3f m/frame: hand %.3f m (along "
            "%+.3f), Blade %.3f m (along %+.3f) (%d)", cnt[0] ? acc[0][0] / cnt[0] : -1.0, cnt[0] ? acc[0][1] / cnt[0] : -1.0,
            cnt[0], sl, acc[1][0] / cnt[1], acc[1][2] / cnt[1], acc[1][1] / cnt[1], acc[1][3] / cnt[1], cnt[1]);
        memset(acc, 0, sizeof(acc));
        cnt[0] = cnt[1] = 0;
    }
}
// draw time: where the chain sim had the hand vs where the hand (same joint) is drawn, along the movement
static void ChainLagCheck() {
    if (g_chainDiag.load() <= 0) return;
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    static double sumA[4] = {}, sumAbs[4] = {}, sumStand[4] = {};
    static int cnt[4] = {}, cntS[4] = {};
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockShared(&g_chainRecLock);
    for (int i = 0; i < 4; ++i) {
        const ChainRec& r = g_chainRec[i];
        if (!r.self || f - r.frame > 2 || r.joint < 0 || r.joint >= hn) continue;
        const float* w = (const float*)(hm + ((size_t)hn + r.joint) * 0x40 + 0x30);
        const float ox = r.simHand[0] - w[0], oy = r.simHand[1] - w[1], oz = r.simHand[2] - w[2];
        const float d = sqrtf(ox * ox + oy * oy + oz * oz);
        if (sl < 0.01f) { sumStand[i] += d; ++cntS[i]; continue; }
        sumAbs[i] += d;
        sumA[i] += (ox * sx + oz * sz) / sl;
        if (++cnt[i] >= 60) {
            Log("chaindiag: chained weapon %p (hand joint %d, %llu updates): sim hand vs drawn hand while moving %.3f m/frame: "
                "%.3f m, along the movement %+.3f m | standing %.3f m", r.self, r.joint, (unsigned long long)r.updates, sl,
                sumAbs[i] / cnt[i], sumA[i] / cnt[i], cntS[i] ? sumStand[i] / cntS[i] : -1.0);
            sumA[i] = sumAbs[i] = sumStand[i] = 0;
            cnt[i] = cntS[i] = 0;
        }
    }
    ReleaseSRWLockShared(&g_chainRecLock);
}
constexpr HookSite kBoatDockState{"BoatDockState", 0x140803760, 5,
    {0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00}};
using PFN_BoatDock = void(__fastcall*)(uint8_t* boat, char state);
PFN_BoatDock o_BoatDock = nullptr;
std::atomic<bool> g_inBoat{false};

void __fastcall hk_BoatDock(uint8_t* boat, char state) {
    o_BoatDock(boat, state);
    // 1/4 boarding (from dock/beach), 2 seated; 3/5 leaving (to dock/beach), 6 = on the beach after 5, 0 idle.
    bool in = state == 1 || state == 2 || state == 4;
    if (in != g_inBoat.load()) Log("fp: boat %s (state %d)", in ? "entered" : "exited", state);
    g_inBoat = in;
}

// ---- attached objects follow the IK arm ------------------------------------------------------------------------------
// The axe (goaxe00, right hand) and the guardian shield (gokratosshield00, left forearm) are separate skinned objects
// whose root the attachment system derives from Kratos' hand a frame late / before our IK: in VR the axe trailed the
// hand and "corrected" itself whenever the body moved (owner report). Each of their world passes is re-anchored rigidly
// to the arm's CURRENT IK result, keeping the grip offset (learned whenever the hand is still, so lag cannot skew it).
struct Attached {
    const char* name;
    int hand;          // 1 right, 0 left
    bool elbow;        // anchor = forearm (elbow joint) instead of the wrist
    uint64_t anchorHash;  // that joint (the attachment system follows its ANIMATED pose, published before our IK)
    const char* key;      // gowvr.ini prefix of the learned grip
    std::atomic<uint8_t*> skel{nullptr};
    std::atomic<uint8_t*> obj{nullptr};  // D-146: its game object (parent link at +0xa0)
    SRWLOCK lock = SRWLOCK_INIT;
    Mat4 offset{};
    bool haveOffset = false;
    // D-150: the normal grip (gowvr.ini / learned outside attacks); a grip taken during an attack or a full-animation move
    // (tempGrip) is used until the axe leaves the hand, then the normal one is back
    Mat4 baseOffset{};
    bool haveBase = false, tempGrip = false;
    Mat4 prevAnchor{}, curAnchor{};
    uint64_t anchorFrame = 0;
    bool calibrated = false;   // grip learned while the arm was NOT motion controlled (authoritative, saved)
    std::atomic<int> awayStreak{0};
    bool dirty = false;        // save pending (game thread)
    bool loaded = false;
    std::atomic<uint64_t> fixes{0}, detached{0};
    double sumDAnim = 0, sumDIk = 0; uint64_t nD = 0;  // stats: which anchor does the game follow?
};
Attached g_attached[2] = {{"goaxe00", 1, false, 0x3626a7d4c72e93aeull, "grip_axe"},
                          {"gokratosshield00", 0, true, 0xa46a6b3097288180ull, "grip_shield"}};
std::atomic<bool> g_axeInHand{false};
std::atomic<ULONGLONG> g_axeCallAt{0}, g_axeSheatheAt{0};
std::atomic<int> g_axeDiag{0};  // vrmap: recall/draw and put-away requests (GetTickCount64)
std::atomic<uint64_t> g_frameNo{0};

// D-036: effects riding on the axe - the owner's "fire effect on the axe" (LIVE skelnear: two 3-joint skeletons whose
// roots sit 0.08 m from the axe root; not in the object tables). The game places them from ITS axe placement, but we
// move the axe (sheathed: glued to the turned chest; held: onto the motion-controlled hand), so they were left behind.
// Each axe pass we correct records (game root R, our root R'); a small skeleton whose root sits on a recent R gets the
// same correction S' = S x R^-1 x R'.
struct AxeCorr { float pos[3]; Mat4 D; Mat4 R, Rn; uint64_t frame; };
AxeCorr g_axeCorr[6];
uint32_t g_axeCorrNext = 0;
SRWLOCK g_axeCorrLock = SRWLOCK_INIT;
std::atomic<uint64_t> g_satellites{0};
// inverse of a rotation x uniform scale + translation (the sheathed axe is scaled 0.85: RigidInverse would be wrong)
static Mat4 InverseUniform(const Mat4& M) {
    const float s2 = M.m[0][0] * M.m[0][0] + M.m[0][1] * M.m[0][1] + M.m[0][2] * M.m[0][2];
    const float k = s2 > 1e-12f ? 1.f / s2 : 1.f;
    Mat4 I{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) I.m[r][c] = M.m[c][r] * k;
    for (int c = 0; c < 3; ++c) I.m[3][c] = -(M.m[3][0] * I.m[0][c] + M.m[3][1] * I.m[1][c] + M.m[3][2] * I.m[2][c]);
    I.m[3][3] = 1.f;
    return I;
}
// D-095 (owner: the glow at the bottom of the handle "jumps to one spot randomly during attack animations"): the axe as
// DRAWN - its joint-0 world right after our placement (FixAttached in hand, the glue on the back) - for the effect reads
// (hk_WorldFromLocals), which no longer depend on what the axe's cache holds at the moment of the read
SRWLOCK g_axeDrawnLock = SRWLOCK_INIT;
Mat4 g_axeDrawnW0{};
uint64_t g_axeDrawnFrame = 0;
// D-153: the drawn axe as a segment from its root (the grip) to its farthest joint (the head), per frame + the last one
float g_axeSeg[2][3] = {}, g_axeSegPrev[2][3] = {};
uint64_t g_axeSegFrame = 0;
bool g_axeSegHavePrev = false;
static void RecordAxeDrawn(uint8_t* skel) {
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint8_t* mats = *(uint8_t**)(skel + 0x58);
    const Mat4 W0 = *(const Mat4*)(mats + (size_t)n * 0x40);
    if (fabsf(W0.m[3][0]) + fabsf(W0.m[3][1]) + fabsf(W0.m[3][2]) < 1.f) return;  // model space: not a placement
    static uint8_t* tipSkel = nullptr;
    static int tipIdx = -1;
    if (skel != tipSkel) {  // the head = the joint farthest from the grip (once per axe skeleton)
        // D-154 fix (owner log D-153: "joint -1 (896.33 m)" - a joint not placed yet in that pass won the search and the
        // axe was never searched again, so the re-test never ran): only joints within 1.5 m count, and no answer = the
        // next pass searches again
        float best = 0.f;
        int bi = -1;
        for (int j = 1; j < n; ++j) {
            const float* t = (const float*)(mats + ((size_t)n + j) * 0x40 + 0x30);
            const float dx = t[0] - W0.m[3][0], dy = t[1] - W0.m[3][1], dz = t[2] - W0.m[3][2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > best && d2 < 1.5f * 1.5f) { best = d2; bi = j; }
        }
        if (best > 0.3f * 0.3f) {
            tipSkel = skel;
            tipIdx = bi;
            Log("fp: axe head for hits = joint %d (%.2f m from the grip)", tipIdx, sqrtf(best));
        }
    }
    const uint64_t fr = g_frameNo.load();
    AcquireSRWLockExclusive(&g_axeDrawnLock);
    g_axeDrawnW0 = W0;
    g_axeDrawnFrame = fr;
    if (tipIdx > 0 && tipIdx < n) {
        const float* t = (const float*)(mats + ((size_t)n + tipIdx) * 0x40 + 0x30);
        if (fr != g_axeSegFrame) {
            if (g_axeSegFrame) { memcpy(g_axeSegPrev, g_axeSeg, sizeof(g_axeSeg)); g_axeSegHavePrev = fr - g_axeSegFrame <= 2; }
            g_axeSegFrame = fr;
        }
        for (int i = 0; i < 3; ++i) { g_axeSeg[0][i] = W0.m[3][i]; g_axeSeg[1][i] = t[i]; }
    }
    ReleaseSRWLockExclusive(&g_axeDrawnLock);
}
bool AxeHitSegment(float cur[2][3], float prev[2][3]) {
    AcquireSRWLockShared(&g_axeDrawnLock);
    const bool ok = g_axeSegFrame && g_frameNo.load() - g_axeSegFrame <= 2;
    memcpy(cur, g_axeSeg, sizeof(g_axeSeg));
    memcpy(prev, g_axeSegHavePrev ? g_axeSegPrev : g_axeSeg, sizeof(g_axeSeg));
    ReleaseSRWLockShared(&g_axeDrawnLock);
    return ok;
}
static bool AxeDrawn(Mat4* W0) {
    AcquireSRWLockShared(&g_axeDrawnLock);
    const uint64_t fr = g_axeDrawnFrame;
    *W0 = g_axeDrawnW0;
    ReleaseSRWLockShared(&g_axeDrawnLock);
    return fr && g_frameNo.load() - fr <= 2;
}
// D-095: the lunge-ghost shift (D-081) this thread applied to the pass it is running (x, z), so FixAttached can record
// the axe correction against the GAME's axe (owner log: "lunge ghost moved back ... axe, 1.47 m" - the effects the
// game placed on its real axe were corrected from a root 1.47 m away)
thread_local float t_ghostShift[2] = {0.f, 0.f};
// D-130: this pass's axe as the GAME placed it, when the back glue moved it - FixAttached judges in hand / away from
// this, never from our own glued placement (D-129: the glue fed its own decision and the axe never came back)
thread_local bool t_axeGlued = false;
thread_local Mat4 t_axeGameRoot{};
static void RecordAxeCorrection(const Mat4& R, const Mat4& Rnew) {
    AcquireSRWLockExclusive(&g_axeCorrLock);
    AxeCorr& c = g_axeCorr[g_axeCorrNext++ % 6];
    for (int i = 0; i < 3; ++i) c.pos[i] = R.m[3][i];
    c.D = Mul(InverseUniform(R), Rnew);
    c.R = R;
    c.Rn = Rnew;
    c.frame = g_frameNo.load();
    ReleaseSRWLockExclusive(&g_axeCorrLock);
}
// diagnostic (fxjump): how the axe's effect passes (3-joint 3124c569...) are matched to an axe correction
struct SatDiag { uint64_t passes = 0, self = 0, matched = 0, noRecent = 0, tooFar = 0, locked = 0; int logged = 0; };
static SatDiag g_satDiag;
static SRWLOCK g_satDiagLock = SRWLOCK_INIT;
// D-061: the effect was matched to the axe by distance to a recorded GAME axe root (< 0.2 m, <= 2 frames old). In
// attacks the animated axe moves more than that per frame, so 27 % of the effect's passes found no match (LIVE fxjump:
// 203 of 750, nearest 0.20-0.34 m, age 1-2) and were drawn at the game's animated axe for that frame -> one-frame spikes
// (vanilla: 0 spikes, F1: 10-20 per attack burst) = the stretched glow / flame / wind the owner saw. Now each effect's
// offset ON the axe is learned from tight matches (< 0.12 m, <= 1 frame) and the effect is placed on the latest DRAWN axe
// with it: our axe follows the controller, so this does not depend on catching the game's fast swing.
struct SatLearn {
    uint8_t* skel = nullptr;
    Mat4 E;
    uint64_t frame = 0, seen = 0;
    // D-094: the root parameter its last world pass was built with (P), that pass's joint-0 world as written (W0) and the
    // matrix buffer / joint count then - the draw-time re-place moves the cached pose by P^-1 x P' (SatDrawReplace)
    Mat4 P, W0;
    uint8_t* mats = nullptr;
    int n = 0;
    bool pValid = false;
    // D-094b: last frame its passes resumed after a break of > 30 frames (a pooled skeleton reused for another effect)
    uint64_t lastGap = 0;
    // D-096: the drawn axe its last pass was placed on (A0) - the draw moves it by A0^-1 x the axe as drawn now
    Mat4 A0;
    bool a0Valid = false;
};
static SatLearn g_satLearn[8];
// strict (default, LIVE A/B: attack spikes 0 / 0 vs 2 / 0, kinks 3 / 1 vs 11 / 3)
std::atomic<bool> g_satLock{true}, g_satStrict{true};
void SatStrict(bool on) { g_satStrict = on; Log("fp: axe effect offset learned from same-frame passes only %s", on ? "ON" : "OFF"); }
void SatLock(bool on) { g_satLock = on; Log("fp: axe effects locked to the drawn axe with a learned offset %s", on ? "ON" : "OFF (distance match only)"); }
// D-096 (owner: the pommel glow "still jumps"; owner log after D-095: every effect read of the axe now answered on the
// drawn axe (69856 of 69856), and "axe effect 0.43-0.45 m from the game's axe - given the axe's latest correction" every
// frame): effects that build their root from the axe through the read hook are ALREADY on the drawn axe, but the matching
// below took them for game placements (nothing within 0.2 m of the game's axe) and the D-091 fallback corrected them a
// second time - the glow ~0.44 m off (one spot), and learned offsets were polluted the same way. Now each pass is told
// apart by where its root sits at this instant: nearer the drawn axe -> left as it is; nearer the game's axe (its root
// read through the original, same call) -> moved by game axe^-1 x drawn axe. The draw then moves each effect with the
// drawn axe since its pass (A0^-1 x the axe now) - no learned offsets, nothing to expire. gowvr.ini sat_v2=0: the old path.
using PFN_WorldFromLocals = uint64_t(__fastcall*)(uint8_t* skel, float* out, int joint);
extern PFN_WorldFromLocals o_WorldFromLocals;
std::atomic<uint64_t> g_satOnDrawn{0}, g_satOnGame{0}, g_satAway{0}, g_satSelf{0};
static bool AxeSatelliteV2(uint8_t* skel, const float* root, float* out, bool self) {
    // D-098 (owner: "some objects like tables are flying"): v2 took ANY small skeleton (<= 16 joints) within 1.5 m of
    // either axe and moved it game axe -> drawn axe - props next to Kratos (tables, debris) flew with the axe. Only the
    // axe's own effect family (hash0 3124c569..., D-059 - the D-091 fallback had the same limit) is placed now
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8) || (hh[0] >> 32) != 0x3124c569ull) return false;
    const uint64_t f = g_frameNo.load();
    Mat4 W0;
    uint8_t* ax = g_attached[0].skel.load();
    if (!ax || !o_WorldFromLocals || !AxeDrawn(&W0)) return false;
    alignas(16) Mat4 g0;
    o_WorldFromLocals(ax, &g0.m[0][0], 0);  // the game's axe root at this instant
    if (fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) < 1.f) return false;
    const Mat4& Rt = *(const Mat4*)root;
    auto dist = [&](const Mat4& a) {
        const float dx = Rt.m[3][0] - a.m[3][0], dy = Rt.m[3][1] - a.m[3][1], dz = Rt.m[3][2] - a.m[3][2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    const float dD = dist(W0), dG = dist(g0);
    AcquireSRWLockExclusive(&g_axeCorrLock);
    SatLearn* s = nullptr;
    int oldest = 0;
    for (int i = 0; i < 8; ++i) {
        if (g_satLearn[i].skel == skel) { s = &g_satLearn[i]; break; }
        if (g_satLearn[i].seen < g_satLearn[oldest].seen) oldest = i;
    }
    bool placed = false;
    if (self) {
        // fed its own cached world = our earlier placement on the axe of that time (A0 kept): drawn as it is
        if (s && s->a0Valid) {
            memcpy(out, root, sizeof(Mat4));
            if (f - s->seen > 30) s->lastGap = f;
            s->seen = f;
            placed = true;
            ++g_satSelf;
        }
    } else if (dD < 1.5f || dG < 1.5f) {
        Mat4 W;
        if (dD + 0.05f < dG) {
            W = Rt;  // already on the drawn axe (built through the read hook)
            ++g_satOnDrawn;
        } else {
            W = Mul(Mul(Rt, AffineInverse(g0)), W0);  // on the game's axe -> the same spot on the drawn axe
            ++g_satOnGame;
        }
        memcpy(out, &W, sizeof(W));
        if (!s) { s = &g_satLearn[oldest]; *s = SatLearn{}; s->skel = skel; }
        if (s->seen && f - s->seen > 30) s->lastGap = f;
        s->seen = f;
        s->frame = f;
        s->A0 = W0;
        s->a0Valid = true;
        placed = true;
    } else {
        ++g_satAway;
        if (s) s->a0Valid = false;  // not on either axe now: never moved with it at the draw
    }
    ReleaseSRWLockExclusive(&g_axeCorrLock);
    return placed;
}
static bool AxeSatellite(uint8_t* skel, const float* root, float* out, bool diag = false, bool self = false) {
    static const bool v2 = config::GetFloat("sat_v2", 1.f) > 0.5f;
    if (v2) return AxeSatelliteV2(skel, root, out, self);
    const uint64_t f = g_frameNo.load();
    float best = 0.2f * 0.2f, nearest = 1e9f;
    int age = -1;
    Mat4 D, Rnear, Rlatest;
    bool have = false, recent = false, locked = false;
    uint64_t latestF = 0;
    AcquireSRWLockExclusive(&g_axeCorrLock);
    for (const AxeCorr& c : g_axeCorr) {
        if (!c.frame || f - c.frame > 2) continue;
        recent = true;
        if (c.frame >= latestF) { latestF = c.frame; Rlatest = c.Rn; }
        const float dx = root[12] - c.pos[0], dy = root[13] - c.pos[1], dz = root[14] - c.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < nearest) { nearest = d2; age = (int)(f - c.frame); Rnear = c.R; }
        if (d2 < best) { best = d2; D = c.D; have = true; }
    }
    // the latest drawn axe: the last one written (several can share a frame)
    Mat4 Dlatest{};
    bool haveDlatest = false;
    if (recent) {
        const AxeCorr& c = g_axeCorr[(g_axeCorrNext + 5) % 6];
        if (c.frame == latestF) { Rlatest = c.Rn; Dlatest = c.D; haveDlatest = true; }
    }
    if (g_satLock.load() && recent) {
        SatLearn* s = nullptr;
        int oldest = 0;
        for (int i = 0; i < 8; ++i) {
            if (g_satLearn[i].skel == skel) { s = &g_satLearn[i]; break; }
            if (g_satLearn[i].frame < g_satLearn[oldest].frame) oldest = i;
        }
        // effect and game axe from the same pose: learn the offset on the axe ("satlock strict": same frame only, once
        // learned). D-069: never from a self-fed pass (its root is our own placement), but those are placed too (the
        // draw-time refresh re-runs the effects with their cached world)
        if (!self && nearest < 0.12f * 0.12f && (age == 0 || (age <= 1 && !(g_satStrict.load() && s)))) {
            if (!s) { s = &g_satLearn[oldest]; s->skel = skel; }
            s->E = Mul(*(const Mat4*)root, InverseUniform(Rnear));
            s->frame = f;
        }
        if (s) {
            if (s->seen && f - s->seen > 30) s->lastGap = f;
            s->seen = f;
        }
        if (s && f - s->frame < 120 && nearest < 1.0f) {
            const Mat4 W = Mul(s->E, Rlatest);
            memcpy(out, &W, sizeof(W));
            locked = true;
        }
    }
    ReleaseSRWLockExclusive(&g_axeCorrLock);
    if (diag) {
        AcquireSRWLockExclusive(&g_satDiagLock);
        ++g_satDiag.passes; g_satDiag.self += self; g_satDiag.matched += have; g_satDiag.locked += locked;
        if (!have && !locked) { if (recent) ++g_satDiag.tooFar; else ++g_satDiag.noRecent; }
        if (!have && !locked && recent && g_satDiag.logged++ < 20)
            Log("fxjump: axe effect pass NOT corrected: %s pass, nearest axe correction %.2f m (age %d frames)", self ? "SELF-fed" : "fresh",
                sqrtf(nearest), age);
        ReleaseSRWLockExclusive(&g_satDiagLock);
    }
    if (locked) return true;
    if (!have) {
        // D-091 (owner: "the fire effect on my axe teleports a few cm to the left when I attack"): the axe's own effect
        // objects (3-joint, hash0 3124c569..., D-059) land > 0.2 m from the recorded game axe in fast swings (LIVE fxjump:
        // "NOT corrected, nearest 0.22-0.41 m") and stayed at the game's axe (its animated hand on the game body) - a
        // few cm to half a metre off our axe for those passes. Such an effect now takes the latest axe correction of
        // this frame (game axe -> drawn axe); not on a self-fed pass (its root is our placement already)
        const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
        const bool axeFx = hh && !IsBadReadPtr(hh, 8) && (hh[0] >> 32) == 0x3124c569ull;
        if (axeFx && !self && haveDlatest && nearest < 1.5f * 1.5f) {
            const Mat4 W = Mul(*(const Mat4*)root, Dlatest);
            memcpy(out, &W, sizeof(W));
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 10)
                Log("fp: axe effect %.2f m from the game's axe - given the axe's latest correction", sqrtf(nearest));
            return true;
        }
        return false;
    }
    const Mat4 W = Mul(*(const Mat4*)root, D);
    memcpy(out, &W, sizeof(W));
    if (++g_satellites % 3000 == 1) Log("fp: effect on the axe follows the axe (%llu passes)", (unsigned long long)g_satellites.load());
    return true;
}

// D-094 (owner: "the glow on the axe isn't fixed"; owner log: "faults 7462 (... axe effects 7462)" - the D-069 draw-time
// refresh, a forced world pass (EnsureWorld 0x140592770) of each axe effect, faulted on ~1-3 effects EVERY frame they
// lived (its root builder sub_14054de50 reads the effect's game object, skel+0x50), so those effects were drawn where
// their own passes left them - up to ~5 frames old, i.e. behind / beside the drawn axe while moving and swinging. Now
// nothing of the game is called: each effect pass records the root it was built with (SatNotePass), and at the draw the
// effect's cached joints are moved rigidly onto the drawn axe: W' = W x P^-1 x (E x drawn axe root). Only when the cache
// still holds exactly what was recorded (same buffer, joint count, joint-0 world): a freed / reused skeleton is skipped.
static void SatNotePass(uint8_t* skel, const float* use) {
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (nj <= 0 || nj > 16) return;
    bool known = false;  // cheap shared check first: this runs for every small skinned object's pass
    AcquireSRWLockShared(&g_axeCorrLock);
    for (const SatLearn& s : g_satLearn) if (s.skel == skel) { known = true; break; }
    ReleaseSRWLockShared(&g_axeCorrLock);
    if (!known) return;
    AcquireSRWLockExclusive(&g_axeCorrLock);
    for (SatLearn& s : g_satLearn) {
        if (s.skel != skel) continue;
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        s.pValid = use && mats && fabsf(use[12]) + fabsf(use[13]) + fabsf(use[14]) >= 1.f;
        if (s.pValid) {
            memcpy(&s.P, use, sizeof(Mat4));
            s.W0 = *(const Mat4*)(mats + (size_t)nj * 0x40);
            s.mats = mats;
            s.n = nj;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_axeCorrLock);
}
// 0 moved, 1 buffer / joint count changed, 2 not an axe-effect family, 3 cache rewritten since, 4 fault
static int SatMoveCache(uint8_t* skel, uint8_t* mats, int n, const Mat4& W0, const Mat4& T, Mat4* newW0) {
    __try {
        if ((int)(uint16_t)*(const int16_t*)(skel + 0x38) != n || *(uint8_t**)(skel + 0x58) != mats) return 1;
        const uint64_t* hh = *(const uint64_t* const*)(skel + 0x30);
        if (!hh || (hh[0] >> 32) != 0x3124c569ull) return 2;
        Mat4* w = (Mat4*)(mats + (size_t)n * 0x40);
        if (memcmp(&w[0], &W0, sizeof(Mat4)) != 0) return 3;  // rewritten since (a pass we did not see) or reused
        for (int j = 0; j < n; ++j) w[j] = Mul(w[j], T);
        *newW0 = w[0];
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 4;
    }
}
std::atomic<uint64_t> g_satReplaced{0}, g_satSkipped{0};
// D-094b diagnostics (owner: the flame glow "maybe a bit better", not fixed): why an effect seen this frame was not
// re-placed - [0] buffer changed, [1] other family, [2] cache rewritten, [3] fault, [4] its learned offset expired
// (> 120 frames since the last same-frame match), [5] its root > 1 m from the axe, [6] no root recorded
std::atomic<uint64_t> g_satWhy[7];
static void SatDrawReplace() {
    const uint64_t f = g_frameNo.load();
    struct Job { uint8_t* skel; uint8_t* mats; int n; Mat4 P, W0, Pn; };
    Job jobs[8];
    int nj = 0;
    static const bool v2 = config::GetFloat("sat_v2", 1.f) > 0.5f;
    if (v2) {  // D-096: each effect moves with the drawn axe since its pass (A0 -> the axe now)
        Mat4 Wnow;
        if (!AxeDrawn(&Wnow)) return;
        AcquireSRWLockShared(&g_axeCorrLock);
        for (const SatLearn& s : g_satLearn) {
            if (!s.skel || f - s.seen > 3) continue;
            if (!s.pValid || !s.a0Valid || s.n <= 0 || s.n > 16) { ++g_satWhy[6]; continue; }
            const Mat4 T = Mul(AffineInverse(s.A0), Wnow);
            jobs[nj++] = {s.skel, s.mats, s.n, s.P, s.W0, Mul(s.P, T)};
        }
        ReleaseSRWLockShared(&g_axeCorrLock);
        for (int i = 0; i < nj; ++i) {
            const Job& j = jobs[i];
            const Mat4 T = Mul(AffineInverse(j.P), j.Pn);
            Mat4 w0;
            const int why = SatMoveCache(j.skel, j.mats, j.n, j.W0, T, &w0);
            if (why != 0) { ++g_satSkipped; ++g_satWhy[why - 1]; continue; }
            ++g_satReplaced;
            AcquireSRWLockExclusive(&g_axeCorrLock);
            for (SatLearn& s : g_satLearn)
                if (s.skel == j.skel && s.mats == j.mats) { s.P = j.Pn; s.W0 = w0; s.A0 = Wnow; break; }  // the cache now
            ReleaseSRWLockExclusive(&g_axeCorrLock);
        }
        return;
    }
    AcquireSRWLockShared(&g_axeCorrLock);
    // the latest drawn axe (this frame's axe pass, run just before by WaitHeroFrame)
    uint64_t latestF = 0;
    for (const AxeCorr& c : g_axeCorr)
        if (c.frame && f - c.frame <= 2 && c.frame >= latestF) latestF = c.frame;
    const AxeCorr& last = g_axeCorr[(g_axeCorrNext + 5) % 6];
    if (latestF && last.frame == latestF)
        for (const SatLearn& s : g_satLearn) {
            if (!s.skel || f - s.seen > 3) continue;
            if (!s.pValid || s.n <= 0 || s.n > 16) { ++g_satWhy[6]; continue; }
            // the learned offset holds while the same effect lives on (no break in its passes since it was learned);
            // after a break only for 120 frames, as AxeSatellite's lock
            if (f - s.frame >= 120 && s.lastGap > s.frame) { ++g_satWhy[4]; continue; }
            // as AxeSatellite's lock: the effect's last root within 1 m of a recent game axe (or of the drawn one) -
            // a pooled effect skeleton reused elsewhere is left alone
            float near2 = 1e9f;
            for (const AxeCorr& c : g_axeCorr) {
                if (!c.frame || f - c.frame > 2) continue;
                for (const Mat4* q : {&c.R, &c.Rn}) {
                    const float dx = s.P.m[3][0] - q->m[3][0], dy = s.P.m[3][1] - q->m[3][1], dz = s.P.m[3][2] - q->m[3][2];
                    near2 = fminf(near2, dx * dx + dy * dy + dz * dz);
                }
            }
            if (near2 < 1.0f) jobs[nj++] = {s.skel, s.mats, s.n, s.P, s.W0, Mul(s.E, last.Rn)};
            else ++g_satWhy[5];
        }
    ReleaseSRWLockShared(&g_axeCorrLock);
    for (int i = 0; i < nj; ++i) {
        const Job& j = jobs[i];
        const Mat4 T = Mul(InverseUniform(j.P), j.Pn);
        Mat4 w0;
        const int why = SatMoveCache(j.skel, j.mats, j.n, j.W0, T, &w0);
        if (why != 0) { ++g_satSkipped; ++g_satWhy[why - 1]; continue; }
        ++g_satReplaced;
        AcquireSRWLockExclusive(&g_axeCorrLock);
        for (SatLearn& s : g_satLearn)
            if (s.skel == j.skel && s.mats == j.mats) { s.P = j.Pn; s.W0 = w0; break; }  // the cache now holds this
        ReleaseSRWLockExclusive(&g_axeCorrLock);
    }
}

// Owner: "in f1 mode make it so that it doesnt show the bow that kratos is holding on his left shoulder". Small skinned
// objects (< 300 joints: not a character) other than the axe and the shield whose root sits within 0.6 m of Kratos'
// neck are carried items; in gameplay first person their joints are shrunk at draw time with the other hide lists.
SRWLOCK g_carriedLock = SRWLOCK_INIT;
std::vector<uint8_t*> g_carried;          // skeletons (legacy shrink path, unused while native hiding works)
std::vector<uint8_t*> g_carriedObjs;      // their game objects: hidden with the game's own model visibility
std::atomic<bool> g_bowHide{true};        // gowvr.ini carried_hide / command "bowhide on|off"

// The game's own HideModel / ShowModel (Lua bindings 0x140190ec0 / 0x140190c50, STATIC) queue a VisibilityRequest whose
// execute (VisibilityRequest::vfunc_1, 0x14018ec20) calls sub_1407b3bb0(obj, state 1 show / 2 hide, component,
// !flag25, -1) and marks the object dirty (obj+0x27c |= 0x13). Hide = (2, 0xC); show = (1, 0xA) then (1, 0xC).
void SetModelVisible(uint8_t* obj, bool visible) {
    if (!obj || IsBadReadPtr(obj, 0x290)) return;
    using PFN = void(__fastcall*)(uint8_t*, int, int, char, int);
    auto f = (PFN)((uint8_t*)GameVa(0x1407b3bb0ull));
    if (visible) { f(obj, 1, 0xa, 1, -1); f(obj, 1, 0xc, 1, -1); }
    else f(obj, 2, 0xc, 1, -1);
    if ((*(uint32_t*)(obj + 0x278) & 0x200) == 0 && ((*(uint32_t*)(obj + 0x27c) >> 9) & 1) == 0 &&
        (*(uint32_t*)(obj + 0x270) & 0x400) == 0 && *(int32_t*)(obj + 0x280) > 0x11)
        *(uint32_t*)(obj + 0x27c) |= 0x13;
}
struct CarriedSave { uint8_t* skel; std::vector<Mat4> saved; };
std::vector<CarriedSave> g_carriedSaved;  // shrunk carried items: their real pose until restored
std::atomic<int> g_carriedSavedN{0};      // D-110: its size, read without the lock

// Carried items are shrunk only between their own pass and the drawing: physics (bow string / strap) reading a shrunk
// pose between frames stretched it (owner: "something brown extruded poking out of my left shoulder").
void CarriedRestore(uint8_t* only) {
    // D-110 (code audit M2): called at the start of every skinned object's world pass; nothing saved (the legacy shrink
    // path is off, so always) -> no exclusive lock
    if (g_carriedSavedN.load(std::memory_order_acquire) == 0) return;
    AcquireSRWLockExclusive(&g_carriedLock);
    for (size_t i = 0; i < g_carriedSaved.size();) {
        CarriedSave& cs = g_carriedSaved[i];
        if (only && cs.skel != only) { ++i; continue; }
        const int16_t n = *(int16_t*)(cs.skel + 0x38);
        uint8_t* mats = *(uint8_t**)(cs.skel + 0x58);
        if ((int)cs.saved.size() == n)
            for (int j = 0; j < n; ++j) *(Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40) = cs.saved[j];
        g_carriedSaved.erase(g_carriedSaved.begin() + i);
        g_carriedSavedN = (int)g_carriedSaved.size();
    }
    ReleaseSRWLockExclusive(&g_carriedLock);
}

// ---- D-033: every item carried on Kratos (Blades on the back, Mimir / the talisman on the belt, effects hanging on
// the axe, later items) gets the same standing-still fix as the bow (D-027), without a name list. Candidates are the
// skinned objects near his neck; one counts as CARRIED once its offset to its nearest body bone stayed the same while
// Kratos walked >= carry_travel metres (Atreus / NPCs standing next to him fail that). While the rendered body is
// turned, each pass of a carried item is re-rooted on the rendered bone - but only while the game still has it on the
// body (drawn Blades / thrown items are left alone).
constexpr uint64_t kLowerBackHash = 0xf1bc14299243b4e3ull;  // joint 271 (spine above the hips; belt items)
constexpr uint64_t kCarryRefs[] = {0xf14d9a553fdad7ccull /*neck*/, 0xf1bc142992438c01ull /*chest*/, kLowerBackHash};
struct CarryItem {
    uint8_t* skel = nullptr;
    char name[48] = {};
    uint64_t ref = 0;          // body bone it rides on
    BodyGlue gl;               // learned offset item x ref^-1
    float winStart[3] = {};    // offset translation when the current walking window started
    float winTravel = 0.f;     // Kratos' travel in the window
    float winMaxDev = 0.f;     // D-143: largest offset change from the window's start (not only at its end)
    float lastRoot[3] = {};
    bool haveLast = false;
    int goodWindows = 0;       // windows in which the offset held
    bool carried = false;
    uint64_t glued = 0;
    bool seen = true;
    bool byPass = false;         // D-036: found by its world pass (not in the object tables: talisman, ...)
    uint64_t lastPassFrame = 0;
    bool identity = false;       // D-162: carried by identity (talisman / Mimir) - never un-marked
};
std::vector<CarryItem> g_carry;  // guarded by g_carriedLock
std::atomic<bool> g_carryGuard{true};  // D-052 A/B: command "carryguard on|off"
void CarryGuard(bool on) { g_carryGuard = on; Log("fp: carried items near a hand never glued %s", on ? "ON" : "OFF"); }

static bool GlueCarry(const float* root, CarryItem& it, float* out) {
    const Mat4& R = *(const Mat4*)root;
    // D-052: an item in a hand is not carried on the body. LIVE (owner, real controllers): the two 48-joint "belt items"
    // are the Blades' own objects - on the back when sheathed, in the hands when drawn; learned as carried while walking,
    // they were then pinned to the turned body ("floating in the air at one point") on some passes only ("appears and
    // disappears"). Nothing is learned or glued while the item's game placement is within 0.4 m of a hand (animated or
    // motion controlled), nor while the game has the Blades drawn and the item is one of those two.
    const uint64_t* hs0 = *(const uint64_t**)(it.skel + 0x30);
    const bool bladeObj = hs0 && !IsBadReadPtr(hs0, 8) && hs0[0] == 0xef450b59487cb7afull;
    // D-085 (owner: "Blades held, L2 then R2 - when I bring the blade too close to my body it sticks into my chest, near
    // my head or into my right shoulder"): a Blades object is learned as carried while sheathed on the back, so its glue
    // spot is the sheath (upper back / right shoulder) on the neck or chest bone. The guard below relied only on the
    // game's weapon mode reading "Blades"; during the Blades' L2 moves (free, the game's own placement) a Blades object
    // is never glued, whatever the weapon mode reads. Logged when it acts (with that weapon mode)
    if (bladeObj && (ArmsBladesFree() || BladesFreeRaw())) {
        static std::atomic<int> logs{0};
        if (CurrentWeapon() != 2 && logs.fetch_add(1) < 20)
            Log("fp: Blades object kept off the body during an L2 move (the weapon mode read %d, not Blades)", CurrentWeapon());
        return false;
    }
    if (g_carryGuard.load() && bladeObj) {  // only the Blades' objects (Mimir / talisman at the belt near a hanging hand stay glued)
        static const uint64_t wh[2] = {0x6de47b0b82506234ull, 0x3626a7d4c72e93aeull};
        for (int h = 0; h < 2; ++h) {
            Mat4 a, w, e;
            auto Near = [&](const Mat4& m) {
                const float dx = R.m[3][0] - m.m[3][0], dy = R.m[3][1] - m.m[3][1], dz = R.m[3][2] - m.m[3][2];
                return dx * dx + dy * dy + dz * dz < 0.4f * 0.4f;
            };
            if ((StableJoint(wh[h], &a) && Near(a)) || (ArmsIkResult(h, &w, &e) && Near(w))) return false;
        }
        if (CurrentWeapon() == 2) return false;
    }
    if (!it.ref) {  // nearest reference bone, once
        float best = 1e9f;
        for (uint64_t h : kCarryRefs) {
            Mat4 J;
            if (!StableJoint(h, &J)) continue;
            const float dx = R.m[3][0] - J.m[3][0], dy = R.m[3][1] - J.m[3][1], dz = R.m[3][2] - J.m[3][2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) { best = d2; it.ref = h; }
        }
        if (!it.ref) return false;
    }
    Mat4 J;
    if (!StableJoint(it.ref, &J)) return false;
    float g[4], r[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g)); memcpy(r, g_renderRoot, sizeof(r));
    ReleaseSRWLockShared(&g_rootLock);
    const float d = g_bodyOverride.load() ? WrapPiB(r[3] - g[3]) : 0.f;
    // D-088 (owner: "combat and walking at the same time make the talisman glitch - it floats in one place, then gets
    // good once I stop"): during a held attack the drawn body is on the held spot (root lock) while the game body plays
    // the attack elsewhere - walking, up to 2.5 m apart. The glue only worked for a TURNED body (weight from the facing
    // difference, ~0 in a hold: the game's facing is set to the view), and the learning took the game's placement against
    // the DRAWN bone, so the gap went into the offset - the talisman stayed where the game had it until the lunge-ghost
    // rule (gaps > 0.5 m only) jumped it over. In a hold (and its fade-out): no learning, full glue on the drawn bone, and
    // the on-the-body test allows for the gap between the two bodies
    const bool inHold = CombatHoldWeight() > 0.001f;
    const float sepX = g[0] - r[0], sepZ = g[2] - r[2];  // game body - drawn body
    if (fabsf(d) < 0.035f && !inHold) {  // facings agree: learn the offset and test it for rigidity while Kratos walks
        const Mat4 O = Mul(R, RigidInverse(J));
        // D-143 (owner: "hand combat is gluing things to me, like enemies' weapons"; owner log: Atreus-sized skeletons -
        // 529 / 87 (goquiver00) / 52 / 10 joints - "carried on Kratos" after one 2 m window, the axe's recall trails
        // taken as candidates): only the window's two ends were compared, so anything that happened to be at the same
        // offset again 2 m later counted as carried, and fights / the boat (others moving with Kratos) were sampled too.
        // Now no window while fighting (and 3 s after), in the boat or with the item > 0.75 m from its body bone (a thing
        // ON Kratos never is - that also un-marks it at once); the offset must stay within 0.5 m for the WHOLE window,
        // and two such windows in a row (4 m of walking) mark it carried
        const float offLen = sqrtf(O.m[3][0] * O.m[3][0] + O.m[3][1] * O.m[3][1] + O.m[3][2] * O.m[3][2]);
        const bool offBody = offLen > 0.75f;
        if (offBody && it.carried && !it.identity) {
            it.carried = false;
            it.goodWindows = 0;
            Log("fp: '%s' is not carried (%.2f m from Kratos' body bone)", it.name, offLen);
        }
        if (offBody || CombatBusy() || CombatIdleMs() < 3000 || InBoat()) {
            it.haveLast = false;  // no rigidity window across this
            it.winTravel = 0.f;
            it.winMaxDev = 0.f;
        } else {
            if (it.haveLast) {
                const float mx = g[0] - it.lastRoot[0], mz = g[2] - it.lastRoot[2];
                it.winTravel += sqrtf(mx * mx + mz * mz);
                const float ex = O.m[3][0] - it.winStart[0], ey = O.m[3][1] - it.winStart[1], ez = O.m[3][2] - it.winStart[2];
                it.winMaxDev = fmaxf(it.winMaxDev, sqrtf(ex * ex + ey * ey + ez * ez));
            }
            memcpy(it.lastRoot, g, sizeof(it.lastRoot));
            if (!it.haveLast) {
                it.haveLast = true;
                it.winMaxDev = 0.f;
                for (int i = 0; i < 3; ++i) it.winStart[i] = O.m[3][i];
            }
            static const float carryTravel = config::GetFloat("carry_travel", 2.0f);
            if (it.winTravel >= carryTravel) {
                // belt items swing while walking (LIVE: Mimir / goexplosive00 flipped at 8 cm): 20 cm, and sticky - one
                // steady walk marks an item carried, three changed walks in a row un-mark it (Atreus / NPCs never get a
                // steady one). D-036: 45 cm - Mimir swings more than 20 cm on the belt and kept losing "carried" (LIVE
                // owner log: carried / not carried every few metres). D-143: 50 cm over the whole window (the largest
                // change, not the end's), two windows in a row
                const bool held = it.winMaxDev < 0.5f;
                it.goodWindows = held ? (it.goodWindows > 0 ? it.goodWindows + 1 : 1) : (it.goodWindows > 0 ? -1 : it.goodWindows - 1);
                const bool was = it.carried;
                if (held && it.goodWindows >= 2) it.carried = true;
                else if (it.goodWindows <= -3 && !it.identity) it.carried = false;
                if (it.carried != was)
                    Log("fp: '%s' %s (offset %s over %.1f m of walking, largest change %.2f m)", it.name,
                        it.carried ? "is carried on Kratos - kept on the turned body" : "is not carried",
                        held ? "held" : "changed", it.winTravel, it.winMaxDev);
                it.winTravel = 0.f;
                it.winMaxDev = 0.f;
                for (int i = 0; i < 3; ++i) it.winStart[i] = O.m[3][i];
            }
        }
        it.gl.O = O;
        it.gl.have = true;
        if (!g_glueBlend.load()) return false;
    }
    if (!it.carried || !it.gl.have) return false;
    const float gt = inHold ? 1.f : GlueWeight(d);
    if (gt <= 0.f) return false;
    // still on the body? the game's placement must match the learned offset on the rendered bone or on the unturned one
    const Mat4 W = Mul(it.gl.O, J);
    if (inHold) {  // D-088: the game's placement vs W carried over to the game body
        const float hx = R.m[3][0] - (W.m[3][0] + sepX), hy = R.m[3][1] - W.m[3][1], hz = R.m[3][2] - (W.m[3][2] + sepZ);
        if (hx * hx + hy * hy + hz * hz > 0.6f * 0.6f) return false;
        memcpy(out, &W, sizeof(W));
        if (++it.glued % 1800 == 1) Log("fp: '%s' glued to the drawn body during a held attack", it.name);
        return true;
    }
    const float c = cosf(-d), sn = sinf(-d), sg = sinf(g[3]), cg = cosf(g[3]);
    const float kR = g_pivotRight.load(), kB = g_pivotBack.load();
    const float px = g[0] + cg * kR + sg * kB, pz = g[2] - sg * kR + cg * kB;
    const float vx = W.m[3][0] - px, vz = W.m[3][2] - pz;
    const float ux = px + vx * c + vz * sn, uz = pz + vz * c - vx * sn;  // W on the unturned body
    const float e1x = R.m[3][0] - W.m[3][0], e1y = R.m[3][1] - W.m[3][1], e1z = R.m[3][2] - W.m[3][2];
    const float e2x = R.m[3][0] - ux, e2z = R.m[3][2] - uz;
    const float e1 = e1x * e1x + e1y * e1y + e1z * e1z, e2 = e2x * e2x + e1y * e1y + e2z * e2z;
    // taken off the body (drawn, thrown, dropped). D-036: 0.6 m (was 0.25): a swinging item's passes further than 0.25 m
    // from the learned pose were left unglued, so it jumped between two places every frame (harness burst, Mimir)
    if (fminf(e1, e2) > 0.6f * 0.6f) return false;
    const Mat4 B = BlendPlacement(R, W, gt);
    memcpy(out, &B, sizeof(B));
    if (++it.glued % 1800 == 1) Log("fp: '%s' glued to the turned body (%.0f deg)", it.name, d * 57.2958f);
    return true;
}

// D-084 (owner: "the talisman flashes: once my wrist gets near it, it teleports to my hand and sticks there for a few
// seconds"): AxeSatellite takes ANY small skeleton (<= 16 joints) whose pass lands within 12 cm of the game's axe as an
// effect riding on the axe and puts it on the drawn axe for 120 frames after that (learned again on every pass while
// the axe stays near) - the talisman on the belt, passed by the axe hand. The talisman (D-036 skelnear: the 7-joint
// skeleton on the front of the belt; owner session fxjump: root joint hash b1098415d96da4b2) is never an axe effect.
// (v1 excluded every item learned as carried - reverted, owner: identify the talisman itself)
// D-085 (owner: the Blades "stick into my chest, near my head or into my right shoulder" during L2 + R2): with the Blades
// drawn the axe is sheathed on the back - the Blades' own 3-joint fire / copy pieces (hash0 65301791f418f62d, D-057)
// passing within 12 cm of it would be taken the same way and kept on the sheathed axe. Not an axe effect either.
constexpr uint64_t kTalismanHash = 0xb1098415d96da4b2ull, kBladesPieceHash = 0x65301791f418f62dull;
static bool NotAnAxeEffect(uint8_t* skel) {
    const uint64_t* h = *(const uint64_t**)(skel + 0x30);
    if (!h || IsBadReadPtr(h, 8)) return false;
    return h[0] == kTalismanHash || h[0] == kBladesPieceHash;
}

// ---- D-162: objects the game hangs on a Kratos joint (Mimir's head ...) + the talisman on the belt ------------------
// Owner: "Mimir's head and the talisman appear and disappear on my hip, and when I attack they move to where the attack
// would happen (2 m in front of me)". STATIC (decompile): gomimirhead00 is a child object of Kratos (sub_1407aa590 finds
// it among his children by name); sub_1409091f0 drives the Kratos attach joints JOMimirWeapon1 / JOMeleeWeapon2 /
// JOBowWeapon1 every frame. An attached object's world = its local x its parent joint's world (ADDR-018 link: obj+0x140
// parent object, +0x148 type 2 = joint, +0x14c joint index, +0xf0 the parent world it was built from); that parent world
// comes from the hero skeleton's world cache (the DRAWN body: turned, shifted under the view, held in attacks) on some
// passes and from a fresh evaluation of the GAME's pose (the lunged body) on others - the flicker, and the jump 2 m out
// during held attacks. Since D-143 the carried-item glue only took them after two 2 m walks outside fights (a fight-heavy
// session never got there). Now, in gameplay F1:
//  - every object linked to a Kratos joint outside his arms (not the axe / shield - their own fixes - nor the Blades'
//    objects) learns its offset to that joint from the game's own passes (C = root x the parent world it used, smoothed,
//    outliers rejected - the D-146 sheathed-axe method) and is placed C x the DRAWN joint on every pass;
//  - the talisman (not in the object tables - D-036; root joint hash b1098415d96da4b2) the same against the belt joint
//    (271, lower back) of the game's own pose (LiveGameJoint).
// gowvr.ini linked_items 0 = off (the talisman too).
struct LinkState {
    Mat4 C{};
    uint32_t good = 0, bad = 0;
    bool have = false;
};
struct LinkedItem {
    uint8_t* skel = nullptr;
    uint8_t* obj = nullptr;
    int joint = -1;
    uint64_t hash = 0;
    char name[32] = {};
    LinkState st;
};
constexpr int kLinkedMax = 8;
SRWLOCK g_linkedLock = SRWLOCK_INIT;
LinkedItem g_linked[kLinkedMax];
int g_linkedCount = 0;                                // under g_linkedLock
std::atomic<uint8_t*> g_linkedSkel[kLinkedMax];       // lock-free membership test for the world-pass hook
std::atomic<uint32_t> g_lkPlaced{0}, g_lkLearn{0}, g_lkReject{0}, g_lkNotSettled{0}, g_lkNoJoint{0}, g_lkTal{0},
    g_talKept{0};  // D-229e: talisman passes already on the drawn belt

static bool LinkNear(const Mat4& a, const Mat4& b, float d) {
    const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
    const float la = sqrtf(a.m[0][0] * a.m[0][0] + a.m[0][1] * a.m[0][1] + a.m[0][2] * a.m[0][2]);
    const float lb = sqrtf(b.m[0][0] * b.m[0][0] + b.m[0][1] * b.m[0][1] + b.m[0][2] * b.m[0][2]);
    return x * x + y * y + z * z < d * d && la > 1e-3f && lb > 1e-3f && fabsf(la - lb) < 0.05f * lb &&
           (a.m[0][0] * b.m[0][0] + a.m[0][1] * b.m[0][1] + a.m[0][2] * b.m[0][2]) / (la * lb) > 0.995f;
}
// one learning step from a pass placed at R from the parent world P (the caller holds the state's lock)
static void LinkLearn(LinkState& st, const Mat4& R, const Mat4& P) {
    if (fabsf(P.m[3][0]) + fabsf(P.m[3][1]) + fabsf(P.m[3][2]) < 1.f || fabsf(P.m[3][3] - 1.f) > 1e-3f) return;
    const Mat4 Cn = Mul(R, AffineInverse(P));
    const float cl = sqrtf(Cn.m[3][0] * Cn.m[3][0] + Cn.m[3][1] * Cn.m[3][1] + Cn.m[3][2] * Cn.m[3][2]);
    if (!(cl < 1.5f)) return;  // (an item hanging on a joint is within a metre and a half of it)
    if (!st.have) {
        st.C = Cn; st.have = true; st.good = 1; st.bad = 0;
    } else if (LinkNear(Cn, st.C, 0.03f)) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) st.C.m[r][c] += (Cn.m[r][c] - st.C.m[r][c]) * 0.1f;
        OrthonormalizeKeepScale(st.C);
        ++st.good; st.bad = 0;
        ++g_lkLearn;
    } else {
        ++g_lkReject;  // a pass built from another parent than the one read (race), or the drawn body
        if (++st.bad >= 60) { st.C = Cn; st.good = 1; st.bad = 0; }  // 60 in a row: the attachment changed
    }
}
static bool LinkedToHero(uint8_t* o, uint8_t* sk, uint8_t* hero) {
    const int type = *(const int*)(o + 0x148);
    const int j = *(const int16_t*)(o + 0x14c);
    uint8_t* parent = *(uint8_t**)(o + 0x140);
    const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
    if (type != 2 || j < 0 || j >= hn || !parent || IsBadReadPtr(parent, 0x240) || *(uint8_t**)(parent + 0x238) != hero)
        return false;
    const uint64_t* sh = *(const uint64_t**)(sk + 0x30);
    if (sh && !IsBadReadPtr(sh, 8) && sh[0] == 0xef450b59487cb7afull) return false;  // the Blades' objects: own rules
    for (uint64_t s : {0x05c2b5ca793d4bf2ull, 0xe783988515993319ull}) {  // the arms (held items): the hands' own rules
        const int a = FindJointByHash(s);
        if (a >= 0 && (j == a || IsDescendant(j, a))) return false;
    }
    return true;
}
static bool LinkedOn() {
    static const bool on = config::GetFloat("linked_items", 1.f) > 0.5f;
    return on;
}
static void LinkedReport() {
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 15000 && tLog.compare_exchange_strong(t0, now))
        Log("fp: items on Kratos' joints, 15 s: %u passes placed on the drawn joint (talisman %u moved, %u already on the "
            "drawn belt), offset learned %u / rejected %u, not settled %u, joint unpublished %u", g_lkPlaced.exchange(0),
            g_lkTal.exchange(0), g_talKept.exchange(0), g_lkLearn.exchange(0), g_lkReject.exchange(0),
            g_lkNotSettled.exchange(0), g_lkNoJoint.exchange(0));
}
// D-165 (owner log D-162: rejected 91-602 / learned 700-1260 per 15 s, "not settled" up to 522 - Mimir swings on his
// joint, so a rigid learned offset kept being rejected and reset): the offset is taken from every consistent game pass
// as it is (C = root x the parent world that pass was built from = the object's own local, swing included); a single
// pass > 0.25 m off the last one is taken as a race and skipped (3 in a row: accepted)
static void LinkPerPass(LinkState& st, const Mat4& R, const Mat4& P) {
    if (fabsf(P.m[3][0]) + fabsf(P.m[3][1]) + fabsf(P.m[3][2]) < 1.f || fabsf(P.m[3][3] - 1.f) > 1e-3f) return;
    const Mat4 Cn = Mul(R, AffineInverse(P));
    const float cl = sqrtf(Cn.m[3][0] * Cn.m[3][0] + Cn.m[3][1] * Cn.m[3][1] + Cn.m[3][2] * Cn.m[3][2]);
    if (!(cl < 1.5f)) return;
    if (st.have) {
        const float x = Cn.m[3][0] - st.C.m[3][0], y = Cn.m[3][1] - st.C.m[3][1], z = Cn.m[3][2] - st.C.m[3][2];
        if (x * x + y * y + z * z > 0.25f * 0.25f && ++st.bad < 3) { ++g_lkReject; return; }
    }
    st.C = Cn;
    st.have = true;
    st.bad = 0;
    st.good = st.good < 1000000 ? st.good + 1 : st.good;
    ++g_lkLearn;
}
// a pass of an object linked to a Kratos joint (gameplay F1)
static bool LinkedFix(uint8_t* skel, const float* root, float* out) {
    int slot = -1;
    for (int i = 0; i < kLinkedMax; ++i)
        if (g_linkedSkel[i].load(std::memory_order_relaxed) == skel) { slot = i; break; }
    if (slot < 0 || !LinkedOn() || FpMode() != 1 || !root || fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) < 1.f)
        return false;
    const Mat4& R = *(const Mat4*)root;
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint8_t* mm = *(const uint8_t**)(skel + 0x58);
    const bool selfFed = (const uint8_t*)root >= mm && (const uint8_t*)root < mm + (size_t)nj * 0x80;  // our own placement
    LinkState st;
    uint64_t hash = 0;
    bool ok = false;
    AcquireSRWLockExclusive(&g_linkedLock);
    if (slot < g_linkedCount && g_linked[slot].skel == skel) {
        LinkedItem& it = g_linked[slot];
        uint8_t* o = it.obj;
        if (o && !IsBadReadPtr(o, 0x240) && *(uint8_t**)(o + 0x238) == skel && *(const int*)(o + 0x148) == 2 &&
            *(const int16_t*)(o + 0x14c) == it.joint) {
            Mat4 P1, P2;
            memcpy(&P1, o + 0xf0, sizeof(Mat4));
            memcpy(&P2, o + 0xf0, sizeof(Mat4));
            if (!selfFed && !memcmp(&P1, &P2, sizeof(Mat4))) LinkPerPass(it.st, R, P1);  // D-165
            st = it.st;
            hash = it.hash;
            ok = true;
        }
    }
    ReleaseSRWLockExclusive(&g_linkedLock);
    LinkedReport();
    if (!ok) return false;
    if (!st.have) { ++g_lkNotSettled; return false; }  // D-165: from the first consistent pass
    Mat4 J;
    if (!hash || !StableJoint(hash, &J)) { ++g_lkNoJoint; return false; }
    const Mat4 W = Mul(st.C, J);
    memcpy(out, &W, sizeof(W));
    ++g_lkPlaced;
    return true;
}
// the talisman (found by its pass): the belt joint of the game's own pose is its parent
constexpr uint64_t kTalismanRootHash = 0xb1098415d96da4b2ull;
SRWLOCK g_talLock = SRWLOCK_INIT;
LinkState g_talSt;
uint8_t* g_talSkel = nullptr;
// D-229e: set when the pass is the talisman's and D-229d already found it on the drawn belt (left as it is: no other rule
// may move it - the carried-item glue placed those passes at its own learned spot, owner: "the talisman keeps moving from
// one place to another, like glitching")
thread_local bool t_talismanKept = false;
// D-229g diagnostic "taltrace N": the next N talisman passes (moved / kept, where they were built) and, while it runs,
// the talisman's cached root right before each eye's kick (where it is drawn) - both against the drawn and the game belt
std::atomic<int> g_talTrace{0};
void TalismanTrace(int n) { g_talTrace = n; Log("taltrace: next %d talisman passes and its drawn spot at each kick", n); }
static bool LiveGameJoint(int j, Mat4* out);
void TalismanDrawSample(int eye) {
    if (g_talTrace.load(std::memory_order_relaxed) <= 0) return;
    uint8_t* tk = g_talSkel;
    if (!tk) return;
    static const uint64_t kBelt = 0xf1bc14299243b4e3ull;
    const int bj = FindJointByHash(kBelt);
    Mat4 G, D;
    if (bj < 0 || !LiveGameJoint(bj, &G) || !StableJoint(kBelt, &D)) return;
    const int n = (int)(uint16_t)*(int16_t*)(tk + 0x38);
    const Mat4 T = *(const Mat4*)(*(const uint8_t**)(tk + 0x58) + (size_t)n * 0x40);  // its root's world, as cached
    const Mat4 tD = Mul(T, AffineInverse(D)), tG = Mul(T, AffineInverse(G));
    auto len = [](const Mat4& m) { return sqrtf(m.m[3][0] * m.m[3][0] + m.m[3][1] * m.m[3][1] + m.m[3][2] * m.m[3][2]); };
    const float sc = sqrtf(T.m[0][0] * T.m[0][0] + T.m[0][1] * T.m[0][1] + T.m[0][2] * T.m[0][2]);
    Log("taltrace: DRAW eye %d | talisman in drawn-belt frame (%.3f %.3f %.3f) %.3f m, in game-belt frame (%.3f %.3f "
        "%.3f) %.3f m, scale %.3f | frame %llu", eye, tD.m[3][0], tD.m[3][1], tD.m[3][2], len(tD), tG.m[3][0], tG.m[3][1],
        tG.m[3][2], len(tG), sc, (unsigned long long)g_frameNo.load());
}
static void NoteItemCorrection(uint8_t* skel, const float* game, const float* drawn);  // D-229 (below)
static bool TalismanFix(uint8_t* skel, const float* root, float* out) {
    t_talismanKept = false;
    if (!LinkedOn() || FpMode() != 1 || !root || fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) < 1.f) return false;
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (nj < 2 || nj > 16) return false;
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8) || hh[0] != kTalismanRootHash) return false;
    static const uint64_t kBelt = 0xf1bc14299243b4e3ull;  // joint 271, lower back (published)
    const int bj = FindJointByHash(kBelt);
    if (bj < 0) return false;
    const uint8_t* mm = *(const uint8_t**)(skel + 0x58);
    const bool selfFed = (const uint8_t*)root >= mm && (const uint8_t*)root < mm + (size_t)nj * 0x80;
    // D-229d (owner: the cave light "comes out of the talisman" and two lights fight; owner logs D-229/229c: the item
    // summary's "not settled" 105-811 per 15 s, talisman passes placed 0-785 per 15 s): the rigid learned offset below kept
    // failing - the talisman swings on the belt, and its passes come built from BOTH belts (the drawn one when Kratos'
    // cache is current, the game's otherwise), so C against the game's belt was two-valued and reset again and again;
    // every pass while unsettled stayed on the game's body - the talisman and its light jumped between the two belts.
    // Now, per pass, no learning: a pass nearer the game's belt joint than the drawn one (by 2 cm) is moved by the
    // belt's own change, root x game belt^-1 x drawn belt (its swing kept); a pass already on the drawn belt is left.
    // Holds too (the game body lunges, the drawn one stays). gowvr.ini talisman_per_pass 0 = the learned offset.
    static const bool perPass = config::GetFloat("talisman_per_pass", 1.f) > 0.5f;
    if (perPass) {
        {
            AcquireSRWLockExclusive(&g_talLock);
            if (g_talSkel != skel) {
                Log("fp: talisman found (skeleton %p, %d joints) - each pass on the drawn belt joint in F1 (D-229d)", skel, nj);
                g_talSkel = skel;
            }
            ReleaseSRWLockExclusive(&g_talLock);
        }
        Mat4 G, D;
        t_talismanKept = true;  // (the talisman's pass: no other rule from here on, moved or not)
        if (selfFed || !LiveGameJoint(bj, &G) || !StableJoint(kBelt, &D)) { ++g_lkNoJoint; return false; }
        const Mat4& R = *(const Mat4*)root;
        auto d = [](const Mat4& a, const Mat4& b) {
            const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
            return sqrtf(x * x + y * y + z * z);
        };
        // D-229i (owner: "still blinking, appearing and disappearing very fast - that's what you need to fix"; LIVE
        // memory read of the owner's game, 2.5 s at ~0.01 ms: the talisman's 7 joints move as one, and 6 times it jumped
        // 0.24-0.26 m for ONE frame and back): the D-229d test "nearer the game's belt joint than the drawn one" is
        // ambiguous - the talisman hangs ~0.24 m from that joint, about as far as the two belts are apart, so a pass
        // already on the drawn belt could sit nearer the game's joint and was "moved" by the belts' difference for that
        // frame. Decided by continuity now: its offset from each belt (root x belt^-1) against the offset accepted on the
        // last pass - the true one hardly changes between passes (a swing), the wrong one is off by the belts' whole
        // difference. (No recent offset: the old distance test.)
        const Mat4 Lg = Mul(R, AffineInverse(G)), Ld = Mul(R, AffineInverse(D));
        bool move;
        {
            static Mat4 Lprev{};
            static uint64_t lFrame = 0;
            const uint64_t fr = g_frameNo.load();
            AcquireSRWLockExclusive(&g_talLock);
            if (lFrame && fr - lFrame <= 30) move = d(Lg, Lprev) + 0.01f < d(Ld, Lprev);
            else move = d(R, G) + 0.02f < d(R, D);
            Lprev = move ? Lg : Ld;
            lFrame = fr;
            ReleaseSRWLockExclusive(&g_talLock);
        }
        // D-229h: the belt's own change is the talisman's correction on EVERY pass (D-229 recorded it only on moved ones -
        // owner taltrace: 9 moved / 391 already drawn - so reads through the talisman, its light's, were mostly left)
        NoteItemCorrection(skel, &G.m[0][0], &D.m[0][0]);
        if (g_talTrace.load(std::memory_order_relaxed) > 0 && g_talTrace.fetch_sub(1) > 0) {
            const Mat4 rD = Mul(R, AffineInverse(D)), rG = Mul(R, AffineInverse(G));
            Log("taltrace: pass %s thread %u | root in drawn-belt frame (%.3f %.3f %.3f) %.3f m, in game-belt frame "
                "(%.3f %.3f %.3f) %.3f m | belts %.3f m apart | frame %llu", move ? "MOVED" : "kept ", GetCurrentThreadId(),
                rD.m[3][0], rD.m[3][1], rD.m[3][2], d(R, D), rG.m[3][0], rG.m[3][1], rG.m[3][2], d(R, G), d(G, D),
                (unsigned long long)g_frameNo.load());
        }
        if (!move) { ++g_talKept; return false; }  // already on the drawn belt (or they agree)
        const Mat4 W = Mul(Mul(R, AffineInverse(G)), D);
        memcpy(out, &W, sizeof(W));
        ++g_lkTal;
        return true;
    }
    Mat4 Pg;
    const bool haveGame = !selfFed && CombatHoldWeight() <= 0.001f && LiveGameJoint(bj, &Pg);
    LinkState st;
    AcquireSRWLockExclusive(&g_talLock);
    if (g_talSkel != skel) {
        if (g_talSkel) Log("fp: talisman skeleton %p (was %p) - offset learned again", skel, g_talSkel);
        else Log("fp: talisman found (skeleton %p, %d joints) - placed on the drawn belt joint in F1", skel, nj);
        g_talSkel = skel;
        g_talSt = LinkState{};
    }
    if (haveGame) LinkLearn(g_talSt, *(const Mat4*)root, Pg);
    st = g_talSt;
    ReleaseSRWLockExclusive(&g_talLock);
    if (!st.have || st.good < 15) { ++g_lkNotSettled; return false; }
    Mat4 J;
    if (!StableJoint(kBelt, &J)) { ++g_lkNoJoint; return false; }
    const Mat4 W = Mul(st.C, J);
    memcpy(out, &W, sizeof(W));
    ++g_lkTal;
    return true;
}

// D-162: emitters owned by these objects are Kratos' own sounds (mutate.cpp centres them in F1): the hero, the axe /
// shield / Blades objects while in his hands or on him
std::atomic<uint8_t*> g_bladeObjs[4];
bool IsKratosSoundOwner(uint8_t* obj) {
    if (!obj) return false;
    uint8_t* hero = g_heroSkel.load();
    if (hero && *(uint8_t**)(obj + 0x238) == hero) return true;
    if (obj == g_attached[0].obj.load()) return !AxeThrown();
    if (obj == g_attached[1].obj.load()) return true;
    for (auto& b : g_bladeObjs)
        if (b.load(std::memory_order_relaxed) == obj) return !BladeThrowActive();
    return false;
}

// D-185: the Blades as DRAWN for the melee hit re-test (combat.cpp hk_VolTest): each Blade object's root -> its farthest
// joint within 1.5 m (D-070: the object's unused joints sit at the world origin), this call and the previous one
static bool BladeSegRead(uint8_t* obj, float seg[2][3]) {
    __try {
        uint8_t* skel = *(uint8_t**)(obj + 0x238);
        if (!skel) return false;
        const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
        const float* m = *(const float**)(skel + 0x58);
        if (!m || n <= 1 || n > 256) return false;
        const float* w = m + (size_t)n * 16;  // the world matrices follow the n locals
        const float r[3] = {w[12], w[13], w[14]};
        if (fabsf(r[0]) + fabsf(r[1]) + fabsf(r[2]) < 1.f) return false;
        float best = 0.f;
        int bi = -1;
        for (int j = 1; j < n; ++j) {
            const float* p = w + (size_t)j * 16 + 12;
            const float dx = p[0] - r[0], dy = p[1] - r[1], dz = p[2] - r[2], d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 2.25f && d2 > best) { best = d2; bi = j; }
        }
        if (bi < 0 || best < 0.04f) return false;
        const float* t = w + (size_t)bi * 16 + 12;
        for (int i = 0; i < 3; ++i) { seg[0][i] = r[i]; seg[1][i] = t[i]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int BladeHitSegments(float cur[2][2][3], float prev[2][2][3]) {
    static uint8_t* lastObj[4] = {};
    static float last[4][2][3] = {};
    int k = 0;
    for (int i = 0; i < 4 && k < 2; ++i) {
        uint8_t* obj = g_bladeObjs[i].load(std::memory_order_relaxed);
        float seg[2][3];
        if (!obj || !BladeSegRead(obj, seg)) { lastObj[i] = nullptr; continue; }
        memcpy(cur[k], seg, sizeof(seg));
        memcpy(prev[k], lastObj[i] == obj ? last[i] : seg, sizeof(seg));
        memcpy(last[i], seg, sizeof(seg));
        lastObj[i] = obj;
        ++k;
    }
    return k;
}

void FindAttachedObjects() {
    // LIVE (axediag): several goaxe00 objects exist (another copy 840 m away at scale 0.85); the last one found used
    // to win, so the fix sometimes ran on the wrong axe. Now the copy nearest to Kratos wins (a thrown axe stays well
    // within 100 m).
    constexpr int kAt = (int)(sizeof(g_attached) / sizeof(g_attached[0]));
    uint8_t* best[kAt] = {};
    uint8_t* bestObj[kAt] = {};  // D-146
    float bestD2[kAt], atCount[kAt] = {};
    for (int k = 0; k < kAt; ++k) bestD2[k] = 100.f * 100.f;
    std::vector<uint8_t*> carried, carriedObjs;
    std::vector<std::pair<uint8_t*, const char*>> carryFound;
    uint8_t *bowSk = nullptr, *bowObj = nullptr;
    float bowD2 = 1e30f;
    int bowJ = 0;
    const char* bowName = "";
    Mat4 neckM{};
    const bool haveNeck = StableJoint(0xf14d9a553fdad7ccull, &neckM);
    uint8_t* heroSkel = Skeleton();
    struct LinkedFound { uint8_t* skel; uint8_t* obj; int joint; const char* name; };
    std::vector<LinkedFound> linkedFound;  // D-162
    uint8_t* bladeObjs[4] = {};
    int nBlade = 0;
    for (int t = 0; t < 64; ++t) {
        uint8_t* tab = G<uint8_t*>(0x1422a98c0 + t * 8);
        if (!tab || IsBadReadPtr(tab, 0x38)) continue;
        for (int set = 0; set < 2; ++set) {
            uint8_t** arr = *(uint8_t***)(tab + (set ? 0x20 : 0x8));
            uint32_t cnt = *(uint32_t*)(tab + (set ? 0x30 : 0x18));
            if (!arr || cnt == 0 || cnt > 100000 || IsBadReadPtr(arr, (size_t)cnt * 8)) continue;
            for (uint32_t i = 0; i < cnt; ++i) {
                uint8_t* o = arr[i];
                if (!o || IsBadReadPtr(o, 0x240)) continue;
                const char* nm = *(const char**)(o + 0x38);
                if (!nm || IsBadReadPtr(nm, 20)) continue;
                bool known = false;
                uint8_t* sk = *(uint8_t**)(o + 0x238);
                for (int k = 0; k < kAt; ++k)
                    if (!strcmp(nm, g_attached[k].name)) {
                        known = true;
                        if (!haveNeck || !PlausibleSkeleton(sk)) continue;
                        ++atCount[k];
                        const int16_t nj = *(int16_t*)(sk + 0x38);
                        const float* rp = (const float*)(*(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)nj * 0x40 + 0x30);
                        const float ddx = rp[0] - neckM.m[3][0], ddy = rp[1] - neckM.m[3][1], ddz = rp[2] - neckM.m[3][2];
                        const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                        if (d2 < bestD2[k]) { bestD2[k] = d2; best[k] = sk; bestObj[k] = o; }
                    }
                // D-162: hung on a Kratos joint by the game's own link -> placed on the drawn joint (no carry candidate)
                if (!known && heroSkel && sk != heroSkel && PlausibleSkeleton(sk) && LinkedToHero(o, sk, heroSkel)) {
                    if (linkedFound.size() < (size_t)kLinkedMax) linkedFound.push_back({sk, o, *(const int16_t*)(o + 0x14c), nm});
                    continue;
                }
                // D-162: the Blades' objects (their emitters are Kratos' own sounds)
                if (!known && nBlade < 4 && PlausibleSkeleton(sk)) {
                    const uint64_t* bh = *(const uint64_t**)(sk + 0x30);
                    if (bh && !IsBadReadPtr(bh, 8) && bh[0] == 0xef450b59487cb7afull) bladeObjs[nBlade++] = o;
                }
                // (no joint limit: the Blades with their chains are big skeletons; only Kratos himself is excluded)
                if (known || !haveNeck || sk == heroSkel || !PlausibleSkeleton(sk) || *(int16_t*)(sk + 0x38) >= 2000) continue;
                const int16_t nj = *(int16_t*)(sk + 0x38);
                const float* rp = (const float*)(*(uint8_t**)(sk + 0x58) + (size_t)(uint16_t)nj * 0x40 + 0x30);
                const float ddx = rp[0] - neckM.m[3][0], ddy = rp[1] - neckM.m[3][1], ddz = rp[2] - neckM.m[3][2];
                const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                // D-143: nor Atreus' quiver (owner log: goquiver00 "carried on Kratos")
                if (d2 < 1.3f * 1.3f && strncmp(nm, "gobow", 5) != 0 && strncmp(nm, "goquiver", 8) != 0)
                    carryFound.push_back({sk, nm});
                // only the bow (owner request); the copy nearest to Kratos within 1 m of the neck (placed by the game's
                // facing it can sit ~0.5 m off the turned body; other copies exist, like the axe's). D-067: was 3 m -
                // Atreus' own bow (same name) was taken for Kratos' carried bow whenever the boy stood close
                if (strncmp(nm, "gobow", 5) != 0 || d2 > 1.f * 1.f || d2 >= bowD2) continue;
                bowD2 = d2;
                bowSk = sk;
                bowObj = o;
                bowJ = nj;
                bowName = nm;
            }
        }
    }
    // D-067 (owner: "Kratos never equips the Atreus bow"): every "gobow" near Kratos is Atreus' bow - never glue or hide it
    // (gowvr.ini carried_bow=1 brings the old rule back)
    static const bool carriedBow = config::GetFloat("carried_bow", 0.f) > 0.5f;
    if (!carriedBow) bowSk = nullptr;
    if (bowSk) {
        carried.push_back(bowSk);
        carriedObjs.push_back(bowObj);
        static uint8_t* logged = nullptr;
        if (bowSk != logged) {
            logged = bowSk;
            Log("fp: carried item '%s' (%d joints, %.2f m from the neck) = skeleton %p", bowName, bowJ, sqrtf(bowD2), bowSk);
        }
    }
    for (int k = 0; k < kAt; ++k) {
        if (!haveNeck) continue;  // keep the previous choice until the hero is known
        if (best[k] != g_attached[k].skel.load())
            Log("fp: %s = skeleton %p (%.1f m from Kratos, %d copies)", g_attached[k].name, best[k],
                best[k] ? sqrtf(bestD2[k]) : -1.f, (int)atCount[k]);
        g_attached[k].skel = best[k];
        g_attached[k].obj = bestObj[k];
    }
    for (int i = 0; i < 4; ++i) g_bladeObjs[i] = i < nBlade ? bladeObjs[i] : nullptr;  // D-162
    {   // D-162: the linked items (learned offsets kept for the ones still there)
        const uint64_t* hh = heroSkel ? *(const uint64_t**)(heroSkel + 0x30) : nullptr;
        LinkedItem next[kLinkedMax];
        int n = 0;
        AcquireSRWLockExclusive(&g_linkedLock);
        for (auto& f : linkedFound) {
            if (n >= kLinkedMax) break;
            bool kept = false;
            for (int i = 0; i < g_linkedCount; ++i)
                if (g_linked[i].skel == f.skel && g_linked[i].obj == f.obj && g_linked[i].joint == f.joint) {
                    next[n] = g_linked[i];
                    kept = true;
                    break;
                }
            if (!kept) {
                LinkedItem it;
                it.skel = f.skel;
                it.obj = f.obj;
                it.joint = f.joint;
                it.hash = hh && !IsBadReadPtr(hh + f.joint, 8) ? hh[f.joint] : 0;
                if (f.name && !IsBadReadPtr(f.name, 1)) strncpy_s(it.name, f.name, _TRUNCATE);
                Log("fp: '%s' (skeleton %p, %d joints) hangs on Kratos joint %d (hash %016llx) - placed on the drawn joint "
                    "in F1", it.name, f.skel, (int)(uint16_t)*(int16_t*)(f.skel + 0x38), f.joint, (unsigned long long)it.hash);
                next[n] = it;
            }
            ++n;
        }
        for (int i = 0; i < kLinkedMax; ++i) {
            g_linked[i] = i < n ? next[i] : LinkedItem{};
            g_linkedSkel[i] = i < n ? next[i].skel : nullptr;
        }
        g_linkedCount = n;
        ReleaseSRWLockExclusive(&g_linkedLock);
    }
    AcquireSRWLockExclusive(&g_carriedLock);
    g_carried = carried;
    g_carriedObjs = carriedObjs;
    for (auto& it : g_carry) it.seen = false;
    for (auto& f : carryFound) {
        auto it = std::find_if(g_carry.begin(), g_carry.end(), [&](const CarryItem& c) { return c.skel == f.first; });
        if (it != g_carry.end()) { it->seen = true; continue; }
        CarryItem c;
        c.skel = f.first;
        strncpy_s(c.name, f.second, _TRUNCATE);
        if (!strncmp(c.name, "gomimirhead", 11)) c.carried = c.identity = true;  // D-162: Mimir is always carried
        g_carry.push_back(c);
        Log("fp: item near Kratos '%s' (skeleton %p) - watching whether he carries it", c.name, c.skel);
    }
    const uint64_t fr = g_frameNo.load();  // pass-found items leave when their passes stop coming near Kratos
    g_carry.erase(std::remove_if(g_carry.begin(), g_carry.end(), [fr](const CarryItem& c) {
        return c.byPass ? fr - c.lastPassFrame > 600 : !c.seen; }), g_carry.end());
    ReleaseSRWLockExclusive(&g_carriedLock);
}

// Game thread, every frame: carried items hidden in gameplay first person with the game's own model visibility (no
// joint shrinking: shrunk joints stretched the bow's strap into spikes), shown again otherwise.
void CarriedVisibility() {
    static std::vector<uint8_t*> hidden;
    static uint64_t frame = 0;
    if (++frame == 1) g_bowHide = config::GetFloat("carried_hide", 0.f) > 0.5f;  // owner: keep the bow on Kratos
    const bool want = g_bowHide.load() && FpMode() == 1;
    std::vector<uint8_t*> objs;
    AcquireSRWLockShared(&g_carriedLock);
    if (want) objs = g_carriedObjs;
    ReleaseSRWLockShared(&g_carriedLock);
    for (uint8_t* o : hidden)  // no longer wanted hidden: show
        if (std::find(objs.begin(), objs.end(), o) == objs.end()) {
            SetModelVisible(o, true);
            Log("fp: carried item %p shown (game model visibility)", o);
        }
    for (uint8_t* o : objs)
        if (std::find(hidden.begin(), hidden.end(), o) == hidden.end() || frame % 60 == 0) {  // re-assert each second
            if (std::find(hidden.begin(), hidden.end(), o) == hidden.end())
                Log("fp: carried item %p hidden (game model visibility)", o);
            SetModelVisible(o, false);
        }
    hidden = objs;
}

bool Still(const Mat4& a, const Mat4& b) {
    float dx = a.m[3][0] - b.m[3][0], dy = a.m[3][1] - b.m[3][1], dz = a.m[3][2] - b.m[3][2];
    float d = a.m[0][0] * b.m[0][0] + a.m[0][1] * b.m[0][1] + a.m[0][2] * b.m[0][2] +
              a.m[1][0] * b.m[1][0] + a.m[1][1] * b.m[1][1] + a.m[1][2] * b.m[1][2];
    return dx * dx + dy * dy + dz * dz < 0.003f * 0.003f && d > 2.f * 0.9998f;
}

std::atomic<int> g_axeTrace{0};

// ---- F1 throw (D-028) ----
std::atomic<ULONGLONG> g_throwAt{0}, g_throwReleasedAt{0};
// D-063: the Blades' aimed throw (L2 + throw gesture / L2 + R2). In F1 it plays on the upper-body layer without an
// attack move (LIVE combattrace: no att* animation, the right Blade flames but stays in the fist - owner: "the right
// blade stays always in my hand"), so the Blades' attack rule (follow the game's pose on our hand) never ran. The throw
// input marks it instead; arms.cpp follows the game's Blades pose for 1.3 s after it.
std::atomic<ULONGLONG> g_bladeThrowAt{0};
void BladeThrowStart() {
    g_bladeThrowAt = GetTickCount64();
    Log("fp: Blades throw - the Blades follow the game's throw for 1.3 s");
}
// D-077: the effective "Blades free" state lives in arms.cpp (ends when the Blades are back in the game's hands)
bool BladeThrowActive() { return ArmsBladesFree(); }
std::atomic<ULONGLONG> g_bladeAimR2At{0};
void BladeAimHeld() { g_bladeAimR2At = GetTickCount64(); }
bool BladesFreeRaw() {
    const ULONGLONG now = GetTickCount64(), th = g_bladeThrowAt.load(), r2 = g_bladeAimR2At.load();
    return CombatBladesFree() || (th && now - th < 500) || (r2 && now - r2 < 200);
}
// diagnostic (fxjump): the axe's own mod-applied offset (our placement - the game's), fresh pass to pass
static std::atomic<bool> g_axeJumpOn{false};
static float g_axeJumpPrev[3];
static bool g_axeJumpHave = false, g_axeJumpPrevFixed = false;
static uint64_t g_axeJumpN = 0, g_axeJumps = 0;
static float g_axeJumpMax = 0.f;
static SRWLOCK g_axeJumpLock = SRWLOCK_INIT;
void AxeJumpReport(const char* tag, bool reset, bool on) {
    AcquireSRWLockExclusive(&g_axeJumpLock);
    if (tag) Log("fxjump: [%s] AXE passes %llu, placement jumps > 5 cm %llu, max %.2f m", tag,
                 (unsigned long long)g_axeJumpN, (unsigned long long)g_axeJumps, g_axeJumpMax);
    if (reset) { g_axeJumpN = g_axeJumps = 0; g_axeJumpMax = 0.f; g_axeJumpHave = false; }
    g_axeJumpOn = on;
    ReleaseSRWLockExclusive(&g_axeJumpLock);
}
static void AxeJumpSample(const float off[3], bool fixed) {
    if (!g_axeJumpOn.load()) return;
    AcquireSRWLockExclusive(&g_axeJumpLock);
    ++g_axeJumpN;
    if (g_axeJumpHave) {
        const float dx = off[0] - g_axeJumpPrev[0], dy = off[1] - g_axeJumpPrev[1], dz = off[2] - g_axeJumpPrev[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.05f) {
            ++g_axeJumps;
            if (g_axeJumps <= 15)
                Log("fxjump: AXE jump %.2f m | fixed %d -> %d | held %d", d, (int)g_axeJumpPrevFixed, (int)fixed,
                    (int)g_axeInHand.load());
        }
        g_axeJumpMax = fmaxf(g_axeJumpMax, d);
    }
    memcpy(g_axeJumpPrev, off, 12);
    g_axeJumpPrevFixed = fixed;
    g_axeJumpHave = true;
    ReleaseSRWLockExclusive(&g_axeJumpLock);
}
std::atomic<bool> g_fastRelease{true};  // D-059 A/B: command "fastrelease on|off"
void FastRelease(bool on) { g_fastRelease = on; Log("fp: thrown axe released at once %s", on ? "ON" : "OFF"); }
static void KinkReport(const char* tag);
// the aim trigger (left) is held on the motion controllers: an axe leaving the hand now is a throw
static bool AimTriggerHeld() {
    const vr::InputSnapshot in = vr::XrInput();
    return in.focused && in.hand[0].active && in.hand[0].trigger > 0.3f;
}
std::atomic<bool> g_axeThrown{false};
SRWLOCK g_calmLock = SRWLOCK_INIT;
std::vector<Mat4> g_calmLocal;  // the hero's local pose from the last world pass before a throw
std::atomic<uint64_t> g_frozenPasses{0};

void ThrowStart() {
    if (FpMode() != 1 || !g_axeInHand.load()) return;
    g_throwAt = GetTickCount64();
    g_throwReleasedAt = 0;
    Log("fp: throw started (time boost until release, body held at the pre-throw pose)");
    // D-211: the axe's passes around the first few throws (which pass lands where: caller, fresh, distances)
    static int diagThrows = 0;
    if (diagThrows++ < 3 && g_axeDiag.load() <= 0) AxeDiag(160);
}
bool AxeThrown() { return g_axeThrown.load(); }

// Weight of the pre-throw pose for the hero's world passes: 1 through the throw, fading out over throw_blend_ms once
// throw_hold_ms passed after the release (the game's follow-through), 0 otherwise. F1 only.
static float ThrowFreezeWeight() {
    const ULONGLONG t0 = g_throwAt.load();
    if (!t0 || FpMode() != 1) return 0.f;
    static const float holdMs = config::GetFloat("throw_hold_ms", 600.f), blendMs = config::GetFloat("throw_blend_ms", 250.f);
    const ULONGLONG now = GetTickCount64(), rel = g_throwReleasedAt.load();
    float since;  // ms into the fade-out
    if (rel) since = (float)(now - rel) - holdMs;
    else since = (float)(now - t0) - 1200.f;  // never released (blocked / cancelled): give up after 1.2 s
    if (since <= 0.f) return 1.f;
    if (since >= blendMs) { g_throwAt = 0; return 0.f; }
    return 1.f - since / blendMs;
}

// The hero world pass reads `local` (n matrices, sub_1405908a0 param_2). Outside a throw the pose is remembered;
// during one the pass is fed the remembered pose (blended out at the end) - one consistent pose, so the published
// joints, the camera base and the body all stay still while the game plays its throw animation underneath.
// Height of joint j above the root in a local pose (locals composed along the parent chain; row vectors).
static float ChainHeight(const Mat4* locals, int j) {
    Mat4 m = locals[j];
    for (int p = JointParent(j), guard = 0; p >= 0 && guard < 64; p = JointParent(p), ++guard) m = Mul(m, locals[p]);
    return m.m[3][1];
}

// D-038: the local pose the CURRENT pass on this thread was fed (sub_1405908a0's second parameter - not necessarily the
// block at skel+0x58, and FreezeLocal may substitute a held pose). The arm rebuild must read exactly these locals.
thread_local const uint8_t* t_passLocal = nullptr;
std::atomic<uint64_t> g_localOther{0}, g_localSame{0};
const uint8_t* PassLocals(uint8_t* skel) {
    const uint8_t* mats = *(const uint8_t**)(skel + 0x58);
    if (!t_passLocal) return mats;
    if (t_passLocal != mats) ++g_localOther; else ++g_localSame;
    return t_passLocal;
}
void PassLocalStats() {
    Log("fp: hero passes fed locals elsewhere than skel+0x58: %llu, at skel+0x58: %llu",
        (unsigned long long)g_localOther.load(), (unsigned long long)g_localSame.load());
}

static const uint8_t* FreezeLocal(uint8_t* skel, const uint8_t* local) {
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (!local || n <= 0) return local;
    const float w = fmaxf(ThrowFreezeWeight(), CombatHoldWeight());
    if (w <= 0.f) {
        AcquireSRWLockExclusive(&g_calmLock);
        g_calmLocal.resize((size_t)n);
        memcpy(g_calmLocal.data(), local, (size_t)n * sizeof(Mat4));
        ReleaseSRWLockExclusive(&g_calmLock);
        return local;
    }
    thread_local std::vector<Mat4> out;
    out.resize((size_t)n);
    AcquireSRWLockShared(&g_calmLock);
    const bool ok = (int)g_calmLocal.size() == n;
    if (ok) {
        const Mat4* cur = (const Mat4*)local;
        for (int j = 0; j < n; ++j) {
            const Mat4& a = cur[j];
            const Mat4& b = g_calmLocal[j];
            Mat4& o = out[j];
            if (w >= 1.f) { o = b; continue; }
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[r][c] + (b.m[r][c] - a.m[r][c]) * w;
            for (int r = 0; r < 3; ++r) {  // keep the axes unit-length-ish (scale of the blended rows)
                const float la = sqrtf(a.m[r][0] * a.m[r][0] + a.m[r][1] * a.m[r][1] + a.m[r][2] * a.m[r][2]);
                const float lb = sqrtf(b.m[r][0] * b.m[r][0] + b.m[r][1] * b.m[r][1] + b.m[r][2] * b.m[r][2]);
                const float lo = sqrtf(o.m[r][0] * o.m[r][0] + o.m[r][1] * o.m[r][1] + o.m[r][2] * o.m[r][2]);
                if (lo > 1e-6f) {
                    const float k = (la + (lb - la) * w) / lo;
                    for (int c = 0; c < 3; ++c) o.m[r][c] *= k;
                }
            }
            // D-090 (owner: after an attack, starting to move, "the Blade stretches a little, the Blade and the axe move
            // in a bad axis - for about a second, then correct"; D-056 saw the same "misaligns for a second"): while
            // the combat hold fades out (CombatHoldWeight 1 -> 0, ~0.55 s) these locals were a component-wise mix of the
            // frozen and the live (run start) pose - sheared, not rotations. Every joint built from them was sheared,
            // and the weapon placements that undo the animated hand with a transpose (RigidInverse) turned and stretched
            // the Blades and the axe. The blended basis is made orthonormal again (lengths and handedness kept)
            OrthonormalizeKeepScale(o);
        }
    }
    // leaps (combat.cpp): the animated neck's height against the held pose's
    static const int neck = -2;
    static int neckIdx = -1;
    static uint8_t* neckSkel = nullptr;
    (void)neck;
    if (ok && skel != neckSkel) { neckSkel = skel; neckIdx = FindJointByHash(0xf14d9a553fdad7ccull); }
    if (ok && neckIdx >= 0 && neckIdx < n)
        CombatReportRise(ChainHeight((const Mat4*)local, neckIdx) - ChainHeight(g_calmLocal.data(), neckIdx));
    ReleaseSRWLockShared(&g_calmLock);
    if (!ok) return local;
    ++g_frozenPasses;
    return (const uint8_t*)out.data();
}

// Kratos' creature time scale: sub_14071db60(creature, slot, scale, 1.0) is what the game's SetTimeScaleMultiplier
// request executes (SetTimeScaleMultiplierRequest::vfunc_1 0x1402219a0, slot 5); creature = object+0x220 (type 2).
static bool SetHeroTimeScale(float scale, float* prev) {
    uint8_t* obj = HeroObject();
    if (!obj || IsBadReadPtr(obj + 0x220, 8)) return false;
    uint8_t* cr = *(uint8_t**)(obj + 0x220);
    if (!cr || IsBadReadPtr(cr, 0x340) || *(int32_t*)(cr + 0x14) != 2) return false;
    if (prev) *prev = *(float*)(cr + 0x308 + 5 * 4);
    using PFN = void(__fastcall*)(uint8_t*, int, float, float);
    ((PFN)((uint8_t*)GameVa(0x14071db60ull)))(cr, 5, scale, 1.f);
    return true;
}

bool HeroTimeScale(float scale, float* prev) { return SetHeroTimeScale(scale, prev); }

// D-203 (owner: "all of a sudden all my movement is sped up - walking, throwing - like the tick speed is way too fast";
// it came after throwing the axe a lot): slot 5 had two owners - the combat boosts (combat.cpp) and the throw boost
// (ThrowTimeTick) - and each saved the slot's value as "normal" when it began and restored that when it ended. One
// starting while the other ran saved the OTHER's boost as normal (attack 2x running, a throw saves 2.0, the attack ends
// -> 1.0, the throw ends -> 2.0) - Kratos stayed fast for good, both owners thinking the speed normal. Now one arbiter:
// each owner states what it wants (or releases), the fastest active wish is applied; with none the slot goes back to the
// value it had before the first wish - never a speed-up (a stuck > 1 base reads as 1).
SRWLOCK g_tsLock = SRWLOCK_INIT;
float g_tsWish[4] = {0.f, 0.f, 0.f, 0.f};
bool g_tsActive = false;
float g_tsBase = 1.f, g_tsApplied = 1.f;
bool HeroTimeScaleWish(int owner, float scale) {
    if (owner < 0 || owner > 3) return false;
    AcquireSRWLockExclusive(&g_tsLock);
    g_tsWish[owner] = scale > 1.001f || scale < 0.999f ? scale : 0.f;
    float m = 0.f;
    for (float w : g_tsWish)
        if (w > 0.f && (m == 0.f || w > m)) m = w;
    bool ok = true;
    if (m == 0.f) {
        if (g_tsActive) {
            ok = SetHeroTimeScale(g_tsBase, nullptr);
            g_tsActive = false;
            g_tsApplied = g_tsBase;
        }
    } else if (!g_tsActive) {
        float prev = 1.f;
        ok = SetHeroTimeScale(m, &prev);
        if (ok) {
            g_tsBase = prev > 0.05f && prev <= 1.001f ? prev : 1.f;  // a leftover speed-up is not "normal"
            if (prev > 1.001f) Log("fp: time scale slot 5 found at %.2f with no boost of ours - treated as 1.0 (D-203)", prev);
            g_tsActive = true;
            g_tsApplied = m;
        }
    } else if (m != g_tsApplied) {
        ok = SetHeroTimeScale(m, nullptr);
        if (ok) g_tsApplied = m;
    }
    ReleaseSRWLockExclusive(&g_tsLock);
    return ok;
}

// D-080 diagnostic "tsdump N": Kratos' creature time-scale slots 0..9 (creature+0x308) for N frames - a melee hit's
// hit pause should show as a slot dipping for a few frames (the hooked per-hit handler never saw a melee hit)
std::atomic<int> g_tsDump{0};
void TsDump(int n) { g_tsDump = n; Log("tsdump: next %d frames", n); }
static void TsDumpTick() {
    if (g_tsDump.load() <= 0) return;
    g_tsDump.fetch_sub(1);
    uint8_t* obj = HeroObject();
    if (!obj || IsBadReadPtr(obj + 0x220, 8)) return;
    uint8_t* cr = *(uint8_t**)(obj + 0x220);
    if (!cr || IsBadReadPtr(cr + 0x308, 0x28)) return;
    const float* s = (const float*)(cr + 0x308);
    char prim[64];
    HeroPrimaryAnim(prim, sizeof(prim));
    Log("tsdump: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | %s", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8],
        s[9], prim);
}

// Game thread, every frame (BodyPostAnim): boost while a throw has not released yet (max 0.7 s).
static void ThrowTimeTick() {
    static bool boosted = false;
    static float saved = 1.f;
    static const float boost = config::GetFloat("throw_timescale", 4.f);
    const ULONGLONG t0 = g_throwAt.load(), now = GetTickCount64();
    const bool want = boost > 1.01f && FpMode() == 1 && t0 && !g_throwReleasedAt.load() && now - t0 < 700;
    if (want == boosted) return;
    (void)saved;
    if (want) {
        if (HeroTimeScaleWish(1, boost)) boosted = true;  // D-203: through the arbiter (owner 1 = the throw)
    } else {
        HeroTimeScaleWish(1, 0.f);
        boosted = false;
        Log("fp: throw time boost off after %llu ms (released %d)", (unsigned long long)(now - t0),
            g_throwReleasedAt.load() ? 1 : 0);
    }
}

// diagnostic "attjitter N": per attached-object pass, the object's final root relative to the motion-controlled anchor
// (IK elbow for the shield, IK wrist for the axe); still hands -> any pass-to-pass change is visible jitter.
std::atomic<int> g_attJitter{0};
struct AttJit { bool have = false; float last[3] = {}; float worst = 0.f; int passes = 0, moved = 0, unfixed = 0, notFresh = 0; };
AttJit g_attJit[2];
SRWLOCK g_attJitLock = SRWLOCK_INIT;
void AttJitter(int n) { g_attJitter = n; Log("attjitter: next %d attached-object passes", n); }
static void AttJitSample(const Attached& at, uint8_t* skel, bool fixed, bool fresh, float dAnim = -1.f, float dIk = -1.f) {
    if (g_attJitter.load() <= 0) return;
    Mat4 wrist, elbow;
    if (!ArmsIkResult(at.hand, &wrist, &elbow)) return;
    const Mat4& A = at.elbow ? elbow : wrist;
    const int16_t n = *(int16_t*)(skel + 0x38);
    const Mat4 R = *(const Mat4*)(*(uint8_t**)(skel + 0x58) + (size_t)(uint16_t)n * 0x40);
    const Mat4 rel = Mul(R, RigidInverse(A));
    AcquireSRWLockExclusive(&g_attJitLock);
    AttJit& j = g_attJit[at.elbow ? 0 : 1];
    ++j.passes;
    if (!fixed && ++j.unfixed <= 6)
        Log("attjitter: %s UNFIXED pass: game root %.3f m from the animated anchor, %.3f m from the IK anchor, streak %d",
            at.elbow ? "shield" : "axe", dAnim, dIk, at.awayStreak.load());
    if (!fresh) ++j.notFresh;
    if (j.have) {
        const float dx = rel.m[3][0] - j.last[0], dy = rel.m[3][1] - j.last[1], dz = rel.m[3][2] - j.last[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.01f) ++j.moved;
        j.worst = fmaxf(j.worst, d);
    }
    for (int i = 0; i < 3; ++i) j.last[i] = rel.m[3][i];
    j.have = true;
    ReleaseSRWLockExclusive(&g_attJitLock);
    if (g_attJitter.fetch_sub(1) == 1) {
        AcquireSRWLockExclusive(&g_attJitLock);
        for (int k = 0; k < 2; ++k) {
            Log("attjitter: %s passes %d moved > 1 cm %d (worst %.3f m) unfixed %d notFresh %d", k ? "axe   " : "shield",
                g_attJit[k].passes, g_attJit[k].moved, g_attJit[k].worst, g_attJit[k].unfixed, g_attJit[k].notFresh);
            g_attJit[k] = AttJit{};
        }
        ReleaseSRWLockExclusive(&g_attJitLock);
    }
}

// ---- D-147: enemy health bars in the world (static RE: r_ui.wad + GoW.exe) -----------------------------------------
// The HUD pieces are skeletal UI game objects (r_ui.wad: goenemyhealthmeter / gobosshealthmeter / gohealth_bar /
// gorage_bar / gospecialmeters, models MDL_*, meshes MG_*_0_gpu, animations ANM_*), drawn with the 2D UI views C / D;
// each skeleton's root joint is named after the piece (goProto*: "enemyHealthMeter", "health_bar", ...). Kratos' own
// bars are positioned by combathud.lua; the enemy bars by C++ (no script mentions them), at the projection of the enemy
// in UI units (view C ortho: x +-16, y +-9). The sharp HUD pastes the whole UI scaled about the eye centre (D-104), so a
// bar sat at s x its angle and followed the head. Its root is re-placed here: the game's UI spot is turned back into a
// world direction through the camera the game projected with, then projected through the head camera, divided by the
// paste scale - after the paste it lands exactly where that direction is in the eye image (no stereo depth: both eyes
// get one HUD). Which camera the game used (view A = head, or view B = the game camera) is decided from the data: the
// right one gives a direction that holds still while the head turns against the game camera. gowvr.ini ui_bars_3d:
// 0 off, 1 auto (default), 2 view A, 3 view B.
constexpr uint64_t kHudEnemyMeter = 0xac05bff0d3c12ab1ull, kHudEnemyMeterPasted = 0x5e02b62b75e2359eull,
                   kHudBossMeter = 0x8272f39e25672fa8ull, kHudHealthBar = 0x34cc475d7694c8abull,
                   kHudRageBar = 0x3f997ecb7b0d11c7ull, kHudSpecialMeters = 0xb29f9e02b685b93aull;
struct MeterTrack { uint8_t* skel = nullptr; float dA[3]{}, dB[3]{}, hF[3]{}, bF[3]{}; bool have = false; };
SRWLOCK g_meterLock = SRWLOCK_INIT;
MeterTrack g_meterTrack[8];
int g_meterTrackNext = 0;
double g_meterSumA = 0.0, g_meterSumB = 0.0, g_meterSumTurn = 0.0;
uint32_t g_meterSamples = 0;
std::atomic<int> g_meterCam{0};  // 0 undecided, 1 view A (head), 2 view B (game camera)
std::atomic<uint32_t> g_meterMoved{0}, g_meterBehind{0}, g_meterNoCam{0};
static float AngleBetween(const float* a, const float* b) {
    const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return acosf(fminf(1.f, fmaxf(-1.f, c)));
}
static void HudSighting(uint64_t h0, uint8_t* skel, int nj, const float* root) {
    static std::atomic<int> logs{0};
    static uint8_t* seen[16] = {};
    for (uint8_t* s : seen) if (s == skel) return;
    const int k = logs.fetch_add(1);
    if (k >= 16) return;
    seen[k] = skel;
    const char* nm = h0 == kHudEnemyMeter ? "enemy health bar" : h0 == kHudEnemyMeterPasted ? "enemy health bar (pasted)"
                   : h0 == kHudBossMeter ? "boss health bar" : h0 == kHudHealthBar ? "Kratos' health bar"
                   : h0 == kHudRageBar ? "Spartan Rage bar" : "runic (special) meters";
    Log("hud: %s skeleton %p (%d joints) at UI (%.2f %.2f %.2f)", nm, skel, nj, root[12], root[13], root[14]);
}
static bool MeterReproject(uint8_t* skel, const float* root, float* out) {
    // D-149: off by default - owner log: every enemy bar skeleton's root sits at the UI origin (0 0 0), so the bar's spot
    // is applied outside its root and this never had anything to place ("still undecided" both ways)
    static const int mode = (int)config::GetFloat("ui_bars_3d", 0.f);
    if (mode <= 0 || !stereo::SharpUiActive()) return false;
    const float s = stereo::UiScaleCurrent();
    Mat4 H;
    float tAx, tAy;
    uint8_t* vb = G<uint8_t*>(0x142d4e5e0);
    if (!stereo::HeadCameraForUi(&H, &tAx, &tAy) || !vb || IsBadReadPtr(vb, 0x200) || s < 0.05f) {
        ++g_meterNoCam;
        return false;
    }
    Mat4 B;
    memcpy(&B, vb + 0x150, sizeof(B));
    const float* PB = (const float*)(vb + 0xd0);
    if (!(PB[0] > 0.f) || !(PB[5] > 0.f)) { ++g_meterNoCam; return false; }
    const float tBx = 1.f / PB[0], tBy = 1.f / PB[5];
    const float nx = root[12] / 16.f, ny = root[13] / 9.f;  // the bar's spot in UI clip space
    auto dirOf = [&](const Mat4& C, float tx, float ty, float* d) {  // camera looks down -Z (rows X, Y, Z)
        const float cx = nx * tx, cy = ny * ty;
        for (int i = 0; i < 3; ++i) d[i] = cx * C.m[0][i] + cy * C.m[1][i] - C.m[2][i];
        const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1e-6f) for (int i = 0; i < 3; ++i) d[i] /= l;
    };
    float dA[3], dB[3];
    dirOf(H, tAx, tAy, dA);
    dirOf(B, tBx, tBy, dB);
    const float hF[3] = {-H.m[2][0], -H.m[2][1], -H.m[2][2]}, bF[3] = {-B.m[2][0], -B.m[2][1], -B.m[2][2]};
    // which camera: per bar, how much each hypothesis' world direction moves while the head turns against the camera
    int cam = mode == 2 ? 1 : mode == 3 ? 2 : g_meterCam.load();
    if (mode == 1) {
        AcquireSRWLockExclusive(&g_meterLock);
        MeterTrack* t = nullptr;
        for (auto& m : g_meterTrack) if (m.skel == skel) { t = &m; break; }
        if (!t) { t = &g_meterTrack[g_meterTrackNext]; g_meterTrackNext = (g_meterTrackNext + 1) % 8; *t = MeterTrack{}; t->skel = skel; }
        if (t->have) {
            const float turnH = AngleBetween(hF, t->hF), turnB = AngleBetween(bF, t->bF);
            if (turnH + turnB > 0.004f && turnH + turnB < 0.3f) {
                g_meterSumA += AngleBetween(dA, t->dA);
                g_meterSumB += AngleBetween(dB, t->dB);
                g_meterSumTurn += fabsf(turnH - turnB);
                ++g_meterSamples;
                if (g_meterCam.load() == 0 && g_meterSamples >= 120 && g_meterSumTurn > 0.35) {
                    const int c = g_meterSumA < 0.5 * g_meterSumB ? 1 : g_meterSumB < 0.5 * g_meterSumA ? 2 : 0;
                    if (c) {
                        g_meterCam = c;
                        cam = c;
                        Log("hud: enemy health bars are projected with %s (direction moved %.1f deg as view A, %.1f deg as "
                            "view B over %u samples, head vs game camera turn %.1f deg) - placed for the head from now",
                            c == 1 ? "view A (the head camera)" : "view B (the game camera)", g_meterSumA * 57.2958,
                            g_meterSumB * 57.2958, g_meterSamples, g_meterSumTurn * 57.2958);
                    } else if (g_meterSamples % 600 == 0) {
                        Log("hud: enemy health bar camera still undecided (%.1f deg as view A, %.1f deg as view B, %u samples)",
                            g_meterSumA * 57.2958, g_meterSumB * 57.2958, g_meterSamples);
                    }
                }
            }
        }
        memcpy(t->dA, dA, sizeof(dA)); memcpy(t->dB, dB, sizeof(dB));
        memcpy(t->hF, hF, sizeof(hF)); memcpy(t->bF, bF, sizeof(bF));
        t->have = true;
        ReleaseSRWLockExclusive(&g_meterLock);
    }
    if (cam != 1 && cam != 2) return false;
    const float* d = cam == 1 ? dA : dB;
    const float lx = d[0] * H.m[0][0] + d[1] * H.m[0][1] + d[2] * H.m[0][2];
    const float ly = d[0] * H.m[1][0] + d[1] * H.m[1][1] + d[2] * H.m[1][2];
    const float lz = d[0] * H.m[2][0] + d[1] * H.m[2][1] + d[2] * H.m[2][2];
    if (lz > -0.05f) { ++g_meterBehind; return false; }  // beside / behind the head: the game's own spot
    const float ex = (lx / -lz) / tAx, ey = (ly / -lz) / tAy;  // where the direction is in the eye image (clip space)
    memcpy(out, root, sizeof(Mat4));
    out[12] = 16.f * ex / s;
    out[13] = 9.f * ey / s;
    ++g_meterMoved;
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 10000 && tLog.compare_exchange_strong(t0, now))
        Log("hud: enemy bars 10 s: %u placed for the head (%s), %u beside / behind, %u without a camera; last UI (%.2f %.2f) "
            "-> (%.2f %.2f)", g_meterMoved.exchange(0), cam == 1 ? "view A" : "view B", g_meterBehind.exchange(0),
            g_meterNoCam.exchange(0), root[12], root[13], out[12], out[13]);
    return true;
}

// ---- D-149: the aiming icon as its own UI objects (static RE: r_ui.wad reticle prototypes) ---------------------------
// Owner: "what's wrong with the enemy bar is the aiming icon you made bigger once I hold L2 - find the aiming icon and do
// it normally". D-104 / D-107 pasted a CIRCLE of the UI image around the game's reticle spot, at native size and at the
// head's aim point - anything inside the circle came along (an enemy bar over the target: big, stuck to the view). The
// aiming icon is a set of skeletal UI objects (goProtoreticle and its pieces: ret_aim, ret_on_no_aim, ret_lockOn,
// ret_notAvailable, ret_enemy_in_range(_precision), ret_aim_rage, ret_hitValid_rage, the diamonds ret_outer / _middle /
// _inner / _backdrop, Atreus' hunt minigame reticles), each identified by its root joint name hash like the other HUD
// pieces (D-147: their world passes come through this hook - the health / rage bars' roots hold their UI spots). While
// head-aiming, only these objects are moved / sized in UI space so that the ordinary scaled paste puts them at the aim
// point at ui_reticle_scale of their native size; the region paste is off while they are (sharpui.cpp). The rest of the
// HUD stays where it was. gowvr.ini ui_reticle_mode 0 = the D-104 region paste again.
static bool IsReticleHash(uint64_t h) {
    static const std::array<uint64_t, 17> k = [] {
        const char* n[17] = {"reticle", "ret_aim", "ret_on_no_aim", "ret_lockOn", "ret_notAvailable", "ret_enemy_in_range",
                             "ret_enemy_in_range_precision", "ret_aim_rage", "ret_hitValid_rage", "ret_outer", "ret_middle",
                             "ret_inner", "ret_backdrop", "ret_aim_minigame_default", "ret_hitValid_minigame_default",
                             "ret_aim_minigame_hold_breath", "ret_hitValid_minigame_hold_breath"};
        std::array<uint64_t, 17> a{};
        for (int i = 0; i < 17; ++i) a[i] = JointHash(n[i]);
        return a;
    }();
    for (uint64_t x : k) if (x == h) return true;
    return false;
}
std::atomic<uint32_t> g_retMoved{0};
// D-162 (owner: "during combat, holding L2 still enlarges the aiming icon"): the region paste is at the HUD's own scale
// (D-154), so the growth is the game's own: in a fight its reticle switches to the bigger "enemy in range" / lock-on
// pieces (r_ui.wad, D-149 list). While head-aiming those pieces are drawn at ui_reticle_inrange_scale (0.5) of their
// size about the game's reticle spot (the region paste then carries them to the aim point as before); 1 = the game's
// size. Every reticle piece's root scale is logged when first seen while aiming and when it changes by > 15 % (evidence:
// which piece grows).
static const char* const kRetNames[17] = {"reticle", "ret_aim", "ret_on_no_aim", "ret_lockOn", "ret_notAvailable",
                                          "ret_enemy_in_range", "ret_enemy_in_range_precision", "ret_aim_rage",
                                          "ret_hitValid_rage", "ret_outer", "ret_middle", "ret_inner", "ret_backdrop",
                                          "ret_aim_minigame_default", "ret_hitValid_minigame_default",
                                          "ret_aim_minigame_hold_breath", "ret_hitValid_minigame_hold_breath"};
static int ReticlePiece(uint64_t h) {
    static const std::array<uint64_t, 17> k = [] {
        std::array<uint64_t, 17> a{};
        for (int i = 0; i < 17; ++i) a[i] = JointHash(kRetNames[i]);
        return a;
    }();
    for (int i = 0; i < 17; ++i) if (k[i] == h) return i;
    return -1;
}
static bool ReticleInRange(uint8_t* skel, const float* root, float* out) {
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (nj < 1 || nj > 64) return false;
    float su, sv, sr, du, dv;
    if (!stereo::AimReticleDisplay(&su, &sv, &sr, &du, &dv) || !stereo::SharpUiActive()) return false;  // aiming only
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8)) return false;
    const int piece = ReticlePiece(hh[0]);
    if (piece < 0) return false;
    const float sc = sqrtf(root[0] * root[0] + root[1] * root[1] + root[2] * root[2]);
    {   // evidence
        static float lastSc[17] = {};
        static std::atomic<int> logs{0};
        if ((lastSc[piece] == 0.f || fabsf(sc - lastSc[piece]) > 0.15f * lastSc[piece]) && logs.fetch_add(1) < 60) {
            Log("hud: aiming icon piece '%s' while aiming: root scale %.3f (was %.3f) at UI (%.2f %.2f)", kRetNames[piece], sc,
                lastSc[piece], root[12], root[13]);
            lastSc[piece] = sc;
        }
    }
    static const float f = config::GetFloat("ui_reticle_inrange_scale", 0.5f);
    const bool inRange = piece == 3 || piece == 5 || piece == 6;  // ret_lockOn, ret_enemy_in_range(_precision)
    if (!inRange || !(f > 0.f) || f >= 0.999f) return false;
    const uint8_t* mm = *(const uint8_t**)(skel + 0x58);
    if ((const uint8_t*)root >= mm && (const uint8_t*)root < mm + (size_t)nj * 0x80) return false;  // ours already
    // UI clip space (x / 16, y / 9); scaled about the game's reticle spot (a piece placed by an offset outside its root -
    // root at the UI origin - is scaled about itself, which sits at that spot)
    const float cx = 2.f * su - 1.f, cy = 1.f - 2.f * sv;
    const float px = root[12] / 16.f, py = root[13] / 9.f;
    const bool atOrigin = fabsf(root[12]) < 1e-3f && fabsf(root[13]) < 1e-3f && (fabsf(cx) > 0.01f || fabsf(cy) > 0.01f);
    memcpy(out, root, sizeof(Mat4));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] *= f;
    if (!atOrigin) {
        out[12] = (cx + (px - cx) * f) * 16.f;
        out[13] = (cy + (py - cy) * f) * 9.f;
    }
    return true;
}
static bool ReticlePlace(uint8_t* skel, const float* root, float* out) {
    // D-152 (owner: "revert the HUD fix for the aiming icon and the enemy bar - the middle circle, 2x smaller"): off by
    // default, the D-104 region paste is back (with half its radius, mutate.cpp); ui_reticle_mode 1 = D-149 again
    const int mode = stereo::SharpUiReticleMode();  // (owner 2026-10-18: 2 = no circle, the default)
    if (mode != 1) return ReticleInRange(skel, root, out);  // D-162
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (nj < 1 || nj > 64) return false;
    float su, sv, sr, du, dv;
    if (!stereo::AimReticleDisplay(&su, &sv, &sr, &du, &dv) || !stereo::SharpUiActive()) return false;
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8) || !IsReticleHash(hh[0])) return false;
    const uint8_t* mm = *(const uint8_t**)(skel + 0x58);
    if ((const uint8_t*)root >= mm && (const uint8_t*)root < mm + (size_t)nj * 0x80) return false;  // self-fed: ours already
    const float s = stereo::UiScaleCurrent(), k = stereo::SharpUiReticleScaleNow();
    if (s < 0.05f || k <= 0.f) return false;
    // UI clip space (view C ortho: x / 16, y / 9, y up); D-107 spots are UI uv (v down)
    const float cx = 2.f * su - 1.f, cy = 1.f - 2.f * sv;  // the game's reticle spot
    const float dx = 2.f * du - 1.f, dy = 1.f - 2.f * dv;  // where it is drawn (the head's aim point) in the eye image
    const float px = root[12] / 16.f, py = root[13] / 9.f;
    // a piece whose root sits at the UI origin is placed by an offset outside its root (like the enemy bars): its spot
    // is taken as the game's reticle spot
    const bool atOrigin = fabsf(root[12]) < 1e-3f && fabsf(root[13]) < 1e-3f && (fabsf(cx) > 0.01f || fabsf(cy) > 0.01f);
    const float ox = atOrigin ? cx : 0.f, oy = atOrigin ? cy : 0.f;
    // after the paste (x s about the centre) the piece must be at d + (its spot - c) x k, k x its native size
    const float tx = (dx + (px + ox - cx) * k) / s - ox, ty = (dy + (py + oy - cy) * k) / s - oy;
    memcpy(out, root, sizeof(Mat4));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] *= k / s;
    out[12] = tx * 16.f;
    out[13] = ty * 9.f;
    stereo::SharpUiReticleObjectsMoved();
    {
        static std::atomic<int> logs{0};
        static uint64_t seen[24] = {};
        bool known = false;
        for (uint64_t x : seen) if (x == hh[0]) { known = true; break; }
        if (!known) {
            const int i = logs.fetch_add(1);
            if (i < 24) {
                seen[i] = hh[0];
                Log("hud: aiming icon piece %016llx (%d joints) at UI (%.2f %.2f)%s -> (%.2f %.2f) x%.2f for the aim point",
                    (unsigned long long)hh[0], nj, root[12], root[13], atOrigin ? " [root at the origin]" : "", out[12], out[13],
                    k / s);
            }
        }
    }
    ++g_retMoved;
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 10000 && tLog.compare_exchange_strong(t0, now))
        Log("hud: aiming icon 10 s: %u passes moved to the aim point (game spot %.3f %.3f -> drawn %.3f %.3f, scale %.2f)",
            g_retMoved.exchange(0), su, sv, du, dv, k);
    return true;
}

// ---- D-146: the sheathed axe hangs on a Kratos JOINT (static RE, reverse_engineering/src) --------------------------
// attachment::tThrowable ctor sub_14088f810 resolves the weapon definition's OutHandJoint (+0x60) / OnBackJoint (+0x68)
// in Kratos' skeleton; the attach modes (AttachMode_AxeEquipped / AttachMode_AxeSheathed: runtime records of 0x58 bytes,
// connect joint hash +0x18, scale +0x20 = the sheathed axe's 0.85 / 0.94, offset +0x24) go through sub_14088f4a0 ->
// sub_14088ec50, which parents the axe's GAME OBJECT to that joint: obj+0x60 = local 4x4 (scale + offset), the parent
// link at obj+0xa0 (+0x140 parent object, +0x148 type 2 = joint, +0x14c joint index). Every frame the object's world is
// local x parent joint world (sub_140553eb0 -> sub_14058c560), the parent world taken from the link's cache (obj+0xf0,
// 4x4) or re-read from Kratos' skeleton - his world cache (our DRAWN pose, once our hero pass ran) or a separate
// evaluation of the GAME's pose. That is the "several passes per frame, some on the game's body, some on the drawn one"
// of D-027 / D-138, and why no learned spot relative to the root or chest could hold. Now the axe's own offset to its
// parent joint is measured from the game's sheathed passes (C = root x parent world^-1, the matrix the game just used),
// and every pass of the put-away axe in F1 is placed C x the DRAWN parent joint: exactly the game's attachment, on the
// body we draw (torso twist, held attack pose, turn - all included). The stray full-size pass at the hand gets the same.
std::atomic<int> g_axeBackJoint{-1};  // Kratos joint the sheathed axe is parented to (published from the hero pass)
SRWLOCK g_axeBackLock = SRWLOCK_INIT;
Mat4 g_axeBackC{};
int g_axeBackCJoint = -1;
uint32_t g_axeBackGood = 0;
std::atomic<uint32_t> g_abLearn{0}, g_abReject{0}, g_abPlaced{0}, g_abNotSettled{0}, g_abNoJoint{0}, g_abOnDrawn{0},
    g_abOnGame{0}, g_abElse{0};
static bool AxeBackJoint(uint8_t* skel, const float* root, float* out, bool place) {
    uint8_t* obj = g_attached[0].obj.load();
    uint8_t* hero = g_heroSkel.load();
    if (!obj || !hero || IsBadReadPtr(obj, 0x150) || *(uint8_t**)(obj + 0x238) != skel) return false;
    const Mat4& R = *(const Mat4*)root;
    const float sc = sqrtf(R.m[0][0] * R.m[0][0] + R.m[0][1] * R.m[0][1] + R.m[0][2] * R.m[0][2]);
    const int type = *(const int*)(obj + 0x148);
    const int j = *(const int16_t*)(obj + 0x14c);
    uint8_t* parent = *(uint8_t**)(obj + 0x140);
    const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
    const bool onHero = type == 2 && j >= 0 && j < hn && parent && !IsBadReadPtr(parent, 0x240) &&
                        *(uint8_t**)(parent + 0x238) == hero;
    auto near2 = [](const Mat4& a, const Mat4& b, float d) {
        const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
        const float la = sqrtf(a.m[0][0] * a.m[0][0] + a.m[0][1] * a.m[0][1] + a.m[0][2] * a.m[0][2]);
        const float lb = sqrtf(b.m[0][0] * b.m[0][0] + b.m[0][1] * b.m[0][1] + b.m[0][2] * b.m[0][2]);
        return x * x + y * y + z * z < d * d && la > 1e-3f && lb > 1e-3f &&
               (a.m[0][0] * b.m[0][0] + a.m[0][1] * b.m[0][1] + a.m[0][2] * b.m[0][2]) / (la * lb) > 0.995f;
    };
    if (onHero && sc > 0.5f && sc <= 0.97f) {  // a sheathed-size pass parented to Kratos: C = root x the parent world used
        Mat4 P1, P2;
        memcpy(&P1, obj + 0xf0, sizeof(Mat4));
        memcpy(&P2, obj + 0xf0, sizeof(Mat4));
        if (!memcmp(&P1, &P2, sizeof(Mat4)) && fabsf(P1.m[3][0]) + fabsf(P1.m[3][1]) + fabsf(P1.m[3][2]) >= 1.f &&
            fabsf(P1.m[3][3] - 1.f) < 1e-3f) {
            const Mat4 C = Mul(R, AffineInverse(P1));
            const float cl = sqrtf(C.m[3][0] * C.m[3][0] + C.m[3][1] * C.m[3][1] + C.m[3][2] * C.m[3][2]);
            if (cl < 1.f) {  // (an axe hanging on a joint is within a metre of it)
                AcquireSRWLockExclusive(&g_axeBackLock);
                if (g_axeBackCJoint != j) {
                    const uint64_t* hh = *(const uint64_t**)(hero + 0x30);
                    Log("fp: sheathed axe parented to Kratos joint %d (hash %016llx), offset (%.3f %.3f %.3f) scale %.2f "
                        "(was joint %d)", j, hh ? (unsigned long long)hh[j] : 0ull, C.m[3][0], C.m[3][1], C.m[3][2], sc,
                        g_axeBackCJoint);
                    g_axeBackCJoint = j;
                    g_axeBackC = C;
                    g_axeBackGood = 0;
                } else if (near2(C, g_axeBackC, 0.01f)) {
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 3; ++c) g_axeBackC.m[r][c] += (C.m[r][c] - g_axeBackC.m[r][c]) * 0.05f;
                    OrthonormalizeKeepScale(g_axeBackC);
                    if (++g_axeBackGood == 30)
                        Log("fp: sheathed axe offset to joint %d settled (%.3f %.3f %.3f)", j, g_axeBackC.m[3][0],
                            g_axeBackC.m[3][1], g_axeBackC.m[3][2]);
                    ++g_abLearn;
                } else {
                    ++g_abReject;  // a pass built from another parent matrix than the cached one (race) - not learned
                    if (g_axeBackGood < 30) { g_axeBackC = C; g_axeBackGood = 0; }
                }
                ReleaseSRWLockExclusive(&g_axeBackLock);
                g_axeBackJoint = j;
            }
        }
    }
    if (!place) return false;
    Mat4 C;
    int cj;
    uint32_t good;
    AcquireSRWLockShared(&g_axeBackLock);
    C = g_axeBackC; cj = g_axeBackCJoint; good = g_axeBackGood;
    ReleaseSRWLockShared(&g_axeBackLock);
    if (cj < 0 || good < 30) { ++g_abNotSettled; return false; }
    const uint64_t* hh = *(const uint64_t**)(hero + 0x30);
    Mat4 J;
    if (cj >= hn || !hh || IsBadReadPtr(hh + cj, 8) || !StableJoint(hh[cj], &J)) { ++g_abNoJoint; return false; }
    const Mat4 W = Mul(C, J);
    // evidence: where the game had this pass - on the drawn joint already, on the game's body, or elsewhere (stray)
    Mat4 Jg;
    if (near2(R, W, 0.03f)) ++g_abOnDrawn;
    else if (LiveGameJoint(cj, &Jg) && near2(R, Mul(C, Jg), 0.03f)) ++g_abOnGame;
    else ++g_abElse;
    memcpy(out, &W, sizeof(W));
    ++g_abPlaced;
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 10000 && tLog.compare_exchange_strong(t0, now))
        Log("fp: sheathed axe on drawn joint %d, 10 s: %u passes placed (game had %u on the drawn joint, %u on its own "
            "body, %u elsewhere), offset learned %u / rejected %u, not settled %u, joint unpublished %u",
            cj, g_abPlaced.exchange(0), g_abOnDrawn.exchange(0), g_abOnGame.exchange(0), g_abElse.exchange(0),
            g_abLearn.exchange(0), g_abReject.exchange(0), g_abNotSettled.exchange(0), g_abNoJoint.exchange(0));
    return true;
}

void FixAttached(uint8_t* skel, Attached& at, const float* root, uintptr_t caller) {
    // Where the game put the object this pass, and the ANIMATED (pre-IK) joint it attaches to. The in-hand test and
    // the grip are measured against that joint: measured against our IK hand the axe was "away" whenever the animated
    // hand was elsewhere and stayed floating near the wrist (owner report).
    Mat4 anim;
    if (!StableJoint(at.anchorHash, &anim)) return;
    int16_t n = *(int16_t*)(skel + 0x38);
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    // the object's root joint (world) = the root param; D-130: the game's placement when the back glue moved it
    const Mat4 R = at.hand == 1 && t_axeGlued ? t_axeGameRoot : *(const Mat4*)(mats + (size_t)(uint16_t)n * 0x40);
    // The engine copies the root param into world[0] (sub_1405908a0). Rebuild passes feed the object's own cached
    // world back in, which holds OUR previous placement: only a root from elsewhere is the game's placement.
    const bool fresh = !((const uint8_t*)root >= mats && (const uint8_t*)root < mats + (size_t)(uint16_t)n * 0x80);
    if (fabsf(R.m[3][0]) + fabsf(R.m[3][1]) + fabsf(R.m[3][2]) < 1.f) return;  // a model-space pass: not placed yet
    float dx = R.m[3][0] - anim.m[3][0], dy = R.m[3][1] - anim.m[3][1], dz = R.m[3][2] - anim.m[3][2];
    const float dAnim = sqrtf(dx * dx + dy * dy + dz * dz);
    Mat4 wrist, elbow;
    const bool ik = ArmsIkResult(at.hand, &wrist, &elbow);
    const Mat4 A = at.elbow ? elbow : wrist;
    // In hand = near the animated hand OR the motion-controlled one: after a recall the game catches the axe at the hand
    // it reads (our IK hand), which can be far from the animated one; tested against the animated hand only, the axe
    // counted as "away", was never re-anchored and floated until F1 was pressed again (owner report).
    float dHand = dAnim, dHandIk = -1.f;
    if (ik) {
        const float ex = R.m[3][0] - A.m[3][0], ey = R.m[3][1] - A.m[3][1], ez = R.m[3][2] - A.m[3][2];
        dHandIk = sqrtf(ex * ex + ey * ey + ez * ez);
        dHand = fminf(dAnim, dHandIk);
    }
    const float thr = at.elbow ? 0.45f : 0.35f;
    // The axe (D-025, LIVE axediag with the harness: sheathe / draw / aimed throw / recall):
    //   drawn:     scale 1.00, root 0.09 m from the animated wrist anchor
    //   sheathed:  scale 0.85 (the game shrinks the axe on the back - the owner's "smaller axe"), 0.9 m away
    //   thrown:    scale 1.00, 7 m .. 45 m away within a second; recalled: back to 0.2 .. 0.09 m
    //   JOMeleeWeapon2 stays in the hand in every state (0.67 m in front of the chest): useless as a signal.
    // In hand = full size and on a hand (animated or ours; the game may catch at either). Scale rules out the sheathed
    // axe even when the player's hand reaches over the shoulder (the old back flicker).
    if (at.hand == 1) {
        const float sc = sqrtf(R.m[0][0] * R.m[0][0] + R.m[0][1] * R.m[0][1] + R.m[0][2] * R.m[0][2]);
        static bool held = false;
        static int farPasses = 0;
        static bool putAway = false;  // D-128: the last release from the hand was the game shrinking it (put away)
        static int inStreak = 0;  // D-129: fresh passes in a row at the hand, full size
        if (fresh) {
            if (!held) {
                // D-129 (owner, D-128 build: "the axe doesn't stay on my back, it shows misaligned on my right hip"; owner
                // log: put away, then "in hand (scale 1.00, 0.08 m)" / "not in hand (scale 0.85, on the back)" every
                // ~18 ms): with the axe on the back one pass per frame still arrives full size AT the hand (the hanging
                // hand = the right hip), the next sheathed - each flipped the state. After a put-away the axe is back in
                // the hand only after 8 such passes in a row (a real draw: every pass is in the hand)
                const bool atHand = sc > 0.95f && dHand < 0.30f;
                inStreak = atHand ? inStreak + 1 : 0;
                held = atHand && (!putAway || inStreak >= 8);
                if (held) { putAway = false; inStreak = 0; }
                farPasses = 0;
            } else if (sc <= 0.95f) {
                held = false;  // put away: the game starts shrinking it at once
                putAway = true;
            } else if (dHand > 0.45f) {  // LIVE: the catch animation jumps 0.77 m for one pass; a throw is 1.7 m at once
                // D-039: single passes also arrive 0.5-1.1 m away while standing / walking (owner log: "axe in hand" /
                // "not in hand (0.74 m)" every 30-50 ms -> fist and axe placement flickering). Released only by a
                // throw-sized jump (> 1.4 m) or 10 far passes in a row.
                // D-059: but right after a throw the axe must leave at once: pinning it to the hand for up to 10 passes
                // while the game already flies it, then snapping > 1 m, stretched its glow / wind / flame trails
                // (owner). A throw = throw gesture within 1.5 s, or the aim trigger held.
                const ULONGLONG ta = g_throwAt.load();
                // D-128 (owner: "holding the axe in L2 mode it misaligns, aligns into my hand and then a quarter metre
                // backward"; owner log: aiming, "axe in hand (0.10 m)" / "not in hand (0.5 m)" every ~20 ms): with the aim
                // trigger held ONE stray 0.5 m pass dropped the axe, the next caught it again - and since D-120 each
                // dropped frame was glued toward the back. Aim held, only a throw-sized jump (> 1 m) releases it
                // D-213 (owner, D-211 build: "the axe flickers worse during the throw and after the recall"; owner logs:
                // "throw released 0 ms after the throw button" at 0.45 / 0.48 / 0.52 m, back "in hand" ~100 ms later, then
                // the real throw; an earlier log: 1.04 .. 1.23 m passes against 0.10 m ones for 760 ms): during the throw
                // boost the game evaluates the axe twice per frame and the second pass runs 0.4 .. 1.2 m ahead of the hand.
                // Any pass past 0.45 m dropped the axe, the next caught it. A real throw jumps 1.6 .. 7.4 m at once (owner
                // logs): only a pass past 1.4 m releases it (D-039's throw-sized jump); D-211's catch streak is removed
                const bool throwing = g_fastRelease.load() && dHand > 1.4f &&
                                      ((ta && GetTickCount64() - ta < 1500) || AimTriggerHeld());
                // D-088 (owner: "when attacks end and I walk forward the axe is not in my hand for a few ms, glitching";
                // owner log: "axe not in hand (0.48 m)" 22 ms after an attack's release, back 400 ms later): around a held
                // attack the game body and the drawn body are apart (the hold, its release), so the game's axe passes
                // land far from the hand for a while - 10 such passes dropped it. During a hold and its fade-out
                // (CombatHoldWeight > 0) only a throw takes the axe out of the hand
                held = throwing ? false : (CombatHoldWeight() > 0.001f || (++farPasses < 10 && dHand < 1.4f));
                if (!held) putAway = false;
            } else {
                farPasses = 0;
            }
        }
        if (fresh) g_axeThrown = !held && sc > 0.95f && dHand > 1.0f;
        static int lastHeld = -1;
        if ((int)held != lastHeld) {
            if (lastHeld == 1 && !held && sc > 0.95f && g_throwAt.load() && !g_throwReleasedAt.load()) {
                g_throwReleasedAt = GetTickCount64();
                Log("fp: throw released %llu ms after the throw button", (unsigned long long)(g_throwReleasedAt.load() - g_throwAt.load()));
            }
            // D-121: left the hand by a throw = not on the back. D-128: on the back only when it left by a PUT AWAY
            // (the game shrinking it) - any other drop (throw, aim, a stray far pass) is not the sheath
            if (held) g_axeOnBack = false;
            else if (lastHeld == 1) {
                const ULONGLONG ta = g_throwAt.load();
                g_axeOnBack = putAway && !((ta && GetTickCount64() - ta < 3000) || AimTriggerHeld());
            }
            if (!held && lastHeld == 1) {  // D-150: out of the hand - a grip from a special move is forgotten
                AcquireSRWLockExclusive(&at.lock);
                if (at.tempGrip) {
                    at.tempGrip = false;
                    if (at.haveBase) {
                        at.offset = at.baseOffset;
                        Log("fp: axe grip back to normal (offset %.3f %.3f %.3f)", at.offset.m[3][0], at.offset.m[3][1],
                            at.offset.m[3][2]);
                    }
                }
                ReleaseSRWLockExclusive(&at.lock);
            }
            lastHeld = held;
            Log("fp: axe %s (scale %.2f, %.2f m from the hand%s)", held ? "in hand" : "not in hand", sc, dHand,
                held ? "" : g_axeOnBack.load() ? ", on the back" : ", thrown");
        }
        if (g_axeDiag > 0) {
            --g_axeDiag;
            Log("axediag: t%u %s caller +0x%llx R(%.2f %.2f %.2f) scale %.3f |R-anim| %.2f |R-hand| %.2f held %d ik %d",
                GetCurrentThreadId(), fresh ? "fresh" : "SELF ", (unsigned long long)(caller - (uintptr_t)Base()),
                R.m[3][0], R.m[3][1], R.m[3][2], sc, dAnim, dHand, (int)held, (int)ik);
        }
        dHand = held ? 0.f : 1.f;  // decided: skip the generic hysteresis below
    }
    // D-039: the guardian shield never leaves the left forearm, but while walking ~1 in 4 of its passes arrive ~1 m away
    // from the forearm (LIVE attjitter: 0.9-1.03 m from the animated elbow); those were skipped as "not on the arm" and
    // showed the shield a metre off (owner: "the shield part on left hand is jittering"). The shield is always
    // re-anchored; its grip is only learned from passes that are really on the arm.
    const bool learnable = dAnim <= thr;
    if (at.hand == 0) dHand = 0.f;
    // hysteresis: a few far passes (fast attack frames) do not drop the object; ~20 in a row or a clearly far one do
    if (dHand > thr) {
        ++at.detached;
        if (++at.awayStreak > 20 || dHand > 0.9f) {  // not on this arm (axe on the back / thrown)
            if (at.hand == 1) g_axeInHand = false;
            if (at.hand == 1 && fresh) { const float z[3] = {0, 0, 0}; AxeJumpSample(z, false); }
            AttJitSample(at, skel, false, fresh, dAnim, dHandIk);
            return;
        }
    } else {
        at.awayStreak = 0;
    }
    if (at.hand == 1) g_axeInHand = true;
    AcquireSRWLockExclusive(&at.lock);
    uint64_t f = g_frameNo.load();
    if (f != at.anchorFrame) { at.prevAnchor = at.curAnchor; at.curAnchor = anim; at.anchorFrame = f; }
    const bool still = Still(at.curAnchor, at.prevAnchor);
    if (!ik && still && learnable) {  // arm authored: the game's own placement is the truth -> learn (and save) the grip
        // D-150 (owner: "a runic attack makes Kratos hold the axe higher and he remembers that pose till the end of the
        // game - forget it once the axe is unequipped"): in F1 the arm is motion controlled except during full-animation
        // moves (runic attacks, ANIMATE), so the only grip samples there came from those moves - a different grip, kept
        // for the rest of the session. A grip seen during an attack / full-animation move is now temporary
        // D-212 (owner: "the axe hold gets bad after csRageBeatdownTroll00Finisher"): since D-210 the troll finisher plays
        // as a scripted scene (F3, arm not motion controlled, combat state "none") - its hand pose was taken as the
        // NORMAL grip, silently. Cutscenes, scripted scenes and sync moves are special too
        const bool special = at.hand == 1 && (CombatBusy() || CombatArmsFromAnimation() || SceneAnimating() ||
                                              HeroInSyncMove() || ContextHandAuthored(1));
        at.offset = Mul(R, RigidInverse(anim));
        at.haveOffset = true;
        if (special) {
            if (!at.tempGrip)
                Log("fp: %s held differently during a move (offset %.3f %.3f %.3f) - kept until it leaves the hand", at.name,
                    at.offset.m[3][0], at.offset.m[3][1], at.offset.m[3][2]);
            at.tempGrip = true;
        } else {
            at.baseOffset = at.offset;
            at.haveBase = true;
            at.tempGrip = false;
            if (!at.calibrated) at.dirty = true;
            at.calibrated = true;
        }
    } else if (ik && !at.calibrated && learnable && (still || !at.haveOffset)) {  // provisional until an authored sample
        at.offset = Mul(R, RigidInverse(anim));
        at.haveOffset = true;
    }
    // D-208 (owner: "right after opening a chest the axe positioning got bad - it remembers the hold from the chest; this
    // happens quite a lot with other animations"): D-150 kept a grip seen during a full-animation move until the axe
    // LEFT the hand - with the axe kept in hand through the move (a chest, a lever, a runic attack) the animation's grip
    // stayed afterwards. Now it is dropped as soon as the arm is motion controlled again and no such move plays
    if (at.hand == 1 && at.tempGrip && ik && !(CombatBusy() || CombatArmsFromAnimation())) {
        at.tempGrip = false;
        if (at.haveBase) {
            at.offset = at.baseOffset;
            Log("fp: axe grip from the move dropped - back to the normal grip (offset %.3f %.3f %.3f) (D-208)",
                at.offset.m[3][0], at.offset.m[3][1], at.offset.m[3][2]);
        }
    }
    if (ik) {
        float ex = R.m[3][0] - A.m[3][0], ey = R.m[3][1] - A.m[3][1], ez = R.m[3][2] - A.m[3][2];
        at.sumDAnim += dAnim; at.sumDIk += sqrtf(ex * ex + ey * ey + ez * ez); ++at.nD;
    }
    // D-213 (owner: "after a catch / pickup the axe doesn't follow the hand"; owner log: "goaxe00 held differently during a
    // move (offset 0.262 ...)" right after a recall catch - a grip with the axe 26 cm off the hand): a grip learned while
    // the arm plays an animation only describes THAT animation's hold. On a motion-controlled hand the axe always takes
    // the normal grip (D-150 / D-208 / D-212 keep learning, but never place with a temporary grip)
    const Mat4 O = at.hand == 1 && at.haveBase ? at.baseOffset : at.offset;
    ReleaseSRWLockExclusive(&at.lock);
    if (!ik) return;  // authored arm: leave the game's placement alone
    // D-088 (owner: "move the axe in the hand a tiny bit backwards, it overlaps one of the fingers"): an offset in the IK
    // hand's frame (x outward, y along the fingers, z up - as blades_grip_x/y/z): axe_grip_y -0.015 m = 1.5 cm back
    // toward the wrist; gowvr.ini axe_grip_x / _y / _z
    Mat4 placed = Mul(O, A);
    if (at.hand == 1) {
        static const float gx = config::GetFloat("axe_grip_x", 0.f), gy = config::GetFloat("axe_grip_y", -0.015f),
                           gz = config::GetFloat("axe_grip_z", 0.f);
        for (int c = 0; c < 3; ++c) {
            float r0 = A.m[0][c], r1 = A.m[1][c], r2 = A.m[2][c];
            const float l0 = sqrtf(A.m[0][0] * A.m[0][0] + A.m[0][1] * A.m[0][1] + A.m[0][2] * A.m[0][2]);
            const float l1 = sqrtf(A.m[1][0] * A.m[1][0] + A.m[1][1] * A.m[1][1] + A.m[1][2] * A.m[1][2]);
            const float l2 = sqrtf(A.m[2][0] * A.m[2][0] + A.m[2][1] * A.m[2][1] + A.m[2][2] * A.m[2][2]);
            if (l0 > 1e-6f) r0 /= l0;
            if (l1 > 1e-6f) r1 /= l1;
            if (l2 > 1e-6f) r2 /= l2;
            placed.m[3][c] += gx * r0 + gy * r1 + gz * r2;
        }
        // D-213 (owner: "picking up items is bad - the axe starts flying and doesn't follow the hand"): around a pickup /
        // chest / ANIMATE move the drawn arm blends between the animation and the controller (arm_blend_ms, arms.cpp:
        // joint = anim + (IK - anim) * weight), but the axe went to the full controller hand at once - it flew ahead of
        // the hand for the whole blend. The axe now blends the same way: game's (animated-hand) placement -> our grip
        const float w = ArmsIkWeight();
        if (w < 0.999f)
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c) placed.m[r][c] = R.m[r][c] + (placed.m[r][c] - R.m[r][c]) * w;
    }
    // D-090: the game's axe placement can be sheared (built on a blended hand). D-131: the move is applied to the joints
    // AS DRAWN - on the pass where a draw / recall flips the axe to "in hand", the back glue (D-130 force 2) has already
    // put them on the back spot, and a move computed from the game's root (R) landed the axe ~1 m off for that frame
    const Mat4 Rdrawn = *(const Mat4*)(mats + (size_t)(uint16_t)n * 0x40);
    const Mat4 D = Mul(AffineInverse(at.hand == 1 && t_axeGlued ? Rdrawn : R), placed);
    if (at.hand == 1 && fresh) {  // effects on the axe follow (D-036) - from the GAME's axe (D-095: without the ghost shift)
        Mat4 Rgame = R;
        Rgame.m[3][0] -= t_ghostShift[0];
        Rgame.m[3][2] -= t_ghostShift[1];
        RecordAxeCorrection(Rgame, placed);
    }
    if (at.hand == 1 && fresh) {
        const Mat4 P = placed;
        const float off[3] = {P.m[3][0] - R.m[3][0], P.m[3][1] - R.m[3][1], P.m[3][2] - R.m[3][2]};
        AxeJumpSample(off, true);
    }
    if (g_axeTrace > 0 && at.hand == 1) {
        --g_axeTrace;
        Log("axetrace: AXE pass t%u root(%.2f %.2f %.2f) axe0(%.3f %.3f %.3f) ikWrist(%.3f %.3f %.3f) move %.3f m frame %llu",
            GetCurrentThreadId(), root ? root[12] : 0.f, root ? root[13] : 0.f, root ? root[14] : 0.f, R.m[3][0], R.m[3][1],
            R.m[3][2], A.m[3][0], A.m[3][1], A.m[3][2], sqrtf(D.m[3][0] * D.m[3][0] + D.m[3][1] * D.m[3][1] + D.m[3][2] * D.m[3][2]),
            (unsigned long long)g_frameNo.load());
    }
    for (int j = 0; j < n; ++j) {
        Mat4* w = (Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40);
        *w = Mul(*w, D);
    }
    ++at.fixes;
    AttJitSample(at, skel, true, fresh);
}

constexpr HookSite kLocalToWorld{"LocalToWorld", 0x1405908a0, 6,
    {0x48, 0x8b, 0xc4, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x81, 0xec, 0xd8, 0x00, 0x00}};
using PFN_LocalToWorld = void(__fastcall*)(uint8_t* skel, uint8_t* local, float* root, char flag);
PFN_LocalToWorld o_LocalToWorld = nullptr;

// D-057: sub_140591550(skel, out, joint) = one joint's world built from the animation's LOCALS. Effects attached to a
// joint (fx sub_14058b400 / sub_14058c560) use it whenever the skeleton's world cache is not current for the frame, so
// the fire on the Blades / hands was emitted at the game's ANIMATED hands (owner: "the fire goes all over the place,
// it should stay at the weapon"; harness: flames in mid-air while the Blades are on our hands). For the hero's arm /
// weapon joints and the Blades' objects the answer is the world cache (our edited pose, at most one pass old).
constexpr HookSite kWorldFromLocals{"WorldFromLocals", 0x140591550, 8,
    {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x73, 0x20, 0x57, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x41}};
using PFN_WorldFromLocals = uint64_t(__fastcall*)(uint8_t* skel, float* out, int joint);
PFN_WorldFromLocals o_WorldFromLocals = nullptr;
// D-057: sub_14058b400(fxInstance, outMatrix, flag) = where an effect attached to an object's joint is (object at
// +0xa0, joint at +0xac, STATIC fx_053). Measured: distance of that answer from the joint's drawn (world cache) pose,
// for effects on the hero and on the Blades' objects; "fxfix" forces the drawn pose.
constexpr HookSite kFxAttach{"FxAttach", 0x14058b400, 5,
    {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xec, 0xb0, 0x00}};
using PFN_FxAttach = void(__fastcall*)(uint8_t* fx, float* out, char flag);
PFN_FxAttach o_FxAttach = nullptr;
// D-057: the Blades drawn DURING attacks are 3-joint copies (hash0 65301791f418f62d; LIVE "flymove 3": the flaming
// Blades and their fire rose 3 m with them) - the fire the owner sees "all over the place" is on them. They were moved
// with the nearest animated WRIST (D-048); a thrown Blade is ~0.8 m from both wrists (flydiag), so the wrong hand's
// correction was often used. Now each copy is paired with the hero weapon point (225-228) it sits on in the ANIMATED
// pose and gets exactly that point's correction animated -> drawn.
// the hero joint j as the GAME has it: the game's unturned root x the live (not held) animation locals
static bool LiveGameJoint(int j, Mat4* out) {
    uint8_t* hs = g_heroSkel.load();
    if (!hs || !g_gameRootYawValid.load()) return false;
    const int n = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    if (j < 0 || j >= n) return false;
    const uint8_t* locs = *(const uint8_t**)(hs + 0x58);
    int chain[64], c = 0;
    for (int k = j; k > 0 && c < 64; k = JointParent(k)) chain[c++] = k;
    AcquireSRWLockShared(&g_rootLock);
    Mat4 M = g_gameRootM;
    ReleaseSRWLockShared(&g_rootLock);
    for (int i = c - 1; i >= 0; --i) M = Mul(*(const Mat4*)(locs + (size_t)chain[i] * 0x40), M);
    *out = M;
    return true;
}
std::atomic<bool> g_copyFix{false};  // experimental (fire), off until verified
std::atomic<uint64_t> g_copyN{0}, g_copyFar{0};
struct BladeCorr { float pos[3] = {}; Mat4 D{}; Mat4 W{}; uint64_t frame = 0; };  // game root -> our root (W drawn), per Blade object
static BladeCorr g_bladeCorr[4];
static SRWLOCK g_bladeCorrLock = SRWLOCK_INIT;
std::atomic<float> g_copyMaxAnim{0.f};
// D-074 (owner: "the flame effect on the Blades misaligns when attacking"; LIVE flydiag: the attack flame copies sit
// 0.75 m out on the chain at the game's attack pose while the drawn Blades are elsewhere): each flame copy takes the
// correction of the Blade object nearest to it in the GAME's placement (<= 2 frames old, within 1 m) - so it sits on
// the drawn Blade. "flameblade on|off".
std::atomic<bool> g_flameBlade{true};
void FlameBlade(bool on) { g_flameBlade = on; Log("fp: Blades attack flames on the drawn Blades %s", on ? "ON" : "OFF"); }
extern std::atomic<uint64_t> g_frameNo;
static bool FlameOnBlade(const float* root, float* out) {
    if (!g_flameBlade.load()) return false;
    const uint64_t f = g_frameNo.load();
    // LIVE: the flames are the game's thrown-out Blades, ~0.76 m from the game's Blade object (which stays at the hand
    // pose) - so the flame is put ON the drawn Blade (the Blade object's drawn root), not corrected alongside it
    float best = 1.3f * 1.3f;
    Mat4 W;
    bool have = false;
    AcquireSRWLockShared(&g_bladeCorrLock);
    for (const auto& c : g_bladeCorr) {
        if (!c.frame || f - c.frame > 2) continue;
        const float dx = root[12] - c.pos[0], dy = root[13] - c.pos[1], dz = root[14] - c.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) { best = d2; W = c.W; have = true; }
    }
    ReleaseSRWLockShared(&g_bladeCorrLock);
    if (!have) return false;
    memcpy(out, &W, sizeof(W));
    return true;
}
void CopyFix(bool on) { g_copyFix = on; Log("fp: attack Blades copies follow their weapon point %s", on ? "ON" : "OFF"); }
void CopyStats() {
    Log("copystats: copy passes %llu, nearest animated weapon point > 0.3 m away %llu, max %.2f m",
        (unsigned long long)g_copyN.exchange(0), (unsigned long long)g_copyFar.exchange(0), g_copyMaxAnim.exchange(0.f));
}
std::atomic<int> g_copyScan{0};
void CopyScan(int n) { g_copyScan = n; Log("copyscan: next %d copy passes", n); }
// world of a copy's joint k from its root param and its locals (generic parent links: int16 in the top of each link)
static Mat4 CopyJointWorld(uint8_t* skel, const float* root, int k) {
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    const uint8_t* mats = *(const uint8_t**)(skel + 0x58);
    const uint8_t* links = *(const uint8_t**)(skel + 0x20);
    int chain[8], c = 0;
    for (int j = k; j > 0 && c < 8; j = (int)(int16_t)(*(const uint64_t*)(links + (size_t)j * 8) >> 48)) chain[c++] = j;
    Mat4 M = *(const Mat4*)root;
    for (int i = c - 1; i >= 0; --i) M = Mul(*(const Mat4*)(mats + (size_t)chain[i] * 0x40), M);
    (void)n;
    return M;
}
struct FireSeen { uint8_t* skel = nullptr; uint64_t frame = 0; };
static FireSeen g_fireSeen[8];
static SRWLOCK g_fireSeenLock = SRWLOCK_INIT;
static bool BladeCopyFix(uint8_t* cskel, const float* root, float* out) {
    {
        AcquireSRWLockExclusive(&g_fireSeenLock);
        int slot = -1, old = 0;
        for (int i = 0; i < 8; ++i) {
            if (g_fireSeen[i].skel == cskel) { slot = i; break; }
            if (g_fireSeen[i].frame < g_fireSeen[old].frame) old = i;
        }
        if (slot < 0) slot = old;
        g_fireSeen[slot] = {cskel, g_frameNo.load()};
        ReleaseSRWLockExclusive(&g_fireSeenLock);
    }
    if (g_copyScan.load() > 0 && fabsf(root[13]) < 1e5f) {  // which hero joint (animated / drawn) is each copy on?
        uint8_t* hs = g_heroSkel.load();
        const int cn = (int)(uint16_t)*(int16_t*)(cskel + 0x38);
        if (hs && cn >= 2) {
            --g_copyScan;
            const Mat4 blade = CopyJointWorld(cskel, root, cn - 1);  // the copy's last joint = the Blade
            root = blade.m[0];
            const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
            const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
            int ja = -1, jd = -1;
            float da = 1e9f, dd = 1e9f;
            for (int j = 0; j < hn; ++j) {
                Mat4 A;
                if (LiveGameJoint(j, &A)) {  // the game's frame + live animation
                    const float x = root[12] - A.m[3][0], y = root[13] - A.m[3][1], z = root[14] - A.m[3][2];
                    const float d2 = x * x + y * y + z * z;
                    if (d2 < da) { da = d2; ja = j; }
                }
                const float* c = (const float*)(hm + ((size_t)hn + j) * 0x40 + 0x30);
                const float x = root[12] - c[0], y = root[13] - c[1], z = root[14] - c[2];
                const float d2 = x * x + y * y + z * z;
                if (d2 < dd) { dd = d2; jd = j; }
            }
            float g[4];
            AcquireSRWLockShared(&g_rootLock);
            memcpy(g, g_gameRoot, sizeof(g));
            ReleaseSRWLockShared(&g_rootLock);
            float lg[3];
            BodyLocal(root + 12, g, lg);
            Log("copyscan: BLADE joint %d: nearest ANIMATED joint %d at %.3f m | nearest DRAWN joint %d at %.3f m | game-frame "
                "(%.2f %.2f %.2f) hold %.2f", cn - 1, ja, sqrtf(da), jd, sqrtf(dd), lg[0], lg[1], lg[2], CombatHoldWeight());
        }
        return false;
    }
    if (!g_copyFix.load()) return false;
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return false;
    const int cn = (int)(uint16_t)*(int16_t*)(cskel + 0x38);
    if (cn < 2) return false;
    const Mat4 cb = CopyJointWorld(cskel, root, cn - 1);  // the fire's emitter joint, where the game put it
    {   // D-057 (LIVE "flymove 3": only the flames rose, the Blades stayed): these 3-joint objects are the FIRE on the
        // Blades -> the correction of the Blade object they burn on (nearest game placement, recent frames)
        const uint64_t f = g_frameNo.load();
        int best = -1;
        float bd = 1.2f * 1.2f;
        Mat4 D{};
        AcquireSRWLockShared(&g_bladeCorrLock);
        for (int k = 0; k < 4; ++k) {
            const BladeCorr& c = g_bladeCorr[k];
            if (!c.frame || f - c.frame > 3) continue;
            const float dx = cb.m[3][0] - c.pos[0], dy = cb.m[3][1] - c.pos[1], dz = cb.m[3][2] - c.pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bd) { bd = d2; best = k; D = c.D; }
        }
        ReleaseSRWLockShared(&g_bladeCorrLock);
        ++g_copyN;
        if (best >= 0) {
            const Mat4 W = Mul(*(const Mat4*)root, D);
            memcpy(out, &W, sizeof(W));
            return true;
        }
        ++g_copyFar;
        return false;
    }
    static const uint64_t ph[4] = {0x08eae59373302b22ull, 0xb41408d8ad411e00ull, 0xb41408d8ad410271ull, 0x6fc442db7837895aull};
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    int bj = -1;
    float bd = 1e9f;
    Mat4 bA{};
    for (uint64_t h : ph) {
        const int j = FindJointByHash(h);
        Mat4 A;
        if (j < 0 || j >= hn || !LiveGameJoint(j, &A)) continue;
        const float dx = cb.m[3][0] - A.m[3][0], dy = cb.m[3][1] - A.m[3][1], dz = cb.m[3][2] - A.m[3][2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd) { bd = d2; bj = j; bA = A; }
    }
    if (bj < 0) return false;
    const float d = sqrtf(bd);
    ++g_copyN;
    if (d > 0.3f) ++g_copyFar;
    float m = g_copyMaxAnim.load();
    while (d > m && !g_copyMaxAnim.compare_exchange_weak(m, d)) {}
    if (d > 1.5f) return false;
    const Mat4& drawn = *(const Mat4*)(hm + ((size_t)hn + bj) * 0x40);
    const Mat4 W = Mul(*(const Mat4*)root, Mul(InverseUniform(bA), drawn));
    memcpy(out, &W, sizeof(W));
    return true;
}
std::atomic<float> g_flyMove{0.f};
void FlyMove(float m) { g_flyMove = m; Log("flymove: 3-joint attack skeletons moved up %.1f m", m); }
std::atomic<bool> g_fxFix{true};
std::atomic<uint64_t> g_fxN{0}, g_fxFar{0}, g_fxFixed{0};
std::atomic<float> g_fxMax{0.f};
void FxFix(bool on) { g_fxFix = on; Log("fp: effects on the hero / Blades forced onto the drawn pose %s", on ? "ON" : "OFF"); }
void FxStats() {
    Log("fxstats: effect attachments on hero/Blades %llu, > 5 cm from the drawn joint %llu (max %.2f m), forced %llu",
        (unsigned long long)g_fxN.exchange(0), (unsigned long long)g_fxFar.exchange(0), g_fxMax.exchange(0.f),
        (unsigned long long)g_fxFixed.exchange(0));
}
// D-067 (owner: effects on the weapons and the Blades' chains sit a few cm behind while walking - forward -> they go
// back): effect attachments are recorded here (position, frame) and compared at the frame's draw (FxLagCheck, from
// WaitHeroFrame) with where their joint is drawn; "fxlead <k>" adds k x the hero's per-frame movement (prediction)
struct FxRec { uint8_t* skel; int joint; float p[3]; uint64_t frame; bool used; };
static FxRec g_fxRec[64];
static std::atomic<uint32_t> g_fxRecN{0};
std::atomic<float> g_fxLead{-1.f};
static float FxLead() { const float v = g_fxLead.load(); return v >= 0.f ? v : config::GetFloat("fx_lead", 0.f); }
void FxLeadSet(float k) { g_fxLead = k; Log("fp: effects on the hero / Blades / axe led by %.2f x the per-frame movement", k); }
static float g_heroStep[3] = {0.f, 0.f, 0.f};  // the hero's drawn root movement over the last frame (BodyPostAnim)
void __fastcall hk_FxAttach(uint8_t* fx, float* out, char flag) {
    o_FxAttach(fx, out, flag);
    if (!fx || !out || flag || FpMode() != 1) return;
    const short joint = *(short*)(fx + 0xac);
    uint8_t* obj = *(uint8_t**)(fx + 0xa0);
    if (joint < 0 || !obj || IsBadReadPtr(obj + 0x238, 8)) return;
    uint8_t* skel = *(uint8_t**)(obj + 0x238);
    if (!skel || IsBadReadPtr(skel + 0x58, 8)) return;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (joint >= n) return;
    bool mine = skel == g_heroSkel.load() || skel == g_attached[0].skel.load();
    if (!mine && n == 48) {
        const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
        mine = hh && !IsBadReadPtr(hh, 8) && hh[0] == 0xef450b59487cb7afull;
    }
    if (!mine) return;
    const float* c = (const float*)(*(uint8_t**)(skel + 0x58) + ((size_t)n + joint) * 0x40);
    const float dx = out[12] - c[12], dy = out[13] - c[13], dz = out[14] - c[14];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    ++g_fxN;
    if (d > 0.05f) {
        ++g_fxFar;
        float m = g_fxMax.load();
        while (d > m && !g_fxMax.compare_exchange_weak(m, d)) {}
        if (g_fxFix.load()) { memcpy(out, c, 0x40); ++g_fxFixed; }
    }
    // prediction lead (the drawn joint of this frame is one hero step further on) + record for the draw-time check
    const float k = FxLead();
    out[12] += g_heroStep[0] * k;
    out[13] += g_heroStep[1] * k;
    out[14] += g_heroStep[2] * k;
    FxRec& r = g_fxRec[g_fxRecN.fetch_add(1) % 64];
    r.skel = skel; r.joint = joint; r.p[0] = out[12]; r.p[1] = out[13]; r.p[2] = out[14];
    r.frame = g_frameNo.load(); r.used = false;
}
static void HeroChainCheck();
static void BladeObjLagCheck();
// D-073 (owner: "the fire glow on the bottom of the axe misaligns when moving, like the chains"): the axe-gem effect
// joints live on the HERO skeleton, placed by the game (not by the pass: physjoints flags 42/40), a frame behind while
// walking. Standing: each joint's offset to the drawn axe root is learned; moving (axe in hand): placed as offset x the
// drawn axe at the draw. "axegem on|off"; diagnostics in "chaindiag" (lag along the movement before the fix).
std::atomic<bool> g_axeGem{true};  // D-092: on (learned offsets on the drawn axe; the D-075 per-frame mapping was off: wander 0.11 -> 0.19 m)
void AxeGem(bool on) { g_axeGem = on; Log("fp: axe gem glow kept on the drawn axe %s", on ? "ON" : "OFF"); }
extern std::atomic<int> g_chainDiag;
extern float g_heroStepDiag[3];
// D-095: the gem joint indices for the effect-read hook (hk_WorldFromLocals), published per hero
std::atomic<uint8_t*> g_gemFor{nullptr};
int g_gemIdx[16];
int g_gemN = 0;
static void GemProbe(int point, const float* worlds);  // D-176 (below)
static void AxeGemTick() {
    if (FpMode() != 1) return;
    const bool fixOn = g_axeGem.load();
    static const char* kNames[] = {"axegem_dots_after1", "axegem_dots_burst1", "axegem_smoke1", "part_axegem_dots_after1",
                                   "emit_axegem_dots_after1", "part_axegem_dots_burst1", "emit_axegem_dots_burst1",
                                   "emit_axegem_dots_burst11", "part_axegem_smoke1", "emit_axegem_smoke1"};
    constexpr int kN = sizeof(kNames) / sizeof(kNames[0]);
    static int idx[kN];
    static uint8_t* cachedFor = nullptr;
    static Mat4 rel[kN];
    static bool have[kN] = {};
    uint8_t* hs = g_heroSkel.load();
    uint8_t* ax = g_attached[0].skel.load();
    if (!hs || !ax) return;  // (in hand or on the back: whichever correction the axe got)
    if (cachedFor != hs) {
        cachedFor = hs;
        for (int i = 0; i < kN; ++i) { idx[i] = FindJointByHash(JointHash(kNames[i])); have[i] = false; }
        Log("fp: axe gem joints %d %d %d .. %d", idx[0], idx[1], idx[2], idx[kN - 1]);
        g_gemFor.store(nullptr);
        g_gemN = 0;
        for (int i = 0; i < kN && g_gemN < 16; ++i) if (idx[i] >= 0) g_gemIdx[g_gemN++] = idx[i];
        g_gemFor.store(hs, std::memory_order_release);
    }
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    uint8_t* hm = *(uint8_t**)(hs + 0x58);
    // D-096 (owner: the glow at the bottom of the handle "jumps to one spot during attack animations"): D-092 learned
    // each gem joint's offset against the GAME's axe, but the game mostly places these joints from the DRAWN axe already
    // (D-073 LIVE: 1-2 mm off the drawn axe; it copies them from the axe after our correction) - the learned offset
    // carried the game-vs-drawn axe difference of the learning moment, and our forced value alternated with the game's
    // correct one. Now, per joint, at the draw: a value the game wrote ON the drawn axe is left as it is (and its offset
    // on the drawn axe is remembered while the axe is steady); a value it wrote on its OWN axe (the stray frames) is put
    // at the remembered offset on the drawn axe (or mapped game axe -> drawn axe, both read now, before one is known);
    // our own value from an earlier frame moves with the drawn axe. gowvr.ini gem_v2=0: D-092's way.
    static const bool gemV2 = config::GetFloat("gem_v2", 1.f) > 0.5f;
    if (gemV2) {
        static Mat4 lastOut[kN], relD[kN];
        static bool haveOut[kN] = {}, haveRel[kN] = {};
        static Mat4 prevW0;
        static bool havePrevW0 = false;
        static uint64_t onDrawn = 0, moved = 0, kept = 0, parked = 0;
        Mat4 W0;
        GemProbe(3, (const float*)(hm + (size_t)hn * 0x40));  // D-176: at the draw - the game's value or ours?
        if (!fixOn || !AxeDrawn(&W0)) { havePrevW0 = false; return; }
        alignas(16) Mat4 g0;
        o_WorldFromLocals(ax, &g0.m[0][0], 0);
        const bool g0ok = fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) >= 1.f;
        const bool steady = havePrevW0 && fabsf(W0.m[3][0] - prevW0.m[3][0]) + fabsf(W0.m[3][1] - prevW0.m[3][1]) +
                                              fabsf(W0.m[3][2] - prevW0.m[3][2]) < 0.01f;
        prevW0 = W0;
        havePrevW0 = true;
        const Mat4 W0i = AffineInverse(W0);
        for (int i = 0; i < kN; ++i) {
            if (idx[i] < 0 || idx[i] >= hn) continue;
            Mat4* J = (Mat4*)(hm + ((size_t)hn + idx[i]) * 0x40);
            if (haveOut[i] && memcmp(J, &lastOut[i], sizeof(Mat4)) == 0) {  // ours, not rewritten: follow the axe
                if (haveRel[i]) { *J = Mul(relD[i], W0); lastOut[i] = *J; ++kept; }
                continue;
            }
            haveOut[i] = false;  // the game wrote it
            const float dxD = J->m[3][0] - W0.m[3][0], dyD = J->m[3][1] - W0.m[3][1], dzD = J->m[3][2] - W0.m[3][2];
            const float dD = sqrtf(dxD * dxD + dyD * dyD + dzD * dzD);
            float dG = 1e9f;
            if (g0ok) {
                const float dx = J->m[3][0] - g0.m[3][0], dy = J->m[3][1] - g0.m[3][1], dz = J->m[3][2] - g0.m[3][2];
                dG = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            if (dD > 3.f && dG > 3.f) { ++parked; continue; }  // parked far away: not on any axe
            if (dD + 0.05f < dG || dD < 0.02f) {  // on the drawn axe: the game's own value is right
                if (steady) {
                    const Mat4 o = Mul(*J, W0i);
                    if (!haveRel[i]) { relD[i] = o; haveRel[i] = true; }
                    else {
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 3; ++c) relD[i].m[r][c] += (o.m[r][c] - relD[i].m[r][c]) * 0.2f;
                        OrthonormalizeKeepScale(relD[i]);
                    }
                }
                ++onDrawn;
                continue;
            }
            // on the game's axe (a stray frame): onto the drawn axe
            *J = haveRel[i] ? Mul(relD[i], W0) : Mul(Mul(*J, AffineInverse(g0)), W0);
            lastOut[i] = *J;
            haveOut[i] = true;
            ++moved;
        }
        static uint64_t ticks = 0;
        if (++ticks % 2700 == 0)
            Log("fp: axe gem joints at the draw - on the drawn axe %llu, moved from the game's axe %llu, kept on the axe %llu, "
                "parked %llu", (unsigned long long)onDrawn, (unsigned long long)moved, (unsigned long long)kept,
                (unsigned long long)parked);
        return;
    }
    // D-075 (owner: the axe's bottom glow misaligns when moving and "during attacks randomly to near my left shoulder"):
    // the gem joints are placed by the game from ITS axe (in held attacks the animated axe swings elsewhere). Every frame
    // they get the axe's own correction (the latest game axe root R -> our drawn axe Rn, RecordAxeCorrection), always.
    // The game's value of each joint is kept: a joint the game did not rewrite since our write is not corrected twice.
    {
        static Mat4 lastOut[kN];
        static bool haveOut[kN] = {};
        Mat4 R, Rn;
        bool haveCorr = false;
        const uint64_t f = g_frameNo.load();
        AcquireSRWLockShared(&g_axeCorrLock);
        uint64_t best = 0;
        for (const AxeCorr& c : g_axeCorr)
            if (c.frame && f - c.frame <= 2 && c.frame >= best) { best = c.frame; R = c.R; Rn = c.Rn; haveCorr = true; }
        ReleaseSRWLockShared(&g_axeCorrLock);
        if (!haveCorr) return;
        // D-092 (owner: "the fire effect on my axe teleports a few cm when I attack; once it was head-tracked, always at
        // the bottom right of the view - like a HUD thing"): these joints follow the GAME's axe - in held attacks its
        // animated hand on the game body, while aiming its aim pose in front of the game camera (which follows the head
        // in VR: head-locked). D-075 mapped them with this frame's axe correction, but the game places them a frame late
        // (LIVE: wander 0.11 -> 0.19 m), so it stayed off. Now each joint's offset to the game's axe is learned while
        // standing still with the axe in hand (same-frame record, no hold, not aiming) and every frame the joint is drawn
        // at that offset on OUR drawn axe - rigid on the axe whatever the game's axe does
        static Mat4 gameVal[kN], off[kN];
        static bool haveOff[kN] = {};
        const bool sameFrame = best == f;
        const bool learnNow = sameFrame && AxeInHand() && CombatHoldWeight() <= 0.001f && !HeroMoving() && !AimTriggerHeld();
        const Mat4 Ri = InverseUniform(R);
        for (int i = 0; i < kN; ++i) {
            if (idx[i] < 0 || idx[i] >= hn) continue;
            Mat4* J = (Mat4*)(hm + ((size_t)hn + idx[i]) * 0x40);
            if (!(haveOut[i] && memcmp(J, &lastOut[i], sizeof(Mat4)) == 0)) gameVal[i] = *J;  // the game wrote it
            const float gx = gameVal[i].m[3][0] - R.m[3][0], gy = gameVal[i].m[3][1] - R.m[3][1], gz = gameVal[i].m[3][2] - R.m[3][2];
            // D-095: 3 m (was 1 m) - parked joints sit hundreds of metres away; in held attacks the game axe and the gem
            // joints the game wrote can be > 1 m apart and those frames kept the game's placement
            if (gx * gx + gy * gy + gz * gz > 9.0f) { haveOut[i] = false; continue; }  // not on the game's axe (parked)
            if (learnNow) {
                const Mat4 o = Mul(gameVal[i], Ri);
                if (!haveOff[i]) { off[i] = o; haveOff[i] = true; }
                else {
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 3; ++c) off[i].m[r][c] += (o.m[r][c] - off[i].m[r][c]) * 0.05f;
                    OrthonormalizeKeepScale(off[i]);
                }
            }
            if (fixOn && haveOff[i]) {
                *J = Mul(off[i], Rn);
                lastOut[i] = *J;
                haveOut[i] = true;
            } else {
                haveOut[i] = false;
            }
        }
        {   // D-094b diagnostic: how many gem joints have a learned offset (logged when it changes)
            static int lastLearned = -1;
            int learned = 0;
            for (int i = 0; i < kN; ++i) learned += haveOff[i] ? 1 : 0;
            if (learned != lastLearned) {
                lastLearned = learned;
                Log("fp: axe gem glow - %d of %d joints have their offset on the axe (learned standing still, axe in hand)",
                    learned, kN);
            }
        }
        // chaindiag: the first gem joint in the DRAWN axe's frame - its spread = how much the glow wanders on the axe
        if (g_chainDiag.load() > 0 && idx[0] >= 0 && idx[0] < hn) {
            const int an = (int)(uint16_t)*(int16_t*)(ax + 0x38);
            const Mat4 A = *(const Mat4*)(*(uint8_t**)(ax + 0x58) + (size_t)an * 0x40);
            const Mat4 rel = Mul(*(const Mat4*)(hm + ((size_t)hn + idx[0]) * 0x40), InverseUniform(A));
            static float mn[3], mx[3];
            static int n = 0;
            for (int c = 0; c < 3; ++c) {
                const float v = rel.m[3][c];
                if (!n || v < mn[c]) mn[c] = v;
                if (!n || v > mx[c]) mx[c] = v;
            }
            if (++n >= 300) {
                Log("chaindiag: axe gem joint on the drawn axe over 300 frames (fix %s): spread x %.3f y %.3f z %.3f m", fixOn ? "ON" : "off",
                    mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
                n = 0;
            }
        }
        return;
    }
    const int an = (int)(uint16_t)*(int16_t*)(ax + 0x38);
    const Mat4 A = *(const Mat4*)(*(uint8_t**)(ax + 0x58) + (size_t)an * 0x40);  // the drawn axe root
    const Mat4 Ai = InverseUniform(A);
    const bool moving = HeroMoving();
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    static double lagSum = 0;
    static int lagN = 0;
    for (int i = 0; i < kN; ++i) {
        if (idx[i] < 0 || idx[i] >= hn) continue;
        Mat4* J = (Mat4*)(hm + ((size_t)hn + idx[i]) * 0x40);
        if (!moving) {
            const Mat4 r = Mul(*J, Ai);
            const float d = sqrtf(r.m[3][0] * r.m[3][0] + r.m[3][1] * r.m[3][1] + r.m[3][2] * r.m[3][2]);
            if (d < 1.0f) { rel[i] = r; have[i] = true; }  // on the axe (not parked elsewhere)
        } else if (have[i]) {
            const Mat4 W = Mul(rel[i], A);
            if (g_chainDiag.load() > 0 && sl > 0.02f) {
                lagSum += ((J->m[3][0] - W.m[3][0]) * sx + (J->m[3][2] - W.m[3][2]) * sz) / sl;
                if (++lagN >= 300) {
                    Log("chaindiag: axe gem joints vs the drawn axe while moving: %+.3f m along the movement (before the fix)",
                        lagSum / lagN);
                    lagSum = 0; lagN = 0;
                }
            }
            *J = W;
        }
    }
}
extern float g_heroStepDiag[3];
// D-164 (owner: "the axe glow on the bottom quarter of the handle misaligns sometimes ~0.2 m from the axe, and during
// attacks I see the fire glow ~2 m in front of me - decompile it further"). STATIC (decompile + disassembly):
//  - the glow's emitters hang on HERO joints (axegem_* / part_ / emit_: joints 1, 2, 3 .. 201 under the root, flags 0x42);
//  - an effect on a joint gets its parent's world through sub_14058c560 (the link update, 384 callers): the parent
//    skeleton's world CACHE when sub_14058f1d0 says it is current, else sub_140591550 (the joint from the game's locals);
//  - these joints are WRITTEN by sub_1405924d0(skel, joint, world) = "set joint world" (19 callers: the cache entry
//    (n + joint) gets the matrix, the local is derived from the parent) - from the game's OWN axe placement. Owner log:
//    at the draw the gem joints sat on the game's axe in 28130 frames, on the drawn axe in 5932. D-096 put them on the
//    drawn axe only AT THE DRAW (AxeGemTick) - every emitter read between the game's write and the draw got the game's
//    axe: the motion-controlled axe vs the animated one (~0.2 m), the lunged game body in held attacks (~2 m ahead).
// Now the setter is hooked (3 arguments: rcx / edx / r8 only, checked in the disassembly): in gameplay F1 a gem joint
// the game writes on its own axe (nearer the game's axe - read through the original sub_140591550 at that instant -
// than the drawn axe, within 3 m) is written as  world x game axe^-1 x drawn axe  - on the drawn axe from the moment it
// exists, for every reader. One already on the drawn axe is left. AxeGemTick stays as the draw-time safety net.
// gowvr.ini gem_set_hook 0 = off.
constexpr HookSite kSetJointWorld{"SetJointWorld", 0x1405924d0, 5,
    {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0x30, 0x01, 0x00, 0x00, 0x48}};
using PFN_SetJointWorld = void(__fastcall*)(uint8_t* skel, int joint, const float* world);
PFN_SetJointWorld o_SetJointWorld = nullptr;
std::atomic<uint32_t> g_gsCalls{0}, g_gsMoved{0}, g_gsOnDrawn{0}, g_gsFar{0}, g_gsNoAxe{0};
static bool GemSetFix(uint8_t* skel, int joint, const float* world, float* out) {
    if (skel != g_gemFor.load(std::memory_order_acquire) || !world || FpMode() != 1 || !g_axeGem.load()) return false;
    bool gem = false;
    for (int i = 0; i < g_gemN; ++i) if (g_gemIdx[i] == joint) { gem = true; break; }
    if (!gem) return false;
    ++g_gsCalls;
    uint8_t* ax = g_attached[0].skel.load();
    Mat4 W0;
    if (!ax || !AxeDrawn(&W0)) { ++g_gsNoAxe; return false; }
    alignas(16) Mat4 g0;
    RawWorldFromLocals()(ax, &g0.m[0][0], 0);  // the game's axe now (the original, not our answer)
    if (fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) < 1.f) { ++g_gsNoAxe; return false; }
    const Mat4& W = *(const Mat4*)world;
    auto dist = [](const Mat4& a, const Mat4& b) {
        const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
        return sqrtf(x * x + y * y + z * z);
    };
    const float dD = dist(W, W0), dG = dist(W, g0);
    if (dG > 3.f) { ++g_gsFar; return false; }                    // parked / not on any axe
    if (dD < 0.02f || dD + 0.05f < dG) { ++g_gsOnDrawn; return false; }  // already on the drawn axe
    const Mat4 M = Mul(Mul(W, AffineInverse(g0)), W0);
    memcpy(out, &M, sizeof(M));
    ++g_gsMoved;
    return true;
}
void __fastcall hk_SetJointWorld(uint8_t* skel, int joint, const float* world) {
    alignas(16) float fixed[16];
    if (GemSetFix(skel, joint, world, fixed)) {
        o_SetJointWorld(skel, joint, fixed);
    } else {
        o_SetJointWorld(skel, joint, world);
    }
    if (g_gsCalls.load(std::memory_order_relaxed) >= 5000) {
        static std::atomic<bool> busy{false};
        if (!busy.exchange(true)) {
            Log("fp: axe gem joints written by the game - moved from its axe onto the drawn axe %u, already on the drawn axe "
                "%u, far %u, no axe %u (of %u)", g_gsMoved.exchange(0), g_gsOnDrawn.exchange(0), g_gsFar.exchange(0),
                g_gsNoAxe.exchange(0), g_gsCalls.exchange(0));
            busy = false;
        }
    }
}

// D-165 (owner log after D-164: the setter hook never logged a batch of gem writes, and at the draw the gem joints still
// sat on the game's axe 28998 times vs 2172 on the drawn axe - they are not written through sub_1405924d0). STATIC: the
// hero world pass recomputes a joint only when (its flag & 0x43) == 0x43 (sub_1405908a0); the gem joints carry 0x42, so
// they are written straight into the world cache by other code. Whoever writes them, an effect gets its parent joint's
// world through sub_14058c560(link, out, flag) (the link update, 384 callers; disassembly: rcx / rdx / r8b only, returns
// out) - from the cache, its own cached copy (link +0x50, generation +0x98) or the game's locals (sub_140591550). The
// answer is now corrected there: a link whose parent is Kratos (link +0xa0 object, its skeleton +0x238 = the hero's,
// type +0xa8 = 2) on a gem joint (+0xac) gets  answer x game axe^-1 x drawn axe  when the answer sits on the game's axe
// (nearer it than the drawn axe, within 3 m) - every emitter of the glow reads the drawn axe, whenever it reads.
// Gameplay F1 only; gowvr.ini gem_link_hook 0 = off.
constexpr HookSite kLinkWorld{"LinkWorld", 0x14058c560, 12,
    {0x40, 0x55, 0x53, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x90, 0xfd, 0xff, 0xff, 0x48, 0x81, 0xec, 0x70}};
using PFN_LinkWorld = float*(__fastcall*)(uint8_t* link, float* out, char flag);
PFN_LinkWorld o_LinkWorld = nullptr;
// D-166 (owner log D-165: the gem-link reads never reached 5000 - the glow is not (only) read through the gem joints):
// the same read-time correction for links on the AXE OBJECT's own joints (an effect hung on the axe's handle reads the
// axe's world cache when current - only the cache-miss path went through the D-075 sub_140591550 answer), and the counts
// of both kinds every 30 s whatever their number, with the first moved reads sampled (joint, distances, caller) - the
// glow's own reads are identified by the next log. Not while the axe is thrown (its game placement is the real one).
std::atomic<uint32_t> g_glCalls[2], g_glMoved[2], g_glOnDrawn[2], g_glFar[2], g_glNoAxe[2], g_glNeither{0}, g_glType4[2],
    g_glGhost{0};
// D-169 (owner: "it's misaligning even during non-attack moments"; owner log D-168: the glow object on the axe is
// goaxe_frost_trail_basic_lvl4 (axe joint 2) - its reads were 95-100 % on the drawn axe outside attacks, so the glow seen
// misaligned is read some other way; Kratos' gem joints: 0 type-2 reads). STATIC (sub_14058c560): a link has a second
// joint kind - type 4: component +0xc0 (flag +0xb0 bit 0 set, +0xac != 1), its object +0x28, that object's skeleton
// +0x238, the joint +0xac (sub_140592d60 = the joint's world in the cache). Type-4 links on the gem joints / the axe are
// corrected the same way now (counted apart: "type 4"). And during held attacks the axe's cache can hold the game's axe
// moved back onto the drawn body by the lunge ghost (D-081: game root -> drawn root) before FixAttached puts it on the
// hand - a read there sat on neither axe (D-168: 1300-2700 per 10 s in fights); that "ghost" axe is a third candidate.
// D-229 (owner: in a dark cave "an artificial flashlight on my character - in first person it flickers"; "in both eyes
// it keeps switching between a light on the left and a light on the right, they fight over each other"; "the light
// comes out of the talisman"; "when attacking the light moves a bit further"): the talisman's (or Mimir's) light reads
// its parent's world through the item's link (sub_14058c560): the item's world cache when current - the DRAWN item,
// glued by D-162 - else the joint from the item's locals on the GAME's body (turned with the game's facing, not
// shifted under the view, lunged in attacks): two places, frame by frame. Every item the mod moves onto the drawn body
// (D-162 linked items, the talisman, carried items) records its correction K = game root^-1 x drawn root; a link read
// through such an item in gameplay F1 that lands nearer  answer x K  than the answer itself (both against the item's
// current cache) gets  answer x K . Hero joints read directly are only counted ("hero-joint reads on the game's body",
// the published joints). gowvr.ini drawn_item_links 0 = off.
struct ItemCorr { std::atomic<uint8_t*> skel{nullptr}; Mat4 K{}; uint64_t frame = 0; };
constexpr int kItemCorrMax = 16;
ItemCorr g_itemCorr[kItemCorrMax];
SRWLOCK g_itemCorrLock = SRWLOCK_INIT;
std::atomic<uint32_t> g_icMoved{0}, g_icKept{0}, g_icHeroGame{0}, g_icHeroDrawn{0}, g_icHeroMoved{0}, g_icHeroOn{0};
std::atomic<int> g_icHeroGameJoint{-1};
static bool DrawnItemLinks() {
    static const bool on = config::GetFloat("drawn_item_links", 1.f) > 0.5f;
    return on;
}
static void NoteItemCorrection(uint8_t* skel, const float* game, const float* drawn) {
    if (!skel || !game || !drawn || !DrawnItemLinks()) return;
    if (fabsf(game[15] - 1.f) > 1e-3f || fabsf(game[12]) + fabsf(game[13]) + fabsf(game[14]) < 1.f) return;
    const Mat4 K = Mul(AffineInverse(*(const Mat4*)game), *(const Mat4*)drawn);
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockExclusive(&g_itemCorrLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < kItemCorrMax; ++i) {
        if (g_itemCorr[i].skel.load(std::memory_order_relaxed) == skel) { slot = i; break; }
        if (g_itemCorr[i].frame < g_itemCorr[oldest].frame) oldest = i;
    }
    if (slot < 0) slot = oldest;
    g_itemCorr[slot].K = K;
    g_itemCorr[slot].frame = f;
    g_itemCorr[slot].skel.store(skel, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_itemCorrLock);
}
static bool ItemCorrection(uint8_t* skel, Mat4* K) {
    int slot = -1;
    for (int i = 0; i < kItemCorrMax && slot < 0; ++i)
        if (g_itemCorr[i].skel.load(std::memory_order_relaxed) == skel) slot = i;
    if (slot < 0) return false;
    AcquireSRWLockShared(&g_itemCorrLock);
    const bool ok = g_itemCorr[slot].skel.load(std::memory_order_relaxed) == skel &&
                    g_frameNo.load() - g_itemCorr[slot].frame <= 3;  // (a correction from this or the last frames)
    if (ok) *K = g_itemCorr[slot].K;
    ReleaseSRWLockShared(&g_itemCorrLock);
    return ok;
}
static uint8_t* ObjSkeletonSeh(uint8_t* obj) {
    __try { return *(uint8_t**)(obj + 0x238); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static void* ReadPtrSeh(uint8_t* p) {
    __try { return *(void**)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// D-229f: a skeleton of any size (the talisman has 7 joints, effects 3 - PlausibleSkeleton wants >= 20)
static bool AnySkeletonSeh(uint8_t* sk) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(sk + 0x38);
        const uint64_t* h = *(const uint64_t* const*)(sk + 0x30);
        const uint8_t* m = *(const uint8_t* const*)(sk + 0x58);
        if (n < 1 || n > 2000 || !h || !m) return false;
        volatile uint64_t h0 = h[0];
        volatile float m0 = *(const float*)(m + (size_t)n * 0x40 + 0x3c);  // joint 0's world, w of its translation row
        (void)h0;
        return m0 > 0.999f && m0 < 1.001f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a game object's name (+0x38) when the owner is one (else false)
static bool ObjNameSeh(uint8_t* obj, char* buf, size_t n) {
    __try {
        const char* s = *(const char**)(obj + 0x38);
        if (!s) return false;
        size_t i = 0;
        for (; i + 1 < n && s[i]; ++i) {
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return false;
            buf[i] = s[i];
        }
        buf[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float MatDist(const Mat4& a, const Mat4& b) {
    const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
    return sqrtf(x * x + y * y + z * z);
}
bool PubJointByIndex(int j, Mat4* out);  // a published hero joint by index (below)
static void ItemLinkReport() {
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 15000 && tLog.compare_exchange_strong(t0, now)) {
        const uint32_t mv = g_icMoved.exchange(0), kp = g_icKept.exchange(0), hg = g_icHeroGame.exchange(0),
                       hd = g_icHeroDrawn.exchange(0);
        const uint32_t hm = g_icHeroMoved.exchange(0), ho = g_icHeroOn.exchange(0);
        if (mv || kp || hg || hd || hm || ho)
            Log("fp: reads through items on Kratos, 15 s: %u moved from the game's body onto the drawn item, %u already "
                "drawn | reads of his joints by other objects / effects / lights: %u moved onto the drawn joint (last "
                "joint %d), %u already on it (D-229, drawn_item_links)", mv, kp, hm, g_icHeroGameJoint.load(), ho);
            (void)hg; (void)hd;
    }
}
static void ItemLinkFix(uint8_t* link, float* out, uint8_t* parent) {
    uint8_t* psk = ObjSkeletonSeh(parent);
    if (!psk) return;
    const int j = *(const int16_t*)(link + 0xac);
    if (psk == g_heroSkel.load(std::memory_order_relaxed)) {
        // D-229b (owner: "still fighting"; owner log D-229: reads through the items 484 moved / 3464 drawn, but hero-joint
        // reads 112 on the game's body / 195 drawn, the last on joint 471 = JOMimirWeapon1, where Mimir hangs): the light
        // hangs on the item's KRATOS joint itself. Reads of the joints his linked items hang on get the published drawn
        // joint - except the item object's own link (D-162 learns its offset from the game's parent world there)
        // D-229c (owner: "still fighting"; owner log D-229b: "their Kratos joints: 0 moved, 0 already on it" - joint 471 is
        // read only by Mimir's own link - while his other joints were read on the game's body mostly at joint 271, the
        // lower back the talisman is measured against): STATIC - an object's / effect's / light's world is
        // sub_140553eb0(transform holder +0x60, out) = its local x sub_14058c560(holder + 0x40 = the owner + 0xa0) - one
        // path for everything, so the reader is told apart by its OWNER (link - 0xa0; a game object's skeleton at +0x238,
        // its name at +0x38). Every read of a published Kratos joint (not the arms, the weapon point, the sheathed axe's
        // joint) by an owner the mod does not place itself gets the published DRAWN joint when the answer is > 1 cm off
        // it. Owners the mod places (axe, shield, Blades' objects, talisman, items on his joints, carried items) keep
        // the game's answer - their own placement learns from it
        if (j < 0) return;
        static std::atomic<uint8_t*> armsFor{nullptr};
        static std::atomic<int> arm[2] = {-1, -1}, melee{-1};
        if (armsFor.load(std::memory_order_relaxed) != psk) {
            arm[0] = FindJointByHash(0x05c2b5ca793d4bf2ull);
            arm[1] = FindJointByHash(0xe783988515993319ull);
            melee = FindJointByHash(0xb41408d8ad410271ull);  // JOMeleeWeapon2
            armsFor = psk;
        }
        for (int s = 0; s < 2; ++s) {
            const int a = arm[s].load(std::memory_order_relaxed);
            if (a >= 0 && (j == a || IsDescendant(j, a))) return;  // (the arms follow the controllers: own rules)
        }
        if (j == melee.load(std::memory_order_relaxed) || j == g_axeBackJoint.load(std::memory_order_relaxed)) return;
        // D-229j (owner: "the talisman isn't moving anymore but the light is still an issue"; owner log D-229i: the only
        // named reader moved was SNDKratos): any non-arm joint, not only the published ones - the body's own correction
        // this frame, K = game root^-1 x drawn root (the hero pass builds the same locals on either root), chosen when
        // answer x K is nearer the live cache entry (the drawn joint, current or a frame old) than the answer
        Mat4 Kh;
        {
            AcquireSRWLockShared(&g_rootLock);
            Kh = Mul(AffineInverse(g_gameRootM), g_renderRootM);
            ReleaseSRWLockShared(&g_rootLock);
        }
        const int hn = (int)(uint16_t)*(int16_t*)(psk + 0x38);
        const uint8_t* hm = *(const uint8_t**)(psk + 0x58);
        if (j >= hn || !hm) return;
        const Mat4 C = *(const Mat4*)(hm + ((size_t)hn + j) * 0x40);
        const Mat4 D = Mul(*(const Mat4*)out, Kh);
        if (MatDist(D, C) + 0.02f < MatDist(*(const Mat4*)out, C)) {
            // owner checks only for a read that would change (the common, already drawn read stays cheap)
            uint8_t* owner = link - 0xa0;
            for (auto& b : g_bladeObjs)
                if (b.load(std::memory_order_relaxed) == owner) return;
            // a SKINNED owner keeps the game's answer: every skinned object's placement is the world-pass rules' job
            // (axe, shield, Blades, bow, talisman, Mimir, carried items - several learn from the game's answer). Only
            // owners without a skeleton - lights, emitters, sounds - get the drawn joint here
            if (uint8_t* osk = ObjSkeletonSeh(owner); osk && (osk == psk || AnySkeletonSeh(osk))) return;
            memcpy(out, &D, sizeof(D));
            ++g_icHeroMoved;
            g_icHeroGameJoint = j;
            static std::atomic<uint8_t*> named[12];  // each reader once (D-229c's log: SNDKratos took all 12 lines)
            bool seen = false;
            int freeSlot = -1;
            for (int i = 0; i < 12; ++i) {
                uint8_t* o = named[i].load(std::memory_order_relaxed);
                if (o == owner) { seen = true; break; }
                if (!o && freeSlot < 0) freeSlot = i;
            }
            if (!seen && freeSlot >= 0) {
                uint8_t* expect = nullptr;
                if (named[freeSlot].compare_exchange_strong(expect, owner)) {
                    char nm[48];
                    if (!ObjNameSeh(owner, nm, sizeof(nm))) strcpy_s(nm, "(no name)");
                    Log("fp: '%s' (%p, vtable %p) read Kratos joint %d on the game's body - given the drawn joint "
                        "(D-229j)", nm, owner, ReadPtrSeh(owner), j);
                }
            }
        } else {
            ++g_icHeroOn;
        }
        ItemLinkReport();
        return;
    }
    Mat4 K;
    if (!ItemCorrection(psk, &K)) return;
    const int pn = (int)(uint16_t)*(int16_t*)(psk + 0x38);
    const uint8_t* pm = *(const uint8_t**)(psk + 0x58);
    if (j < 0 || j >= pn || !pm) return;
    const Mat4 A = *(const Mat4*)out;
    const Mat4 B = Mul(A, K);
    const Mat4 D = *(const Mat4*)(pm + ((size_t)pn + j) * 0x40);  // the item's cache (the drawn item once glued)
    if (MatDist(B, D) + 0.02f < MatDist(A, D)) {
        memcpy(out, &B, sizeof(B));
        ++g_icMoved;
    } else {
        ++g_icKept;
    }
    ItemLinkReport();
}

float* __fastcall hk_LinkWorld(uint8_t* link, float* out, char flag) {
    float* r = o_LinkWorld(link, out, flag);
    if (!link || !out) return r;
    const int type = *(const int*)(link + 0xa8);
    uint8_t* parent = nullptr;
    if (type == 2) {
        parent = *(uint8_t**)(link + 0xa0);
    } else if (type == 4) {  // (the same tests the game made before reading through it)
        uint8_t* c = *(uint8_t**)(link + 0xc0);
        if (c && (*(c + 0xb0) & 1) && *(const int*)(c + 0xac) != 1) parent = *(uint8_t**)(c + 0x28);
    }
    if (!parent) return r;
    if (FpMode() == 1 && DrawnItemLinks()) ItemLinkFix(link, out, parent);  // D-229
    static const bool gemLinks = config::GetFloat("gem_link_hook", 1.f) > 0.5f;
    if (!gemLinks) return r;
    int kind = -1;  // 0: a gem joint on Kratos, 1: a joint of the axe object
    const int j = *(const int16_t*)(link + 0xac);
    uint8_t* axObj = g_attached[0].obj.load(std::memory_order_relaxed);
    if (axObj && parent == axObj) {
        kind = 1;
    } else if (uint8_t* hero = g_gemFor.load(std::memory_order_relaxed); hero && *(uint8_t**)(parent + 0x238) == hero) {
        for (int i = 0; i < g_gemN; ++i) if (g_gemIdx[i] == j) { kind = 0; break; }
    }
    if (kind < 0 || FpMode() != 1 || !g_axeGem.load() || AxeThrown()) return r;
    ++g_glCalls[kind];
    if (type == 4) ++g_glType4[kind];
    uint8_t* ax = g_attached[0].skel.load();
    Mat4 W0;
    alignas(16) Mat4 g0;
    if (ax && AxeDrawn(&W0)) {
        RawWorldFromLocals()(ax, &g0.m[0][0], 0);
        if (fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) >= 1.f) {
            // the ghost axe: the game's axe shifted by drawn root - game root (horizontal), as the lunge ghost does
            Mat4 gS = g0;
            {
                AcquireSRWLockShared(&g_rootLock);
                gS.m[3][0] += g_renderRoot[0] - g_gameRoot[0];
                gS.m[3][2] += g_renderRoot[2] - g_gameRoot[2];
                ReleaseSRWLockShared(&g_rootLock);
            }
            const bool ghostApart = fabsf(gS.m[3][0] - g0.m[3][0]) + fabsf(gS.m[3][2] - g0.m[3][2]) > 0.3f;
            const Mat4 W = *(const Mat4*)out;
            auto dist = [](const Mat4& a, const Mat4& b) {
                const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
                return sqrtf(x * x + y * y + z * z);
            };
            // where this joint is on each axe: an axe joint by its offset to the root (the axe's own cache, D-167); a gem
            // joint (Kratos', no offset of its own) by the axe roots
            Mat4 rel;
            bool haveRel = false;
            if (kind == 1 && j > 0) {
                const int an = (int)(uint16_t)*(int16_t*)(ax + 0x38);
                const uint8_t* am = *(const uint8_t**)(ax + 0x58);
                if (j < an && am) {
                    rel = Mul(*(const Mat4*)(am + ((size_t)an + j) * 0x40), AffineInverse(*(const Mat4*)(am + (size_t)an * 0x40)));
                    haveRel = true;
                } else {
                    ++g_glNeither;
                    goto report;
                }
            }
            {
                const float dD = dist(W, haveRel ? Mul(rel, W0) : W0);
                const float dG = dist(W, haveRel ? Mul(rel, g0) : g0);
                const float dS = ghostApart ? dist(W, haveRel ? Mul(rel, gS) : gS) : 1e9f;
                const bool ghost = dS < dG;
                const float dC = ghost ? dS : dG;  // the nearer game candidate
                if (dG > 3.f && dS > 3.f) ++g_glFar[kind];
                else if (dD < 0.02f || dD + 0.05f < dC) ++g_glOnDrawn[kind];
                else if (dC < 0.02f || dC + 0.05f < dD) {  // clearly on a game axe (D-168: never when equally near)
                    const Mat4 M = Mul(Mul(W, AffineInverse(ghost ? gS : g0)), W0);
                    memcpy(out, &M, sizeof(M));
                    ++g_glMoved[kind];
                    if (ghost) ++g_glGhost;
                    static std::atomic<int> samples{0};
                    if (samples.fetch_add(1) < 24) {
                        char nm[40] = "?";
                        const uint8_t* ob = link - 0xa0;
                        if (type == 2 && !IsBadReadPtr(ob + 0x38, 8)) {
                            const char* sn = *(const char* const*)(ob + 0x38);
                            if (sn && !IsBadReadPtr(sn, 24)) {
                                strncpy_s(nm, sn, _TRUNCATE);
                                for (char& c : nm) if (c && (c < 32 || c > 126)) c = '?';
                            }
                        }
                        Log("fp: axe effect read moved onto the drawn axe - %s joint %d ('%s', link type %d), %.2f m from the "
                            "game's axe%s, %.2f m from the drawn axe (caller %llx)", kind ? "axe" : "Kratos gem", j, nm, type, dC,
                            ghost ? " (ghost - moved back by the lunge)" : "", dD,
                            (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr) + 0x140000000ull));
                    }
                } else {
                    ++g_glNeither;
                }
            }
        } else {
            ++g_glNoAxe[kind];
        }
    } else {
        ++g_glNoAxe[kind];
    }
report:
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 10000 && tLog.compare_exchange_strong(t0, now))
        Log("fp: axe effect link reads, 10 s - Kratos gem joints %u (type 4: %u; moved onto the drawn axe %u, already on it %u, "
            "far %u, no axe %u) | axe joints %u (type 4: %u; moved %u, already on it %u, far %u, no axe %u) | of the moved from "
            "the lunge ghost %u, on neither %u", g_glCalls[0].exchange(0), g_glType4[0].exchange(0), g_glMoved[0].exchange(0),
            g_glOnDrawn[0].exchange(0), g_glFar[0].exchange(0), g_glNoAxe[0].exchange(0), g_glCalls[1].exchange(0),
            g_glType4[1].exchange(0), g_glMoved[1].exchange(0), g_glOnDrawn[1].exchange(0), g_glFar[1].exchange(0),
            g_glNoAxe[1].exchange(0), g_glGhost.exchange(0), g_glNeither.exchange(0));
    return r;
}

// D-170 (owner logs D-165..D-169: no effect reads Kratos' gem joints through a link - 0 type-2, 0 type-4 reads - and the
// frost trail on the axe reads the drawn axe outside fights, yet the glow at the bottom of the handle still sits off the
// axe "even during non-attack moments"). The gem joints are hero joints (axegem_dots_after1 / _burst1 / _smoke1, part_ /
// emit_ - the pommel gem's particles), and every frame the game writes them on ITS axe (at the draw: on the game's axe
// 19962-28998 times, on the drawn axe 1972-3406). STATIC: the GPU gets Kratos' joints from the render submission
// sub_140470b70 -> sub_140471c30(render instance, ...) (4 register arguments, no stack ones - disassembly): instance +0x10
// = the skeleton, +0x18 = its world matrices, each multiplied into the frame's skinning palette (TLS scratch) that
// sub_140592a40 copies to the GPU ring. Whatever reads the gem joints on the GPU (the gem mesh / GPU emitters) gets them
// from there - built before the draw-time AxeGemTick ran. Now, right before Kratos' palette is built (gameplay F1), each
// gem joint in that world array that sits on the game's axe - or on the lunge ghost of it (D-169) - is put on the drawn
// axe (same mapping), so the palette carries the drawn axe. gowvr.ini gem_palette_hook 0 = off.
constexpr HookSite kPaletteBuild{"PaletteBuild", 0x140471c30, 5,
    {0x48, 0x8b, 0xc4, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}};
using PFN_PaletteBuild = void(__fastcall*)(uint8_t* inst, void* a2, void* a3, void* a4);
PFN_PaletteBuild o_PaletteBuild = nullptr;
std::atomic<uint32_t> g_gpBuilds{0}, g_gpMoved{0}, g_gpGhost{0}, g_gpOnDrawn{0}, g_gpOther{0}, g_gpForced{0};
// D-171 (owner log D-170: per 10 s "other" 2938-8978 of ~10500 gem joint checks - the test measured each gem joint from
// the two axe ROOTS; the gem sits on the pommel, away from the root, so near-coinciding axes looked alike - D-167's
// defect again): each gem joint's own offset to the GAME's axe root is learned from the builds where it is consistent
// (O = J x game axe^-1 within 2 cm of the running value, blended; 40 inconsistent in a row = the attachment changed:
// relearn) - the game writes them from its axe in most builds, our own corrected values in between never agree. Once
// settled (20 good), the joint is written at that offset on the DRAWN axe in every build (rigid on the axe, whatever the
// game left there); before that, the D-170 test. The first "other" cases are logged with their distances.
// D-172 (owner log D-171: 10 of 10 settled outside fights, but in a fight window "0 of 10 settled" - light axe combos at
// 4x: the game writes the gem joints from its axe a frame late, so with the axe moving fast every build disagreed and 40
// in a row threw the offsets away for the rest of the fight): only values the GAME wrote are learned from (not our own
// write still in place), and only while the game's axe is steady from one frame to the next (< 1 cm); a relearn needs
// 120 steady disagreements in a row. A fight never unlearns them.
// D-173 (owner: "standing still the flame is right; once I turn with the stick two copies of the flame fight over which
// shows, one appearing and the other disappearing every ~0.5 s"; owner log D-172: "6 of 10 joints settled" all session -
// 4 joints never settled, and for them the root test left a build's value where the game put it ("other") or moved it,
// build by build - the same flame in two places; turning moves the game's axe every frame): the 4 are particle locators
// that turn (smoke ...): the learning checked the ROTATION too (LinkNear: 0.995). Now: consistency by position only
// (2 cm); an unsettled joint the game wrote within 1.5 m of its axe is always mapped game axe (or its lunge ghost) ->
// drawn axe (no "leave it" case any more); our own value still in place follows the drawn axe (x the drawn axe it was
// written on ^-1 x the drawn axe now). Every gem joint is in one consistent place in every build.
struct GemOff { Mat4 O{}; uint32_t good = 0, bad = 0; bool have = false; Mat4 lastOut{}; bool haveOut = false; Mat4 outW0{}; };
Mat4 g_gemG0Prev{};
uint64_t g_gemG0Frame = 0;
bool g_gemSteady = false;
SRWLOCK g_gemOffLock = SRWLOCK_INIT;
GemOff g_gemOff[16];
uint8_t* g_gemOffFor = nullptr;
std::atomic<uint32_t> g_gpPost{0}, g_gpRecomputed{0}, g_gpPostAxe{0};
// D-176 (owner log D-175: no crash; "the pass had moved them with the body 0 times" - the turned-body pass never moved the
// gem joints; yet at the draw they were on the game's axe 15936 vs 8348 on the drawn axe after ~5 corrections per frame
// right after Kratos' passes - the game rewrites them LATER in the frame, and the post-pass writes starved the learning:
// "0 of 10 settled" for long stretches): at every point the mod touches them (0 palette build, 1 after Kratos' pass,
// 2 after the axe's pass, 3 the draw) the first gem joint is checked - still our last value, or rewritten by the game? -
// so the next log shows between which two points the game writes it. And the joints are also put on the drawn axe after
// the axe's own pass (the drawn axe is final there); learning from a game-written value also there / after Kratos' pass
// while the game's axe is steady (its palette-read position at most 1 frame old).
std::atomic<uint32_t> g_probeGame[4], g_probeOurs[4];
static void GemProbe(int point, const float* worlds) {
    if (!worlds || g_gemN <= 0 || point < 0 || point > 3) return;
    uint8_t* hero = g_gemFor.load(std::memory_order_relaxed);
    if (!hero) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
    const int j = g_gemIdx[0];
    if (j < 0 || j >= hn) return;
    const GemOff& go = g_gemOff[0];
    if (!go.haveOut) return;
    if (memcmp(worlds + (size_t)j * 16, &go.lastOut, sizeof(Mat4)) == 0) ++g_probeOurs[point];
    else ++g_probeGame[point];
}
static void GemFixWorlds(uint8_t* hero, float* worlds, int where);
static void GemPaletteFix(uint8_t* inst) {
    uint8_t* hero = g_gemFor.load(std::memory_order_relaxed);
    if (!hero || !inst || *(uint8_t**)(inst + 0x10) != hero) return;
    float* worlds = *(float**)(inst + 0x18);
    if (!worlds) return;
    static uint8_t* loggedFor = nullptr;  // D-176: is the palette's world array Kratos' live world cache or a copy?
    if (loggedFor != hero) {
        loggedFor = hero;
        const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
        const float* live = (const float*)(*(uint8_t**)(hero + 0x58) + (size_t)hn * 0x40);
        Log("fp: Kratos' skinning palette is built from %s (%p, live cache %p)", worlds == live ? "his LIVE world cache" :
            "a COPY of his world matrices", worlds, live);
    }
    GemFixWorlds(hero, worlds, 0);
}
// D-176: after the axe's own world pass (its drawn placement is final there)
static void GemAfterAxePass() {
    uint8_t* hero = g_gemFor.load(std::memory_order_relaxed);
    if (!hero || FpMode() != 1) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
    GemFixWorlds(hero, (float*)(*(uint8_t**)(hero + 0x58) + (size_t)hn * 0x40), 2);
}
// D-174: the gem joints on the drawn axe in a world array (0: the palette's, 1: the hero's live world cache after its pass)
// D-175 (owner: "crash" right after F1 turned on with the D-174 build - the log just stops, no first-chance line: a stack
// overflow): D-174 called this from INSIDE Kratos' world pass, and it read the game's axe through sub_140591550 - the axe
// hangs on Kratos' hand, whose world is not current mid-pass, so the game evaluated Kratos again: his pass -> our hook ->
// the axe -> his pass ... without end. Inside the pass (where 1) the game's axe is never evaluated now: the one the
// palette build read (where 0, after the pass) at most 2 frames ago is used; without it only the settled joints are
// placed. A per-thread guard keeps any nesting out.
thread_local bool t_inGemFix = false;
Mat4 g_gemLastG0{};
uint64_t g_gemLastG0Frame = 0;
static void GemFixWorlds(uint8_t* hero, float* worlds, int where) {
    if (t_inGemFix || FpMode() != 1 || !g_axeGem.load() || AxeThrown()) return;
    GemProbe(where, worlds);  // D-176: before we touch them
    if (where == 0) ++g_gpBuilds;
    else if (where == 1) ++g_gpPost;
    else ++g_gpPostAxe;
    bool g0Fresh = where == 0;
    uint8_t* ax = g_attached[0].skel.load();
    Mat4 W0;
    alignas(16) Mat4 g0;
    if (!ax || !AxeDrawn(&W0)) return;
    bool haveG0 = false;
    if (where == 0) {
        t_inGemFix = true;
        RawWorldFromLocals()(ax, &g0.m[0][0], 0);
        t_inGemFix = false;
        if (fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) < 1.f) return;
        haveG0 = true;
        AcquireSRWLockExclusive(&g_gemOffLock);
        g_gemLastG0 = g0;
        g_gemLastG0Frame = g_frameNo.load();
        ReleaseSRWLockExclusive(&g_gemOffLock);
    } else {
        AcquireSRWLockShared(&g_gemOffLock);
        g0 = g_gemLastG0;
        const uint64_t f = g_gemLastG0Frame;
        ReleaseSRWLockShared(&g_gemOffLock);
        haveG0 = f && g_frameNo.load() - f <= 2;
        g0Fresh = f && g_frameNo.load() - f <= 1;
        if (!haveG0) g0 = W0;  // (placeholder: no game-axe test below without it)
    }
    Mat4 gS = g0;
    AcquireSRWLockShared(&g_rootLock);
    gS.m[3][0] += g_renderRoot[0] - g_gameRoot[0];
    gS.m[3][2] += g_renderRoot[2] - g_gameRoot[2];
    ReleaseSRWLockShared(&g_rootLock);
    const bool ghostApart = fabsf(gS.m[3][0] - g0.m[3][0]) + fabsf(gS.m[3][2] - g0.m[3][2]) > 0.3f;
    const int hn = (int)(uint16_t)*(int16_t*)(hero + 0x38);
    auto dist = [](const Mat4& a, const Mat4& b) {
        const float x = a.m[3][0] - b.m[3][0], y = a.m[3][1] - b.m[3][1], z = a.m[3][2] - b.m[3][2];
        return sqrtf(x * x + y * y + z * z);
    };
    const Mat4 g0i = AffineInverse(g0);
    AcquireSRWLockExclusive(&g_gemOffLock);
    if (g_gemOffFor != hero) {  // a new hero: learn again
        g_gemOffFor = hero;
        for (auto& o : g_gemOff) o = GemOff{};
    }
    if (where == 0) {   // D-172: is the game's axe steady since the last frame? (D-175: read in the palette build only)
        const uint64_t fr = g_frameNo.load();
        if (fr != g_gemG0Frame) {
            g_gemSteady = g_gemG0Frame && fr - g_gemG0Frame <= 2 && dist(g0, g_gemG0Prev) < 0.01f;
            g_gemG0Prev = g0;
            g_gemG0Frame = fr;
        }
    }
    for (int i = 0; i < g_gemN && i < 16; ++i) {
        const int j = g_gemIdx[i];
        if (j < 0 || j >= hn) continue;
        Mat4* J = (Mat4*)(worlds + (size_t)j * 16);
        // D-171 / D-172: learn this joint's offset to the game's axe - from the game's own writes, axe steady
        GemOff& go = g_gemOff[i];
        const bool ours = go.haveOut && memcmp(J, &go.lastOut, sizeof(Mat4)) == 0;
        // D-175: learn in the palette; D-176: also after the passes while the game's axe is steady (fresh position)
        if ((where == 0 || (g_gemSteady && g0Fresh && go.have)) && !ours && dist(*J, g0) < 1.f && (g_gemSteady || !go.have)) {
            const Mat4 On = Mul(*J, g0i);
            const float ex = On.m[3][0] - go.O.m[3][0], ey = On.m[3][1] - go.O.m[3][1], ez = On.m[3][2] - go.O.m[3][2];
            if (!go.have) { go.O = On; go.have = true; go.good = 1; go.bad = 0; }
            else if (ex * ex + ey * ey + ez * ez < 0.02f * 0.02f) {  // D-173: position only
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 3; ++c) go.O.m[r][c] += (On.m[r][c] - go.O.m[r][c]) * 0.05f;
                OrthonormalizeKeepScale(go.O);
                if (go.good < 1000000) ++go.good;
                go.bad = 0;
            } else if (++go.bad >= 120) {
                go.O = On; go.good = 1; go.bad = 0;
            }
        }
        if (go.have && go.good >= 20) {  // settled: rigid on the drawn axe
            *J = Mul(go.O, W0);
            go.lastOut = *J;
            go.haveOut = true;
            go.outW0 = W0;
            ++g_gpForced;
            continue;
        }
        if (!haveG0) continue;  // D-175: unsettled joints need the game's axe
        if (ours) {  // D-173: our value from an earlier build - it follows the drawn axe
            *J = Mul(Mul(*J, AffineInverse(go.outW0)), W0);
            go.lastOut = *J;
            go.outW0 = W0;
            ++g_gpOnDrawn;
            continue;
        }
        const float dG = dist(*J, g0), dS = ghostApart ? dist(*J, gS) : 1e9f;
        const bool ghost = dS < dG;
        const float dC = ghost ? dS : dG;
        if (dC > 1.5f) {  // not on (near) any game axe: parked / not driven - left
            ++g_gpOther;
            static std::atomic<int> otherLogs{0};
            if (otherLogs.fetch_add(1) < 12)
                Log("fp: axe gem joint %d in the palette away from the game's axe - %.3f m from it, %.3f from the drawn axe root, "
                    "%.3f from the ghost (offset learned: %s)", j, dG, dist(*J, W0), dS,
                    go.have ? (go.good >= 20 ? "yes" : "settling") : "no");
            go.haveOut = false;
            continue;
        }
        // D-173: always from the game's axe (or its lunge ghost) onto the drawn axe
        *J = Mul(Mul(*J, AffineInverse(ghost ? gS : g0)), W0);
        go.lastOut = *J;
        go.haveOut = true;
        go.outW0 = W0;
        ++g_gpMoved;
        if (ghost) ++g_gpGhost;
    }
    int settled = 0;
    for (int i = 0; i < g_gemN && i < 16; ++i) settled += g_gemOff[i].have && g_gemOff[i].good >= 20 ? 1 : 0;
    ReleaseSRWLockExclusive(&g_gemOffLock);
    static std::atomic<ULONGLONG> tLog{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tLog.load();
    if (!t0) tLog = now;
    else if (now - t0 >= 10000 && tLog.compare_exchange_strong(t0, now))
        Log("fp: axe gem joints, 10 s - %u palette builds + %u after Kratos' world pass (the pass had moved them with the body "
            "%u times - put back) + %u after the axe's pass: on the drawn axe at the learned offset %u (%d of %d joints "
            "settled); before that: moved from the game's axe %u (from the lunge ghost %u), already on it %u, other %u | "
            "game-written / ours when seen: palette %u/%u, after Kratos' pass %u/%u, after the axe's pass %u/%u, draw %u/%u",
            g_gpBuilds.exchange(0), g_gpPost.exchange(0), g_gpRecomputed.exchange(0), g_gpPostAxe.exchange(0),
            g_gpForced.exchange(0), settled, g_gemN, g_gpMoved.exchange(0), g_gpGhost.exchange(0), g_gpOnDrawn.exchange(0),
            g_gpOther.exchange(0), g_probeGame[0].exchange(0), g_probeOurs[0].exchange(0), g_probeGame[1].exchange(0),
            g_probeOurs[1].exchange(0), g_probeGame[2].exchange(0), g_probeOurs[2].exchange(0), g_probeGame[3].exchange(0),
            g_probeOurs[3].exchange(0));
}
static void PaletteHeadHide(uint8_t* inst);  // D-228 (below ApplyPlan)
void __fastcall hk_PaletteBuild(uint8_t* inst, void* a2, void* a3, void* a4) {
    static const bool gem = config::GetFloat("gem_palette_hook", 1.f) > 0.5f;
    if (gem) GemPaletteFix(inst);
    PaletteHeadHide(inst);
    o_PaletteBuild(inst, a2, a3, a4);
    stereo::R1SkinPrevious(inst, a2, a3, a4);  // dlss-vr: eye 1's previous pose = eye 0's
}

// D-069b "handjoints N" (owner: the loose chain from the fist to the Blade lags while moving): every joint of every
// skeleton passing near Kratos that sits within 0.5 m of a drawn wrist, in that wrist's frame - standing baseline vs
// moving; the worst joints are logged with their skeleton / hash / index. SEH-guarded (short-lived skeletons).
std::atomic<int> g_handJoints{0};
void HandJoints(int n) { g_handJoints = n; Log("handjoints: next %d moving frames", n); }
struct NearSk { uint8_t* skel = nullptr; uint64_t frame = 0; };
static NearSk g_nearSk[24];
static SRWLOCK g_nearSkLock = SRWLOCK_INIT;
static void NoteNearSkel(uint8_t* skel, const float* root) {
    if (g_handJoints.load() <= 0 || !skel || !root) return;
    float g[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g));
    ReleaseSRWLockShared(&g_rootLock);
    const float dx = root[12] - g[0], dz = root[14] - g[2];
    if (dx * dx + dz * dz > 2.5f * 2.5f) return;
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockExclusive(&g_nearSkLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < 24; ++i) {
        if (g_nearSk[i].skel == skel) { slot = i; break; }
        if (g_nearSk[i].frame < g_nearSk[oldest].frame) oldest = i;
    }
    if (slot < 0) slot = oldest;
    g_nearSk[slot].skel = skel;
    g_nearSk[slot].frame = f;
    ReleaseSRWLockExclusive(&g_nearSkLock);
}
struct HJ { uint8_t* skel; int j; uint64_t h0, hj; float base[3]; bool have; double sumDev, sumAlong; int n; float maxDev;
            double standJit = 0; int standN = 0; };
static std::vector<HJ> g_hj;
static int ReadJointsSeh(uint8_t* skel, int maxJ, float* out, uint64_t* hashes, int* nOut) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(skel + 0x38);
        if (n <= 0 || n > 2000) return 0;
        const uint8_t* m = *(const uint8_t* const*)(skel + 0x58);
        const uint64_t* hh = *(const uint64_t* const*)(skel + 0x30);
        const int k = n < maxJ ? n : maxJ;
        for (int j = 0; j < k; ++j) {
            const float* w = (const float*)(m + ((size_t)n + j) * 0x40 + 0x30);
            out[j * 3] = w[0]; out[j * 3 + 1] = w[1]; out[j * 3 + 2] = w[2];
            hashes[j] = hh ? hh[j] : 0;
        }
        *nOut = n;
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
static void HandJointsCheck() {
    if (g_handJoints.load() <= 0) return;
    Mat4 wr[2], el;
    bool hw[2] = {ArmsIkResult(0, &wr[0], &el), ArmsIkResult(1, &wr[1], &el)};
    if (!hw[0] && !hw[1]) return;
    const Mat4 iw[2] = {RigidInverse(wr[0]), RigidInverse(wr[1])};
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    const bool moving = sl > 0.02f;
    uint8_t* sks[24];
    int ns = 0;
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockShared(&g_nearSkLock);
    for (auto& s : g_nearSk) if (s.skel && f - s.frame <= 2 && ns < 23) sks[ns++] = s.skel;
    ReleaseSRWLockShared(&g_nearSkLock);
    if (uint8_t* hsk = g_heroSkel.load()) sks[ns++] = hsk;  // the hero's own joints (its passes take the hero branch)
    static float pos[1200 * 3];
    static uint64_t hs[1200];
    for (int si = 0; si < ns; ++si) {
        int n = 0;
        const int k = ReadJointsSeh(sks[si], 1200, pos, hs, &n);
        for (int j = 0; j < k; ++j) {
            const float* p = pos + j * 3;
            int h = -1;
            float best = 0.5f * 0.5f;
            for (int q = 0; q < 2; ++q) {
                if (!hw[q]) continue;
                const float dx = p[0] - wr[q].m[3][0], dy = p[1] - wr[q].m[3][1], dz = p[2] - wr[q].m[3][2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; h = q; }
            }
            if (h < 0) continue;
            float rel[3];
            for (int c = 0; c < 3; ++c) rel[c] = p[0] * iw[h].m[0][c] + p[1] * iw[h].m[1][c] + p[2] * iw[h].m[2][c] + iw[h].m[3][c];
            HJ* e = nullptr;
            for (auto& x : g_hj) if (x.skel == sks[si] && x.j == j) { e = &x; break; }
            if (!e) {
                if (g_hj.size() >= 3000) continue;
                g_hj.push_back(HJ{sks[si], j, hs[0], hs[j], {rel[0], rel[1], rel[2]}, false, 0, 0, 0, 0});
                e = &g_hj.back();
            }
            if (!moving) {
                if (e->have) {
                    const float d[3] = {rel[0] - e->base[0], rel[1] - e->base[1], rel[2] - e->base[2]};
                    e->standJit += sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    ++e->standN;
                }
                for (int c = 0; c < 3; ++c) e->base[c] = e->have ? e->base[c] + (rel[c] - e->base[c]) * 0.1f : rel[c];
                e->have = true;
            } else if (e->have) {
                const float d[3] = {rel[0] - e->base[0], rel[1] - e->base[1], rel[2] - e->base[2]};
                float ww[3];
                for (int c = 0; c < 3; ++c) ww[c] = d[0] * wr[h].m[0][c] + d[1] * wr[h].m[1][c] + d[2] * wr[h].m[2][c];
                const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                e->sumDev += dl;
                e->sumAlong += (ww[0] * sx + ww[2] * sz) / sl;
                e->maxDev = fmaxf(e->maxDev, dl);
                ++e->n;
            }
        }
    }
    if (moving && g_handJoints.fetch_sub(1) == 1) {
        std::vector<HJ*> v;
        // only joints held rigidly by the hand while standing (mean standing jitter < 1 cm), not the hand's own children
        static const int wristJ[2] = {FindJointByHash(0x6de47b0b82506234ull), FindJointByHash(0x3626a7d4c72e93aeull)};
        uint8_t* hsk = g_heroSkel.load();
        for (auto& x : g_hj) {
            if (x.n < 10 || x.standN < 20 || x.standJit / x.standN > 0.01) continue;
            if (x.skel == hsk && ((wristJ[0] >= 0 && (x.j == wristJ[0] || IsDescendant(x.j, wristJ[0]))) ||
                                  (wristJ[1] >= 0 && (x.j == wristJ[1] || IsDescendant(x.j, wristJ[1]))))) continue;
            v.push_back(&x);
        }
        std::sort(v.begin(), v.end(), [](HJ* a, HJ* b) { return a->sumDev / a->n > b->sumDev / b->n; });
        Log("handjoints: %zu joints near the hands measured while moving; worst:", v.size());
        for (size_t i = 0; i < v.size() && i < 25; ++i)
            Log("handjoints: skel %p hash0 %016llx joint %d hash %016llx | vs its wrist %.3f m mean, %.3f max, along the movement "
                "%+.3f m (%d frames)", v[i]->skel, (unsigned long long)v[i]->h0, v[i]->j, (unsigned long long)v[i]->hj,
                v[i]->sumDev / v[i]->n, v[i]->maxDev, v[i]->sumAlong / v[i]->n, v[i]->n);
        g_hj.clear();
    }
}
extern std::atomic<int> g_chainDiag;
// D-069 draw time: the axe's effect objects (learned offset E on the axe) vs E x the axe as drawn now, along the hero's
// movement. Effect skeletons are short-lived (D-058 crash: a freed one was read): SEH-guarded copy of the root only.
std::atomic<bool> g_axeFxRefresh{true};  // D-069 A/B "axefxrefresh on|off"
void AxeFxRefresh(bool on) { g_axeFxRefresh = on; Log("fp: axe effects re-placed at the draw %s", on ? "ON" : "OFF"); }
static bool AxeFxStillValid(const uint8_t* skel) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(skel + 0x38);
        if (n <= 0 || n > 16) return false;
        const uint64_t* hh = *(const uint64_t* const*)(skel + 0x30);
        return hh && (hh[0] >> 32) == 0x3124c569ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static bool ReadRootSeh(const uint8_t* skel, float out[3]) {
    __try {
        const int n = (int)(uint16_t)*(const int16_t*)(skel + 0x38);
        if (n <= 0 || n > 16) return false;
        const float* w = (const float*)(*(const uint8_t* const*)(skel + 0x58) + (size_t)n * 0x40 + 0x30);
        out[0] = w[0]; out[1] = w[1]; out[2] = w[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static void AxeFxLagCheck() {
    if (g_chainDiag.load() <= 0) return;
    uint8_t* ax = g_attached[0].skel.load();
    if (!ax || IsBadReadPtr(ax + 0x38, 0x28)) return;
    const int an = (int)(uint16_t)*(int16_t*)(ax + 0x38);
    const Mat4 A = *(const Mat4*)(*(uint8_t**)(ax + 0x58) + (size_t)an * 0x40);  // the axe root as drawn
    const float sx = g_heroStep[0], sz = g_heroStep[2], sl = sqrtf(sx * sx + sz * sz);
    if (sl < 0.02f) return;
    static double sumA = 0, sumAbs = 0;
    static int cnt = 0;
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockShared(&g_axeCorrLock);
    for (const auto& s : g_satLearn) {
        if (!s.skel || f - s.seen > 3) continue;
        float p[3];
        if (!ReadRootSeh(s.skel, p)) continue;
        const Mat4 want = Mul(s.E, A);
        const float ox = p[0] - want.m[3][0], oz = p[2] - want.m[3][2];
        if (ox * ox + oz * oz > 1.f) continue;  // not on the axe now (a stale / reused entry)
        sumA += (ox * sx + oz * sz) / sl;
        sumAbs += sqrtf(ox * ox + oz * oz);
        ++cnt;
    }
    ReleaseSRWLockShared(&g_axeCorrLock);
    if (cnt >= 60) {
        Log("chaindiag: axe effects while moving %.3f m/frame: vs the drawn axe %.3f m, along the movement %+.3f m (%d samples)",
            sl, sumAbs / cnt, sumA / cnt, cnt);
        sumA = sumAbs = 0;
        cnt = 0;
    }
}
// Draw time (WaitHeroFrame, after the frame's passes): recorded effect positions vs their joint's drawn position, the
// offset along the hero's movement (+ = ahead); logged every ~300 samples
static void FxLagCheck() {
    static double sumAlong = 0.0, sumAbs = 0.0;
    static int nS = 0;
    const float sl = sqrtf(g_heroStep[0] * g_heroStep[0] + g_heroStep[2] * g_heroStep[2]);
    const uint64_t f = g_frameNo.load();
    for (auto& r : g_fxRec) {
        if (r.used || !r.skel || f - r.frame > 1) continue;
        r.used = true;
        if (sl < 0.01f || IsBadReadPtr(r.skel + 0x38, 0x28)) continue;  // standing: nothing to measure
        const int n = (int)(uint16_t)*(int16_t*)(r.skel + 0x38);
        const uint8_t* m = *(uint8_t**)(r.skel + 0x58);
        if (r.joint >= n || !m || IsBadReadPtr(m + ((size_t)n + r.joint) * 0x40, 0x40)) continue;
        const float* c = (const float*)(m + ((size_t)n + r.joint) * 0x40);
        const float ox = r.p[0] - c[12], oz = r.p[2] - c[14];
        sumAlong += (ox * g_heroStep[0] + oz * g_heroStep[2]) / sl;
        sumAbs += sqrtf(ox * ox + oz * oz);
        if (++nS >= 300) {
            Log("fxlag: %d effect samples while moving %.3f m/frame: offset along the movement %+.3f m (mean), |offset| %.3f m, "
                "lead %.2f", nS, sl, sumAlong / nS, sumAbs / nS, FxLead());
            sumAlong = sumAbs = 0.0;
            nS = 0;
        }
    }
}
std::atomic<bool> g_wflFix{true};
std::atomic<uint64_t> g_wflHero{0}, g_wflHeroArm{0}, g_wflBlade{0}, g_wflFixed{0};
void WflFix(bool on) { g_wflFix = on; Log("fp: effects read the edited pose (world-from-locals override) %s", on ? "ON" : "OFF"); }
void WflStats() {
    Log("wfl: hero calls %llu (arm/weapon joints %llu) | Blades objects %llu | answered from the edited pose %llu",
        (unsigned long long)g_wflHero.exchange(0), (unsigned long long)g_wflHeroArm.exchange(0),
        (unsigned long long)g_wflBlade.exchange(0), (unsigned long long)g_wflFixed.exchange(0));
}
static bool EditedHeroJoint(int j) {
    static int clav[2] = {-2, -2}, j4 = -2;
    if (clav[0] == -2) {
        const int sh[2] = {FindJointByHash(0x05c2b5ca793d4bf2ull), FindJointByHash(0xe783988515993319ull)};
        for (int h = 0; h < 2; ++h) clav[h] = sh[h] >= 0 ? JointParent(sh[h]) : -1;
        j4 = FindJointByHash(0x169277bfd5da3f44ull);
    }
    if (j4 >= 0 && JointParent(j) == j4) return true;  // weapon / attach points (Blades, chains)
    for (int c : clav) if (c >= 0 && (j == c || IsDescendant(j, c))) return true;
    return false;
}
// D-075 (owner: the axe's bottom glow "during attacks randomly near my left shoulder"): effects attached to the AXE read
// its joints through here (built from the axe object's own game placement = the game's animated axe); answered from the
// axe's world cache (the drawn axe). The hero / Blades overrides stay behind gowvr.ini fx_hooks (D-057, unverified).
std::atomic<bool> g_wflAxe{true};
std::atomic<uint64_t> g_wflAxeCalls{0}, g_wflAxeMoved{0};
std::atomic<float> g_wflAxeMax{0.f};
static bool g_wflHeroBlade = false;
void WflAxe(bool on) { g_wflAxe = on; Log("fp: effects on the axe read the drawn axe %s", on ? "ON" : "OFF"); }
void WflAxeStats() {
    Log("wflaxe: axe joint reads %llu, moved > 3 cm %llu (max %.2f m)", (unsigned long long)g_wflAxeCalls.exchange(0),
        (unsigned long long)g_wflAxeMoved.exchange(0), g_wflAxeMax.exchange(0.f));
}
// D-095 (owner: the glow at the bottom of the handle "jumps to one spot randomly during attack animations"): the read
// was answered from the axe's world CACHE, which is not always the drawn axe at that moment - a model-space pass of the
// axe (root at the origin) leaves it in model space (> 5 m off: the guard kept the GAME's answer = the game's animated
// axe, e.g. its wind-up over the left shoulder), a game pass not yet corrected leaves the game's placement. Now the
// joint's own game pose relative to the game's axe root (both from this game call, same instant) is put on the axe as
// last drawn: out = game joint x game root^-1 x drawn root (RecordAxeDrawn, <= 2 frames old). The hero's axe-gem joints
// (the pommel emitters, placed by the game on its axe) are answered the same way when read through here.
std::atomic<uint64_t> g_wflDrawn{0}, g_wflGem{0}, g_wflGemFar{0};
static bool AxeJointOnDrawn(uint8_t* axe, const Mat4& gameJ, Mat4* outJ) {
    Mat4 W0;
    if (!AxeDrawn(&W0)) return false;
    alignas(16) Mat4 g0;
    o_WorldFromLocals(axe, &g0.m[0][0], 0);  // the game's axe root now
    if (fabsf(g0.m[3][0]) + fabsf(g0.m[3][1]) + fabsf(g0.m[3][2]) < 1.f) return false;
    const float dx = gameJ.m[3][0] - g0.m[3][0], dy = gameJ.m[3][1] - g0.m[3][1], dz = gameJ.m[3][2] - g0.m[3][2];
    if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) return false;  // not on the game's axe
    *outJ = Mul(Mul(gameJ, AffineInverse(g0)), W0);
    return true;
}
uint64_t __fastcall hk_WorldFromLocals(uint8_t* skel, float* out, int joint) {
    const uint64_t rv = o_WorldFromLocals(skel, out, joint);
    if (!skel || !out || FpMode() != 1) return rv;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (joint < 0 || joint >= n) return rv;
    if (skel == g_attached[0].skel.load()) {
        ++g_wflAxeCalls;
        const float* c = (const float*)(*(uint8_t**)(skel + 0x58) + ((size_t)n + joint) * 0x40);
        const float dx = out[12] - c[12], dy = out[13] - c[13], dz = out[14] - c[14];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.03f) {
            ++g_wflAxeMoved;
            float m = g_wflAxeMax.load();
            while (d > m && !g_wflAxeMax.compare_exchange_weak(m, d)) {}
        }
        if (!g_wflAxe.load()) return rv;
        Mat4 o;
        if (AxeJointOnDrawn(skel, *(const Mat4*)out, &o)) {  // D-095
            memcpy(out, &o, 0x40);
            ++g_wflDrawn;
        } else if (d > 0.03f && d < 5.f) {
            memcpy(out, c, 0x40);  // no recent drawn axe: the cache, as D-075 (not when the axe is thrown far)
        }
        return rv;
    }
    if (skel == g_heroSkel.load() && skel == g_gemFor.load() && g_axeGem.load()) {
        bool gem = false;
        for (int i = 0; i < g_gemN; ++i) if (g_gemIdx[i] == joint) { gem = true; break; }
        if (gem) {
            ++g_wflGem;
            Mat4 o;
            uint8_t* ax = g_attached[0].skel.load();
            if (ax && AxeJointOnDrawn(ax, *(const Mat4*)out, &o)) memcpy(out, &o, 0x40);
            else ++g_wflGemFar;
        }
    }
    if (!g_wflHeroBlade) return rv;
    bool use = false;
    if (skel == g_heroSkel.load()) {
        ++g_wflHero;
        if (EditedHeroJoint(joint)) { ++g_wflHeroArm; use = true; }
    } else if (n == 48) {
        const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
        if (hh && !IsBadReadPtr(hh, 8) && hh[0] == 0xef450b59487cb7afull) { ++g_wflBlade; use = true; }
    }
    if (use && g_wflFix.load()) {
        memcpy(out, *(uint8_t**)(skel + 0x58) + ((size_t)n + joint) * 0x40, 0x40);
        ++g_wflFixed;
    }
    return rv;
}

struct EditPlan {
    uint8_t* skel = nullptr;
    std::vector<int> subtree;  // joints to translate (test)
    float dy = 0.f;
    // F7 "arms only" (owner): in gameplay first person every joint except the two arms (clavicle down to the fingers)
    // and the already-collapsed head is shrunk in place, hiding the body mesh.
    std::vector<int> hide;
    int hideTo = -1;  // >= 0: hidden joints collapse onto this joint (the neck list), else shrink in place
    // D-094: joints of `hide` left hidden after the drawing until the next hero pass puts them back (the neck / beard)
    std::vector<int> hideKeep;
    // First person: every joint under the neck anchor (face, beard, skull) collapsed onto the anchor, so the head mesh
    // shrinks to a point that still moves with the body. (Joint visibility only freezes joints: the frozen face stayed
    // where F3 was pressed and stretched - owner report.)
    std::vector<int> collapse;
    int collapseTo = -1;
    // (D-116's collapse of the head onto the neck base 323 was reverted in D-119: anchor 411 exactly on 323 = a
    // zero-length bone -> NaN in the hero's pose the next frame, body / axe / hands not drawn)
    // D-120 (owner: "a black spike of neck" in F1 / F3): skin weighted across the collapsed head (on 411, the top of the
    // neck) and the neck list (on 323, its base) / the chest was stretched up toward the view. >= 0 both: the collapsed
    // head goes to the midpoint of these two joints (the neck base 323 and the chest 296) - inside the upper chest, a
    // nonzero distance from every real joint (no zero-length bone), so that skin folds down into the body
    int collapseMidA = -1, collapseMidB = -1;
    // D-228b: the neck list for Kratos' skinning copy (head_hide_palette): shrunk onto paletteHideTo there, with the head
    // collapse, instead of in his world cache at draw time (`hide` stays empty then)
    std::vector<int> paletteHide;
    int paletteHideTo = -1;
};
std::atomic<bool> g_headCollapse{false};
SRWLOCK g_planLock = SRWLOCK_INIT;
EditPlan g_plan;
std::atomic<uint64_t> g_passes{0}, g_applied{0};
std::atomic<uint64_t> g_heroWorldPasses{0}, g_heroModelPasses{0};  // eyeprobe
std::atomic<uintptr_t> g_lastHeroCaller{0};
std::atomic<int> g_lastHeroWorld{0};
std::atomic<unsigned> g_lastHeroThread{0};
std::atomic<int64_t> g_lastHeroPassQpc{0};

bool HeadHideInDrawing();                 // D-228 (below)
static void CollapseHead(Mat4* W, int n);  // D-228 (below)
std::atomic<bool> g_paletteHooked{false};  // the skinning palette hook is in (D-170 / D-228)
void ApplyPlan(uint8_t* skel) {
    AcquireSRWLockShared(&g_planLock);
    if (skel == g_plan.skel && g_plan.dy != 0.f) {
        int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        for (int j : g_plan.subtree) ((Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40))->m[3][1] += g_plan.dy;
        ++g_applied;
    }
    if (skel == g_plan.skel && g_plan.collapseTo >= 0 && !g_plan.collapse.empty() && !HeadHideInDrawing()) {
        int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        CollapseHead((Mat4*)(mats + (size_t)(uint16_t)n * 0x40), (uint16_t)n);
    }
    ReleaseSRWLockShared(&g_planLock);
}

// D-228 (owner: "inside a cave I have like an artificial flashlight on my character - in third person it doesn't flicker,
// in first person it does: sometimes in both eyes, sometimes only the left, sometimes only the right"; "in third person
// the flashlight follows where you look"; LIVE in the owner's game, 120-frame eye captures while walking: the light went
// out in whole frames - 6 of 120 with the head hidden, 0 of 40 with "headcollapse off", still 2 of 40 with only the neck
// shown): the head was hidden by collapsing its 228 joints IN KRATOS' SKELETON (the world cache), which every reader
// sees - the cave light rides on his head, so whenever it read the collapsed head (shrunk to 0.001, moved into the chest)
// it went out. Kratos' skinning palette is built from a COPY of his world matrices (D-176, inst + 0x18): the head is now
// collapsed in that copy only - the drawn mesh has no head, his skeleton keeps the real one for lights, effects and the
// camera. gowvr.ini head_hide_palette 0 = in the skeleton, as before.
bool HeadHideInDrawing() {
    static const bool on = config::GetFloat("head_hide_palette", 1.f) > 0.5f;
    return on && g_paletteHooked.load(std::memory_order_relaxed);  // (no palette hook: in the skeleton, as before)
}
static void PaletteHeadHide(uint8_t* inst) {
    if (!HeadHideInDrawing() || !inst) return;
    AcquireSRWLockShared(&g_planLock);
    uint8_t* skel = g_plan.skel;
    if (skel && *(uint8_t**)(inst + 0x10) == skel && g_plan.collapseTo >= 0 && !g_plan.collapse.empty()) {
        if (Mat4* w = *(Mat4**)(inst + 0x18)) {
            const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            // a copy shared by several of his meshes is collapsed once (its anchor already shrunk = done)
            const Mat4& a = w[g_plan.collapseTo < n ? g_plan.collapseTo : 0];
            const float row = a.m[1][0] * a.m[1][0] + a.m[1][1] * a.m[1][1] + a.m[1][2] * a.m[1][2];
            if (g_plan.collapseTo < n && row > 0.25f) {
                // D-228b (owner: "you made it worse - maybe that's a lead"; LIVE: with only the head in the copy, 33 of
                // 120 frames shadowed - the cave light threw a big shadow from the stretched skin between the still-real
                // neck and the collapsed head; the neck was shrunk in the world cache at draw time, so whether a palette
                // got it depended on timing): the neck list is shrunk onto the neck base here too, before the head -
                // every drawn frame the same hidden head and neck
                const int to = g_plan.paletteHideTo;
                if (to >= 0 && to < n && !g_plan.paletteHide.empty()) {
                    float to3[3];
                    memcpy(to3, w[to].m[3], sizeof(to3));
                    for (int j : g_plan.paletteHide) {
                        if (j < 0 || j >= n) continue;
                        Mat4& m = w[j];
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) m.m[r][c] *= 0.001f;
                        m.m[3][0] = to3[0]; m.m[3][1] = to3[1]; m.m[3][2] = to3[2];
                    }
                }
                CollapseHead(w, n);
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true))
                    Log("fp: head hidden in Kratos' drawing only (his skinning copy %p) - his skeleton keeps the real head for "
                        "lights / effects (D-228, head_hide_palette)", (void*)w);
            }
        }
    }
    ReleaseSRWLockShared(&g_planLock);
}

// The head collapse (g_plan, under g_planLock) in an array of world matrices indexed by joint: Kratos' world cache (as
// before D-228) or his skinning copy
static void CollapseHead(Mat4* W, int n) {
    if (g_plan.collapseTo >= n) return;
    Mat4 to = W[g_plan.collapseTo];  // copy: collapsed too
    const int ma = g_plan.collapseMidA, mb = g_plan.collapseMidB;
    if (ma >= 0 && ma < n && mb >= 0 && mb < n && ma != g_plan.collapseTo && mb != g_plan.collapseTo) {  // D-120
        const Mat4* A = &W[ma];
        const Mat4* B = &W[mb];
        float p[3];
        for (int c = 0; c < 3; ++c) p[c] = 0.5f * (A->m[3][c] + B->m[3][c]);
        const float gx = p[0] - A->m[3][0], gy = p[1] - A->m[3][1], gz = p[2] - A->m[3][2];
        // finite and >= 1 cm from the neck (never a zero-length bone - D-119); else the anchor's own spot as before
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && gx * gx + gy * gy + gz * gz > 1e-4f)
            for (int c = 0; c < 3; ++c) to.m[3][c] = p[c];
    }
    const float k = 0.001f;  // not exactly zero: keeps skinned normals finite
    for (int j : g_plan.collapse) {
        if (j < 0 || j >= n) continue;
        Mat4* m = &W[j];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) m->m[r][c] = to.m[r][c] * k;
        m->m[3][0] = to.m[3][0]; m->m[3][1] = to.m[3][1]; m->m[3][2] = to.m[3][2]; m->m[3][3] = 1.f;
    }
}

// ---- joints published from COMPLETED world passes -------------------------------------------------------------
// Reading the world cache from the game thread races the job threads that rewrite it several times per frame (torn /
// intermediate matrices -> F1 camera jumping, owner report "flashes between first and third person"). The hook copies
// the few joints the mod needs after each finished pass; readers use a seqlock.
constexpr int kPubMax = 24;  // D-146: + the sheathed axe's parent joint; D-162: + the joints items hang on
struct PubCfg { uint8_t* skel = nullptr; int n = 0; int idx[kPubMax]; uint64_t hash[kPubMax]; };
SRWLOCK g_pubCfgLock = SRWLOCK_INIT;
PubCfg g_pubCfg;
std::atomic<uint32_t> g_pubSeq{0};
std::atomic_flag g_pubWriter = ATOMIC_FLAG_INIT;
Mat4 g_pubMat[kPubMax];
std::atomic<uint64_t> g_pubPass{0};

// D-190 (owner: "in F3 during a walking cinematic my head was too low"; "in the boat in F1 my head is a bit low - I'm
// inside Kratos' chest"; owner log: "F3 eye only 1.05 .. 1.20 m above Kratos' root" in narFor...cineEnter /
// narForLogBoat...CineScene, the boat's current eye "up 1.007" - Kratos' eye stands ~1.46 m above it): the head anchor
// 411 is COLLAPSED in the world cache after every pass (scale 0.001, moved between neck 323 and chest 296 - D-120's head /
// neck hide). Publish() runs before that - but a pass that does not RECOMPUTE 411 (sub_1405908a0 rebuilds a joint only
// when forced, flag 0x400 or (flags & 0x43) == 0x43 - these scenes' passes) leaves last frame's collapsed matrix in the
// cache, and that was published: its axes shrunk to 0.001, so the eye offset vanished and the view sat at the collapse
// point in the chest. Now a shrunk joint is never published as the pose: it is rebuilt from this pass's own locals up its
// parent chain to the first joint that is not shrunk (D-191: the neck base 323 is shrunk too by the neck hide - D-190
// kept the last good value then, 10426 of 10466 times in the owner log, a FROZEN head point: the body lunged ahead of it
// in attacks); with no unshrunk ancestor within 16 joints it is published as it is (as before D-190).
static bool PubShrunk(const Mat4& m) {
    return m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2] < 0.25f;
}
std::atomic<uint64_t> g_pubRebuilt{0}, g_pubKept{0};
void Publish(uint8_t* skel) {
    AcquireSRWLockShared(&g_pubCfgLock);
    if (skel == g_pubCfg.skel && g_pubCfg.n > 0 && !g_pubWriter.test_and_set(std::memory_order_acquire)) {
        int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        const uint8_t* locals = PassLocals(skel);
        g_pubSeq.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
        for (int k = 0; k < g_pubCfg.n; ++k) {
            const int j = g_pubCfg.idx[k];
            const Mat4& w = *(const Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40);
            if (!PubShrunk(w)) {
                memcpy(&g_pubMat[k], &w, sizeof(Mat4));
                continue;
            }
            bool rebuilt = false;
            if (locals) {
                Mat4 m = *(const Mat4*)(locals + (size_t)j * 0x40);
                for (int p = JointParent(j), guard = 0; p >= 0 && p < n && guard < 16; p = JointParent(p), ++guard) {
                    const Mat4& pw = *(const Mat4*)(mats + ((size_t)(uint16_t)n + p) * 0x40);
                    if (!PubShrunk(pw)) {  // the first real ancestor: compose onto its world
                        g_pubMat[k] = Mul(m, pw);
                        rebuilt = !PubShrunk(g_pubMat[k]);
                        break;
                    }
                    m = Mul(m, *(const Mat4*)(locals + (size_t)p * 0x40));
                }
            }
            if (rebuilt) {
                ++g_pubRebuilt;
            } else {
                memcpy(&g_pubMat[k], &w, sizeof(Mat4));  // as before D-190
                ++g_pubKept;
            }
        }
        g_pubSeq.fetch_add(1, std::memory_order_acq_rel);  // even: stable
        ++g_pubPass;
        static uint64_t lastLog = 0;
        if (g_pubPass.load() - lastLog >= 20000) {
            lastLog = g_pubPass.load();
            Log("fp: published joints - collapsed in the cache, rebuilt up their parent chain %llu, published as they are %llu (D-190 / D-191)",
                (unsigned long long)g_pubRebuilt.exchange(0), (unsigned long long)g_pubKept.exchange(0));
        }
        g_pubWriter.clear(std::memory_order_release);
    }
    ReleaseSRWLockShared(&g_pubCfgLock);
}

std::atomic<uint64_t> g_modelSpacePasses{0};

// diagnostic: per-caller statistics for the hero's passes (which call site is the live animated pose?)
struct CallerStat { std::atomic<uintptr_t> ret{0}; std::atomic<uint64_t> n{0}; float head[3]; float root[3]; int flag; };
CallerStat g_callers[8];
std::atomic<int> g_headIdxForStats{-1};
std::atomic<uint8_t*> g_heroSkelForStats{nullptr};

void CallerSample(uintptr_t ret, uint8_t* skel, float* root, char flag) {
    if (skel != g_heroSkelForStats.load()) return;
    for (auto& c : g_callers) {
        uintptr_t r = c.ret.load();
        if (r == 0) { if (!c.ret.compare_exchange_strong(r, ret) && r != ret) continue; }
        else if (r != ret) continue;
        ++c.n;
        int hi = g_headIdxForStats.load();
        if (hi >= 0) {
            int16_t n = *(int16_t*)(skel + 0x38);
            const float* m = (const float*)(*(uint8_t**)(skel + 0x58) + ((size_t)(uint16_t)n + hi) * 0x40);
            memcpy(c.head, m + 12, 12);
        }
        if (root) memcpy(c.root, root + 12, 12);
        c.flag = flag;
        return;
    }
}

// Hidden-body joints are shrunk AFTER the arm solve and put back before the next world pass: sub_1405908a0 rebuilds a
// joint from its parent's CACHED world whenever the parent is not recomputed, so a shrunken chest left in the cache
// would shrink the arms (and the head anchor) in later passes. Shrunk in place (position kept), scale 0.001 as the
// head collapse.
std::atomic<bool> g_hideBody{false};
std::atomic<bool> g_neckHideOn{true};
SRWLOCK g_hideLock = SRWLOCK_INIT;
struct HideSave { uint8_t* skel = nullptr; std::vector<int> joints; std::vector<Mat4> saved, shrunk; } g_hideSave;
std::atomic<uint64_t> g_hidePasses{0};
// D-082: between BodyHideDrawBegin and BodyHideDrawEnd (the frame's drawing); a hero pass finishing inside it hides again
std::atomic<bool> g_drawHide{false};
std::atomic<uint64_t> g_hideRedo{0}, g_hideEnsured{0}, g_hideRedoModel{0}, g_hideKept{0};

// D-110 (code audit M2): the skeleton HideApply last worked on (always the hero's). HideRestore runs at the start of EVERY
// skinned object's world pass; any other skeleton returns without taking the exclusive lock (it serialised every
// animation job thread on it, VR off included)
std::atomic<uint8_t*> g_hideOwner{nullptr};

void HideRestore(uint8_t* skel) {
    if (!skel || (skel != g_hideOwner.load(std::memory_order_acquire) && skel != g_heroSkel.load())) return;
    AcquireSRWLockExclusive(&g_hideLock);
    if (g_hideSave.skel == skel && !g_hideSave.joints.empty()) {
        int16_t n = *(int16_t*)(skel + 0x38);
        uint8_t* mats = *(uint8_t**)(skel + 0x58);
        for (size_t i = 0; i < g_hideSave.joints.size(); ++i)
            *(Mat4*)(mats + ((size_t)(uint16_t)n + g_hideSave.joints[i]) * 0x40) = g_hideSave.saved[i];
        g_hideSave.joints.clear();
    }
    ReleaseSRWLockExclusive(&g_hideLock);
}

void HideApply(uint8_t* skel, bool early = false) {
    if (skel) g_hideOwner.store(skel, std::memory_order_release);  // D-110 (before anything is saved / shrunk)
    AcquireSRWLockShared(&g_planLock);
    const bool on = skel == g_plan.skel && !g_plan.hide.empty();
    std::vector<int> list;
    int to = -1;
    if (on) { list = g_plan.hide; to = g_plan.hideTo; }
    ReleaseSRWLockShared(&g_planLock);
    if (!on) {
        if (!early) HideRestore(skel);  // D-094: nothing to hide - joints kept hidden after the last drawing come back
        return;
    }
    // D-092: the early re-hide only for the neck list (collapsed onto the neck, hideTo >= 0). F7's body list holds the
    // chest / spine the arm IK builds on right after (D-018: a shrunk chest shrank the arms) - only at the pass's end
    if (early && to < 0) return;
    int16_t n = *(int16_t*)(skel + 0x38);
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    // hideTo >= 0 (the neck list): every joint collapses onto that joint's position. Shrunk in place, skin bound partly
    // to a helper between neck and shoulder was pulled OUT toward it (owner: "something brown extruded poking out of
    // my left shoulder"); pulled to the neck centre it goes into the body.
    float to3[3] = {0, 0, 0};
    if (to >= 0 && to < n) memcpy(to3, ((Mat4*)(mats + ((size_t)(uint16_t)n + to) * 0x40))->m[3], sizeof(to3));
    AcquireSRWLockExclusive(&g_hideLock);
    // D-092: idempotent - applied again while still applied (the early re-hide in a pass, then the end one), a joint
    // still holding OUR shrunk value keeps its saved real value (re-saving it would save the shrunk one and the restore
    // would never bring the real pose back); a joint something rewrote since is saved and shrunk again
    const bool again = g_hideSave.skel == skel && g_hideSave.joints == list && g_hideSave.shrunk.size() == list.size();
    if (!again) {
        // D-094: joints kept hidden after the last drawing (no hero pass since put them back) and still holding OUR
        // shrunk value keep their saved real value; ones no longer listed are put back now
        std::vector<int> oldJ;
        std::vector<Mat4> oldSaved, oldShrunk;
        if (g_hideSave.skel == skel && g_hideSave.saved.size() == g_hideSave.joints.size() &&
            g_hideSave.shrunk.size() == g_hideSave.joints.size()) {
            oldJ = std::move(g_hideSave.joints);
            oldSaved = std::move(g_hideSave.saved);
            oldShrunk = std::move(g_hideSave.shrunk);
        }
        std::vector<char> carried(oldJ.size(), 0);
        g_hideSave.skel = skel;
        g_hideSave.joints = list;
        g_hideSave.saved.assign(list.size(), Mat4{});
        g_hideSave.shrunk.assign(list.size(), Mat4{});
        for (size_t i = 0; i < list.size() && !oldJ.empty(); ++i)
            for (size_t k = 0; k < oldJ.size(); ++k)
                if (oldJ[k] == list[i] && list[i] >= 0 && list[i] < n) {
                    const Mat4* m = (const Mat4*)(mats + ((size_t)(uint16_t)n + list[i]) * 0x40);
                    if (memcmp(m, &oldShrunk[k], sizeof(Mat4)) == 0) {
                        g_hideSave.saved[i] = oldSaved[k];
                        g_hideSave.shrunk[i] = oldShrunk[k];
                        carried[k] = 1;
                    }
                    break;
                }
        for (size_t k = 0; k < oldJ.size(); ++k) {
            if (carried[k] || oldJ[k] < 0 || oldJ[k] >= n) continue;
            Mat4* m = (Mat4*)(mats + ((size_t)(uint16_t)n + oldJ[k]) * 0x40);
            if (memcmp(m, &oldShrunk[k], sizeof(Mat4)) == 0) *m = oldSaved[k];
        }
    }
    static int clavs[2] = {-2, -2};
    static uint8_t* clavFor = nullptr;
    if (early && clavFor != skel) {
        clavFor = skel;
        const int sl = FindJointByHash(0x05c2b5ca793d4bf2ull), sr = FindJointByHash(0xe783988515993319ull);
        clavs[0] = sl >= 0 ? JointParent(sl) : -1;
        clavs[1] = sr >= 0 ? JointParent(sr) : -1;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        Mat4* m = (Mat4*)(mats + ((size_t)(uint16_t)n + list[i]) * 0x40);
        // early re-hide (inside a pass, before the arm IK): joints under a clavicle are rebuilt by the arm IK from their
        // parents - left real until the end re-hide (saved as they are, nothing to restore wrongly)
        if (early && ((clavs[0] >= 0 && IsDescendant(list[i], clavs[0])) || (clavs[1] >= 0 && IsDescendant(list[i], clavs[1])))) {
            if (!again && memcmp(m, &g_hideSave.shrunk[i], sizeof(Mat4)) != 0) {
                g_hideSave.saved[i] = *m;
                memset(&g_hideSave.shrunk[i], 0, sizeof(Mat4));
            }
            continue;
        }
        // still ours (applied again, or carried over from the last drawing - D-094); fresh entries hold a zero matrix
        if (memcmp(m, &g_hideSave.shrunk[i], sizeof(Mat4)) == 0) continue;
        g_hideSave.saved[i] = *m;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m->m[r][c] *= 0.001f;
        if (to >= 0 && to < n) { m->m[3][0] = to3[0]; m->m[3][1] = to3[1]; m->m[3][2] = to3[2]; }
        g_hideSave.shrunk[i] = *m;
    }
    ReleaseSRWLockExclusive(&g_hideLock);
    ++g_hidePasses;
}

// D-042: passes in progress on the hero or its attached objects (the renderer waits for them before eye 0)
std::atomic<int> g_heroInFlight{0};
struct InFlightGuard {
    bool on;
    explicit InFlightGuard(bool b) : on(b) { if (on) ++g_heroInFlight; }
    ~InFlightGuard() { if (on) --g_heroInFlight; }
};

// D-042: eye 0 is recorded while the frame's hero pass may still be running on a job thread (LIVE eyeprobe: in ~55 %
// of frames the hero pass that applies this frame's arm plan lands after kick 1 started; eye 0 then drew the previous
// frame's arms, 8-15 cm off while walking, or a half-finished pass = the raw animation, e.g. the draw animation's hand
// over the shoulder: owner "misaligns my hands a lot but only in left eye"). Before kick 1 the render thread waits
// (bounded) until the latest arm plan is in the hero's world cache and no hero/attached pass is in progress.
std::atomic<bool> g_eyeSync{true};
void EyeSync(bool on) { g_eyeSync = on; Log("eyesync: hero pose made current before eye 0 %s", on ? "ON" : "OFF"); }
// The hero's world pose is computed lazily: sub_140592770(skel, flag, force) (STATIC, 173 callers; the hero pass caller
// +0x59280b is inside it) runs the local->world pass when the skeleton's frame stamp (skel+0x48 vs DAT_141062298,
// sub_14058f1d0) is old. LIVE: in ~half the frames the first request came from eye 0's own (asynchronous) recording, so
// parts of eye 0 read the previous pose. We request it ourselves before kick 1: hero first, then the shield / axe
// (placed from the hero's IK hands by FixAttached in their passes).
using PFN_EnsureWorld = void(__fastcall*)(uint8_t* skel, char flag, char force);
std::atomic<bool> g_ensureBlades{true};  // D-052 A/B: command "ensureblades on|off"
void EnsureBlades(bool on) { g_ensureBlades = on; Log("eyesync: Blades objects re-placed after the hero %s", on ? "ON" : "OFF"); }
static bool EnsureWorldSafe(uint8_t* skel, char force = 0) {
    __try {
        ((PFN_EnsureWorld)((uint8_t*)GameVa(0x140592770ull)))(skel, 0, force);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WaitNoPass(double limitMs, int64_t freq) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    for (int spins = 0; g_heroInFlight.load() > 0; ++spins) {
        QueryPerformanceCounter(&t1);
        if ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq > limitMs) return false;
        if (spins < 200) YieldProcessor(); else Sleep(0);
    }
    return true;
}
void WaitHeroFrame() {
    if (!g_eyeSync.load() || FpMode() != 1) return;
    static int64_t freq = 0;
    static uint64_t frames = 0, timeouts = 0, faults = 0, ranPass = 0, faultKind[5] = {};
    static double sumMs = 0, maxMs = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (!WaitNoPass(3.0, freq)) ++timeouts;  // a half-finished pass must not be drawn (LIVE: 40 torn eye-0 frames)
    const uint64_t before = g_heroWorldPasses.load();
    uint8_t* sk[3] = {g_heroSkel.load(), g_attached[1].skel.load(), g_attached[0].skel.load()};
    for (int k = 0; k < 3; ++k)  // D-082: faults per object kind (hero / shield / axe)
        if (sk[k] && PlausibleSkeleton(sk[k]) && !EnsureWorldSafe(sk[k])) { ++faults; ++faultKind[k]; }
    // D-052: the Blades' own objects are placed from the hero's hands: re-placed (forced) after the hero's pass, so a
    // pass that ran earlier this frame on the previous hands is not what gets drawn
    if (g_ensureBlades.load() && CurrentWeapon() == 2) {
        uint8_t* bl[4];
        int nb = 0;
        AcquireSRWLockShared(&g_carriedLock);
        for (auto& it : g_carry) {
            const uint64_t* hh = *(const uint64_t**)(it.skel + 0x30);
            if (nb < 4 && hh && !IsBadReadPtr(hh, 8) && hh[0] == 0xef450b59487cb7afull) bl[nb++] = it.skel;
        }
        ReleaseSRWLockShared(&g_carriedLock);
        for (int i = 0; i < nb; ++i)
            if (PlausibleSkeleton(bl[i]) && !EnsureWorldSafe(bl[i], 1)) { ++faults; ++faultKind[3]; }
    }
    // D-069 (owner: "the fire effect on my axe is offset when moving"; LIVE chaindiag: the axe's effects 3-34 cm BEHIND the
    // drawn axe along the movement, i.e. up to ~5 frames old - their passes do not run every frame): the axe's live
    // effects are re-placed after the axe (AxeSatellite puts them on the drawn axe). Only effects seen in a pass within
    // the last 3 frames whose joint count / name hash still match (short-lived objects: D-058)
    // D-094: by default the cached pose is moved directly (SatDrawReplace) - the forced pass faulted on most effects
    // (gowvr.ini axefx_ensure 1 = the D-069 forced pass as well, before it)
    static const bool axeFxEnsure = config::GetFloat("axefx_ensure", 0.f) > 0.5f;
    if (g_axeFxRefresh.load() && axeFxEnsure) {
        uint8_t* fx[8];
        int nf = 0;
        const uint64_t f = g_frameNo.load();
        AcquireSRWLockShared(&g_axeCorrLock);
        for (const auto& s : g_satLearn)
            if (s.skel && f - s.seen <= 3 && nf < 8) fx[nf++] = s.skel;
        ReleaseSRWLockShared(&g_axeCorrLock);
        for (int i = 0; i < nf; ++i)
            if (AxeFxStillValid(fx[i]) && !EnsureWorldSafe(fx[i], 1)) { ++faults; ++faultKind[4]; }
    }
    if (g_axeFxRefresh.load()) SatDrawReplace();
    if (g_heroWorldPasses.load() != before) ++ranPass;
    if (!WaitNoPass(3.0, freq)) ++timeouts;
    FxLagCheck();  // D-067: effects vs the drawn joints, this frame
    HeroChainCheck();  // D-069: the forearm chains vs their wrists while moving (chaindiag)
    AxeFxLagCheck();   // D-069: the axe's effects vs the drawn axe while moving (chaindiag)
    BladeObjLagCheck();  // D-069: the Blades objects vs their weapon point in the drawn hand while moving (chaindiag)
    HandJointsCheck();   // D-069b: every joint near the hands (handjoints)
    ChainEndsCheck();    // D-070: the loose chain's end particles vs its drawn pins (chaindiag)
    ChainCorrTick();     // D-070: the next chain sim's pin shift
    AxeGemTick();        // D-073: the axe's gem glow joints on the drawn axe while moving
    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq;
    sumMs += ms;
    if (ms > maxMs) maxMs = ms;
    if (++frames % 900 == 0) {
        Log("eyesync: %llu frames, hero pass run before eye 0 in %llu, avg %.3f ms, max %.3f ms, timeouts %llu, faults %llu "
            "(hero %llu shield %llu axe %llu Blade objects %llu axe effects %llu), neck hidden again: after a pass in the "
            "draw %llu (model-space %llu), before a kick %llu, kept hidden past the drawing %llu | axe effects re-placed at "
            "the draw %llu (skipped %llu: buffer %llu family %llu rewritten %llu fault %llu expired %llu far %llu no-root %llu)"
            " | effect reads of the axe %llu (on the drawn axe %llu), of the gem joints %llu (not on the game's axe %llu)"
            " | axe effect passes: already on the drawn axe %llu, moved from the game's axe %llu, self-fed %llu, on neither %llu",
            (unsigned long long)frames, (unsigned long long)ranPass, sumMs / (double)frames, maxMs,
            (unsigned long long)timeouts, (unsigned long long)faults, (unsigned long long)faultKind[0],
            (unsigned long long)faultKind[1], (unsigned long long)faultKind[2], (unsigned long long)faultKind[3],
            (unsigned long long)faultKind[4], (unsigned long long)g_hideRedo.load(), (unsigned long long)g_hideRedoModel.load(),
            (unsigned long long)g_hideEnsured.load(), (unsigned long long)g_hideKept.load(),
            (unsigned long long)g_satReplaced.load(), (unsigned long long)g_satSkipped.load(),
            (unsigned long long)g_satWhy[0].load(), (unsigned long long)g_satWhy[1].load(), (unsigned long long)g_satWhy[2].load(),
            (unsigned long long)g_satWhy[3].load(), (unsigned long long)g_satWhy[4].load(), (unsigned long long)g_satWhy[5].load(),
            (unsigned long long)g_satWhy[6].load(), (unsigned long long)g_wflAxeCalls.load(),
            (unsigned long long)g_wflDrawn.load(), (unsigned long long)g_wflGem.load(), (unsigned long long)g_wflGemFar.load(),
            (unsigned long long)g_satOnDrawn.load(), (unsigned long long)g_satOnGame.load(), (unsigned long long)g_satSelf.load(),
            (unsigned long long)g_satAway.load());
        maxMs = 0;
    }
}

// D-054: the Blades' own objects (48 joints, hash0 ef450b59487cb7af) ride on the hero's weapon points 225-228, but the
// game places them from the same alternating bogus data (LIVE drawcheck: after attacks the right Blade object flipped
// every frame to 0.37 m beside BOTH the animated and the motion-controlled hand, "appears and disappears"). Like the
// axe: each object's offset to its weapon point is learned from good passes only (within 0.15 m of a point, not in an
// attack), then every pass of the object is placed as offset x that point's final world (our hands / the game's throw).
struct BladeObj { uint8_t* skel = nullptr; int joint = -1; Mat4 rel{}; bool have = false; uint64_t fixes = 0; };
static BladeObj g_bladeObj[4];
static SRWLOCK g_bladeObjLock = SRWLOCK_INIT;
// D-079 diagnostic "wtrace N" (owner: after an attack, moving, "the axe and the Blades misalign, then settle back"):
// for N frames (game thread), the axe object's root and the right Blade object's root in the motion-controlled right
// wrist's frame, with the combat hold state - a settling misalignment shows as a deviation that decays
std::atomic<int> g_wtrace{0};
void WeaponTrace(int n) { g_wtrace = n; Log("wtrace: next %d frames", n); }
static void WeaponTraceTick() {
    if (g_wtrace.load() <= 0) return;
    g_wtrace.fetch_sub(1);
    Mat4 w{}, e{};
    if (!ArmsIkResult(1, &w, &e)) return;
    const Mat4 wi = RigidInverse(w);
    float ax[3] = {NAN, NAN, NAN}, bl[3] = {NAN, NAN, NAN};
    auto rootRel = [&](uint8_t* s, int rootIdx, float out[3]) {
        if (!s || IsBadReadPtr(s + 0x38, 0x28)) return;
        const uint8_t* m = *(const uint8_t**)(s + 0x58);
        if (!m || IsBadReadPtr(m + (size_t)rootIdx * 0x40, 0x40)) return;
        const Mat4 rel = Mul(*(const Mat4*)(m + (size_t)rootIdx * 0x40), wi);
        for (int i = 0; i < 3; ++i) out[i] = rel.m[3][i];
    };
    if (uint8_t* s = g_attached[0].skel.load()) rootRel(s, (int)(uint16_t)*(int16_t*)(s + 0x38), ax);
    AcquireSRWLockShared(&g_bladeObjLock);
    for (auto& b : g_bladeObj)
        if (b.skel && b.joint == FindJointByHash(0x08eae59373302b22ull)) { rootRel(b.skel, 48, bl); break; }
    ReleaseSRWLockShared(&g_bladeObjLock);
    float hx, hz, hy, ox = 0.f, oz = 0.f;
    const bool held = CombatHeldRoot(&hx, &hz, &hy);
    const bool eased = !held && CombatReleaseOffset(&ox, &oz);
    float gp[3] = {}, rp[3] = {}, gy, ry;
    GameRoot(gp, &gy);
    RenderRoot(rp, &ry);
    // the DRAWN right wrist (last completed hero pass) vs the controller target, and the axe vs that drawn wrist
    Mat4 dw{};
    float handLag = NAN, axeHand = NAN;
    static int wristIdx = -1;
    if (wristIdx < 0) wristIdx = FindJointByHash(0x3626a7d4c72e93aeull);
    // (the hero's world cache, same frame as the axe object's - not StableJoint, a pass behind while walking)
    if (wristIdx >= 0 && JointWorld(wristIdx, &dw)) {
        const float dx = dw.m[3][0] - w.m[3][0], dy = dw.m[3][1] - w.m[3][1], dz = dw.m[3][2] - w.m[3][2];
        handLag = sqrtf(dx * dx + dy * dy + dz * dz);
        if (uint8_t* s = g_attached[0].skel.load(); s && !isnan(ax[0]) && !IsBadReadPtr(s + 0x38, 0x28)) {
            const uint8_t* m = *(const uint8_t**)(s + 0x58);
            const int n = (int)(uint16_t)*(int16_t*)(s + 0x38);
            if (m && !IsBadReadPtr(m + (size_t)n * 0x40, 0x40)) {
                const float* r = (const float*)(m + (size_t)n * 0x40 + 0x30);
                const float ex = r[0] - dw.m[3][0], ey = r[1] - dw.m[3][1], ez = r[2] - dw.m[3][2];
                axeHand = sqrtf(ex * ex + ey * ey + ez * ez);
            }
        }
    }
    Log("wtrace: axe %.3f %.3f %.3f | blade %.3f %.3f %.3f | hand-target %.3f axe-hand %.3f | hold %.2f held %d ease %.3f | "
        "render-game %.3f %.3f | moving %d", ax[0], ax[1], ax[2], bl[0], bl[1], bl[2], handLag, axeHand, CombatHoldWeight(),
        (int)held, eased ? sqrtf(ox * ox + oz * oz) : 0.f, rp[0] - gp[0], rp[2] - gp[2], (int)HeroMoving());
}
std::atomic<bool> g_bladeObjFix{true};  // A/B: command "bladeobj on|off"
void BladeObjFix(bool on) { g_bladeObjFix = on; Log("fp: Blades objects placed on their weapon points %s", on ? "ON" : "OFF"); }
// diagnostic "bladeobjdiag": each Blades object's learned offset on its weapon point (translation, rotation angle, axes)
static void BladeObjLagCheck() {
    if (g_chainDiag.load() <= 0 || CurrentWeapon() != 2) return;
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return;
    const float sx = g_heroStep[0], sz = g_heroStep[2], sl = sqrtf(sx * sx + sz * sz);
    if (sl < 0.02f) return;
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    static double sumA = 0, sumAbs = 0;
    static int cnt = 0;
    AcquireSRWLockShared(&g_bladeObjLock);
    for (auto& b : g_bladeObj) {
        if (!b.skel || !b.have || b.joint < 0 || b.joint >= hn || IsBadReadPtr(b.skel + 0x58, 8)) continue;
        const Mat4 want = Mul(b.rel, *(const Mat4*)(hm + ((size_t)hn + b.joint) * 0x40));
        const float* r = (const float*)(*(uint8_t**)(b.skel + 0x58) + (size_t)48 * 0x40 + 0x30);
        const float ox = r[0] - want.m[3][0], oz = r[2] - want.m[3][2];
        sumA += (ox * sx + oz * sz) / sl;
        sumAbs += sqrtf(ox * ox + oz * oz);
        ++cnt;
    }
    ReleaseSRWLockShared(&g_bladeObjLock);
    if (cnt >= 120) {
        Log("chaindiag: Blades objects while moving %.3f m/frame: vs the weapon point in the drawn hand %.3f m, along the "
            "movement %+.3f m (%d samples)", sl, sumAbs / cnt, sumA / cnt, cnt);
        sumA = sumAbs = 0;
        cnt = 0;
    }
}
// diagnostic "bladejoints": every joint of each Blades object (index, parent, hash, world, relative to its root)
void BladeJoints() {
    AcquireSRWLockShared(&g_bladeObjLock);
    for (auto& b : g_bladeObj) {
        if (!b.skel || IsBadReadPtr(b.skel + 0x58, 8)) continue;
        const uint8_t* m = *(const uint8_t**)(b.skel + 0x58);
        const uint64_t* hh = *(const uint64_t**)(b.skel + 0x30);
        const uint8_t* links = *(const uint8_t**)(b.skel + 0x20);
        const Mat4& R = *(const Mat4*)(m + (size_t)48 * 0x40);
        const Mat4 inv = InverseUniform(R);
        for (int j = 0; j < 48; ++j) {
            const float* w = (const float*)(m + (size_t)(48 + j) * 0x40 + 0x30);
            float rel[3];
            for (int c = 0; c < 3; ++c) rel[c] = w[0] * inv.m[0][c] + w[1] * inv.m[1][c] + w[2] * inv.m[2][c] + inv.m[3][c];
            const int parent = links ? (int)(int16_t)(*(const uint64_t*)(links + (size_t)j * 8) >> 48) : -9;
            Log("bladejoints: skel %p joint %2d parent %3d hash %016llx world (%.3f %.3f %.3f) rel (%.3f %.3f %.3f)", b.skel, j,
                parent, (unsigned long long)(hh ? hh[j] : 0), w[0], w[1], w[2], rel[0], rel[1], rel[2]);
        }
    }
    ReleaseSRWLockShared(&g_bladeObjLock);
}
void BladeObjDiag() {
    AcquireSRWLockShared(&g_bladeObjLock);
    for (auto& b : g_bladeObj) {
        if (!b.skel || !b.have) continue;
        const Mat4& m = b.rel;
        const float sx = sqrtf(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
        const float tr = (m.m[0][0] + m.m[1][1] + m.m[2][2]) / fmaxf(sx, 1e-6f);
        const float ang = acosf(fminf(fmaxf((tr - 1.f) * 0.5f, -1.f), 1.f)) * 57.2958f;
        Log("bladeobjdiag: skel %p joint %d fixes %llu | offset (%.3f %.3f %.3f) m, rotation %.1f deg, scale %.2f | X(%.2f %.2f %.2f) "
            "Y(%.2f %.2f %.2f) Z(%.2f %.2f %.2f)", b.skel, b.joint, (unsigned long long)b.fixes, m.m[3][0], m.m[3][1], m.m[3][2],
            ang, sx, m.m[0][0] / sx, m.m[0][1] / sx, m.m[0][2] / sx, m.m[1][0] / sx, m.m[1][1] / sx, m.m[1][2] / sx,
            m.m[2][0] / sx, m.m[2][1] / sx, m.m[2][2] / sx);
    }
    ReleaseSRWLockShared(&g_bladeObjLock);
}
static bool BladeObjectFix(uint8_t* skel, const float* root, float* out) {
    if (!g_bladeObjFix.load() || FpMode() != 1 || CurrentWeapon() != 2) return false;
    // D-075 (owner: in L2 + R1 / R2 "the blade gets close to my hand and sticks there like glue for eternity"): during
    // the Blades' aimed moves the objects are the game's (no placement, no learning - a Blade passing a hand point was
    // learned onto it); also not right after (1 s) while the Blades come back
    // D-077: exactly while the Blades are free (no fixed 1 s tail: that was the owner's "2 s floating to the right")
    if (ArmsBladesFree() || BladesFreeRaw()) return false;
    const int on = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (on != 48) return false;
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8) || hh[0] != 0xef450b59487cb7afull) return false;
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return false;
    static const uint64_t ph[4] = {0x08eae59373302b22ull, 0xb41408d8ad411e00ull, 0xb41408d8ad410271ull, 0x6fc442db7837895aull};
    const int hn = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint8_t* hm = *(const uint8_t**)(hs + 0x58);
    auto PW = [&](int j) -> const Mat4& { return *(const Mat4*)(hm + ((size_t)hn + j) * 0x40); };
    const Mat4& R = *(const Mat4*)root;
    AcquireSRWLockExclusive(&g_bladeObjLock);
    BladeObj* b = nullptr;
    for (auto& x : g_bladeObj) if (x.skel == skel) { b = &x; break; }
    if (!b) for (auto& x : g_bladeObj) if (!x.skel) { x = BladeObj{}; x.skel = skel; b = &x; break; }
    bool placed = false;
    if (b) {
        // learn from a good pass (no combat hold, no Blades throw - D-063): nearest weapon point within 0.15 m
        if (CombatHoldWeight() <= 0.001f && !BladeThrowActive() && !ArmsBladeFlipped()) {  // D-087: not while flipped
            int bj = -1;
            float bd = 0.15f * 0.15f;
            for (uint64_t h : ph) {
                const int j = FindJointByHash(h);
                if (j < 0 || j >= hn) continue;
                const Mat4& P = PW(j);
                const float dx = R.m[3][0] - P.m[3][0], dy = R.m[3][1] - P.m[3][1], dz = R.m[3][2] - P.m[3][2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bd) { bd = d2; bj = j; }
            }
            if (bj >= 0 && (b->joint < 0 || bj == b->joint)) {
                const Mat4 rel = Mul(R, InverseUniform(PW(bj)));
                if (!b->have || b->joint != bj) { b->rel = rel; b->joint = bj; b->have = true;
                    Log("fp: Blades object %p rides on hero joint %d", skel, bj); }
                else for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) b->rel.m[r][c] += (rel.m[r][c] - b->rel.m[r][c]) * 0.05f;
            }
        }
        if (b->have && b->joint >= 0 && b->joint < hn) {
            const Mat4 W = Mul(b->rel, PW(b->joint));
            memcpy(out, &W, sizeof(W));
            placed = true;
            ++b->fixes;
            const int slot = (int)(b - g_bladeObj);  // D-057: the fire on this Blade follows the same correction
            AcquireSRWLockExclusive(&g_bladeCorrLock);
            g_bladeCorr[slot].pos[0] = R.m[3][0]; g_bladeCorr[slot].pos[1] = R.m[3][1]; g_bladeCorr[slot].pos[2] = R.m[3][2];
            g_bladeCorr[slot].D = Mul(InverseUniform(R), W);
            g_bladeCorr[slot].W = W;
            g_bladeCorr[slot].frame = g_frameNo.load();
            ReleaseSRWLockExclusive(&g_bladeCorrLock);
        }
    }
    ReleaseSRWLockExclusive(&g_bladeObjLock);
    return placed;
}

// diagnostic "fxjump on|seg <tag>|off" (D-059): for every small skeleton (<= 16 joints: effects, flying copies) within
// 4 m of Kratos, the offset the MOD applied (our root - the game's root) pass to pass. A change > 5 cm between two
// passes of the same object = a discontinuity only we introduced -> motion vectors / trails stretch (owner: wind,
// flame, glow on the axe stretching during throws). Pass-local only: never dereferences an object outside its pass.
struct FxTrack { uint8_t* skel = nullptr; float off[3] = {}; uint64_t frame = 0; bool corrected = false; };
static FxTrack g_fxTrack[64];
static SRWLOCK g_fxTrackLock = SRWLOCK_INIT;
static std::atomic<bool> g_fxJumpOn{false};
static uint64_t g_fxJumpPasses = 0, g_fxJumps = 0, g_fxToggles = 0;
static float g_fxJumpMax = 0.f;
static int g_fxJumpLogged = 0;
void FxJump(const char* mode, const char* tag) {
    AcquireSRWLockExclusive(&g_fxTrackLock);
    if (!strcmp(mode, "seg"))
        Log("fxjump: [%s] effect passes %llu, correction jumps > 5 cm %llu (on/off toggles %llu), max %.2f m", tag,
            (unsigned long long)g_fxJumpPasses, (unsigned long long)g_fxJumps, (unsigned long long)g_fxToggles, g_fxJumpMax);
    g_fxJumpPasses = g_fxJumps = g_fxToggles = 0;
    g_fxJumpMax = 0.f;
    g_fxJumpLogged = 0;
    g_fxJumpOn = strcmp(mode, "off") != 0;
    ReleaseSRWLockExclusive(&g_fxTrackLock);
    AxeJumpReport(!strcmp(mode, "seg") ? tag : nullptr, true, strcmp(mode, "off") != 0);
    KinkReport(!strcmp(mode, "seg") ? tag : nullptr);
    AcquireSRWLockExclusive(&g_satDiagLock);
    if (!strcmp(mode, "seg"))
        Log("fxjump: [%s] axe effect passes %llu (self-fed %llu): locked %llu, distance-matched %llu, not corrected: no recent axe correction %llu, too far %llu",
            tag, (unsigned long long)g_satDiag.passes, (unsigned long long)g_satDiag.self, (unsigned long long)g_satDiag.locked,
            (unsigned long long)g_satDiag.matched,
            (unsigned long long)g_satDiag.noRecent, (unsigned long long)g_satDiag.tooFar);
    g_satDiag = SatDiag{};
    ReleaseSRWLockExclusive(&g_satDiagLock);
}
// D-059 v2: kinks in an object's FINAL drawn root path (world), one sample per frame (the last pass of the frame):
// second difference |p0 - 2 p1 + p2| > 0.12 m = the object snapped (a flying axe moves fast but smoothly)
struct KinkTrack { uint8_t* skel = nullptr; float p[3][3] = {}; uint64_t f[3] = {}; int n = 0; float cur[3] = {}; uint64_t curF = 0; };
static KinkTrack g_kink[48];
static SRWLOCK g_kinkLock = SRWLOCK_INIT;
static uint64_t g_kinkSamples = 0, g_kinkCount = 0;
static float g_kinkMax = 0.f;
static int g_kinkLogged = 0;
// per object kind (hash0): kinks, SPIKES (one frame away and straight back = a stretched trail) and the max
struct KinkObj { uint64_t h = 0; int joints = 0; int kinks = 0, spikes = 0; float mx = 0.f; };
static KinkObj g_kinkObj[16];
static void KinkReport(const char* tag) {
    AcquireSRWLockExclusive(&g_kinkLock);
    if (tag) {
        Log("fxjump: [%s] KINKS in drawn paths (hero, axe, shield, effects, items near Kratos): %llu of %llu frame samples, max %.2f m",
            tag, (unsigned long long)g_kinkCount, (unsigned long long)g_kinkSamples, g_kinkMax);
        for (auto& o : g_kinkObj)
            if (o.kinks) Log("fxjump: [%s]   %016llx joints %d: kinks %d spikes %d max %.2f m", tag, (unsigned long long)o.h, o.joints,
                             o.kinks, o.spikes, o.mx);
    }
    for (auto& o : g_kinkObj) o = KinkObj{};
    g_kinkSamples = g_kinkCount = 0;
    g_kinkMax = 0.f;
    g_kinkLogged = 0;
    for (auto& k : g_kink) k = KinkTrack{};
    ReleaseSRWLockExclusive(&g_kinkLock);
}
static void KinkSample(uint8_t* skel) {
    if (!g_fxJumpOn.load() || !skel || skel == g_heroSkel.load() || !g_gameRootYawValid.load()) return;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (n <= 0 || n > 400) return;
    const float* w = (const float*)(*(uint8_t**)(skel + 0x58) + (size_t)n * 0x40 + 0x30);  // world root, final
    float g[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g));
    ReleaseSRWLockShared(&g_rootLock);
    const float hx = w[0] - g[0], hz = w[2] - g[2];
    if (hx * hx + hz * hz > 25.f) return;
    const uint64_t f = g_frameNo.load();
    AcquireSRWLockExclusive(&g_kinkLock);
    KinkTrack* t = nullptr;
    int oldest = 0;
    for (int i = 0; i < 48; ++i) {
        if (g_kink[i].skel == skel) { t = &g_kink[i]; break; }
        if (g_kink[i].curF < g_kink[oldest].curF) oldest = i;
    }
    if (!t) { t = &g_kink[oldest]; *t = KinkTrack{}; t->skel = skel; }
    if (t->curF && f != t->curF) {  // the previous frame is complete: push its last sample
        for (int i = 2; i > 0; --i) { memcpy(t->p[i], t->p[i - 1], 12); t->f[i] = t->f[i - 1]; }
        memcpy(t->p[0], t->cur, 12);
        t->f[0] = t->curF;
        if (t->n < 3) ++t->n;
        if (t->n == 3 && t->f[0] - t->f[1] == 1 && t->f[1] - t->f[2] == 1) {
            float d2 = 0.f;
            for (int c = 0; c < 3; ++c) { const float a = t->p[0][c] - 2.f * t->p[1][c] + t->p[2][c]; d2 += a * a; }
            const float d = sqrtf(d2);
            ++g_kinkSamples;
            if (d > 0.12f) {
                ++g_kinkCount;
                g_kinkMax = fmaxf(g_kinkMax, d);
                const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
                const uint64_t h0 = hh && !IsBadReadPtr(hh, 8) ? hh[0] : 0ull;
                float back = 0.f, out = 0.f;  // |p0 - p2| vs |p1 - p2|
                for (int c = 0; c < 3; ++c) {
                    const float a = t->p[0][c] - t->p[2][c], b = t->p[1][c] - t->p[2][c];
                    back += a * a; out += b * b;
                }
                const bool spike = out > 0.08f * 0.08f && back < 0.25f * out;
                for (auto& o : g_kinkObj)
                    if (o.h == h0 || !o.kinks) {
                        o.h = h0; o.joints = n; ++o.kinks; o.spikes += spike; o.mx = fmaxf(o.mx, d);
                        break;
                    }
                if (g_kinkLogged++ < 25)
                    Log("fxjump: KINK %.2f m skel %p joints %d hash0 %016llx | axe %s held %d thrown %d | weapon %d hold %.2f", d, skel, n,
                        hh && !IsBadReadPtr(hh, 8) ? (unsigned long long)hh[0] : 0ull,
                        skel == g_attached[0].skel.load() ? "YES" : skel == g_attached[1].skel.load() ? "(shield)" : "no",
                        (int)g_axeInHand.load(), (int)g_axeThrown.load(), CurrentWeapon(), CombatHoldWeight());
            }
        }
    }
    memcpy(t->cur, w, 12);
    t->curF = f;
    ReleaseSRWLockExclusive(&g_kinkLock);
}
static void EffectJumpSample(uint8_t* skel, const float* root, const float* use, uintptr_t caller) {
    if (!g_fxJumpOn.load() || !root || !use || skel == g_heroSkel.load()) return;
    const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (nj > 16 || !g_gameRootYawValid.load()) return;
    float g[4];
    AcquireSRWLockShared(&g_rootLock);
    memcpy(g, g_gameRoot, sizeof(g));
    ReleaseSRWLockShared(&g_rootLock);
    const float hx = root[12] - g[0], hz = root[14] - g[2];
    if (hx * hx + hz * hz > 16.f) return;
    const float off[3] = {use[12] - root[12], use[13] - root[13], use[14] - root[14]};
    const bool corr = use != root;
    const uint64_t f = g_frameNo.load();
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    const uint64_t h0 = hh && !IsBadReadPtr(hh, 8) ? hh[0] : 0;
    AcquireSRWLockExclusive(&g_fxTrackLock);
    FxTrack* t = nullptr;
    int oldest = 0;
    for (int i = 0; i < 64; ++i) {
        if (g_fxTrack[i].skel == skel) { t = &g_fxTrack[i]; break; }
        if (g_fxTrack[i].frame < g_fxTrack[oldest].frame) oldest = i;
    }
    ++g_fxJumpPasses;
    if (t && f - t->frame <= 2) {
        const float dx = off[0] - t->off[0], dy = off[1] - t->off[1], dz = off[2] - t->off[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.05f) {
            ++g_fxJumps;
            if (corr != t->corrected) ++g_fxToggles;
            g_fxJumpMax = fmaxf(g_fxJumpMax, d);
            if (g_fxJumpLogged++ < 25)
                Log("fxjump: skel %p joints %d hash0 %016llx jump %.2f m | corrected %d -> %d | caller +0x%llx | axe held %d thrown %d "
                    "weapon %d hold %.2f", skel, nj, (unsigned long long)h0, d, (int)t->corrected, (int)corr,
                    (unsigned long long)(caller - (uintptr_t)Base()), (int)g_axeInHand.load(), (int)g_axeThrown.load(),
                    CurrentWeapon(), CombatHoldWeight());
        }
    }
    if (!t) { t = &g_fxTrack[oldest]; t->skel = skel; }
    memcpy(t->off, off, sizeof(off));
    t->frame = f;
    t->corrected = corr;
    ReleaseSRWLockExclusive(&g_fxTrackLock);
}

// diagnostic "chaindiag N" (D-069, owner: the Blades' chains are offset while walking, "try going in a circle"): each
// Blades object's joints relative to its own root, against the shape they had standing still; logs the largest change
// and its component along the hero's movement
std::atomic<int> g_chainDiag{0};
void ChainDiag(int n) { g_chainDiag = n; Log("chaindiag: next %d Blades object passes", n); }
struct ChainBase { uint8_t* skel = nullptr; float rel[48][3]; bool have = false; double sumDev = 0, sumAlong = 0; float maxDev = 0; int n = 0, maxJ = -1; };
static ChainBase g_chainBase[2];
float g_heroStepDiag[3] = {0.f, 0.f, 0.f};
static void BladeChainSample(uint8_t* skel, const float* root) {
    if (g_chainDiag.load() <= 0 || !skel || !root) return;
    if ((int)(uint16_t)*(int16_t*)(skel + 0x38) != 48) return;
    const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
    if (!hh || IsBadReadPtr(hh, 8) || hh[0] != 0xef450b59487cb7afull) return;
    ChainBase* b = nullptr;
    for (auto& x : g_chainBase) if (x.skel == skel) { b = &x; break; }
    if (!b) for (auto& x : g_chainBase) if (!x.skel) { x = ChainBase{}; x.skel = skel; b = &x; break; }
    if (!b) return;
    const Mat4& R = *(const Mat4*)root;
    const Mat4 inv = InverseUniform(R);
    const uint8_t* mats = *(const uint8_t**)(skel + 0x58);
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    const bool moving = sl > 0.02f;
    float worst = 0.f, wv[3] = {0, 0, 0};
    int wj = -1;
    for (int j = 0; j < 48; ++j) {
        const float* w = (const float*)(mats + (size_t)(48 + j) * 0x40 + 0x30);
        float rel[3];
        for (int c = 0; c < 3; ++c) rel[c] = w[0] * inv.m[0][c] + w[1] * inv.m[1][c] + w[2] * inv.m[2][c] + inv.m[3][c];
        if (!moving) {
            for (int c = 0; c < 3; ++c) b->rel[j][c] = b->have ? b->rel[j][c] + (rel[c] - b->rel[j][c]) * 0.1f : rel[c];
        } else if (b->have) {
            // (joint 4 and any joint far from the root live in world space - LIVE: hundreds of metres - not chain links)
            if (b->rel[j][0] * b->rel[j][0] + b->rel[j][1] * b->rel[j][1] + b->rel[j][2] * b->rel[j][2] > 1.2f * 1.2f) continue;
            const float d[3] = {rel[0] - b->rel[j][0], rel[1] - b->rel[j][1], rel[2] - b->rel[j][2]};
            const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > worst) { worst = dl; wj = j; for (int c = 0; c < 3; ++c) wv[c] = d[c]; }
        }
    }
    if (!moving) { b->have = true; return; }
    if (!b->have || wj < 0) return;
    // the worst joint's change back in world axes (object rows), its part along the movement
    float ww[3];
    for (int c = 0; c < 3; ++c) ww[c] = wv[0] * R.m[0][c] + wv[1] * R.m[1][c] + wv[2] * R.m[2][c];
    b->sumDev += worst;
    b->sumAlong += (ww[0] * sx + ww[2] * sz) / sl;
    if (worst > b->maxDev) { b->maxDev = worst; b->maxJ = wj; }
    if (++b->n >= 60) {
        Log("chaindiag: Blades object %p while moving %.3f m/frame: joints moved vs the standing shape - mean worst %.3f m, max %.3f m "
            "(joint %d), worst joint along the movement %+.3f m (mean)", skel, sl, b->sumDev / b->n, b->maxDev, b->maxJ,
            b->sumAlong / b->n);
        b->sumDev = b->sumAlong = 0;
        b->maxDev = 0;
        b->n = 0;
    }
    g_chainDiag.fetch_sub(1);
}
// the hero's forearm chain joints (two hash families, 10 each) in their wrist's frame at the draw, vs the standing shape
static void HeroChainCheck() {
    if (g_chainDiag.load() <= 0) return;
    uint8_t* hs = g_heroSkel.load();
    if (!hs) return;
    const int n = (int)(uint16_t)*(int16_t*)(hs + 0x38);
    const uint64_t* hh = *(const uint64_t**)(hs + 0x30);
    const uint8_t* m = *(const uint8_t**)(hs + 0x58);
    static uint8_t* cached = nullptr;
    static std::vector<int> chain;
    static int wr[2] = {-1, -1};
    if (cached != hs) {
        cached = hs;
        chain.clear();
        for (int j = 0; j < n; ++j) {
            const uint64_t fam = hh[j] >> 16;
            if (fam == (0xb6bb38650c362623ull >> 16) || fam == (0x2af813186d6c1e78ull >> 16)) chain.push_back(j);
        }
        wr[0] = FindJointByHash(0x6de47b0b82506234ull);
        wr[1] = FindJointByHash(0x3626a7d4c72e93aeull);
        Log("chaindiag: %zu forearm chain joints, wrists %d %d", chain.size(), wr[0], wr[1]);
    }
    if (chain.empty() || wr[0] < 0 || wr[1] < 0) return;
    static std::vector<std::array<float, 3>> base;
    static std::vector<int> side;
    static bool have = false;
    static double sumW = 0, sumA = 0;
    static float maxW = 0;
    static int cnt = 0;
    base.resize(chain.size());
    side.resize(chain.size(), -1);
    const Mat4* W[2] = {(const Mat4*)(m + ((size_t)n + wr[0]) * 0x40), (const Mat4*)(m + ((size_t)n + wr[1]) * 0x40)};
    const Mat4 iw[2] = {RigidInverse(*W[0]), RigidInverse(*W[1])};
    const float sx = g_heroStepDiag[0], sz = g_heroStepDiag[2], sl = sqrtf(sx * sx + sz * sz);
    const bool moving = sl > 0.02f;
    float worst = 0.f, wv[3] = {0, 0, 0};
    int wh = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        const float* p = (const float*)(m + ((size_t)n + chain[i]) * 0x40 + 0x30);
        if (side[i] < 0) {  // the nearer wrist
            float d[2];
            for (int h = 0; h < 2; ++h) {
                const float dx = p[0] - W[h]->m[3][0], dy = p[1] - W[h]->m[3][1], dz = p[2] - W[h]->m[3][2];
                d[h] = dx * dx + dy * dy + dz * dz;
            }
            side[i] = d[0] < d[1] ? 0 : 1;
        }
        const Mat4& I = iw[side[i]];
        float rel[3];
        for (int c = 0; c < 3; ++c) rel[c] = p[0] * I.m[0][c] + p[1] * I.m[1][c] + p[2] * I.m[2][c] + I.m[3][c];
        if (!moving) {
            for (int c = 0; c < 3; ++c) base[i][c] = have ? base[i][c] + (rel[c] - base[i][c]) * 0.1f : rel[c];
        } else if (have) {
            const float d[3] = {rel[0] - base[i][0], rel[1] - base[i][1], rel[2] - base[i][2]};
            const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > worst) { worst = dl; wh = side[i]; for (int c = 0; c < 3; ++c) wv[c] = d[c]; }
        }
    }
    if (!moving) { have = true; return; }
    if (!have) return;
    float ww[3];  // back to world axes (wrist rows)
    for (int c = 0; c < 3; ++c) ww[c] = wv[0] * W[wh]->m[0][c] + wv[1] * W[wh]->m[1][c] + wv[2] * W[wh]->m[2][c];
    sumW += worst;
    sumA += (ww[0] * sx + ww[2] * sz) / sl;
    maxW = fmaxf(maxW, worst);
    if (++cnt >= 60) {
        Log("chaindiag: forearm chains while moving %.3f m/frame: worst link vs its wrist %.3f m mean, %.3f max, along the "
            "movement %+.3f m", sl, sumW / cnt, maxW, sumA / cnt);
        sumW = sumA = 0;
        maxW = 0;
        cnt = 0;
    }
}
std::atomic<int> g_passOrder{0};
void PassOrder(int n) { g_passOrder = n; Log("passorder: next %d passes", n); }
thread_local uintptr_t t_passCaller = 0;
thread_local int t_passFlag = 0;
void PassInfo(uintptr_t* caller, int* flag) { *caller = t_passCaller ? t_passCaller - (uintptr_t)Base() : 0; *flag = t_passFlag; }

// D-080 (owner: "fist hits always activate the shield - nothing sends L1, it's part of the punch animation"): the
// bare-handed light attack (R1) is the game's attshieldcombolightl01 (its shield combo). LIVE: the shield object's own
// joints near its root do not move; its plates are PARKED at the world origin while folded (joints ~490 m from the
// shield root) and the punch brings them onto the arm (~230 m in a frame) - that is the shield opening. While no weapon
// is in the hand and an attack plays without block (L1) held, the shield object keeps its folded state: the joints near
// its root in the root's frame, the parked plates at their remembered world spot (remembered after 1 s of neither
// attacking nor blocking); faded back over 150 ms after the attack. Blocking opens it as always.
// gowvr.ini shield_fold_punch=0 turns it off.
static void ShieldFold(uint8_t* skel) {
    static const bool on = config::GetFloat("shield_fold_punch", 1.f) > 0.5f;
    if (!on || !skel || FpMode() != 1) return;
    const int n = (int)(uint16_t)*(int16_t*)(skel + 0x38);
    if (n < 2 || n > 128) return;
    uint8_t* mats = *(uint8_t**)(skel + 0x58);
    if (!mats) return;
    static Mat4 rel[128], abs[128];
    static bool parked[128];
    static uint8_t* refSkel = nullptr;
    static int refN = 0;
    static bool have = false;
    static ULONGLONG lastActive = 0, lastPunch = 0;
    static int logs = 0;
    const ULONGLONG now = GetTickCount64();
    const bool block = input::GameBlockHeld();
    const bool busy = CombatBusy();
    const bool bare = !AxeInHand() && CurrentWeapon() != 2;
    if (block || busy) lastActive = now;
    if (busy && bare && !block) lastPunch = now;
    const Mat4 R = *(const Mat4*)(mats + (size_t)n * 0x40);  // the shield's root joint, as placed (FixAttached)
    const ULONGLONG since = lastPunch ? now - lastPunch : 1000000ull;
    const float w = block ? 0.f : since <= 100 ? 1.f : since >= 250 ? 0.f : 1.f - (float)(since - 100) / 150.f;
    if (w <= 0.f) {
        if (!block && !busy && now - lastActive > 1000) {  // folded and settled: remember it
            const Mat4 inv = InverseUniform(R);
            for (int j = 1; j < n; ++j) {
                abs[j] = *(const Mat4*)(mats + (size_t)(n + j) * 0x40);
                rel[j] = Mul(abs[j], inv);
                const float* t = rel[j].m[3];
                parked[j] = t[0] * t[0] + t[1] * t[1] + t[2] * t[2] > 1.f;
            }
            refSkel = skel;
            refN = n;
            if (!have && logs++ < 5) {
                int np = 0;
                for (int j = 1; j < n; ++j) np += parked[j] ? 1 : 0;
                Log("fp: shield folded state remembered (%d joints, %d plates parked away from the arm)", n, np);
            }
            have = true;
        }
        return;
    }
    if (!have || refSkel != skel || refN != n) return;
    float worst = 0.f;
    for (int j = 1; j < n; ++j) {
        Mat4& W = *(Mat4*)(mats + (size_t)(n + j) * 0x40);
        const Mat4 want = parked[j] ? abs[j] : Mul(rel[j], R);
        const float dx = W.m[3][0] - want.m[3][0], dy = W.m[3][1] - want.m[3][1], dz = W.m[3][2] - want.m[3][2];
        worst = fmaxf(worst, sqrtf(dx * dx + dy * dy + dz * dz));
        if (w >= 1.f || parked[j]) { W = want; continue; }  // (a parked plate never blends in from the origin)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) W.m[r][c] += (want.m[r][c] - W.m[r][c]) * w;
        OrthonormalizeKeepScale(W);  // D-090: no shear from the blend
    }
    static float peak = 0.f;
    static ULONGLONG peakLog = 0;
    peak = fmaxf(peak, worst);
    if (peak > 0.02f && now - peakLog > 1500 && logs++ < 40) {
        Log("fp: shield kept folded during a bare-handed attack (its plates were moving in up to %.2f m)", peak);
        peakLog = now;
        peak = 0.f;
    }
}

void __fastcall hk_LocalToWorld(uint8_t* skel, uint8_t* local, float* root, char flag) {
    t_passCaller = (uintptr_t)_ReturnAddress();
    t_passFlag = flag;
    const InFlightGuard inFlight(skel && (skel == g_heroSkel.load() || skel == g_attached[0].skel.load() ||
                                          skel == g_attached[1].skel.load()));
    HideRestore(skel);  // the engine must see the real body (see above)
    CarriedRestore(skel);
    alignas(16) float r[16];  // turned copy of the root matrix (outlives the call: root points here afterwards)
    // hero world pass: measure the game's facing, then build the pose with the body turned to our yaw (BodyFollowYaw)
    if (root && skel == g_heroSkel.load() && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) {
        const float gy = atan2f(root[8], root[10]);
        // D-137 (owner: the put-away axe "stays inside my body, right chest"; owner log: the back spot learned in IDLE
        // still swung round the root between sessions of idle, (0.08 1.01 0.14) / (-0.12 1.00 0.08) / (-0.26 1.00
        // -0.12)): a rebuild pass feeds the skeleton's own cached world[0] back in as the root - for the hero that is OUR
        // drawn (turned, shifted) root. Recorded as "the game's root", it swung everything measured against the game's
        // body (carried-item spots, ...) by the body turn. Only a root from outside the skeleton's buffer is the game's.
        const int16_t hnj = *(int16_t*)(skel + 0x38);
        const uint8_t* hmats = *(const uint8_t**)(skel + 0x58);
        const bool heroFresh = !((const uint8_t*)root >= hmats && (const uint8_t*)root < hmats + (size_t)(uint16_t)hnj * 0x80);
        {
            static std::atomic<uint32_t> nFresh{0}, nSelf{0};
            static std::atomic<ULONGLONG> tLog{0};
            ++(heroFresh ? nFresh : nSelf);
            const ULONGLONG tn = GetTickCount64();
            ULONGLONG t0 = tLog.load();
            if (!t0) tLog = tn;
            else if (tn - t0 >= 10000 && tLog.compare_exchange_strong(t0, tn)) {
                const uint32_t f = nFresh.exchange(0), sf = nSelf.exchange(0);
                static std::atomic<int> logs{0};
                if (sf || logs.fetch_add(1) < 3)
                    Log("fp: hero world passes 10 s: %u with the game's root, %u fed back their own (drawn) root (not "
                        "recorded as the game's)", f, sf);
            }
        }
        // (fallback: with no fresh pass for 0.5 s - never seen - record anyway rather than freeze the game's root)
        static std::atomic<ULONGLONG> lastFresh{0};
        const ULONGLONG tnow = GetTickCount64();
        if (heroFresh) lastFresh = tnow;
        if (heroFresh || tnow - lastFresh.load() > 500) {
            g_gameRootYaw = gy;
            g_gameRootYawValid = true;
            AcquireSRWLockExclusive(&g_rootLock);
            g_gameRoot[0] = root[12]; g_gameRoot[1] = root[13]; g_gameRoot[2] = root[14]; g_gameRoot[3] = gy;
            memcpy(&g_gameRootM, root, sizeof(Mat4));  // D-057: the game's full (unturned) root this pass
            ReleaseSRWLockExclusive(&g_rootLock);
        }
        // D-065 (owner: "the body still moves a little when I attack, it must stay completely still"): the position hold
        // writes the controller once per frame, but a pass can still carry that frame's lunge; while an attack holds the
        // position the drawn body is put exactly on the held spot (horizontal; the game's height stays)
        float hx = 0.f, hz = 0.f, hy = NAN;
        const bool heldRoot = CombatHeldRoot(&hx, &hz, &hy);
        float ldx = heldRoot ? hx - root[12] : 0.f, ldz = heldRoot ? hz - root[14] : 0.f;
        const float ldy = heldRoot && !isnan(hy) ? hy - root[13] : 0.f;  // the attack's vertical root motion too
        // D-088 (owner: "when attacks end and I walk forward the axe and Blades are not in my hands for a few ms"): at a
        // hold's release the VIEW eases from the held spot onto the game's position (CombatReleaseOffset, < 0.6 m) but the
        // drawn body snapped there at once - the hand targets (relative to the view) were up to 0.6 m off the shoulders,
        // out of reach for a moment. The body now eases with the view (same offset)
        float rox = 0.f, roz = 0.f;
        const bool eased = !heldRoot && CombatReleaseOffset(&rox, &roz);
        if (eased) { ldx = rox; ldz = roz; }
        // D-114: gameplay F1 - the drawn body moved so its eye stays under the view (object frame -> world with the game
        // facing, like the view's own eye point in firstperson.cpp)
        bool eyeShift = false;
        if (FpMode() == 1) {
            const float sR = g_eyeShiftR.load(), sB = g_eyeShiftB.load();
            if (sR != 0.f || sB != 0.f) {
                const float sgy = sinf(gy), cgy = cosf(gy);
                ldx += cgy * sR + sgy * sB;
                ldz += -sgy * sR + cgy * sB;
                eyeShift = true;
            }
        }
        const bool lockRoot = heldRoot || eased || eyeShift;
        // D-174 (owner: "maybe it's the code that turns my body where I look, and the flame counts as body?"; owner: one
        // flame copy stays at one point and stretches / moves when I turn with the HEAD, the other sits on the handle):
        // the gem joints are hero joints under the root. This pass runs with the root turned / shifted to the drawn body;
        // a pass that recomputes them (the game's force flag) rebuilds them from locals the game derived against ITS root -
        // the game's axe swung round with the body. They are not body: saved before the pass, put back if it moved them,
        // then placed on the drawn axe in the live world cache at once (not only in the palette, D-170)
        Mat4 gemSave[16];
        int gemSaveN = 0;
        const bool gemGuard = FpMode() == 1 && g_gemFor.load() == skel && g_gemN > 0 && g_axeGem.load() && root &&
                              fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f;  // world-space passes only
        if (gemGuard) {
            const int hn = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            const uint8_t* hm = *(const uint8_t**)(skel + 0x58);
            for (int i = 0; i < g_gemN && i < 16; ++i) {
                const int j = g_gemIdx[i];
                if (j >= 0 && j < hn) gemSave[i] = *(const Mat4*)(hm + ((size_t)hn + j) * 0x40);
            }
            gemSaveN = g_gemN < 16 ? g_gemN : 16;
        }
        if (g_bodyOverride.load()) {
            memcpy(r, root, sizeof(r));
            const float d = WrapPiB(g_bodyYaw.load() - gy), c = cosf(d), s = sinf(d);
            for (int row = 0; row < 3; ++row) {
                const float x = r[row * 4 + 0], z = r[row * 4 + 2];
                r[row * 4 + 0] = x * c + z * s;
                r[row * 4 + 2] = z * c - x * s;
            }
            // about the vertical through the eye pivot: P = O + right * kR + back * kB (game facing)
            const float sg = sinf(gy), cg = cosf(gy), kR = g_pivotRight.load(), kB = g_pivotBack.load();
            const float px = root[12] + cg * kR + sg * kB, pz = root[14] - sg * kR + cg * kB;
            const float vx = root[12] - px, vz = root[14] - pz;
            r[12] = px + vx * c + vz * s + ldx;
            r[14] = pz + vz * c - vx * s + ldz;
            r[13] += CombatBodyLift() + ldy;
            o_LocalToWorld(skel, (uint8_t*)(t_passLocal = FreezeLocal(skel, local)), r, flag);
            root = r;
        } else if (CombatBodyLift() != 0.f || lockRoot) {  // leap under HOLD / held attack spot (after measuring the game root)
            memcpy(r, root, sizeof(r));
            r[13] += CombatBodyLift() + ldy;
            r[12] += ldx;
            r[14] += ldz;
            o_LocalToWorld(skel, (uint8_t*)(t_passLocal = FreezeLocal(skel, local)), r, flag);
            root = r;
        } else {
            o_LocalToWorld(skel, (uint8_t*)(t_passLocal = FreezeLocal(skel, local)), root, flag);
        }
        AcquireSRWLockExclusive(&g_rootLock);
        g_renderRoot[0] = root[12]; g_renderRoot[1] = root[13]; g_renderRoot[2] = root[14];
        memcpy(&g_renderRootM, root, sizeof(Mat4));  // D-229j
        g_renderRoot[3] = atan2f(root[8], root[10]);
        ReleaseSRWLockExclusive(&g_rootLock);
        if (gemGuard && gemSaveN > 0) {  // D-174
            const int hn = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            uint8_t* hm = *(uint8_t**)(skel + 0x58);
            for (int i = 0; i < gemSaveN; ++i) {
                const int j = g_gemIdx[i];
                if (j < 0 || j >= hn) continue;
                Mat4* J = (Mat4*)(hm + ((size_t)hn + j) * 0x40);
                if (memcmp(J, &gemSave[i], sizeof(Mat4)) != 0) {
                    *J = gemSave[i];
                    ++g_gpRecomputed;
                }
            }
            GemFixWorlds(skel, (float*)(hm + (size_t)hn * 0x40), 1);
        }
    } else {
        float* use = root;
        bool talismanLeft = false;  // D-229e: a talisman pass left as it is (no rule after the talisman's)
        t_ghostShift[0] = t_ghostShift[1] = 0.f;  // D-095
        t_axeGlued = false;                        // D-130: set only by this pass's back glue
        // D-149: the aiming icon's own UI objects, while head-aiming (third person too - D-103)
        if (root && skel && ReticlePlace(skel, root, r)) {
            o_LocalToWorld(skel, local, r, flag);
            t_passLocal = local;
            ++g_passes;
            return;
        }
        // D-110 (code audit M2): outside gameplay first person, with Kratos' body not turned, the mod places no other
        // object (glue, axe effects, Blades, carried items, lunge ghosts are all F1 rules): every skinned object in the
        // world went through the checks below (locks, pointer probes) on the animation job threads, VR off included.
        // The axe / shield keep the full path (their grips are learned from the game's own placement in third person).
        if (FpMode() != 1 && !g_bodyOverride.load() && skel != g_attached[0].skel.load() &&
            skel != g_attached[1].skel.load()) {
            o_LocalToWorld(skel, local, root, flag);
            t_passLocal = local;
            ++g_passes;
            return;
        }
        bool uiMeter = false;  // D-147
        if (root && skel) {
            const int njm = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            const uint64_t* hm = njm >= 4 && njm <= 64 ? *(const uint64_t**)(skel + 0x30) : nullptr;
            if (hm && !IsBadReadPtr(hm, 8)) {
                const uint64_t h = hm[0];
                if (h == kHudEnemyMeter || h == kHudEnemyMeterPasted || h == kHudBossMeter || h == kHudHealthBar ||
                    h == kHudRageBar || h == kHudSpecialMeters) {
                    HudSighting(h, skel, njm, root);
                    uiMeter = true;
                    // (a rebuild pass fed with the skeleton's own cached root already holds our placement)
                    const uint8_t* mm = *(const uint8_t**)(skel + 0x58);
                    const bool selfFed = (const uint8_t*)root >= mm && (const uint8_t*)root < mm + (size_t)njm * 0x80;
                    if ((h == kHudEnemyMeter || h == kHudEnemyMeterPasted) && !selfFed && MeterReproject(skel, root, r))
                        use = r;
                }
            }
        }
        if (!uiMeter && root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) {
            BodyGlue* gl = nullptr;
            bool glueLearn = true;
            int glueForce = 0;
            if (skel == g_attached[0].skel.load()) t_axeGlued = false;  // D-130 (set below when glued)
            const char* what = "";
            if (skel == g_attached[0].skel.load()) {
                // not in hand -> the back glue; in hand / thrown are left to the game and FixAttached. D-120 (owner,
                // after D-116: "punching with my fists the axe teleports from my back to in front of me"; owner log:
                // the sheathed axe at scale 0.94 / 1.00 in this save, the axe "lunge ghost moved back ... 1.3-1.85 m"
                // during attshieldcombolightl01): the D-025 rule "sheathed = scale < 0.9" (0.85 in that save) kept the
                // glue off, so the axe stayed at the game's lunged placement. A thrown / recalled axe is far from the
                // learned back spot: GlueRoot's distance guard lets go of it (0.3 .. 0.5 m)
                // D-128: only an axe PUT AWAY onto the back (g_axeOnBack) - not one that left the hand otherwise
                if (!g_axeInHand.load() && g_axeOnBack.load()) { gl = &g_glueAxe; what = "sheathed axe"; }
                // the back spot is learned only from an axe settled on the back: 1.5 s neither in hand nor thrown (a
                // recalled axe arriving at the hand, before it counts as caught, must not teach "the back is in front")
                static std::atomic<ULONGLONG> lastAway{0};
                const ULONGLONG now = GetTickCount64();
                if (g_axeInHand.load() || g_axeThrown.load()) lastAway = now;
                glueLearn = now - lastAway.load() > 1500 && g_axeOnBack.load();
                // D-132: the sheathed axe is SHRUNK by the game (0.85 / 0.94 depending on the save); the stray pass at the
                // hanging hand (D-129, the "hip") is full size. D-129 told them apart by distance to the hand, but in the
                // owner's save the real sheathed axe hangs within 0.4 m of that hand too: every real back pass was kept out
                // of the learning and forced - only odd far passes taught the spot (owner: left foot / left hip). Scale
                // decides now: learn only from sheathed-size passes
                const float axeScale = sqrtf(root[0] * root[0] + root[1] * root[1] + root[2] * root[2]);
                if (axeScale > 0.97f) glueLearn = false;
                // D-130: put away -> glued while the bodies are apart (1), and every pass at the hand (2) - D-132: only a
                // FULL-SIZE pass at the hand (the stray one); a sheathed-size pass there is the real axe on the back
                glueForce = 0;
                if (g_axeOnBack.load()) {
                    glueForce = 2;  // D-135: put away = on the back in every pass (the spot is reliable now)
                    Mat4 hand{};
                    if (axeScale > 0.97f && StableJoint(g_attached[0].anchorHash, &hand)) {
                        const float hx = root[12] - hand.m[3][0], hy = root[13] - hand.m[3][1], hz = root[14] - hand.m[3][2];
                        if (hx * hx + hy * hy + hz * hz < 0.35f * 0.35f) glueForce = 2;
                    }
                }
            } else {
                AcquireSRWLockShared(&g_carriedLock);
                if (std::find(g_carried.begin(), g_carried.end(), skel) != g_carried.end()) { gl = &g_glueBow; what = "bow"; }
                ReleaseSRWLockShared(&g_carriedLock);
            }
            // D-146: the put-away axe on its drawn parent joint (F1 / turned body); learned in every mode
            bool axeGlued = false;
            if (skel == g_attached[0].skel.load())
                axeGlued = AxeBackJoint(skel, root, r, gl == &g_glueAxe && (FpMode() == 1 || g_bodyOverride.load()));
            if (!axeGlued) axeGlued = gl && GlueRoot(root, *gl, what, r, glueLearn, glueForce);
            if (gl == &g_glueAxe) {
                // D-132 evidence (owner: "when I raise my hand the axe raises a little"): while the axe is on the back,
                // every 3 s - its passes by size and where they are (animated hand / motion-controlled hand), glued
                static std::atomic<uint32_t> sP{0}, sSh{0}, sFullAnim{0}, sFullIk{0}, sGlued{0}, sForced{0};
                static std::atomic<ULONGLONG> sT{0};
                const float sc = sqrtf(root[0] * root[0] + root[1] * root[1] + root[2] * root[2]);
                ++sP;
                if (sc <= 0.97f) ++sSh;
                else {
                    Mat4 an{}, wr{}, el{};
                    auto closeTo = [&](const Mat4& m) {  // ("near" is a Windows macro)
                        const float x = root[12] - m.m[3][0], y = root[13] - m.m[3][1], z = root[14] - m.m[3][2];
                        return x * x + y * y + z * z < 0.3f * 0.3f;
                    };
                    if (StableJoint(g_attached[0].anchorHash, &an) && closeTo(an)) ++sFullAnim;
                    if (ArmsIkResult(1, &wr, &el) && closeTo(wr)) ++sFullIk;
                }
                if (axeGlued) ++sGlued;
                if (glueForce == 2) ++sForced;
                const ULONGLONG now = GetTickCount64();
                ULONGLONG t0 = sT.load();
                if (!t0) sT = now;
                else if (now - t0 >= 3000 && sT.compare_exchange_strong(t0, now))
                    Log("fp: put-away axe 3 s: %u passes, %u sheathed-size, full-size %u at the animated hand / %u at the "
                        "controller hand, %u glued (%u forced), back spot %s", sP.exchange(0), sSh.exchange(0),
                        sFullAnim.exchange(0), sFullIk.exchange(0), sGlued.exchange(0), sForced.exchange(0),
                        g_glueAxe.have ? "learned" : "NOT learned");
            }
            if (axeGlued) {
                if (gl == &g_glueAxe) { memcpy(&t_axeGameRoot, root, sizeof(Mat4)); t_axeGlued = true; }  // D-130
                use = r;
                if (gl == &g_glueAxe) RecordAxeCorrection(*(const Mat4*)root, *(const Mat4*)r);  // effects follow (D-036)
            }
            const bool attachedObj = skel == g_attached[0].skel.load() || skel == g_attached[1].skel.load();
            const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            const uint64_t* h0 = nj <= 4 ? *(const uint64_t**)(skel + 0x30) : nullptr;
            Mat4 hd;
            bool talKept = false;
            if (!gl && !attachedObj && (LinkedFix(skel, root, r) || TalismanFix(skel, root, r) ||
                                        (talKept = t_talismanKept))) {
                if (!talKept) {
                    use = r;  // D-162: an item hung on Kratos (Mimir, the talisman ...) on the drawn joint
                    NoteItemCorrection(skel, root, r);  // D-229: reads through it (its light) follow
                } else {
                    talismanLeft = true;  // D-229e: the talisman already on the drawn belt - left as it is
                }
            } else if (!gl && !attachedObj && h0 && !IsBadReadPtr(h0, 8) && h0[0] == 0x65301791f418f62dull && g_flyMove.load() != 0.f) {
                memcpy(r, root, sizeof(r));  // diagnostic "flymove <m>": are these the stray fire?
                r[13] += g_flyMove.load();
                use = r;
            } else if (!gl && !attachedObj && h0 && !IsBadReadPtr(h0, 8) && h0[0] == 0x65301791f418f62dull && FpMode() == 1 &&
                       BladeCopyFix(skel, root, r)) {
                use = r;  // D-057: an attack Blades copy on its weapon point's correction
            } else if (!gl && !attachedObj && BladeObjectFix(skel, root, r)) {
                use = r;  // D-054: a Blades object on its weapon point
            } else if (!gl && !attachedObj && h0 && !IsBadReadPtr(h0, 8) && h0[0] == 0x65301791f418f62dull && FpMode() == 1 &&
                       FlameOnBlade(root, r)) {
                use = r;  // D-074: an attack flame copy takes its Blade object's correction (game Blade -> drawn Blade)
            } else if (!gl && !attachedObj && h0 && !IsBadReadPtr(h0, 8) && h0[0] == 0x65301791f418f62dull && FpMode() == 1 &&
                       (CurrentWeapon() == 2 || BladesFreeRaw() || BladeThrowActive()) &&  // D-169: the Blades' own only
                ArmsHandCorrection(root + 12, &hd)) {
                // D-169 (owner: "sometimes the flame effect gets stuck on my left hand for a few seconds"): this took ANY
                // 3-joint flame piece (hash0 65301791f418f62d - not only the Blades': other fire effects use it) within
                // 1.6 m of either animated hand and kept it on that motion-controlled hand for its life, whatever weapon
                // was out. Now only while the Blades are the weapon / thrown / in an L2 move
                // D-048: the flying Blades (spawned by Blades attacks, placed from the game's animated hand on the
                // unturned body) get the same correction as that hand: they leave from OUR fist (owner: blades
                // disappear / appear glitching during combat)
                const Mat4 W = Mul(*(const Mat4*)root, hd);
                memcpy(r, &W, sizeof(W));
                use = r;
            } else if (!gl && !attachedObj && nj <= 16 && !NotAnAxeEffect(skel) &&  // D-084/085: talisman, Blades pieces
                       AxeSatellite(skel, root, r,g_fxJumpOn.load() && h0 && !IsBadReadPtr(h0, 8) && (h0[0] >> 32) == 0x3124c569ull,
                                    (const uint8_t*)root >= *(uint8_t**)(skel + 0x58) &&
                                    (const uint8_t*)root < *(uint8_t**)(skel + 0x58) + (size_t)nj * 0x80)) {
                use = r;  // an effect riding on the axe: same correction as the axe
            } else if (!gl && !attachedObj) {
                bool cand = false;  // cheap shared check first: this runs for every skinned object in the world
                AcquireSRWLockShared(&g_carriedLock);
                for (auto& it : g_carry) if (it.skel == skel) { cand = true; break; }
                ReleaseSRWLockShared(&g_carriedLock);
                // D-036: items found by their pass (the talisman is not in the object tables - LIVE skelnear: a 7-joint
                // skeleton on the belt): anything whose root is on Kratos' rendered body becomes a candidate; the walking
                // test in GlueCarry decides whether he carries it
                bool nearBody = false;
                if (nj < 2000 && g_gameRootYawValid.load()) {
                    float rr[4];
                    AcquireSRWLockShared(&g_rootLock);
                    memcpy(rr, g_renderRoot, sizeof(rr));
                    ReleaseSRWLockShared(&g_rootLock);
                    const float dx = root[12] - rr[0], dy = root[13] - rr[1], dz = root[14] - rr[2];
                    nearBody = dx * dx + dz * dz < 0.8f * 0.8f && dy > 0.2f && dy < 2.2f;
                }
                // D-143: a new pass-found candidate only outside fights (enemies' weapons, the axe's recall trails pass
                // by Kratos then) and only a small skeleton (the talisman: 7 joints; Atreus: 529)
                const uint64_t* ht = nj <= 16 ? *(const uint64_t**)(skel + 0x30) : nullptr;  // D-162: the talisman
                const bool talisman = ht && !IsBadReadPtr(ht, 8) && ht[0] == kTalismanRootHash;
                if (!cand && nearBody && nj <= 64 && ((!CombatBusy() && CombatHoldWeight() <= 0.001f) || talisman)) {
                    {
                        AcquireSRWLockExclusive(&g_carriedLock);
                        bool have = false;
                        for (auto& it : g_carry) if (it.skel == skel) { have = true; break; }
                        if (!have && g_carry.size() < 32) {
                            CarryItem c;
                            c.skel = skel;
                            c.byPass = true;
                            c.lastPassFrame = g_frameNo.load();
                            sprintf_s(c.name, "skeleton %d joints", nj);
                            if (talisman) { c.carried = c.identity = true; strcpy_s(c.name, "talisman"); }
                            g_carry.push_back(c);
                            Log("fp: item near Kratos '%s' (skeleton %p, found by its pass) - watching whether he carries it",
                                c.name, skel);
                        }
                        ReleaseSRWLockExclusive(&g_carriedLock);
                        cand = true;
                    }
                }
                if (cand) {
                    AcquireSRWLockExclusive(&g_carriedLock);  // (writes the item's learning state)
                    for (auto& it : g_carry)
                        if (it.skel == skel) {
                            if (nearBody) it.lastPassFrame = g_frameNo.load();
                            // (no D-229 correction record here: carried items include the Blades on the back, whose
                            // flames the D-074 / D-169 rules already move onto the drawn Blades - twice would misplace them)
                            if (GlueCarry(root, it, r)) use = r;
                            break;
                        }
                    ReleaseSRWLockExclusive(&g_carriedLock);
                }
            }
        }
        // D-081 (owner, with the 2.5 m reach: "every attack makes copies of the talisman, axe, Blades ~2 m in front of me
        // that disappear when the animation ends"): while a held attack lets the game body lunge (the drawn body stays on
        // the held spot - the hero passes are shifted back by the root lock), some passes of Kratos' own items were still
        // placed by the game from the lunged body. Kratos' item families only (axe / shield objects, Blade objects and
        // their flame copies, axe effects, carried items) whose root ends up next to the lunged game body and > 1 m from
        // the drawn one get the same shift as the body (drawn root - game root). Enemies / their items never qualify.
        // D-086 (owner: "during light attacks with the Blades the left Blade disappears"; owner session log: "lunge ghost
        // moved back ... Blade object" during Blades light attacks): it also took items the mod had ALREADY put on the
        // drawn body (a Blades object on our hand, its flames, a glued carried item) - a hand reaching forward in the
        // swing sits next to the lunged game body, so the Blade in it was pulled back by the lunge, into / behind the
        // body. Only an item still at the game's own placement (use == root) is moved now
        if (!talismanLeft && use == root && use && fabsf(use[12]) + fabsf(use[13]) + fabsf(use[14]) >= 1.f &&
            CombatHoldWeight() > 0.001f) {
            float gx, gz, rx, rz;
            AcquireSRWLockShared(&g_rootLock);
            gx = g_gameRoot[0]; gz = g_gameRoot[2]; rx = g_renderRoot[0]; rz = g_renderRoot[2];
            ReleaseSRWLockShared(&g_rootLock);
            const float dx = rx - gx, dz = rz - gz;
            if (dx * dx + dz * dz > 0.5f * 0.5f) {
                const int nw = (int)(uint16_t)*(int16_t*)(skel + 0x38);
                const uint64_t* hw = *(const uint64_t**)(skel + 0x30);
                const uint64_t w0 = hw && !IsBadReadPtr(hw, 8) ? hw[0] : 0;
                bool mine = skel == g_attached[0].skel.load() || skel == g_attached[1].skel.load() ||
                            w0 == 0xef450b59487cb7afull || (nw <= 16 && w0 == 0x65301791f418f62dull) ||
                            (nw <= 16 && (w0 >> 32) == 0x3124c569ull);
                if (!mine) {  // D-143: only an item judged CARRIED (a mere candidate - an enemy's weapon next to the
                              // lunged body - was pulled onto Kratos with it)
                    AcquireSRWLockShared(&g_carriedLock);
                    for (auto& it : g_carry) if (it.skel == skel) { mine = it.carried; break; }
                    ReleaseSRWLockShared(&g_carriedLock);
                }
                // next to the lunged body and clearly closer to it than to the drawn one (the sheathed weapons on the
                // back, the talisman: ~0.3-0.6 m from the body - a lunge of only ~0.9 m must already count)
                const float ux = use[12] - gx, uz = use[14] - gz, vx = use[12] - rx, vz = use[14] - rz;
                const float dGame = sqrtf(ux * ux + uz * uz), dDrawn = sqrtf(vx * vx + vz * vz);
                if (mine && dGame < 1.8f && dGame < dDrawn - 0.3f) {
                    if (use != r) { memcpy(r, use, sizeof(r)); use = r; }
                    r[12] += dx;
                    r[14] += dz;
                    t_ghostShift[0] = dx;  // D-095: FixAttached records the axe correction from the unshifted axe
                    t_ghostShift[1] = dz;
                    static std::atomic<int> logs{0};
                    if (logs.fetch_add(1) < 40)
                        Log("fp: lunge ghost moved back onto the drawn body (skeleton %p, %d joints, %s, %.2f m)", skel, nw,
                            skel == g_attached[0].skel.load() ? "axe" : skel == g_attached[1].skel.load() ? "shield"
                            : w0 == 0xef450b59487cb7afull ? "Blade object" : (w0 >> 32) == 0x3124c569ull ? "axe effect"
                            : w0 == 0x65301791f418f62dull ? "Blade copy" : "carried item", sqrtf(dx * dx + dz * dz));
                }
            }
        }
        EffectJumpSample(skel, root, use, (uintptr_t)_ReturnAddress());
        o_LocalToWorld(skel, local, use, flag);
        if (skel && (uint16_t)*(int16_t*)(skel + 0x38) <= 16) SatNotePass(skel, use);  // D-094: for the draw-time re-place
        t_passLocal = local;
        root = use;
        BladeChainSample(skel, use);
        NoteNearSkel(skel, use);
    }
    ++g_passes;
    // diagnostic "passorder N": the order of hero / axe / Blades object / small effect passes within frames (D-067:
    // effects and chains lag a few cm behind while walking - placed from the previous frame's weapon?)
    if (g_passOrder.load() > 0 && root && skel) {
        const int nj = (int)(uint16_t)*(int16_t*)(skel + 0x38);
        const uint64_t* hh = *(const uint64_t**)(skel + 0x30);
        const uint64_t h0 = hh && !IsBadReadPtr(hh, 8) ? hh[0] : 0;
        const char* kind = skel == g_heroSkel.load() ? "HERO" : skel == g_attached[0].skel.load() ? "AXE" :
                           h0 == 0xef450b59487cb7afull ? "BLADEOBJ" : (nj <= 16 && (h0 >> 32) == 0x3124c569ull) ? "axefx" :
                           (nj <= 16 && h0 == 0x65301791f418f62dull) ? "bladecopy" : nullptr;
        if (kind && g_passOrder.fetch_sub(1) > 0) {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            Log("passorder: frame %llu t %lld %s skel %p thread %u root (%.3f %.3f %.3f)", (unsigned long long)g_frameNo.load(),
                (long long)(q.QuadPart % 100000000), kind, skel, GetCurrentThreadId(), root[12], root[13], root[14]);
        }
    }
    if (root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) {
        SkelNearSample(skel, root, (uintptr_t)_ReturnAddress());
        FlyDiagLog(skel, root);
    }
    CallerSample((uintptr_t)_ReturnAddress(), skel, root, flag);
    // Only world-space passes: a pass with the root at the origin is a model-space evaluation (world targets and the
    // published head would be wrong there). Game worlds here are hundreds of units from the origin.
    for (auto& at : g_attached)  // attached objects first: their passes may run with an identity root (world locals)
        if (skel == at.skel.load()) {
            if (root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f)
                AttDiagLog(at.hand == 1 ? "axe   " : "shield", skel, (uintptr_t)_ReturnAddress());
            FixAttached(skel, at, root, (uintptr_t)_ReturnAddress());
            if (at.hand == 1) RecordAxeDrawn(skel);  // D-095: the axe as it will be drawn (world-space passes only)
            if (at.hand == 1 && root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) GemAfterAxePass();  // D-176
            if (at.elbow && root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) ShieldFold(skel);  // D-080
            if (root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) KinkSample(skel);
            return;
        }
    if (root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) KinkSample(skel);
    // D-110 (code audit M2): everything below is the hero's (published joints, edit plan, arm IK, hide lists - each
    // checks for the hero skeleton under its own lock): any other skeleton is done here instead of taking those locks
    if (skel != g_heroSkel.load()) {
        if (g_attDiag.load() > 0 && root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f) {
            bool carried = false;
            AcquireSRWLockShared(&g_carriedLock);
            carried = std::find(g_carried.begin(), g_carried.end(), skel) != g_carried.end();
            ReleaseSRWLockShared(&g_carriedLock);
            if (carried) AttDiagLog("bow   ", skel, (uintptr_t)_ReturnAddress());
        }
        return;
    }
    if (false) {  // (legacy: carried items shrunk in their passes - replaced by the game's model visibility)
        bool carried = false;
        if (carried) {  // saved real pose restored before its next pass and right after drawing (CarriedRestore)
            const int16_t n = *(int16_t*)(skel + 0x38);
            uint8_t* mats = *(uint8_t**)(skel + 0x58);
            AcquireSRWLockExclusive(&g_carriedLock);
            CarriedSave* cs = nullptr;
            for (auto& c : g_carriedSaved) if (c.skel == skel) cs = &c;
            if (!cs) { g_carriedSaved.push_back({skel, {}}); cs = &g_carriedSaved.back(); g_carriedSavedN = (int)g_carriedSaved.size(); }
            cs->saved.resize(n);
            for (int j = 0; j < n; ++j) {
                Mat4* m = (Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40);
                cs->saved[j] = *m;
                for (int rr = 0; rr < 3; ++rr)
                    for (int c = 0; c < 3; ++c) m->m[rr][c] *= 0.001f;
            }
            ReleaseSRWLockExclusive(&g_carriedLock);
            return;
        }
    }
    if (skel == g_heroSkel.load()) {
        g_lastHeroCaller = (uintptr_t)_ReturnAddress();
        g_lastHeroThread = GetCurrentThreadId();
        g_lastHeroWorld = root && fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) >= 1.f;
    }
    if (!root || fabsf(root[12]) + fabsf(root[13]) + fabsf(root[14]) < 1.f) {
        ++g_modelSpacePasses;
        if (skel == g_heroSkel.load()) {
            ++g_heroModelPasses;
            // D-082 v3 (owner, after v2: "the neck is still flashing"): a MODEL-space pass on the hero also put the neck
            // back at its start (HideRestore, top of this hook) but returned here, before the re-hide at the end of the
            // world passes - one run while a frame draws left that eye with the real neck / beard (the head stayed hidden:
            // its collapse lives in the world passes). Shrunk again here too
            if (g_drawHide.load()) {
                HideApply(skel);
                ++g_hideRedoModel;
            }
        }
        return;
    }
    if (skel == g_heroSkel.load()) {
        ++g_heroWorldPasses;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_lastHeroPassQpc = q.QuadPart;
    }
    Publish(skel);  // the ANIMATED pose (before the head collapse and the arm IK): the camera must not see our edits
    // D-092 (owner: the neck still flashes "every ~2 s, less than before"; owner log: ~1 in 3 frames has a hero pass inside
    // the draw window): such a pass puts the real neck back at its start (HideRestore) and only hid it again at its END
    // (after ApplyPlan / ArmsApply) - the whole pass long a recording job reading the hero could draw the real neck. Now
    // hidden again as soon as the pass has written the pose and the camera has its copy (idempotent HideApply); the end
    // re-hide still catches what the arm IK rewrites
    if (g_drawHide.load() && skel == g_heroSkel.load()) HideApply(skel, true);
    ApplyPlan(skel);
    ArmsApply(skel);
    // D-082 (owner: "the bottom neck and neck beard are flashing"): the neck is shrunk for the drawing only, and every
    // hero pass first puts it back (HideRestore). A pass run while a frame draws (eyesync re-runs the hero pass before
    // eye 0 in ~15% of frames) left that frame with the real neck: it is shrunk again as the pass ends
    if (g_drawHide.load() && skel == g_heroSkel.load()) {
        HideApply(skel);
        ++g_hideRedo;
    }
    if (g_axeTrace > 0 && skel == Skeleton()) {
        --g_axeTrace;
        Log("axetrace: HERO pass t%u root(%.2f %.2f %.2f)", GetCurrentThreadId(), root[12], root[13], root[14]);
    }
}

// diagnostic "eyeprobe N": at 4 points of each double-kick frame (before kick 1 = eye 0, after it, after the
// between-eyes render-begin, after kick 2) the hero's wrists as the renderer would read them from the world cache, and
// how many hero passes ran in between. Logs frames where the two eyes' moments see hands > 2 cm apart.
std::atomic<int> g_eyeProbe{0};
struct ProbePt { float w[2][3]; uint64_t wp, mp; bool ok; };
static ProbePt g_probe[4];
void EyeProbeStart(int n) { g_eyeProbe = n; Log("eyeprobe: next %d double-kick frames", n); }
// diagnostic "drawcheck on|seg <tag>|off": at each frame's eye-0 moment, the Blades weapon points (225-228) relative to
// the IK wrist of their hand; frame-to-frame jumps > 15 cm = what the player sees flicker
static std::atomic<bool> g_dcOn{false};
static int g_dcFrames = 0, g_dcJumps[4] = {}, g_dcFar[4] = {};
static int g_dcObjFar[2] = {}, g_dcObjJumps[2] = {};
static int g_dcFireN = 0, g_dcFireOff = 0;
static float g_dcFireMax = 0.f;
static float g_dcObjPrev[2];
static bool g_dcObjHave[2] = {};
static float g_dcPrev[4][3];
static bool g_dcHave[4] = {};
void DrawCheck(const char* mode, const char* tag) {
    if (!strcmp(mode, "seg")) {
        Log("drawcheck: [%s] frames %d weapon %d | jumps R225 %d L226 %d R227 %d L228 %d | frames > 0.3 m from the hand "
            "R225 %d L226 %d R227 %d L228 %d", tag, g_dcFrames, CurrentWeapon(), g_dcJumps[0], g_dcJumps[1], g_dcJumps[2],
            g_dcJumps[3], g_dcFar[0], g_dcFar[1], g_dcFar[2], g_dcFar[3]);
        Log("drawcheck: [%s] Blade OBJECTS: frames > 0.35 m from both hands %d / %d, jumps %d / %d", tag, g_dcObjFar[0],
            g_dcObjFar[1], g_dcObjJumps[0], g_dcObjJumps[1]);
    }
    if (!strcmp(mode, "seg"))
        Log("drawcheck: [%s] FIRE: %d fire samples, %d more than 0.25 m from a Blade (max %.2f m)", tag, g_dcFireN, g_dcFireOff,
            g_dcFireMax);
    g_dcFireN = g_dcFireOff = 0;
    g_dcFireMax = 0.f;
    for (int k = 0; k < 2; ++k) { g_dcObjFar[k] = g_dcObjJumps[k] = 0; g_dcObjHave[k] = false; }
    g_dcFrames = 0;
    for (int k = 0; k < 4; ++k) { g_dcJumps[k] = g_dcFar[k] = 0; g_dcHave[k] = false; }
    g_dcOn = strcmp(mode, "off") != 0;
}
// diagnostic "drawtrace N": the next N drawn frames, per frame: time, combat hold weight, Blades points 227 (right) /
// 226 (left) in their IK wrist frame and their deviation from the pose at trace start
std::atomic<int> g_dtLeft{0};
void DrawTrace(int n) { g_dtLeft = n; Log("drawtrace: next %d frames", n); }
static void DrawTraceSample() {
    if (g_dtLeft.load() <= 0) return;
    uint8_t* s = g_heroSkel.load();
    if (!s) return;
    static float ref[2][3];
    static ULONGLONG t0 = 0;
    static int started = 0;
    const int n = (int)(uint16_t)*(int16_t*)(s + 0x38);
    const uint8_t* mats = *(const uint8_t**)(s + 0x58);
    static const uint64_t hs[2] = {0xb41408d8ad411e00ull, 0xb41408d8ad410271ull};  // 227 right, 226 left
    float p[2][3] = {}, dev[2] = {-1.f, -1.f};
    for (int k = 0; k < 2; ++k) {
        const int j = FindJointByHash(hs[k]);
        Mat4 w, e;
        if (j < 0 || j >= n || !ArmsIkResult(k == 0 ? 1 : 0, &w, &e)) continue;
        const Mat4 rel = Mul(*(const Mat4*)(mats + ((size_t)n + j) * 0x40), RigidInverse(w));
        memcpy(p[k], rel.m[3], 12);
    }
    if (!started) { memcpy(ref, p, sizeof(ref)); t0 = GetTickCount64(); started = 1; }
    for (int k = 0; k < 2; ++k)
        dev[k] = sqrtf((p[k][0] - ref[k][0]) * (p[k][0] - ref[k][0]) + (p[k][1] - ref[k][1]) * (p[k][1] - ref[k][1]) +
                       (p[k][2] - ref[k][2]) * (p[k][2] - ref[k][2]));
    Log("drawtrace: %5llu ms hold %.2f | R227 (%.2f %.2f %.2f) dev %.3f | L226 (%.2f %.2f %.2f) dev %.3f",
        (unsigned long long)(GetTickCount64() - t0), CombatHoldWeight(), p[0][0], p[0][1], p[0][2], dev[0], p[1][0], p[1][1],
        p[1][2], dev[1]);
    if (g_dtLeft.fetch_sub(1) == 1) started = 0;
}

static void DrawCheckSample() {
    DrawTraceSample();
    if (!g_dcOn.load()) return;
    uint8_t* s = g_heroSkel.load();
    if (!s) return;
    static const uint64_t hs[4] = {0x08eae59373302b22ull, 0xb41408d8ad410271ull, 0xb41408d8ad411e00ull, 0x6fc442db7837895aull};
    static const int hand[4] = {1, 0, 1, 0};
    const int n = (int)(uint16_t)*(int16_t*)(s + 0x38);
    const uint8_t* mats = *(const uint8_t**)(s + 0x58);
    ++g_dcFrames;
    if (false) {  // REMOVED (crash): fire objects are short-lived; their skeletons were read after the game freed them
        const uint64_t f = g_frameNo.load();
        uint8_t* fires[8];
        int nf = 0;
        AcquireSRWLockShared(&g_fireSeenLock);
        for (auto& fs : g_fireSeen) if (fs.skel && f - fs.frame <= 2) fires[nf++] = fs.skel;
        ReleaseSRWLockShared(&g_fireSeenLock);
        uint8_t* blades[4];
        int nb = 0;
        AcquireSRWLockShared(&g_bladeObjLock);
        for (auto& b : g_bladeObj) if (b.skel && b.have) blades[nb++] = b.skel;
        ReleaseSRWLockShared(&g_bladeObjLock);
        for (int i = 0; i < nf; ++i) {
            const int cn = (int)(uint16_t)*(int16_t*)(fires[i] + 0x38);
            const float* p = (const float*)(*(uint8_t**)(fires[i] + 0x58) + ((size_t)cn + cn - 1) * 0x40 + 0x30);
            float best = 99.f;
            for (int k = 0; k < nb; ++k) {
                const int bn = (int)(uint16_t)*(int16_t*)(blades[k] + 0x38);
                const uint8_t* bm = *(uint8_t**)(blades[k] + 0x58);
                for (int j = 0; j < bn; ++j) {
                    const float* q = (const float*)(bm + ((size_t)bn + j) * 0x40 + 0x30);
                    const float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
                    best = fminf(best, d);
                }
            }
            if (best < 50.f) {
                ++g_dcFireN;
                if (best > 0.25f) ++g_dcFireOff;
                g_dcFireMax = fmaxf(g_dcFireMax, best);
            }
        }
    }
    if (CurrentWeapon() == 2) {  // D-052: the Blades' own objects (48 joints, hash0 ef450b59487cb7af) vs the IK hands
        Mat4 w[2], e;
        const bool hv[2] = {ArmsIkResult(0, &w[0], &e), ArmsIkResult(1, &w[1], &e)};
        AcquireSRWLockShared(&g_carriedLock);
        int k = 0;
        for (auto& it : g_carry) {
            const uint64_t* hh = *(const uint64_t**)(it.skel + 0x30);
            if (!hh || IsBadReadPtr(hh, 8) || hh[0] != 0xef450b59487cb7afull || k >= 2) continue;
            const int on = (int)(uint16_t)*(int16_t*)(it.skel + 0x38);
            const float* r = (const float*)(*(uint8_t**)(it.skel + 0x58) + (size_t)on * 0x40 + 0x30);
            float best = 99.f;
            for (int h = 0; h < 2; ++h)
                if (hv[h]) {
                    const float dx = r[0] - w[h].m[3][0], dy = r[1] - w[h].m[3][1], dz = r[2] - w[h].m[3][2];
                    best = fminf(best, sqrtf(dx * dx + dy * dy + dz * dz));
                }
            if (best > 0.35f && best < 90.f) {
                ++g_dcObjFar[k];
                static int det = 0;
                if (det < 30) {
                    ++det;
                    int cnt = 0;
                    for (auto& q : g_carry) {
                        const uint64_t* h2 = *(const uint64_t**)(q.skel + 0x30);
                        if (h2 && !IsBadReadPtr(h2, 8) && h2[0] == 0xef450b59487cb7afull) ++cnt;
                    }
                    float g[4], rr[4];
                    AcquireSRWLockShared(&g_rootLock);
                    memcpy(g, g_gameRoot, sizeof(g)); memcpy(rr, g_renderRoot, sizeof(rr));
                    ReleaseSRWLockShared(&g_rootLock);
                    float lg[3], lr[3];
                    BodyLocal(r, g, lg);
                    BodyLocal(r, rr, lr);
                    // what could have placed it: animated wrists, hero chain ends / weapon points (world cache)
                    float da[2] = {-1, -1}, dj[6];
                    static const uint64_t wh[2] = {0x6de47b0b82506234ull, 0x3626a7d4c72e93aeull};
                    static const uint64_t jh[6] = {0xa112bfa7b2a4c278ull, 0x0b69651d25912452ull, 0x08eae59373302b22ull,
                                                   0xb41408d8ad411e00ull, 0xb41408d8ad410271ull, 0x6fc442db7837895aull};
                    for (int h = 0; h < 2; ++h) {
                        Mat4 a;
                        if (StableJoint(wh[h], &a))
                            da[h] = sqrtf((r[0] - a.m[3][0]) * (r[0] - a.m[3][0]) + (r[1] - a.m[3][1]) * (r[1] - a.m[3][1]) +
                                          (r[2] - a.m[3][2]) * (r[2] - a.m[3][2]));
                    }
                    uint8_t* hsk = g_heroSkel.load();
                    for (int q = 0; q < 6; ++q) {
                        dj[q] = -1.f;
                        const int j = FindJointByHash(jh[q]);
                        if (!hsk || j < 0) continue;
                        const int hn = (int)(uint16_t)*(int16_t*)(hsk + 0x38);
                        const float* m = (const float*)(*(uint8_t**)(hsk + 0x58) + ((size_t)hn + j) * 0x40 + 0x30);
                        dj[q] = sqrtf((r[0] - m[0]) * (r[0] - m[0]) + (r[1] - m[1]) * (r[1] - m[1]) + (r[2] - m[2]) * (r[2] - m[2]));
                    }
                    Log("drawcheck: OBJ far k%d skel %p dist %.2f | to animated wrist L %.2f R %.2f | to hero chainend R214 %.2f "
                        "L224 %.2f pt225 %.2f pt227 %.2f pt226 %.2f pt228 %.2f | carried %d | %d objs | combat hold %.2f", k,
                        it.skel, best, da[0], da[1], dj[0], dj[1], dj[2], dj[3], dj[4], dj[5], (int)it.carried, cnt,
                        CombatHoldWeight());
                    (void)lg; (void)lr;
                }
            }
            if (g_dcObjHave[k] && fabsf(best - g_dcObjPrev[k]) > 0.15f) ++g_dcObjJumps[k];
            g_dcObjPrev[k] = best;
            g_dcObjHave[k] = true;
            ++k;
        }
        ReleaseSRWLockShared(&g_carriedLock);
    }
    for (int k = 0; k < 4; ++k) {
        const int j = FindJointByHash(hs[k]);
        Mat4 w, e;
        if (j < 0 || j >= n || !ArmsIkResult(hand[k], &w, &e)) continue;
        const Mat4 rel = Mul(*(const Mat4*)(mats + ((size_t)n + j) * 0x40), RigidInverse(w));
        const float* p = rel.m[3];
        if (sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) > 0.3f) ++g_dcFar[k];
        if (g_dcHave[k]) {
            const float dx = p[0] - g_dcPrev[k][0], dy = p[1] - g_dcPrev[k][1], dz = p[2] - g_dcPrev[k][2];
            if (sqrtf(dx * dx + dy * dy + dz * dz) > 0.15f) ++g_dcJumps[k];
        }
        memcpy(g_dcPrev[k], p, 12);
        g_dcHave[k] = true;
    }
}

void EyeProbe(int stage) {
    if (stage == 0) DrawCheckSample();
    if (stage == 0 || stage == 2) TalismanDrawSample(stage / 2);  // D-229g (right before each eye's kick)
    if (g_eyeProbe.load() <= 0) return;
    uint8_t* s = g_heroSkel.load();
    ProbePt& p = g_probe[stage & 3];
    p.ok = false;
    {   // timing: stage times and where the frame's hero passes fall (ms relative to kick 1 start)
        static int64_t t[4] = {}, prevEnd = 0, freq = 0;
        static uint64_t before = 0, prevWp = 0;
        static double sumK1 = 0, sumBe = 0, sumK2 = 0, sumLag = 0;
        static int nT = 0, nBefore = 0, nBetween = 0;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        t[stage & 3] = q.QuadPart;
        const uint64_t wp = g_heroWorldPasses.load();
        if (stage == 0) { if (prevWp && wp != prevWp) ++nBefore; }
        if (stage == 3) {
            prevWp = wp;
            auto ms = [&](int64_t a, int64_t b) { return (double)(b - a) * 1000.0 / (double)freq; };
            sumK1 += ms(t[0], t[1]); sumBe += ms(t[1], t[2]); sumK2 += ms(t[2], t[3]);
            const int64_t lp = g_lastHeroPassQpc.load();
            if (lp > t[0] && lp < t[2]) { ++nBetween; sumLag += ms(t[0], lp); }
            if (++nT % 200 == 0)
                Log("eyeprobe: timing over %d frames: kick1 %.2f ms, between %.2f ms, kick2 %.2f ms | frames with a hero "
                    "pass before kick1 %d, between the eyes %d (last pass %.2f ms after kick1 start)", nT, sumK1 / nT,
                    sumBe / nT, sumK2 / nT, nBefore, nBetween, nBetween ? sumLag / nBetween : 0.0);
        }
        (void)prevEnd; (void)before;
    }
    p.wp = g_heroWorldPasses.load();
    p.mp = g_heroModelPasses.load();
    static const int wr[2] = {FindJointByHash(0x6de47b0b82506234ull), FindJointByHash(0x3626a7d4c72e93aeull)};
    if (!s || wr[0] < 0 || wr[1] < 0) return;
    const int n = (int)(uint16_t)*(int16_t*)(s + 0x38);
    const uint8_t* mats = *(const uint8_t**)(s + 0x58);
    for (int h = 0; h < 2; ++h) memcpy(p.w[h], mats + ((size_t)n + wr[h]) * 0x40 + 0x30, 12);
    p.ok = true;
    if (stage == 0 || stage == 2) {  // what each eye is about to draw vs the latest arm solve
        static uint64_t off[2] = {0, 0}, seen[2] = {0, 0};
        const int e = stage == 0 ? 0 : 1;
        ++seen[e];
        for (int k = 0; k < 2; ++k) {  // shield (on the IK forearm) / axe (in the IK hand): what this eye draws
            uint8_t* os = g_attached[k].skel.load();
            Mat4 ikw, ike;
            if (!os || !ArmsIkResult(g_attached[k].hand, &ikw, &ike)) continue;
            if (k == 0 && !g_axeInHand.load()) continue;
            const Mat4& A = g_attached[k].elbow ? ike : ikw;
            const int on = (int)(uint16_t)*(int16_t*)(os + 0x38);
            const float* R = (const float*)(*(uint8_t**)(os + 0x58) + (size_t)on * 0x40);
            const float dx = R[12] - A.m[3][0], dy = R[13] - A.m[3][1], dz = R[14] - A.m[3][2];
            const float dd = sqrtf(dx * dx + dy * dy + dz * dz), sc = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
            static uint64_t farN[2][2] = {};
            if ((dd > 0.45f || sc < 0.5f) && ++farN[e][k] <= 40)
                Log("eyeprobe: eye %d draws the %s %.3f m from its IK anchor (scale %.2f)", e, k ? "shield" : "axe", dd, sc);
        }
        for (int h = 0; h < 2; ++h) {
            Mat4 ikw, ike;
            if (!ArmsIkResult(h, &ikw, &ike)) continue;
            const float dx = p.w[h][0] - ikw.m[3][0], dy = p.w[h][1] - ikw.m[3][1], dz = p.w[h][2] - ikw.m[3][2];
            const float dd = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dd > 0.05f && ++off[e] <= 40)
                Log("eyeprobe: eye %d draws the %s wrist %.3f m from the arm solve | last hero pass caller +0x%llx world %d "
                    "thread %u", e, h ? "right" : "left", dd, (unsigned long long)(g_lastHeroCaller.load() - (uintptr_t)Base()),
                    (int)g_lastHeroWorld.load(), g_lastHeroThread.load());
        }
    }
    if (stage != 3) return;
    static uint64_t frames = 0, bad = 0;
    ++frames;
    auto D = [](const float* a, const float* b) {
        return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    const ProbePt &a = g_probe[0], &b = g_probe[1], &c = g_probe[2], &d = g_probe[3];
    if (a.ok && b.ok && c.ok && d.ok) {
        const float k1 = fmaxf(D(a.w[0], b.w[0]), D(a.w[1], b.w[1]));   // during kick 1
        const float be = fmaxf(D(b.w[0], c.w[0]), D(b.w[1], c.w[1]));   // between eyes
        const float k2 = fmaxf(D(c.w[0], d.w[0]), D(c.w[1], d.w[1]));   // during kick 2
        const float e01 = fmaxf(D(a.w[0], c.w[0]), D(a.w[1], c.w[1]));  // eye0 start vs eye1 start
        if (fmaxf(fmaxf(k1, be), fmaxf(k2, e01)) > 0.02f && ++bad <= 60)
            Log("eyeprobe: frame %llu wrists moved: during k1 %.3f, between %.3f, during k2 %.3f, k1start-k2start %.3f m "
                "| hero passes world %llu/%llu/%llu model %llu/%llu/%llu | AxeInHand %d", (unsigned long long)frames, k1, be,
                k2, e01, (unsigned long long)(b.wp - a.wp), (unsigned long long)(c.wp - b.wp),
                (unsigned long long)(d.wp - c.wp), (unsigned long long)(b.mp - a.mp), (unsigned long long)(c.mp - b.mp),
                (unsigned long long)(d.mp - c.mp), (int)g_axeInHand.load());
    }
    if (g_eyeProbe.fetch_sub(1) == 1)
        Log("eyeprobe: done, %llu of %llu frames with hands moving between the eyes' render moments",
            (unsigned long long)bad, (unsigned long long)frames), frames = bad = 0;
}

// D-043: the player's weapon mode as the game keeps it (STATIC GetCurrentWeapon 0x140827410 -> sub_140827e90: the
// case-folded hash at [[0x1422e7d50]+8]+0x49f0 compared with "Axe" / "Blades" / "Bare"; LIVE: Axe after Y / D-pad
// right, Blades after D-pad left). 0 unknown, 1 axe, 2 blades, 3 bare.
int CurrentWeapon() {
    static const uint64_t kAxe = JointHash("Axe"), kBlades = JointHash("Blades"), kBare = JointHash("Bare");
    uint8_t* p = G<uint8_t*>(0x1422e7d50);
    if (!p || IsBadReadPtr(p + 8, 8)) return 0;
    uint8_t* q = *(uint8_t**)(p + 8);
    if (!q || IsBadReadPtr(q + 0x49f0, 8)) return 0;
    const uint64_t h = *(uint64_t*)(q + 0x49f0);
    return h == kAxe ? 1 : h == kBlades ? 2 : h == kBare ? 3 : 0;
}

bool InBoat() { return g_inBoat.load(); }
uint64_t HeroWorldPassCount() { return g_heroWorldPasses.load(); }  // D-083 loadwatch
// Draw-time shrink (F7 arms only, neck): applied just before the frame's render kicks and put back right after them.
// Shrunk in the pass hook instead, other game code rebuilt the arms from the shrunk chest in between (LIVE: F7 arms
// "all centered in the middle of the body and shrunk"); at draw time every joint's pose is final.
void BodyHideDrawBegin() {
    g_drawHide = true;
    if (uint8_t* s = g_heroSkel.load()) HideApply(s);
}
// D-082: right before each eye's kick - a hero pass that put the neck back and never reached its end (a fault inside
// the game's pass, or one on another thread) leaves it unhidden; shrunk again here
void BodyHideEnsure() {
    uint8_t* s = g_heroSkel.load();
    if (!s || !g_drawHide.load()) return;
    AcquireSRWLockShared(&g_planLock);
    const bool want = s == g_plan.skel && !g_plan.hide.empty();
    const size_t wantN = g_plan.hide.size();
    ReleaseSRWLockShared(&g_planLock);
    if (!want) return;
    AcquireSRWLockShared(&g_hideLock);
    // D-094: the full list (a remainder kept hidden past the last drawing alone is not applied)
    const bool applied = g_hideSave.skel == s && g_hideSave.joints.size() == wantN;
    ReleaseSRWLockShared(&g_hideLock);
    if (applied) return;
    HideApply(s);
    ++g_hideEnsured;
}
// D-094 (owner: "the neck only appears now in the right eye"): kick 2 (eye 1, the right eye) returns before its
// recording jobs finish (F-006: they can't be waited for before the frame tail), so putting the neck back right after it
// let the late-recorded beard / neck draw the real neck in the right eye only (eye 0's jobs are drained between the
// eyes). The neck list (plan.hideKeep) stays hidden until the next hero pass puts it back at its start (HideRestore),
// as the head collapse does; nothing of ours reads it in between (StableJoint copies). F7's chest / spine are still put
// back here (D-082: game code rebuilt the arms from a shrunk chest)
void BodyHideDrawEnd() {
    g_drawHide = false;
    if (uint8_t* s = g_heroSkel.load()) {
        AcquireSRWLockShared(&g_planLock);
        std::vector<int> keep;
        if (s == g_plan.skel) keep = g_plan.hideKeep;
        ReleaseSRWLockShared(&g_planLock);
        if (keep.empty()) {
            HideRestore(s);
        } else {
            std::sort(keep.begin(), keep.end());
            AcquireSRWLockExclusive(&g_hideLock);
            if (g_hideSave.skel == s && !g_hideSave.joints.empty() && g_hideSave.saved.size() == g_hideSave.joints.size() &&
                g_hideSave.shrunk.size() == g_hideSave.joints.size()) {
                const int16_t n = *(int16_t*)(s + 0x38);
                uint8_t* mats = *(uint8_t**)(s + 0x58);
                size_t w = 0;
                for (size_t i = 0; i < g_hideSave.joints.size(); ++i) {
                    const int j = g_hideSave.joints[i];
                    Mat4* m = (Mat4*)(mats + ((size_t)(uint16_t)n + j) * 0x40);
                    if (std::binary_search(keep.begin(), keep.end(), j) && memcmp(m, &g_hideSave.shrunk[i], sizeof(Mat4)) == 0) {
                        g_hideSave.joints[w] = j;  // stays hidden, saved real value kept for the next pass's restore
                        g_hideSave.saved[w] = g_hideSave.saved[i];
                        g_hideSave.shrunk[w] = g_hideSave.shrunk[i];
                        ++w;
                    } else {
                        *m = g_hideSave.saved[i];
                    }
                }
                g_hideSave.joints.resize(w);
                g_hideSave.saved.resize(w);
                g_hideSave.shrunk.resize(w);
                if (w) ++g_hideKept;
            }
            ReleaseSRWLockExclusive(&g_hideLock);
        }
    }
    CarriedRestore(nullptr);
}
// Owner 2026-10-19: the F7 "arms only" key is removed altogether - the whole body always shows in first person (a
// gowvr.ini hide_body=1 left from an older version is ignored, g_hideBody stays off)
bool HideBody() { return false; }
void AxeTrace(int n) { g_axeTrace = n; Log("axetrace: next %d passes", n); }
void AxeDiag(int n) { g_axeDiag = n; Log("axediag: next %d axe passes", n); }
bool AxeInHand() { return g_axeInHand.load(); }
void SetCarriedHide(bool on) { g_bowHide = on; Log("fp: carried item hiding %s", on ? "on" : "off"); }
void SetNeckHide(bool on) { g_neckHideOn = on; Log("fp: neck hiding %s", on ? "on" : "off"); }
void AxeRequest(bool toHand) { (toHand ? g_axeCallAt : g_axeSheatheAt) = GetTickCount64(); }

bool NarrativeCarry() {
    static const uint64_t h = JointHash("NarrativeCarry");  // same case-folded hash the game uses for state tags
    uint8_t* obj = HeroObject();
    if (!obj) return false;
    using PFN_HasTag = char(__fastcall*)(uint8_t* obj, uint64_t hash);  // sub_1405041e0
    return ((PFN_HasTag)((uint8_t*)GameVa(0x1405041e0ull)))(obj, h) != 0;
}
void SetHeadCollapsed(bool on) { g_headCollapse = on; }

// D-229: a published hero joint by its index (the joints Kratos' items hang on are published)
bool PubJointByIndex(int j, Mat4* out) {
    uint64_t hash = 0;
    bool found = false;
    AcquireSRWLockShared(&g_pubCfgLock);
    for (int i = 0; i < g_pubCfg.n; ++i)
        if (g_pubCfg.idx[i] == j) { hash = g_pubCfg.hash[i]; found = true; }
    ReleaseSRWLockShared(&g_pubCfgLock);
    return found && StableJoint(hash, out);
}
bool StableJoint(uint64_t hash, Mat4* out) {
    int k = -1;
    AcquireSRWLockShared(&g_pubCfgLock);
    for (int i = 0; i < g_pubCfg.n; ++i)
        if (g_pubCfg.hash[i] == hash) k = i;
    ReleaseSRWLockShared(&g_pubCfgLock);
    if (k < 0 || g_pubPass.load() == 0) return false;
    for (int tries = 0; tries < 64; ++tries) {
        uint32_t a = g_pubSeq.load(std::memory_order_acquire);
        if (a & 1) { YieldProcessor(); continue; }
        Mat4 m = g_pubMat[k];
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_pubSeq.load(std::memory_order_acquire) == a) { *out = m; return true; }
    }
    return false;
}

void BodyPostAnim() {
    YawTestTick();
    ThrowTimeTick();
    MoveLogTick();
    CombatTick();
    PosDiagTick();
    WeaponTraceTick();
    TsDumpTick();
    {  // D-067: the hero's drawn root movement over the last frame (effect prediction lead)
        static float prev[3];
        static bool have = false;
        float rp[3], ry;
        if (RenderRoot(rp, &ry)) {
            if (have) for (int i = 0; i < 3; ++i) g_heroStep[i] = g_heroStepDiag[i] = rp[i] - prev[i];
            {  // D-072: moving = stepped > 5 mm within the last 20 frames (walk cycles have slow moments)
                static int still = 0;
                const float st = sqrtf(g_heroStep[0] * g_heroStep[0] + g_heroStep[2] * g_heroStep[2]);
                still = st > 0.005f ? 0 : still + 1;
                g_heroMoving = still < 20;
            }
            memcpy(prev, rp, sizeof(prev));
            have = true;
        }
    }
    uint64_t fno = ++g_frameNo;
    if (fno % 120 == 1) FindAttachedObjects();  // cheap table walk; objects stream in/out with levels
    CarriedVisibility();
    for (auto& at : g_attached) {  // learned grips persist in gowvr.ini (game thread only)
        AcquireSRWLockExclusive(&at.lock);
        char k[64];
        if (!at.loaded) {
            at.loaded = true;
            sprintf_s(k, "%s_valid", at.key);
            if (config::GetFloat(k, 0.f) > 0.5f) {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 3; ++c) { sprintf_s(k, "%s_%d%d", at.key, r, c); at.offset.m[r][c] = config::GetFloat(k, 0.f); }
                at.offset.m[3][3] = 1.f;
                at.haveOffset = at.calibrated = true;
                at.baseOffset = at.offset;  // D-150: the normal grip
                at.haveBase = true;
                Log("fp: %s grip loaded from gowvr.ini (offset %.3f %.3f %.3f)", at.name, at.offset.m[3][0], at.offset.m[3][1], at.offset.m[3][2]);
            }
        }
        if (at.dirty) {
            at.dirty = false;
            // D-110: one write for the 13 values (it was 13 full rewrites of gowvr.ini in a row)
            static char keys[13][64];
            std::vector<std::pair<const char*, float>> kv;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c) {
                    char* key = keys[r * 3 + c];
                    sprintf_s(key, 64, "%s_%d%d", at.key, r, c);
                    kv.push_back({key, at.offset.m[r][c]});
                }
            sprintf_s(keys[12], 64, "%s_valid", at.key);
            kv.push_back({keys[12], 1.f});
            config::SetFloats(kv);
            Log("fp: %s grip learned from the game's own placement and saved (offset %.3f %.3f %.3f)", at.name,
                at.offset.m[3][0], at.offset.m[3][1], at.offset.m[3][2]);
        }
        ReleaseSRWLockExclusive(&at.lock);
    }
    if (fno % 1800 == 2) {
        auto avg = [](double s, uint64_t n) { return n ? s / (double)n : -1.0; };
        Log("fp: attached axe fixes=%llu away=%llu inHand=%d calibrated=%d dAnim=%.3f dIk=%.3f | shield fixes=%llu away=%llu "
            "calibrated=%d dAnim=%.3f dIk=%.3f", (unsigned long long)g_attached[0].fixes.load(),
            (unsigned long long)g_attached[0].detached.load(), (int)g_axeInHand.load(), (int)g_attached[0].calibrated,
            avg(g_attached[0].sumDAnim, g_attached[0].nD), avg(g_attached[0].sumDIk, g_attached[0].nD),
            (unsigned long long)g_attached[1].fixes.load(), (unsigned long long)g_attached[1].detached.load(),
            (int)g_attached[1].calibrated, avg(g_attached[1].sumDAnim, g_attached[1].nD), avg(g_attached[1].sumDIk, g_attached[1].nD));
    }
    static bool installed = false;
    if (!installed) {
        installed = true;
        DetourInstall(kLocalToWorld, (void*)&hk_LocalToWorld, (void**)&o_LocalToWorld);
        // D-057 experiments (effect hooks) not installed: the build with them was laggy / crashed (owner)
        g_wflHeroBlade = config::GetFloat("fx_hooks", 0.f) > 0.5f;
        if (g_wflHeroBlade || config::GetFloat("wfl_axe", 1.f) > 0.5f) {  // D-075: installed for the axe's effects
            if (DetourInstall(kWorldFromLocals, (void*)&hk_WorldFromLocals, (void**)&o_WorldFromLocals))
                g_wflTrampoline = (void*)o_WorldFromLocals;
        }
        // D-067: the effect-attachment hook alone (effects on the weapons one frame behind while walking)
        // (default OFF: LIVE, walking with the axe / Blades it was never called for them - not the lagging effects)
        if (config::GetFloat("fx_hooks", 0.f) > 0.5f || config::GetFloat("fx_attach_hook", 0.f) > 0.5f)
            DetourInstall(kFxAttach, (void*)&hk_FxAttach, (void**)&o_FxAttach);
        DetourInstall(kBoatDockState, (void*)&hk_BoatDock, (void**)&o_BoatDock);
        // D-170: the axe glow's joints in Kratos' GPU palette; D-228: the hidden head (head_hide_palette)
        if (config::GetFloat("gem_palette_hook", 1.f) > 0.5f || config::GetFloat("head_hide_palette", 1.f) > 0.5f) {
            g_paletteHooked = DetourInstall(kPaletteBuild, (void*)&hk_PaletteBuild, (void**)&o_PaletteBuild);
            Log("fp: skinning palette hook (axe glow joints, hidden head) %s", g_paletteHooked.load() ? "installed" : "FAILED");
        }
        // D-165: the axe glow's joints on the drawn axe when READ; D-229: reads through items on Kratos (their lights)
        if (config::GetFloat("gem_link_hook", 1.f) > 0.5f || config::GetFloat("drawn_item_links", 1.f) > 0.5f)
            Log("fp: link reads hook (axe glow joints, items on Kratos) %s",
                DetourInstall(kLinkWorld, (void*)&hk_LinkWorld, (void**)&o_LinkWorld) ? "installed" : "FAILED");
        if (config::GetFloat("gem_set_hook", 1.f) > 0.5f)  // D-164: the axe glow's joints on the drawn axe when written
            Log("fp: axe glow joint writes hook %s",
                DetourInstall(kSetJointWorld, (void*)&hk_SetJointWorld, (void**)&o_SetJointWorld) ? "installed" : "FAILED");
        if (config::GetFloat("chain_hook", 1.f) > 0.5f)
            DetourInstall(kChainUpdate, (void*)&hk_ChainUpdate, (void**)&o_ChainUpdate);
    }
    uint8_t* skel = Skeleton();
    {   // joints published from completed passes: head, root, neck, shoulders, wrists
        static const uint64_t kPub[] = {0xf2648c102f472469ull, 0xfe57caacf699afe4ull, 0xf14d9a553fdad7ccull,
                                        0x05c2b5ca793d4bf2ull, 0xe783988515993319ull, 0x6de47b0b82506234ull,
                                        0x3626a7d4c72e93aeull, 0xa46a6b3097288180ull /*L elbow*/,
                                        0x21ead304ebcdad0bull /*R elbow*/, 0xb41408d8ad410271ull /*JOMeleeWeapon2*/,
                                        0xf1bc142992438c01ull /*JOBack3 = chest 296*/,
                                        0xf1bc14299243b4e3ull /*joint 271, lower back (belt items, D-033)*/};
        PubCfg c;
        c.skel = skel;
        for (uint64_t h : kPub) {
            int i = skel ? FindJointByHash(h) : -1;
            if (i >= 0 && c.n < kPubMax) { c.idx[c.n] = i; c.hash[c.n] = h; ++c.n; }
        }
        // D-146: the joint the sheathed axe hangs on (from its attachment link)
        const int bj = g_axeBackJoint.load();
        const uint64_t* hh = skel ? *(const uint64_t**)(skel + 0x30) : nullptr;
        if (bj >= 0 && hh && bj < (int)(uint16_t)*(int16_t*)(skel + 0x38) && c.n < kPubMax) {
            c.idx[c.n] = bj; c.hash[c.n] = hh[bj]; ++c.n;
        }
        if (hh) {  // D-162: the joints Kratos' linked items hang on (Mimir: JOMimirWeapon1 ...)
            const int hn = (int)(uint16_t)*(int16_t*)(skel + 0x38);
            AcquireSRWLockShared(&g_linkedLock);
            for (int i = 0; i < g_linkedCount && c.n < kPubMax; ++i) {
                const int j = g_linked[i].joint;
                bool dup = false;
                for (int k = 0; k < c.n; ++k) if (c.idx[k] == j) dup = true;
                if (!dup && j >= 0 && j < hn) { c.idx[c.n] = j; c.hash[c.n] = hh[j]; ++c.n; }
            }
            ReleaseSRWLockShared(&g_linkedLock);
        }
        AcquireSRWLockExclusive(&g_pubCfgLock);
        if (c.skel != g_pubCfg.skel) { g_pubPass = 0; g_inBoat = false; }  // new hero (level/save load): fresh state
        g_pubCfg = c;
        ReleaseSRWLockExclusive(&g_pubCfgLock);
    }
    EditPlan p;
    p.skel = skel;
    if (skel && g_armTestDy != 0.f) {
        int wrist = FindJointByHash(0x3626a7d4c72e93aeull);  // right wrist
        int16_t n = *(int16_t*)(skel + 0x38);
        for (int j = 0; j < n; ++j)
            if (IsDescendant(j, wrist)) p.subtree.push_back(j);
        p.dy = g_armTestDy;
    }
    if (skel && g_headCollapse.load()) {
        static uint8_t* cachedSkel = nullptr;
        static std::vector<int> cached;
        static int cachedAnchor = -1;
        if (skel != cachedSkel) {
            cachedSkel = skel;
            cached.clear();
            // Head anchor 411 and everything under it, plus the upper-neck helper 410 (its sibling). NOT the neck bone
            // 323 itself: some engine passes rebuild 411 from its parent, so a collapsed 323 dragged the camera anchor
            // into the chest (owner: F1 showed only skin).
            cachedAnchor = FindJointByHash(0xf2648c102f472469ull);
            int16_t n = *(int16_t*)(skel + 0x38);
            // The anchor itself too: skull, upper teeth, gums and eye sockets are skinned to it (owner screenshot).
            // The camera still sees its real pose - Publish() copies it before ApplyPlan() collapses it.
            if (cachedAnchor >= 0)
                for (int j = 0; j < n; ++j)
                    if (j == cachedAnchor || IsDescendant(j, cachedAnchor)) cached.push_back(j);
            if (cachedAnchor >= 0) {  // 410: the other child of the anchor's parent (upper neck / beard base)
                int par = JointParent(cachedAnchor);
                for (int j = 0; j < n; ++j)
                    if (j != cachedAnchor && JointParent(j) == par && !IsDescendant(j, cachedAnchor)) cached.push_back(j);
            }
            Log("fp: head collapse list %zu joints under anchor %d", cached.size(), cachedAnchor);
        }
        p.collapse = cached;
        p.collapseTo = cachedAnchor;
        if (g_neckHideOn.load()) {  // D-120
            p.collapseMidA = FindJointByHash(0xf14d9a553fdad7ccull);  // neck 323
            p.collapseMidB = FindJointByHash(kChestHash);             // chest 296 (JOBack3)
        }
    }
    {   // Shrink lists, built from a CLEAN pose (third person: no IK, no collapse in the cache), rebuilt per hero.
        // - F7 "arms only": every joint except the arm chains AND every joint lying inside the arm volume. Kratos' arm
        //   skin is also bound to helper joints parented to the chest/spine (LIVE: hiding all but the two clavicle
        //   subtrees shrank the arms to ~5 cm), so "inside the arm" is measured: within 9 cm of the clavicle-shoulder-
        //   elbow-wrist segments.
        // - Neck (F1/F3, whenever the head is hidden; owner: "remove the upper neck and bottom neck beard"): the neck
        //   bone 323, everything under it that the head collapse does not take, and any other joint (not an ancestor
        //   of the head) within 7 cm of the 323 -> 411 segment. Shrinking 323 in the cache once dragged the head anchor
        //   into the chest (passes rebuild 411 from the cached 323); the shrink list is put back before every pass
        //   (HideRestore), so the passes always see the real neck.
        static bool neckLoaded = false;
        if (!neckLoaded) { neckLoaded = true; g_neckHideOn = config::GetFloat("neck_hide", 1.f) > 0.5f; }
        const bool neckHide = g_neckHideOn.load();
        static uint8_t* lSkel = nullptr;
        static bool lClean = false;
        static std::vector<int> bodyList, neckList;
        const bool cleanNow = FpMode() == 0 && !g_headCollapse.load();
        if (skel && (skel != lSkel || (!lClean && cleanNow))) {
            lSkel = skel;
            lClean = cleanNow;
            bodyList.clear();
            neckList.clear();
            int16_t n = *(int16_t*)(skel + 0x38);
            struct P3 { float x, y, z; };
            std::vector<P3> pos(n);
            for (int j = 0; j < n; ++j) {
                Mat4 m{};
                JointWorld(j, &m);
                pos[j] = {m.m[3][0], m.m[3][1], m.m[3][2]};
            }
            auto segDist = [&](int j, P3 a, P3 b2) {
                P3 d{b2.x - a.x, b2.y - a.y, b2.z - a.z}, q{pos[j].x - a.x, pos[j].y - a.y, pos[j].z - a.z};
                float dd = d.x * d.x + d.y * d.y + d.z * d.z, t = dd > 1e-8f ? (q.x * d.x + q.y * d.y + q.z * d.z) / dd : 0.f;
                t = fmaxf(0.f, fminf(1.f, t));
                P3 e{q.x - d.x * t, q.y - d.y * t, q.z - d.z * t};
                return sqrtf(e.x * e.x + e.y * e.y + e.z * e.z);
            };
            std::vector<char> arm(n, 0), armMain(n, 0), head(n, 0), headAnc(n, 0), nearArm(n, 0);
            const uint64_t kChain[2][3] = {{0x05c2b5ca793d4bf2ull, 0xa46a6b3097288180ull, 0x6de47b0b82506234ull},
                                           {0xe783988515993319ull, 0x21ead304ebcdad0bull, 0x3626a7d4c72e93aeull}};
            for (auto& ch : kChain) {
                int sh = FindJointByHash(ch[0]), el = FindJointByHash(ch[1]), wr = FindJointByHash(ch[2]);
                int clav = sh >= 0 ? JointParent(sh) : -1;
                if (clav < 0 || el < 0 || wr < 0) continue;
                for (int j = 0; j < n; ++j) {
                    if (j == clav || IsDescendant(j, clav)) arm[j] = 1;
                    if (j == clav || j == sh || IsDescendant(j, sh)) armMain[j] = 1;  // the chain itself and the arm
                    if (segDist(j, pos[clav], pos[sh]) < 0.09f || segDist(j, pos[sh], pos[el]) < 0.09f ||
                        segDist(j, pos[el], pos[wr]) < 0.09f)
                        nearArm[j] = 1;
                }
            }
            // D-092 (owner: "F7 does not make the lower body disappear"): the arm volume is measured in the clean
            // third-person idle, where the hands hang beside the thighs - hip / thigh / kilt joints within 9 cm of the
            // forearms counted as arm helpers and stayed. Arm-volume helpers are only kept above the elbows' height
            // (shoulder, armpit, upper arm: the chest / back helpers the arm skin is bound to - D-017)
            {
                float elbowY = 1e9f;
                for (auto& ch : kChain) {
                    const int el = FindJointByHash(ch[1]);
                    if (el >= 0 && el < n) elbowY = fminf(elbowY, pos[el].y);
                }
                if (elbowY < 1e8f)
                    for (int j = 0; j < n; ++j)
                        if (nearArm[j] && !arm[j] && pos[j].y < elbowY) nearArm[j] = 0;
            }
            const int anchor = FindJointByHash(0xf2648c102f472469ull), neck = FindJointByHash(0xf14d9a553fdad7ccull);
            if (anchor >= 0) {
                const int par = JointParent(anchor);
                for (int j = 0; j < n; ++j) {
                    if (j == anchor || IsDescendant(j, anchor) || (JointParent(j) == par && j != anchor)) head[j] = 1;
                    if (j != anchor && IsDescendant(anchor, j)) headAnc[j] = 1;  // chest, spine, root chain
                }
            }
            // D-094 (owner: "F7 now hides body but it also hid my axe and blades, I see the axe during hits"): weapons
            // (axe, Blades, shield, forearm chains) ride on the attach joints under joint 4 (world-space root of the 47
            // JO* points, D-034). The D-092 elbow rule dropped the ones at the hanging hands from the arm volume -
            // shrunk, the axe drawn from its attach point was 0.001 big (and FixAttached took it as put away) unless a
            // combat hold placed it from the hand. The attach POINTS under joint 4 are never part of the body.
            // D-094b (owner: "F7 now doesn't do anything"; log: "344 weapon attach joints kept"): joint 4's two children
            // that have children of their own (202, 243 - "skeleton bones", arms.cpp FindAttachJoints) carry most of the
            // body; only joint 4's childless children (the JO* points: weapons, forearm chains, holsters) are kept
            const int attachRoot = FindJointByHash(0x169277bfd5da3f44ull);
            std::vector<char> hasKids(n, 0);
            for (int j = 0; j < n; ++j) {
                const int pj = JointParent(j);
                if (pj >= 0 && pj < n) hasKids[pj] = 1;
            }
            int keptNear = 0, keptAttach = 0;
            for (int j = 1; j < n; ++j) {  // joint 0 is the root the pass is built from
                if (!arm[j] && nearArm[j]) ++keptNear;
                const bool attach = attachRoot >= 0 && JointParent(j) == attachRoot && !hasKids[j];
                if (attach && !arm[j] && !head[j] && !nearArm[j]) ++keptAttach;
                if (!arm[j] && !head[j] && !nearArm[j] && !attach) bodyList.push_back(j);
                // (not filtered by the arm volume: the clavicles start at the base of the neck). Shrinking happens at
                // draw time only, so a helper under a clavicle can go (owner: "a little spike left of the neck") while
                // the clavicle, the arm chain and everything below the shoulders stay.
                if (neck >= 0 && anchor >= 0 && !armMain[j] && !head[j] &&
                    (j == neck || IsDescendant(j, neck) || (!headAnc[j] && segDist(j, pos[neck], pos[anchor]) < 0.09f)))
                    neckList.push_back(j);
            }
            for (int j : neckList)  // F7 hides the neck too
                if (std::find(bodyList.begin(), bodyList.end(), j) == bodyList.end()) bodyList.push_back(j);
            std::sort(bodyList.begin(), bodyList.end());
            Log("fp: shrink lists (%s pose): arms-only hides %zu joints (%d arm-volume helpers, %d weapon attach joints "
                "kept), neck %zu joints", cleanNow ? "clean" : "NOT clean", bodyList.size(), keptNear, keptAttach,
                neckList.size());
        }
        if (skel && g_hideBody.load() && FpMode() == 1) {  // F7: gameplay first person only
            p.hide = bodyList;
            p.hideKeep = neckList;
        } else if (skel && g_headCollapse.load() && neckHide && HeadHideInDrawing()) {  // D-228b: in the skinning copy
            p.paletteHide = neckList;
            p.paletteHideTo = FindJointByHash(0xf14d9a553fdad7ccull);
        } else if (skel && g_headCollapse.load() && neckHide) {  // F1 / F3: the neck with the head, onto the neck base
            p.hide = neckList;
            p.hideTo = FindJointByHash(0xf14d9a553fdad7ccull);
            p.hideKeep = neckList;
        }
    }
    AcquireSRWLockExclusive(&g_planLock);
    g_plan = std::move(p);
    ReleaseSRWLockExclusive(&g_planLock);
    static uint64_t k = 0;
    g_heroSkelForStats = skel;
    if (skel != g_heroSkel.load()) g_gameRootYawValid = false;  // new hero: measure again
    g_heroSkel = skel;
    g_headIdxForStats = skel ? FindJointByHash(0xaf26b97f3448d61aull) : -1;
    if (false) {  // diagnostic (F-016): per-caller pass stats
        for (auto& c : g_callers)
            if (c.ret.load())
                Log("fp: pass caller 0x%llx n=%llu flag=%d head(%.3f %.3f %.3f) root(%.3f %.3f %.3f)",
                    (unsigned long long)(c.ret.load() - (uintptr_t)Base() + 0x140000000ull), (unsigned long long)c.n.load(),
                    c.flag, c.head[0], c.head[1], c.head[2], c.root[0], c.root[1], c.root[2]);
    }
    if (++k % 900 == 1)
        Log("fp: body passes=%llu modelSpace=%llu published=%llu applied=%llu", (unsigned long long)g_passes.load(),
            (unsigned long long)g_modelSpacePasses.load(), (unsigned long long)g_pubPass.load(), (unsigned long long)g_applied.load());
}

void ArmTest(float dy) { g_armTestDy = dy; Log("fp: armtest dy=%.2f", dy); }

void DumpJoints() {
    uint8_t* hero = G<uint8_t*>(kHeroPtr);
    uint8_t* skel = Skeleton();
    if (!skel) { Log("fp: no hero skeleton (hero=%p)", hero); return; }
    int16_t n = *(int16_t*)(skel + 0x38);
    uint64_t* hashes = *(uint64_t**)(skel + 0x30);
    using PFN_Valid = char(__fastcall*)(uint8_t*);
    Log("fp: hero=%p skel=%p joints=%d hashes=%p mats=%p worldCacheValid=%d", hero, skel, n, hashes, *(void**)(skel + 0x58),
        (int)((PFN_Valid)((uint8_t*)GameVa(0x14058f1d0ull)))(skel));
    for (int i = 0; i < n && i < 1200; ++i) {
        const char* name = "";
        for (const char* c : kCandidates)
            if (JointHash(c) == hashes[i]) { name = c; break; }
        Mat4 m{};
        JointWorld(i, &m);
        uint64_t link = *(uint64_t*)(*(uint8_t**)(skel + 0x20) + (size_t)i * 8);
        Log("fp: joint %3d parent=%4d hash=%016llx %-12s pos(%9.3f %9.3f %9.3f) Y(%6.3f %6.3f %6.3f) Z(%6.3f %6.3f %6.3f)", i,
            (int)(int16_t)(link >> 48), (unsigned long long)hashes[i], name, m.m[3][0], m.m[3][1], m.m[3][2], m.m[1][0], m.m[1][1], m.m[1][2],
            m.m[2][0], m.m[2][1], m.m[2][2]);
    }
}

}  // namespace gowvr::fp
