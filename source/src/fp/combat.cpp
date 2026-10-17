#include "combat.h"
#include "../core/addr.h"
#include "body.h"
#include "firstperson.h"
#include "movelog.h"
#include "../input/vpad.h"
#include "../core/log.h"
#include "../core/config.h"
#include "../core/detour.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace gowvr::fp {
namespace {

constexpr uint64_t kNeckHash = 0xf14d9a553fdad7ccull;

enum class Policy { None, Hold, Animate };
const char* PolicyName(Policy p) { return p == Policy::Hold ? "HOLD" : p == Policy::Animate ? "ANIMATE" : "none"; }

std::atomic<bool> g_on{true};
std::atomic<float> g_holdW{0.f}, g_lift{0.f}, g_rise{0.f};
std::atomic<bool> g_armsAnim{false};
float g_camOff[3] = {0.f, 0.f, 0.f};
SRWLOCK g_camLock = SRWLOCK_INIT;

// ---- D-079 hit log (owner: "the hitting of enemies works but only ~20 % of the time registers"). STATIC: weapon hits
// go through sub_14088ba00 (OnHitGameObject / OnWeaponHitGameObject / HitCreature), called directly from 7 sites: the
// thrown axe (tThrowable), a shared helper, three weapon-attachment methods and the Blades' chain (tChainedWeapon).
// The hit spheres ride a hero joint (CombatCollision::HitJoint) between the attack animation's CombatCollision On / Off
// events, i.e. in F1 they follow the DRAWN pose (frozen body + the VR weapon). Each call site gets a relay that counts /
// logs the hit (which path, how far into the held attack) and calls the handler.
std::atomic<ULONGLONG> g_finisherUntil{0};  // D-080: an R3 press (stun finisher / lock-on): no holds until then
std::atomic<ULONGLONG> g_finisherAt{0};     // D-154: that press
std::atomic<bool> g_ownFollowUp{false};     // D-154: Kratos' own attack follow-up plays (cs*)
std::atomic<ULONGLONG> g_lastAttackAt{0};   // D-154: the last attack move start
std::atomic<int> g_attackHits{0};          // hits during the current held attack
std::atomic<float> g_attackGameT{0.f};      // game time played of the current held attack (s)
std::atomic<ULONGLONG> g_attackStartMs{0};  // its start (0 = none)
std::atomic<int> g_hitLogs{0};
using PFN_Hit = uint64_t(__fastcall*)(int64_t, int64_t, int64_t, uint32_t);
PFN_Hit g_hitFn = nullptr;
const char* const kHitPath[7] = {"thrown axe", "thrown axe 2", "helper", "weapon A", "weapon B", "weapon C", "Blades chain"};
void OnHit(int path, int64_t target, uint32_t p4) {
    const ULONGLONG t0 = g_attackStartMs.load();
    if (t0) ++g_attackHits;
    if (g_hitLogs.fetch_add(1) >= 400) return;
    if (t0)
        Log("combat: HIT via %s (target %p, %u) - %llu ms / %.2f s game time into the held attack", kHitPath[path],
            (void*)target, p4, (unsigned long long)(GetTickCount64() - t0), g_attackGameT.load());
    else
        Log("combat: HIT via %s (target %p, %u) - no held attack", kHitPath[path], (void*)target, p4);
}
template <int I>
uint64_t __fastcall HitRelay(int64_t a, int64_t b, int64_t c, uint32_t d) {
    if (b) OnHit(I, b, d);  // the handler returns at once without a target
    return g_hitFn(a, b, c, d);
}
constexpr HookSite kHitSites[7] = {
    {"Hit_throwA", 0x1406b8f23, 5, {0xe8, 0xd8, 0x2a, 0x1d, 0x00, 0x48, 0x83, 0x7e, 0x08, 0x00, 0x0f, 0x84, 0xaa, 0x01, 0x00, 0x00}},
    {"Hit_throwB", 0x1406b97a5, 5, {0xe8, 0x56, 0x22, 0x1d, 0x00, 0xeb, 0x26, 0x48, 0x8d, 0x54, 0x24, 0x20, 0x49, 0x8b, 0xc8, 0xe8}},
    {"Hit_helper", 0x1406b9e30, 5, {0xe8, 0xcb, 0x1b, 0x1d, 0x00, 0x48, 0x85, 0xf6, 0x74, 0x56, 0x0f, 0xb6, 0x83, 0xa0, 0x15, 0x00}},
    {"Hit_weapA", 0x140882f97, 5, {0xe8, 0x64, 0x8a, 0x00, 0x00, 0x40, 0x0f, 0xb6, 0xcf, 0x83, 0xe9, 0x01, 0x74, 0x5b, 0x83, 0xe9}},
    {"Hit_weapB", 0x1408830f4, 5, {0xe8, 0x07, 0x89, 0x00, 0x00, 0x40, 0x0f, 0xb6, 0xce, 0x83, 0xe9, 0x01, 0x74, 0x7a, 0x83, 0xe9}},
    {"Hit_weapC", 0x140884945, 5, {0xe8, 0xb6, 0x70, 0x00, 0x00, 0x48, 0x8b, 0xce, 0xe8, 0x8e, 0x62, 0xe3, 0xff, 0x0f, 0x10, 0x96}},
    {"Hit_chain", 0x1408858b9, 5, {0xe8, 0x42, 0x61, 0x00, 0x00, 0x40, 0x0f, 0xb6, 0xcf, 0x83, 0xe9, 0x01, 0x74, 0x2d, 0x83, 0xe9}},
};
void InstallHitLog() {
    static bool done = false;
    if (done) return;
    done = true;
    if (config::GetFloat("hit_log", 1.f) <= 0.5f) return;
    g_hitFn = (PFN_Hit)((uint8_t*)GameVa(0x14088ba00ull));
    void* relays[7] = {(void*)&HitRelay<0>, (void*)&HitRelay<1>, (void*)&HitRelay<2>, (void*)&HitRelay<3>,
                       (void*)&HitRelay<4>, (void*)&HitRelay<5>, (void*)&HitRelay<6>};
    int ok = 0;
    for (int i = 0; i < 7; ++i) ok += DetourCallSite(kHitSites[i], relays[i]) ? 1 : 0;
    Log("combat: hit log installed on %d of 7 call sites", ok);
}

// ---- D-080 hit probes (owner: hits should register within ~2 m; melee hits do not pass sub_14088ba00). Downstream
// functions that build hit data (strings HitType / Hit_Intensity) are hooked on command ("hitprobe on", in a fight) and
// log their call stack (module RVAs) - whichever runs on a melee hit shows the game's melee hit path.
std::atomic<int> g_probeLogs[4];
std::atomic<uint64_t> g_probeCount[4];
const char* const kProbeName[4] = {"HitType 140698590", "Hit_Intensity 140835920", "Hit_Intensity 140835e20",
                                   "Hit_Intensity 14087f5e0"};
void ProbeLog(int i) {
    const uint64_t c = ++g_probeCount[i];
    if (g_probeLogs[i].fetch_add(1) >= 40) return;
    void* fr[16];
    const USHORT nf = RtlCaptureStackBackTrace(1, 16, fr, nullptr);
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    char b[512];
    int o = 0;
    for (USHORT k = 0; k < nf && o < 480; ++k) {
        const uintptr_t a = (uintptr_t)fr[k];
        o += (a >= base && a < base + 0x6000000) ? sprintf_s(b + o, sizeof(b) - o, " %llx", (unsigned long long)(a - base + 0x140000000ull))
                                                  : sprintf_s(b + o, sizeof(b) - o, " -");
    }
    const ULONGLONG t0 = g_attackStartMs.load();
    Log("combat: probe %s #%llu%s | stack%s", kProbeName[i], (unsigned long long)c,
        t0 ? " (during a held attack)" : "", b);
}
using PFN_P0 = uint64_t(__fastcall*)(int64_t);
using PFN_P1 = void(__fastcall*)(int64_t, int64_t, int64_t, int64_t, int64_t);
using PFN_P2 = uint64_t(__fastcall*)(uint64_t, char, int64_t);
using PFN_P3 = void(__fastcall*)(float*);
PFN_P0 o_p0 = nullptr;
PFN_P1 o_p1 = nullptr;
PFN_P2 o_p2 = nullptr;
PFN_P3 o_p3 = nullptr;
uint64_t __fastcall hk_p0(int64_t a) { ProbeLog(0); return o_p0(a); }
void __fastcall hk_p1(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) { ProbeLog(1); o_p1(a, b, c, d, e); }
uint64_t __fastcall hk_p2(uint64_t a, char b, int64_t c) { ProbeLog(2); return o_p2(a, b, c); }
void __fastcall hk_p3(float* a) { ProbeLog(3); o_p3(a); }
constexpr HookSite kProbe0{"Probe_HitType", 0x140698590, 5, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x49}};
constexpr HookSite kProbe1{"Probe_HitInt1", 0x140835920, 6, {0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x90}};
constexpr HookSite kProbe2{"Probe_HitInt2", 0x140835e20, 6, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f, 0xb6, 0xca, 0x49, 0x8b, 0xf8, 0x84, 0xd2, 0x0f, 0x84}};
constexpr HookSite kProbe3{"Probe_HitInt3", 0x14087f5e0, 6, {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xb8}};

// D-080 time-scale probe ("tsprobe on"): LIVE owner battle - a landed Blades / bare-hand hit shows as Kratos' creature
// time-scale slot 3 dropping to ~0.0 ~140-250 ms into the attack (the hit pause; never on whiffs). The game's slot setter
// (sub_14071db60, the one the mod uses for slot 5) is hooked and, for a slot < 0.6 that is not ours, logs the call
// stack: it leads back into the melee hit code.
using PFN_TsSet = void(__fastcall*)(int64_t, int, float, float);
PFN_TsSet o_tsSet = nullptr;
std::atomic<int> g_tsProbeLogs{0};
std::atomic<bool> g_tsProbeOn{false};
void __fastcall hk_TsSet(int64_t cr, int slot, float v, float blend) {
    if (g_tsProbeOn.load() && slot != 5 && v < 0.6f && g_tsProbeLogs.fetch_add(1) < 80) {
        void* fr[18];
        const USHORT nf = RtlCaptureStackBackTrace(1, 18, fr, nullptr);
        const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        char b[560];
        int o = 0;
        for (USHORT k = 0; k < nf && o < 530; ++k) {
            const uintptr_t a = (uintptr_t)fr[k];
            o += (a >= base && a < base + 0x6000000) ? sprintf_s(b + o, sizeof(b) - o, " %llx", (unsigned long long)(a - base + 0x140000000ull))
                                                      : sprintf_s(b + o, sizeof(b) - o, " -");
        }
        uint8_t* obj = HeroObject();
        const bool hero = obj && !IsBadReadPtr(obj + 0x220, 8) && *(int64_t*)(obj + 0x220) == cr;
        const ULONGLONG t0 = g_attackStartMs.load();
        Log("combat: tsprobe slot %d -> %.2f (blend %.2f) %s%s | stack%s", slot, v, blend, hero ? "KRATOS" : "other creature",
            t0 ? " (during a held attack)" : "", b);
    }
    o_tsSet(cr, slot, v, blend);
}
constexpr HookSite kTsSet{"TimeScaleSet", 0x14071db60, 6, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0x79, 0x08, 0x00, 0x48, 0x8b, 0xd9, 0x48, 0x63}};

// ---- D-081 hit radius (owner: "I need a radius of ~2 m of hits registering"). LIVE (owner battles, watchpoint on the
// hit pause + stacks): melee hits are found by the combat-collision step of the main loop on the system 0x1411c01e0 -
// sub_140729c40 tests every participant pair (sub_14072a780 "can A hit B", sub_1408a76e0 A's volumes x B's volumes),
// the volume test sub_1408a54b0(volA, volB, A, B, ctx) sweeps two spheres from the last frame to this one; the hit records
// are then dispatched (sub_1407282b0 -> sub_140727180: hit pause, reactions, listeners). A volume's shape data
// (*(vol+0x88), floats): this frame's world matrix [0..15], sphere centre + RADIUS [0x10..0x13], last frame's matrix
// [0x1c..0x2b], centre + radius [0x2c..0x2f]. In first-person gameplay, while Kratos is the attacker, his sphere radius is
// raised to hit_radius (default 1.0 m around the weapon - with the weapon ~1 m out, ~2 m from Kratos) for the game's own
// test, then restored: no movement, no lunge; the game still makes the hit, reaction and damage itself.
using PFN_VolTest = uint32_t(__fastcall*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
PFN_VolTest o_volTest = nullptr;
std::atomic<int> g_volLogs{0};
std::atomic<uint64_t> g_volHeroTests{0}, g_volHeroHits{0};
// D-151: last time the game tested Kratos' attack volume against a target - his hit spheres are live only between the
// attack animation's CombatCollision On / Off events, so the first test of an attack = its wind-up is over
std::atomic<ULONGLONG> g_heroVolAt{0};
std::atomic<float> g_hitRadius{-1.f};
// D-108 (owner: "make the radius lower when doing the fist combat - the hit detection radius"): bare-handed attacks
// (weapon slot Bare, or the axe slot with the axe not in the hand: sheathed / thrown - also Spartan Rage) use
// hit_radius_fist (default 0.5 m, half the weapon's 1.0 m); command "hitradiusfist <m>"
std::atomic<float> g_hitRadiusFist{-1.f};
bool FistAttack() {
    const int w = CurrentWeapon();
    return w == 3 || (w == 1 && !AxeInHand());
}
bool IsHeroEntry(int64_t entry) {
    if (!entry || IsBadReadPtr((void*)entry, 8)) return false;
    const int64_t o = *(const int64_t*)entry;
    uint8_t* obj = HeroObject();
    if (!obj || !o) return false;
    if (o == (int64_t)obj) return true;
    return !IsBadReadPtr(obj + 0x220, 8) && o == *(const int64_t*)(obj + 0x220);
}
// D-196 diagnostic (owner: "in this part of the game the axe doesn't register hits well - the green monsters and their
// bigger variants; human-like enemies like Baldur are fine. Why?"). STATIC (sub_1408a5c90, the target side of the volume
// test): a target sphere i counts only when bit i is set in BOTH masks of the target's shape data (+0x60 and +0xd0) and
// its definition has a flag 0x201 - an enemy next to the axe can still have its live spheres elsewhere, or none. For a
// held axe attack this logs, against targets within 3 m: the target (object name), its spheres - how many, which are
// live, the nearest live / switched-off one to the drawn axe (distance beyond its radius) - and whether the test hit.
// At most 3 lines per attack, 300 in all. Read only.
// D-198 diagnostic (owner log D-197: the Draugr - godraugr00 - not in a can't-be-hit move, yet with the drawn axe inside
// or within 0.5 m of a live hurt sphere only 32 of 74 tests hit): which stage after the geometry drops them. During
// Kratos' held axe attack tests only (t_axeTest, set around the volume test), counted and returned unchanged:
//   sub_1408a64f0(ctx) - the sweep of one sphere pair: time of impact in [0, 1] -> filters -> hit record (1 arg, rcx)
//   sub_140914950(filter, 0, creature) - the creature hit filter (3 args) - non-zero = rejected
//   sub_1408a6bc0(.., .., body, float, out) - the contact check on the target's body (rcx rdx r8 xmm3 + [rsp+0x28])
// Logged every 10 s with counts.
thread_local bool t_axeTest = false;
std::atomic<uint64_t> g_stSweep{0}, g_stSweepHit{0}, g_stFilter{0}, g_stFilterRej{0}, g_stBody{0}, g_stBodyRej{0};
std::atomic<uint64_t> g_stRejDir{0}, g_stForced{0};
thread_local bool t_bladesTest = false;  // D-199: the Blades' melee tests too
using PFN_Sweep = uint64_t(__fastcall*)(int64_t*);
using PFN_Filter = uint64_t(__fastcall*)(int64_t, int64_t, int64_t);
using PFN_BodyCheck = bool(__fastcall*)(float*, uint32_t, int64_t, float, void*);
PFN_Sweep o_sweep = nullptr;
PFN_Filter o_filter = nullptr;
PFN_BodyCheck o_bodyCheck = nullptr;
uint64_t __fastcall hk_Sweep(int64_t* c) {
    const uint64_t r = o_sweep(c);
    if (t_axeTest) { ++g_stSweep; if ((uint8_t)r) ++g_stSweepHit; }
    return r;
}
uint64_t __fastcall hk_Filter(int64_t a, int64_t b, int64_t c) {
    const uint64_t r = o_filter(a, b, c);
    if (t_axeTest) { ++g_stFilter; if ((uint8_t)r) ++g_stFilterRej; }
    return r;
}
// D-199 (owner log D-198: the body contact check rejected 454 / 656 and 732 / 787 of the axe's contacts with Draugr, the
// creature filter none): its early rejection is the direction test of body parts flagged 0x4000 (+0x34 of the body data
// it is given - a temporary the sweep builds on its own stack, sub_1408a74b0) - the hit only counts when two facing axes
// agree (sub_1400f6790). For Kratos' own held axe / Blades attack tests a rejected contact on such a part is checked once
// more with the flag cleared on that temporary, so the game completes the contact and fills the hit itself; any other
// rejection stands. gowvr.ini hit_any_direction 0 = off. Counted: rejections with the flag, let through.
bool __fastcall hk_BodyCheck(float* a, uint32_t b, int64_t c, float d, void* e) {
    bool r = o_bodyCheck(a, b, c, d, e);
    if (!t_axeTest && !t_bladesTest) return r;
    ++g_stBody;
    if (r) return r;
    ++g_stBodyRej;
    static const bool anyDir = config::GetFloat("hit_any_direction", 0.f) > 0.5f;  // D-200: off (it never applied: D-199 log)
    if (c && !IsBadWritePtr((void*)(c + 0x34), 4) && (*(uint32_t*)(c + 0x34) & 0x4000)) {
        ++g_stRejDir;
        if (anyDir) {
            const uint32_t keep = *(uint32_t*)(c + 0x34);
            *(uint32_t*)(c + 0x34) = keep & ~0x4000u;
            r = o_bodyCheck(a, b, c, d, e);
            *(uint32_t*)(c + 0x34) = keep;
            if (r) ++g_stForced;
        }
    }
    return r;
}
constexpr HookSite kSweep{"VolSweep", 0x1408a64f0, 7, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55}};
constexpr HookSite kFilter{"HitFilter", 0x140914950, 5, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0xf7}};
constexpr HookSite kBodyCheck{"HitBodyCheck", 0x1408a6bc0, 7, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}};
void StageLog() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    const uint64_t sw = g_stSweep.exchange(0);
    if (!sw) return;
    Log("combat: melee test stages, 10 s - sphere-pair sweeps %llu -> contact %llu | creature filter checked %llu, rejected %llu | "
        "body contact checked %llu, rejected %llu (on direction-only parts %llu, let through %llu) (D-198 / D-199)",
        (unsigned long long)sw, (unsigned long long)g_stSweepHit.exchange(0), (unsigned long long)g_stFilter.exchange(0),
        (unsigned long long)g_stFilterRej.exchange(0), (unsigned long long)g_stBody.exchange(0),
        (unsigned long long)g_stBodyRej.exchange(0), (unsigned long long)g_stRejDir.exchange(0),
        (unsigned long long)g_stForced.exchange(0));
}
static float SegDist(const float* p, const float a[3], const float b[3]) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-8f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float q[3] = {a[0] + ab[0] * t - p[0], a[1] + ab[1] * t - p[1], a[2] + ab[2] * t - p[2]};
    return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}
struct TgtInfo { int n = 0, live = 0; float nearLive = 99.f, nearLiveR = 0.f, nearOff = 99.f, centre = 99.f; uint64_t m60 = 0, md0 = 0;
                 char name[48] = {}; int immune = -1; };
static bool TargetRead(int64_t volB, int64_t B, const float seg[2][3], TgtInfo* o) {
    __try {
        const float* sh = *(const float**)(volB + 0x88);
        const int64_t def = *(const int64_t*)(volB + 0x30);
        if (!sh || !def) return false;
        o->n = *(const uint8_t*)(def + 0x43);
        if (o->n <= 0 || o->n > 64) return false;
        const float* sp = *(const float* const*)((const uint8_t*)sh + 0x50);
        o->m60 = *(const uint64_t*)((const uint8_t*)sh + 0x60);
        o->md0 = *(const uint64_t*)((const uint8_t*)sh + 0xd0);
        float c[3];  // its coarse centre (local to its matrix)
        for (int k = 0; k < 3; ++k) c[k] = sh[12 + k] + sh[0x10] * sh[k] + sh[0x11] * sh[4 + k] + sh[0x12] * sh[8 + k];
        o->centre = SegDist(c, seg[0], seg[1]);
        for (int i = 0; i < o->n && sp; ++i) {
            const float* l = sp + i * 4;
            float w[3];
            for (int k = 0; k < 3; ++k) w[k] = sh[12 + k] + l[0] * sh[k] + l[1] * sh[4 + k] + l[2] * sh[8 + k];
            const float dd = SegDist(w, seg[0], seg[1]) - l[3];
            const bool live = ((o->m60 & o->md0) >> i) & 1;
            if (live) {
                ++o->live;
                if (dd < o->nearLive) { o->nearLive = dd; o->nearLiveR = l[3]; }
            } else if (dd < o->nearOff) {
                o->nearOff = dd;
            }
        }
        const int64_t b0 = B ? *(const int64_t*)B : 0;
        // D-197: the entry is the target CREATURE (no name at +0x38); its object at +0x08 carries the name
        const int64_t obj = b0 ? *(const int64_t*)(b0 + 8) : 0;
        const char* nm = obj ? *(const char* const*)(obj + 0x38) : nullptr;
        if (nm) {
            int k = 0;
            for (; k < 47 && nm[k] >= 0x20 && nm[k] < 0x7f; ++k) o->name[k] = nm[k];
            o->name[k] = 0;
        }
        // D-197: the hit filter's state test (sub_140725910, read here without calling game code): a move of the
        // creature flagged 0x10000 (+0x3a70 -> +0x4c, and +0x11c of each of its +0x48-counted sub-moves) = can't be hit now
        if (b0) {
            const uint8_t* mp = *(const uint8_t* const*)(b0 + 0x3a70);
            if (mp) {
                uint32_t f = *(const uint32_t*)(mp + 0x4c) & 0x10000u;
                const int cnt = *(const int*)(mp + 0x48);
                for (int k = 0; k < cnt && k < 16; ++k) {
                    const uint8_t* sm = *(const uint8_t* const*)(mp + 8 + (size_t)k * 8);
                    if (sm) f |= *(const uint32_t*)(sm + 0x11c) & 0x10000u;
                }
                o->immune = f ? 1 : 0;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::atomic<ULONGLONG> g_attackStartMsDiag{0};
static void TargetDiag(int64_t volB, int64_t B, bool hit, bool retested) {
    static ULONGLONG attack = 0;
    static int perAttack = 0;
    static std::atomic<int> total{0};
    const ULONGLONG a = g_attackStartMsDiag.load();
    if (!a || total.load() >= 300) return;
    if (a != attack) { attack = a; perAttack = 0; }
    if (perAttack >= 6) return;  // D-197: 6 (two targets / later frames)
    float sc[2][3], spv[2][3];
    if (!AxeHitSegment(sc, spv)) return;
    TgtInfo t;
    if (!TargetRead(volB, B, sc, &t) || t.centre > 3.f) return;
    ++perAttack;
    ++total;
    Log("combat: axe vs target '%s' (%s%s): enemy in a can't-be-hit move %s; centre %.2f m from the axe, %d spheres, %d live "
        "(masks %llx & %llx), nearest live %.2f m beyond its radius %.2f, nearest switched-off %.2f m", t.name[0] ? t.name : "?",
        hit ? "HIT" : "miss", retested ? ", re-tested" : "", t.immune < 0 ? "?" : t.immune ? "YES" : "no", t.centre, t.n, t.live,
        (unsigned long long)t.m60, (unsigned long long)t.md0, t.nearLive, t.nearLiveR, t.nearOff);
}
uint32_t __fastcall hk_VolTest(int64_t volA, int64_t volB, int64_t A, int64_t B, int64_t ctx, int64_t p6, int64_t p7,
                               int64_t p8, int64_t p9) {
    if (g_hitRadius.load() < -0.5f) g_hitRadius = config::GetFloat("hit_radius", 1.0f);
    if (g_hitRadiusFist.load() < -0.5f) g_hitRadiusFist = config::GetFloat("hit_radius_fist", 0.5f);
    const float R0 = g_hitRadius.load();
    {   // diagnostic: the first calls' attacker / victim entries vs Kratos' object and creature
        static std::atomic<int> diag{0};
        if (diag.fetch_add(1) < 12) {
            uint8_t* obj = HeroObject();
            const int64_t a0 = A && !IsBadReadPtr((void*)A, 8) ? *(const int64_t*)A : 0;
            const int64_t b0 = B && !IsBadReadPtr((void*)B, 8) ? *(const int64_t*)B : 0;
            Log("combat: volume test A[0] %p B[0] %p | Kratos object %p creature %p | heroA %d heroB %d", (void*)a0, (void*)b0,
                obj, obj && !IsBadReadPtr(obj + 0x220, 8) ? *(void**)(obj + 0x220) : nullptr, (int)IsHeroEntry(A), (int)IsHeroEntry(B));
        }
    }
    float* s = nullptr;
    const bool heroA = FpMode() == 1 && g_on.load() && IsHeroEntry(A);
    if (heroA) g_heroVolAt = GetTickCount64();  // D-151
    if (R0 > 0.f && heroA && volA && !IsBadReadPtr((void*)(volA + 0x88), 8)) {
        s = *(float**)(volA + 0x88);
        if (s && IsBadReadPtr(s, 0x30 * 4)) s = nullptr;
    }
    if (!s) return o_volTest(volA, volB, A, B, ctx, p6, p7, p8, p9);
    const float R = FistAttack() ? g_hitRadiusFist.load() : R0;  // (Kratos attacking only: the weapon read is not free)
    // D-081 v2 (LIVE: s[0x13] / s[0x2f] read 0.00 / 43.7 / 44.4 - the volume's coarse bound, not a hit radius): the fine
    // test walks the volume's spheres - count = byte +0x43 of the definition at *(vol+0x30), (x y z radius) entries of
    // 16 bytes at the pointers in the shape data +0x50 (this frame) / +0xc0 (last frame). Each radius -> max(r, R)
    const int64_t def = !IsBadReadPtr((void*)(volA + 0x30), 8) ? *(const int64_t*)(volA + 0x30) : 0;
    const int n = def && !IsBadReadPtr((void*)(def + 0x43), 1) ? *(const uint8_t*)(def + 0x43) : 0;
    float* cur = *(float**)((uint8_t*)s + 0x50);
    float* prev = *(float**)((uint8_t*)s + 0xc0);
    if (n <= 0 || n > 32 || !cur || !prev || IsBadReadPtr(cur, (size_t)n * 16) || IsBadReadPtr(prev, (size_t)n * 16))
        return o_volTest(volA, volB, A, B, ctx, p6, p7, p8, p9);
    float saveC[32], saveP[32];
    const float b0 = s[0x13], b1 = s[0x2f];
    if (g_volLogs.fetch_add(1) < 16) {
        char t[256];
        int o = 0;
        for (int i = 0; i < n && o < 230; ++i) o += sprintf_s(t + o, sizeof(t) - o, " %.2f", cur[i * 4 + 3]);
        Log("combat: Kratos' attack volume %p: %d spheres, radii%s (bound %.2f) -> %.2f", (void*)volA, n, t, b0, R);
    }
    for (int i = 0; i < n; ++i) {
        saveC[i] = cur[i * 4 + 3];
        saveP[i] = prev[i * 4 + 3];
        if (saveC[i] > 0.f && saveC[i] < R) cur[i * 4 + 3] = R;
        if (saveP[i] > 0.f && saveP[i] < R) prev[i * 4 + 3] = R;
    }
    if (b0 > 0.f && b0 < R + 0.5f) s[0x13] = R + 0.5f;  // the coarse bound must not reject the bigger spheres
    if (b1 > 0.f && b1 < R + 0.5f) s[0x2f] = R + 0.5f;
    const bool axeT = g_attackStartMs.load() && CurrentWeapon() == 1 && AxeInHand() && !FistAttack();  // D-198
    const bool bladesT = g_attackStartMs.load() && CurrentWeapon() == 2 && !BladeThrowActive();     // D-199
    t_axeTest = axeT;
    t_bladesTest = bladesT;
    const uint32_t res = o_volTest(volA, volB, A, B, ctx, p6, p7, p8, p9);
    t_axeTest = false;
    for (int i = 0; i < n; ++i) {
        cur[i * 4 + 3] = saveC[i];
        prev[i * 4 + 3] = saveP[i];
    }
    s[0x13] = b0;
    s[0x2f] = b1;
    ++g_volHeroTests;
    if (res) ++g_volHeroHits;
    // D-153 (owner: "close to enemies, axe hits don't register until a while"; owner logs D-151 / D-152: the hit pause
    // came in 29 of 29 punches but 4 of 62 light axe swings, although the game tested Kratos' attack volume against the
    // enemies during them, at 1 m radius): the fists' spheres ride the VR hands, the axe attack's spheres evidently do not
    // ride the VR axe. A miss of an axe attack is tested once more with the volume's spheres laid along the DRAWN axe
    // (grip -> head, this frame and the last), radius hit_radius - an enemy within reach of the axe in the player's hand
    // is hit. Only adds hits; axe_hit_retest 0 = off. Evidence logged (where the game's own spheres were).
    uint32_t out = res;
    // D-185 (owner: "the punching mechanics are great - the hits always land; make something similar for the Blades and
    // the axe - the axe seems to hit only when I swing downwards, as if their hit boxes were in the lower body"; owner
    // log: 416 axe re-tests, 2 hits). STATIC (sub_1408a54b0): the fine test checks only the volume's spheres whose bit is
    // set in its last argument (the mask the attack's CombatCollision events switch on), and also a second sphere list -
    // the sets chained at vol+0x60 (node: +0x00 this frame / +0x08 last frame arrays of x y z radius, +0x2c count, +0x20
    // next), stored in WORLD space (no matrix). D-153 moved only the first list and kept the mask: an attack whose live
    // spheres were in the chained sets, or masked off, could not hit from the re-test. Now the re-test lays the spheres of
    // BOTH lists along the weapon in the player's hand, with every sphere of the first list switched on; for the axe
    // (grip -> head) and now also the Blades (each Blade, root -> tip, as drawn). The punches need none of it: their
    // spheres ride the motion-controlled hands. gowvr.ini axe_hit_retest 0 / blades_hit_retest 0 = off.
    static const bool retestOn = config::GetFloat("axe_hit_retest", 1.f) > 0.5f;
    static const bool bladesRetestOn = config::GetFloat("blades_hit_retest", 1.f) > 0.5f;
    const int weapon = CurrentWeapon();
    float segC[2][2][3], segP[2][2][3];
    int nseg = 0;
    const bool held = g_attackStartMs.load() != 0;
    if (!res && held && retestOn && !FistAttack() && weapon == 1 && AxeInHand()) {
        if (AxeHitSegment(segC[0], segP[0])) nseg = 1;
    } else if (!res && held && bladesRetestOn && weapon == 2 && !BladeThrowActive()) {
        nseg = BladeHitSegments(segC, segP);
    }
    const bool axeDiag = held && weapon == 1 && AxeInHand() && !FistAttack();
    if (axeDiag) g_attackStartMsDiag = g_attackStartMs.load();
    uint32_t res2Seen = 0;
    if (nseg > 0) {
        const float* M = s;  // this frame's matrix, row vectors: world = x*M[0..2] + y*M[4..6] + z*M[8..10] + M[12..14]
        const float* Mp = s + 0x1c;
        const float tx = cur[0] - M[12], ty = cur[1] - M[13], tz = cur[2] - M[14];
        const bool world = tx * tx + ty * ty + tz * tz < 25.f || fabsf(M[12]) + fabsf(M[13]) + fabsf(M[14]) < 1.f;
        auto toLocal = [](const float* m, const float* w, float* o) {  // w x m^-1 for a rigid (scaled) row-major m
            const float d[3] = {w[0] - m[12], w[1] - m[13], w[2] - m[14]};
            for (int r = 0; r < 3; ++r) {
                const float l2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
                o[r] = l2 > 1e-8f ? (d[0] * m[r * 4] + d[1] * m[r * 4 + 1] + d[2] * m[r * 4 + 2]) / l2 : 0.f;
            }
        };
        auto fromLocal = [](const float* m, const float* l, float* o) {
            for (int c = 0; c < 3; ++c) o[c] = m[12 + c] + l[0] * m[c] + l[1] * m[4 + c] + l[2] * m[8 + c];
        };
        // point k of K along the segments (spread over all of them), this frame and the last
        auto along = [&](int k, int K, float* wc, float* wp) {
            const int per = (K + nseg - 1) / nseg;
            int sg = per > 0 ? k / per : 0;
            if (sg >= nseg) sg = nseg - 1;
            const int kk = k - sg * per;
            const float t = per > 1 ? (float)kk / (float)(per - 1) : 1.f;
            for (int c = 0; c < 3; ++c) {
                wc[c] = segC[sg][0][c] + (segC[sg][1][c] - segC[sg][0][c]) * t;
                wp[c] = segP[sg][0][c] + (segP[sg][1][c] - segP[sg][0][c]) * t;
            }
        };
        float g0[3];
        if (world) memcpy(g0, cur, 12); else fromLocal(M, cur, g0);
        // first list (+0x50 / +0xc0): saved, laid along the weapon, every sphere switched on
        float sv[32 * 4], spv[32 * 4];
        memcpy(sv, cur, (size_t)n * 16);
        memcpy(spv, prev, (size_t)n * 16);
        float sb[8];
        memcpy(sb, s + 0x10, 16);
        memcpy(sb + 4, s + 0x2c, 16);
        for (int i = 0; i < n; ++i) {
            float wc[3], wp[3];
            along(i, n, wc, wp);
            if (world) { memcpy(cur + i * 4, wc, 12); memcpy(prev + i * 4, wp, 12); }
            else { toLocal(M, wc, cur + i * 4); toLocal(Mp, wp, prev + i * 4); }
            cur[i * 4 + 3] = R;
            prev[i * 4 + 3] = R;
        }
        // the chained world-space sets (+0x60): saved and laid along the weapon too
        struct Saved { float* a; float v[4]; };
        static Saved chained[256];
        int nch = 0;
        int64_t node = !IsBadReadPtr((void*)(volA + 0x60), 8) ? *(const int64_t*)(volA + 0x60) : 0;
        for (int guard = 0; node && guard < 16 && !IsBadReadPtr((void*)node, 0x34); ++guard) {
            float* ac = *(float**)node;
            float* ap = *(float**)(node + 8);
            const int cnt = *(const int*)(node + 0x2c);
            if (ac && ap && cnt > 0 && cnt <= 32 && !IsBadWritePtr(ac, (size_t)cnt * 16) && !IsBadWritePtr(ap, (size_t)cnt * 16)) {
                for (int i = 0; i < cnt && nch + 2 <= 256; ++i) {
                    chained[nch].a = ac + i * 4; memcpy(chained[nch].v, ac + i * 4, 16); ++nch;
                    chained[nch].a = ap + i * 4; memcpy(chained[nch].v, ap + i * 4, 16); ++nch;
                    float wc[3], wp[3];
                    along(i, cnt, wc, wp);
                    memcpy(ac + i * 4, wc, 12);
                    memcpy(ap + i * 4, wp, 12);
                    ac[i * 4 + 3] = R;
                    ap[i * 4 + 3] = R;
                }
            }
            node = *(const int64_t*)(node + 0x20);
        }
        // the coarse bound around all of it (centre = the middle of the first segment, radius covers every segment)
        float mc[3], mp[3], reach = 0.f;
        for (int c = 0; c < 3; ++c) { mc[c] = 0.5f * (segC[0][0][c] + segC[0][1][c]); mp[c] = 0.5f * (segP[0][0][c] + segP[0][1][c]); }
        for (int sg = 0; sg < nseg; ++sg)
            for (int e = 0; e < 2; ++e) {
                const float dx = segC[sg][e][0] - mc[0], dy = segC[sg][e][1] - mc[1], dz = segC[sg][e][2] - mc[2];
                reach = fmaxf(reach, sqrtf(dx * dx + dy * dy + dz * dz));
            }
        {
            const float bx = s[0x10] - M[12], by = s[0x11] - M[13], bz = s[0x12] - M[14];
            const bool boundWorld = bx * bx + by * by + bz * bz < 25.f || fabsf(M[12]) + fabsf(M[13]) + fabsf(M[14]) < 1.f;
            if (boundWorld) { memcpy(s + 0x10, mc, 12); memcpy(s + 0x2c, mp, 12); }
            else { toLocal(M, mc, s + 0x10); toLocal(Mp, mp, s + 0x2c); }
        }
        s[0x13] = s[0x2f] = reach + R + 0.5f;
        const int64_t mask = n >= 63 ? -1 : (int64_t)((1ull << n) - 1);
        t_axeTest = axeT;
        t_bladesTest = bladesT;
        const uint32_t res2 = o_volTest(volA, volB, A, B, ctx, p6, p7, p8, p9 | mask);
        t_axeTest = false;
        t_bladesTest = false;
        for (int i = nch - 1; i >= 0; --i) memcpy(chained[i].a, chained[i].v, 16);
        memcpy(cur, sv, (size_t)n * 16);
        memcpy(prev, spv, (size_t)n * 16);
        memcpy(s + 0x10, sb, 16);
        memcpy(s + 0x2c, sb + 4, 16);
        static std::atomic<uint32_t> nRetest{0}, nHit{0};
        static std::atomic<int> evid{0};
        ++nRetest;
        if (res2) { ++nHit; out = res2; }
        res2Seen = res2;
        if (evid.fetch_add(1) < 30 || (res2 && nHit.load() <= 20) || nRetest.load() % 500 == 0) {
            auto dist = [](const float* a, const float* b) {
                return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            };
            // the target's coarse centre in world space (stored like Kratos': relative to its own matrix)
            float* sB = volB && !IsBadReadPtr((void*)(volB + 0x88), 8) ? *(float**)(volB + 0x88) : nullptr;
            float tgt = -1.f;
            if (sB && !IsBadReadPtr(sB, 0x14 * 4)) {
                float tw[3];
                const float ox = sB[0x10] - sB[12], oy = sB[0x11] - sB[13], oz = sB[0x12] - sB[14];
                if (ox * ox + oy * oy + oz * oz < 25.f) memcpy(tw, sB + 0x10, 12); else fromLocal(sB, sB + 0x10, tw);
                tgt = dist(tw, mc);
            }
            Log("combat: %s hit re-test %s - game's sphere 0 %.2f m from the drawn weapon (spheres %s, %d chained, mask %llx), "
                "target %.2f m from the weapon's middle (%u re-tests, %u hits so far)", weapon == 2 ? "Blades" : "axe",
                res2 ? "HIT" : "miss", dist(g0, mc), world ? "world" : "local", nch / 2, (unsigned long long)p9, tgt,
                nRetest.load(), nHit.load());
        }
    }
    if (axeDiag) TargetDiag(volB, B, res != 0 || res2Seen != 0, nseg > 0);  // D-196
    t_bladesTest = false;
    if (axeT || bladesT) StageLog();  // D-198
    return out;
}
// ---- D-157: the game's collision query, read-only (static RE: reverse_engineering/src) -------------------------------
// Owner: "find out what walls are - the head and the hands must not go through walls in F1, rumble when touching them".
// The game's synchronous collision query is sub_1401536c0 (Sony Physics Effects 2.0 - assert path "physicseffects2.0";
// 43 call sites: SCR_WallSyncProbe's worker sub_14092ca20, ledge / traversal probes, camera, AI, pickups). Call shape
// int f(query*, result*, char mode, float, float): the wall probe fills the query (+0x08 0x10002, +0x18 mask 0x400, +0x48
// the probing object, +0x80 start xyz, +0x90 direction xyz ...) and reads hit point / normal / object from the result.
// Before the mod calls it itself, this hook LOGS real queries (caller, the query's first 0x110 bytes) before the call -
// nothing is changed. gowvr.ini phys_query_log 0 = not installed.
// D-158 (owner: "it crashed" - owner log: access violation at GoW.exe+0x15371a, inside sub_1401536c0, entered through
// D-157's C hook, r8 = 1): the query reads more of its arguments from the caller's stack than the decompile showed, and
// a C hook that calls the original passes only the parameters it declares - the original read garbage pointers from
// the hook's frame and wrote through them. Now the entry jumps into a machine-code stub that saves every argument
// register (rcx rdx r8 r9 r10 r11 rax, xmm0-3), calls the logger with the query / result pointers, mode and the caller,
// restores them all and jumps on to the original with the stack untouched - nothing of the call changes. The logger
// reads only the query's inputs (before the call; the result is not filled yet).
struct PqSite { std::atomic<uintptr_t> ret{0}; std::atomic<uint32_t> n{0}; std::atomic<int> logged{0}; };
PqSite g_pqSites[48];
extern "C" void PhysQueryPre(void* q, void* r, uint64_t mode, uintptr_t ret) {
    PqSite* site = nullptr;
    for (auto& e : g_pqSites) {  // lock-free: find this caller or claim a free slot
        uintptr_t cur = e.ret.load(std::memory_order_relaxed);
        if (cur == ret) { site = &e; break; }
        if (cur == 0 && e.ret.compare_exchange_strong(cur, ret)) { site = &e; break; }
        if (cur == ret) { site = &e; break; }
    }
    if (!site) return;
    const uint32_t n = site->n.fetch_add(1);
    int lg = site->logged.load();
    // 3 samples per call site: the 1st, the 50th and the 1000th call (different situations)
    if ((n == 0 || n == 49 || n == 999) && lg < 3 && site->logged.compare_exchange_strong(lg, lg + 1) && q &&
        !IsBadReadPtr(q, 0x110)) {
        const float* f = (const float*)q;
        const uint32_t* u = (const uint32_t*)q;
        char line[2600];
        int o = sprintf_s(line, "physq: caller %llx mode %llu result %p | query:",
                          (unsigned long long)(ret - (uintptr_t)GetModuleHandleW(nullptr) + 0x140000000ull),
                          (unsigned long long)(mode & 0xff), r);
        for (int i = 0; i < 0x110 / 4 && o > 0 && o < (int)sizeof(line) - 40; ++i) {
            const float v = f[i];
            const int k = (fabsf(v) > 1e-4f && fabsf(v) < 1e5f) ? sprintf_s(line + o, sizeof(line) - o, " %x:%.3f", i * 4, v)
                                                                 : sprintf_s(line + o, sizeof(line) - o, " %x:#%x", i * 4, u[i]);
            if (k <= 0) break;
            o += k;
        }
        Log("%s", line);
    }
    static std::atomic<ULONGLONG> tSum{0};
    static std::atomic<int> sums{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG t0 = tSum.load();
    if (!t0) tSum = now;
    else if (now - t0 >= 30000 && sums.load() < 4 && tSum.compare_exchange_strong(t0, now)) {
        ++sums;
        char line[1400];
        int o = sprintf_s(line, "physq: 30 s per call site:");
        for (auto& e : g_pqSites) {
            const uintptr_t rr = e.ret.load();
            if (!rr || o <= 0 || o >= (int)sizeof(line) - 40) continue;
            const int k = sprintf_s(line + o, sizeof(line) - o, " %llx=%u",
                                    (unsigned long long)(rr - (uintptr_t)GetModuleHandleW(nullptr) + 0x140000000ull), e.n.exchange(0));
            if (k > 0) o += k;
        }
        Log("%s", line);
    }
}
// the stub (offsets checked with capstone: the stack stays 16-aligned for the call, the return address is at [rsp+0xb8]
// after 7 pushes and sub 0x80; logger imm64 at +64; the trampoline slot right after "jmp [rip]" at +134 - DetourInstall
// writes it before it patches the entry). xmm4 / xmm5 are kept too (a vectorcall-style 5th / 6th float argument)
constexpr size_t kPqStubSize = 142, kPqStubFn = 64, kPqStubOrig = 134;
uint8_t* BuildPhysQueryStub() {
    static const uint8_t code[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x50,  // push rcx rdx r8 r9 r10 r11 rax
        0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                          // sub rsp, 0x80
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,  // movdqu [rsp+20/30], xmm0/1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,  // movdqu [rsp+40/50], xmm2/3
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,  // movdqu [rsp+60/70], xmm4/5
        0x4C, 0x8B, 0x8C, 0x24, 0xB8, 0x00, 0x00, 0x00,                    // mov r9, [rsp+0xb8] (the caller)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                                // mov rax, PhysQueryPre
        0xFF, 0xD0,                                                        // call rax
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,  // movdqu xmm0/1, [rsp+20/30]
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,  // movdqu xmm2/3, [rsp+40/50]
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,  // movdqu xmm4/5, [rsp+60/70]
        0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                          // add rsp, 0x80
        0x58, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,  // pop rax r11 r10 r9 r8 rdx rcx
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,                                // jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0};                                           // -> the trampoline
    static_assert(sizeof(code) == kPqStubSize, "stub size");
    uint8_t* m = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!m) return nullptr;
    memcpy(m, code, sizeof(code));
    const uint64_t fn = (uint64_t)(uintptr_t)&PhysQueryPre;
    memcpy(m + kPqStubFn, &fn, 8);
    FlushInstructionCache(GetCurrentProcess(), m, sizeof(code));
    return m;
}
constexpr HookSite kPhysQuery{"PhysQuery", 0x1401536c0, 9, {0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x10, 0xfd, 0xff}};

constexpr HookSite kVolTest{"VolTest", 0x1408a54b0, 6, {0x4c, 0x8b, 0xdc, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8d, 0xab, 0x08}};
void InstallHitRadius() {
    static bool done = false;
    if (done) return;
    done = true;
    if (config::GetFloat("hit_radius_hook", 1.f) <= 0.5f) return;
    Log("combat: hit radius hook %s (hit_radius %.2f m)", DetourInstall(kVolTest, (void*)&hk_VolTest, (void**)&o_volTest) ? "installed" : "FAILED",
        config::GetFloat("hit_radius", 1.0f));
    if (config::GetFloat("hit_stage_log", 1.f) > 0.5f) {  // D-198
        const bool a = DetourInstall(kSweep, (void*)&hk_Sweep, (void**)&o_sweep);
        const bool b = DetourInstall(kFilter, (void*)&hk_Filter, (void**)&o_filter);
        const bool c = DetourInstall(kBodyCheck, (void*)&hk_BodyCheck, (void**)&o_bodyCheck);
        Log("combat: hit stage counters %d %d %d", (int)a, (int)b, (int)c);
    }
    if (config::GetFloat("phys_query_log", 0.f) > 0.5f) {  // D-157 / D-158: the register-preserving stub (D-160: off)
        uint8_t* stub = BuildPhysQueryStub();
        Log("combat: physics query logger %s", stub && DetourInstall(kPhysQuery, stub, (void**)(stub + kPqStubOrig)) ? "installed (stub)" : "FAILED");
    }
}

bool StartsWithI(const char* s, const char* p) { return _strnicmp(s, p, strlen(p)) == 0; }
bool ContainsI(const char* s, const char* p) {
    std::string a(s), b(p);
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    for (auto& c : b) c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

// Owner-editable override: gowvr.ini move_policy_<animation base name>=0 none / 1 hold / 2 animate.
bool IsBladesFreeAnim(const char* a);
bool IsScriptedWeaponAnim(const char* a);  // D-193 (below)
Policy Classify(const char* anim, bool sync) {
    if (sync) return Policy::Animate;
    if (!anim || !anim[0]) return Policy::None;
    std::string base(anim);
    base = base.substr(0, base.find('#'));
    const float o = config::GetFloat(("move_policy_" + base).c_str(), -1.f);
    if (o >= 0.f) return o > 1.5f ? Policy::Animate : o > 0.5f ? Policy::Hold : Policy::None;
    // D-062 (owner attack review): the bare-hand R2 kick and the Rage rock throw play in full as well
    if (IsBladesFreeAnim(anim)) return Policy::None;
    if (IsScriptedWeaponAnim(anim)) return Policy::None;  // D-193: the game places Kratos (no hold, aim or speed-up)  // D-073: body from the game, arms on the controllers
    static const char* kAnimate[] = {"attRageEnter", "attRageGrabEnter", "attRageProjectileEnter", "attRageProjectileToss",
                                     "spartankick"};
    for (const char* a : kAnimate)
        if (StartsWithI(anim, a)) return Policy::Animate;
    if (StartsWithI(anim, "att") || StartsWithI(anim, "defevade") || StartsWithI(anim, "spartankick") ||
        ContainsI(anim, "delaystance"))
        return Policy::Hold;
    return Policy::None;
}

float Ease(float cur, float target, float dt, float tau) { return cur + (target - cur) * (1.f - expf(-dt / tau)); }

// D-062 combat rework (docs/F1_ATTACK_REVIEW.md). State read by the VR input mapping (input thread):
//   in Rage   - the hero's primary animation names ("attRage*", "navRage*", "navCombatRageIdle*" on; "*RageExit*" or any
//               other locomotion "nav*" off; attacks / evades of other names leave it as it is)
//   sprinting - locomotion "navBrutalRun*" (the game's sprint)
//   busy      - an attack / evade / kick animation is playing; the input gate holds new attacks until it has ended, so
//               every attack is the FIRST of its combo (no combos, no sprint / dodge attacks)
//   animate request - a button-triggered move the owner wants in full (kick, shield + R2, runic, Rage L1+R1 / L1+R2):
//               the next attack move that starts gets ANIMATE for as long as it (and its follow-up att* parts) plays
std::atomic<bool> g_inRage{false}, g_sprinting{false}, g_busy{false};
std::atomic<ULONGLONG> g_idleSince{0}, g_animateReq{0};
std::atomic<uint64_t> g_animateReqMove{0};
std::atomic<int> g_trace{0};  // diagnostic "combattrace N": per-frame move / animation / busy

// D-193 (owner, the Baldur fight: "it shows R1 to pick up the item - during this I shouldn't be able to move, and I
// think I should have motion controls"; owner log: attTreeBatLoop - Kratos swinging a tree trunk - ran as one of HIS
// attacks: "attack aimed at the view (game facing -180 -> 0, turned about the eye)", the wind-up sped up (time scale 2),
// the body following the head, the attack gate holding the next R1): a scripted object weapon is the game's scene, not an
// attack - no hold / aim / speed-up / gate; the body stays where the game puts it, facing its way (D-194: the hands from
// the animation, plain buttons - ClassifyContext)
bool IsScriptedWeaponAnim(const char* a) {
    return a && StartsWithI(a, "atttreebat");
}
bool IsAttackAnim(const char* a) {
    return a && !IsScriptedWeaponAnim(a) && (StartsWithI(a, "att") || StartsWithI(a, "def") || StartsWithI(a, "spartan"));
}
// D-073 (owner): the Blades' aimed moves (L2 + R1 throw "Hyperion", L2 + R2 / hold elemental toss / spin): the body is
// NOT frozen and not held in place, the hands stay motion-controlled, and the Blades follow the game's own throw / spin in
// the world (placed relative to the controller-driven wrist the throw went the wrong way - "the blade went the opposite
// way, its particles the right way")
bool IsBladesFreeAnim(const char* a) {
    return a && (StartsWithI(a, "attBladeToss") || StartsWithI(a, "attbladeselementaltoss") || StartsWithI(a, "attChaosElemental"));
}
std::atomic<bool> g_bladesFree{false};
std::atomic<bool> g_inStance{false};
std::atomic<float> g_camYaw{0.f}, g_nudge{0.f};
std::atomic<float> g_nudgeAmt{-1.f};
std::atomic<bool> g_heldValid{false};
std::atomic<float> g_heldX{0.f}, g_heldZ{0.f}, g_heldY{0.f};
std::atomic<bool> g_heldYValid{false};
std::atomic<float> g_heldYaw{0.f};
std::atomic<bool> g_heldYawValid{false}, g_attackAim{true};
std::atomic<float> g_relOffX{0.f}, g_relOffZ{0.f};
std::atomic<long long> g_relQpc{0};
std::atomic<ULONGLONG> g_ffAt{0}, g_ffStanceAt{0};
std::atomic<float> g_reach{-1.f};  // D-075/076 attack reach (m), command "attackreach <m>", -1 = gowvr.ini attack_reach  // D-067: a new swing is queued (vrmap gate) - fast-forward the held attack  // command "nudge <0..1>" (A/B; -1 = gowvr.ini recovery_nudge)

// D-062 G1 (owner: attacks must not move the player forward): the player's character controller - the game's
// authoritative position. LIVE (posscanall / postrack / poshold, 2026-09-30): of 125 position copies only one holds
// Kratos when written every frame (walk 0.08 m vs 2-2.8 m); its only writer is sub_1407930b0 (animation, watchpoint
// GoW.exe+0x7934ee); the struct is the creature's controller: creature+0x3bf8 (creature[0x77f], STATIC animation_070.c),
// a static in GoW.exe (+0x2c31c90), back pointer controller+0x918 = creature, position (x y z w) at controller+0x410.
std::atomic<bool> g_attackHold{true};
float* ControllerPos() {
    uint8_t* obj = HeroObject();
    if (!obj || IsBadReadPtr(obj + 0x220, 8)) return nullptr;
    uint8_t* cr = *(uint8_t**)(obj + 0x220);
    if (!cr || IsBadReadPtr(cr + 0x3bf8, 8)) return nullptr;
    uint8_t* ctl = *(uint8_t**)(cr + 0x3bf8);
    if (!ctl || IsBadReadPtr(ctl + 0x410, 0x510) || *(uint8_t**)(ctl + 0x918) != cr) return nullptr;
    return (float*)(ctl + 0x410);
}

}  // namespace

// diagnostic "ctldump": the character controller's floats around the position (+0x3c0..+0x4c0) and the game root yaw -
// where is the facing kept?
void CombatCtlDump() {
    float* p = ControllerPos();
    float gp[3], gy = 0.f;
    GameRoot(gp, &gy);
    if (!p) { Log("ctldump: no controller"); return; }
    const float* base = p - (0x410 - 0x3c0) / 4;
    char line[512];
    for (int row = 0; row < 16; ++row) {
        int o = 0;
        for (int k = 0; k < 4; ++k) o += sprintf_s(line + o, sizeof(line) - o, " %9.4f", base[row * 4 + k]);
        Log("ctldump: +0x%03x%s", 0x3c0 + row * 16, line);
    }
    Log("ctldump: game yaw %.2f deg (sin %.4f cos %.4f, half-angle sin %.4f cos %.4f)", gy * 57.2958f, sinf(gy), cosf(gy),
        sinf(gy * 0.5f), cosf(gy * 0.5f));
}
bool CombatInRage() { return g_inRage.load(); }

// D-094 (owner: "when Spartan Rage is ready (L3+R3), pressing both thumbsticks must not turn on F1"): the Spartan Rage
// meter is the hero creature's meter "Blood" (r_heroa00.wad, heroa00\main.lua: RageMeterCurrent =
// playerCreature:MeterGetValue("Blood"), next to HealthMeterCurrent = MeterGetValue("Health")). STATIC, as
// LuaInteract::MeterGetValue / MeterGetMax (0x1407d45f0 / 0x1407d4430) read it: the stats owner = the creature (object
// +0x220, type 2) -> its +0x3a58 interface, vfunc +0x18 -> [1]; meters = list at +0x968 (node: next, prev, context, def
// whose first qword is the name hash; the value is the float at node+0x20); max = sub_1407c3850(context, def + 8, 0)
// (read-only attribute sum, clamped). The name hash as the game builds it: per character (a-z upper-cased)
// h = (c + h) * 0x401, h ^= h >> 6.
static uint64_t MeterNameHash(const char* s) {
    uint64_t h = 0;
    for (; *s; ++s) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 0x20);
        h = (c + h) * 0x401ull;
        h ^= h >> 6;
    }
    return h;
}
using PFN_MeterMax = float(__fastcall*)(void*, const void*, void*);
// D-094b (owner log: "Rage meter NOT read" at every press): the stats owner depends on the Lua handle's type
// (sub_1407dea20): a Creature handle - the player's - resolves with sub_140549bc0 to the creature ITSELF (object
// +0x220, type 2) and the binding reads creature+0x968; only a GameObject handle goes through the +0x3a58 interface
// (the first version used that one alone). Both are tried, the creature first. 1 found, 0 list read but no such meter,
// -1 fault; `seen` collects the first name hashes (diagnostic)
static int FindMeter(uint8_t* owner, uint64_t hash, PFN_MeterMax meterMax, float* value, float* max, uint64_t* seen,
                     int* nSeen) {
    __try {
        uint8_t* head = owner + 0x968;
        int guard = 0;
        for (uint8_t* node = *(uint8_t**)head; node && node != head && guard < 64; node = *(uint8_t**)node, ++guard) {
            const uint64_t* def = *(const uint64_t**)(node + 0x18);
            if (!def) continue;
            if (*nSeen < 8) seen[(*nSeen)++] = *def;
            if (*def != hash) continue;
            *value = *(const float*)(node + 0x20);
            *max = meterMax(*(void**)(node + 0x10), def + 1, nullptr);
            return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
static uint8_t* MeterOwnerViaInterface(uint8_t* cr) {
    __try {
        uint8_t* iface = *(uint8_t**)(cr + 0x3a58);
        if (!iface) return nullptr;
        using PFN_Get = uint8_t*(__fastcall*)(uint8_t*);
        uint8_t* got = (*(PFN_Get**)iface)[0x18 / 8](iface);
        return got ? *(uint8_t**)(got + 8) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
static bool ReadHeroMeter(uint8_t* cr, uint64_t hash, PFN_MeterMax meterMax, float* value, float* max) {
    uint64_t seen[8];
    int nSeen = 0;
    const int direct = FindMeter(cr, hash, meterMax, value, max, seen, &nSeen);
    if (direct == 1) return true;
    uint8_t* owner = MeterOwnerViaInterface(cr);
    int viaIf = -2;
    if (owner && owner != cr) {
        viaIf = FindMeter(owner, hash, meterMax, value, max, seen, &nSeen);
        if (viaIf == 1) return true;
    }
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 5) {
        char hs[8 * 18 + 1] = {};
        int o = 0;
        for (int i = 0; i < nSeen; ++i) o += sprintf_s(hs + o, sizeof(hs) - o, " %016llx", (unsigned long long)seen[i]);
        Log("combat: Rage meter (\"Blood\" %016llx) not found - creature list %s, interface owner %p %s | meters seen:%s",
            (unsigned long long)hash, direct == 0 ? "read" : "faulted", owner,
            viaIf == -2 ? "not tried" : viaIf == 0 ? "read" : "faulted", nSeen ? hs : " none");
    }
    return false;
}
bool CombatRageMeter(float* value, float* max) {
    static const uint64_t kBlood = MeterNameHash("Blood");
    static const PFN_MeterMax meterMax =
        (PFN_MeterMax)((uint8_t*)GameVa(0x1407c3850ull));
    static std::atomic<int> logs{0};
    uint8_t* obj = HeroObject();
    uint8_t* cr = obj && !IsBadReadPtr(obj + 0x220, 8) ? *(uint8_t**)(obj + 0x220) : nullptr;
    if (!cr || IsBadReadPtr(cr, 0x3a60) || *(int32_t*)(cr + 0x14) != 2) {
        if (logs.fetch_add(1) < 5) Log("combat: Rage meter - no hero creature (object %p, creature %p)", obj, cr);
        return false;
    }
    float v = 0.f, m = 0.f;
    if (!ReadHeroMeter(cr, kBlood, meterMax, &v, &m)) return false;
    if (!(v == v) || !(m == m)) {
        if (logs.fetch_add(1) < 5) Log("combat: Rage meter read but not a number (%f / %f)", v, m);
        return false;
    }
    if (value) *value = v;
    if (max) *max = m;
    return true;
}
bool CombatRageReady() {
    float v = 0.f, m = 0.f;
    if (!CombatRageMeter(&v, &m)) return false;
    return m > 0.f && v >= m - fmaxf(0.001f, m * 0.001f);
}
bool CombatBladesFree() { return g_bladesFree.load(); }
bool CombatInStance() { return g_inStance.load(); }
static std::atomic<bool> g_heroIdle{false};
bool CombatHeroIdle() { return g_heroIdle.load(); }
void SetGameCamYaw(float yaw) { g_camYaw = yaw; }
float CombatStickNudge() { return g_nudge.load(); }
// D-200 (owner: the axe "doesn't register hits well" on the Draugr; owner log D-199: 21 of 39 light axe swings turned into
// the game's axe-STUCK follow-up csaxelightcombo01blendlight / csglobalmeleestickf01 the moment their hit spheres went
// live - the game's own hit, the axe lodged in the Draugr (human-like enemies do not lodge it) - and every one of those
// follow-ups ended 40 - 80 ms later "handed over to the game's run" / "game blocked": the player's stick, walking as F1
// fighters do, cut the follow-up in which Kratos yanks the axe back out. While that follow-up plays (F1, at most
// stuck_hold_ms 600) the left stick reaches the game centred, so the game finishes it as when standing still; the head,
// hands and the next swing are untouched. gowvr.ini stuck_hold_ms 0 = off.
std::atomic<bool> g_blockMove{false};
bool CombatBlockMove() { return g_blockMove.load(std::memory_order_relaxed); }
void CombatFastForward() { g_ffAt = GetTickCount64(); }
void CombatStanceFastForward() { g_ffStanceAt = GetTickCount64(); }
void CombatReach(float m) { g_reach = m; Log("combat: attack reach %.2f m", m); }
float CombatReachNow() { return g_reach.load(); }
bool CombatHeldYaw(float* yaw) {
    if (!g_heldYawValid.load()) return false;
    *yaw = g_heldYaw.load();
    return true;
}
void CombatAttackAim(bool on) { g_attackAim = on; Log("combat: attacks aimed at the view (facing held) %s", on ? "ON" : "OFF"); }
bool CombatHeldRoot(float* x, float* z, float* y) {
    if (!g_heldValid.load()) return false;
    *x = g_heldX.load();
    *z = g_heldZ.load();
    *y = g_heldYValid.load() ? g_heldY.load() : NAN;
    return true;
}
bool CombatReleaseOffset(float* dx, float* dz) {
    const long long q = g_relQpc.load();
    if (!q || g_heldValid.load()) return false;
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER nq;
    QueryPerformanceCounter(&nq);
    const float t = (float)(nq.QuadPart - q) / (float)freq.QuadPart;
    if (t < 0.f || t > 0.5f) return false;
    static const float tau = config::GetFloat("release_ease_tau", 0.09f);
    const float k = tau > 0.f ? expf(-t / tau) : 0.f;
    *dx = g_relOffX.load() * k;
    *dz = g_relOffZ.load() * k;
    return true;
}
// D-084 (owner: "while moving and doing attacks the camera moves up and down"): walking during a held attack the view
// took the game root's height, which carries the attack's own vertical root motion (LIVE stilltrace: 5-12 cm). VIEW
// only (the drawn body is untouched - owner: the reverted body-height hold broke the Blades): while a hold is on and
// its height is not held, the eye height follows the game's through a low-pass (walk_view_height_tau 0.35 s: the bob
// is damped, slopes are followed), starting from the height in use; after such a hold it eases onto the game's height
// (0.1 s, at most 0.6 s / 0.6 m). gameY in, the height to use out; false = use gameY as it is.
bool CombatViewHeight(float gameY, float* y) {
    static float vy = 0.f;
    static bool have = false, filtering = false;
    static LARGE_INTEGER freq{}, last{};
    static long long heldQ = 0;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    float dt = last.QuadPart ? (float)(q.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.f;
    last = q;
    if (dt < 0.f || dt > 0.1f) dt = 1.f / 90.f;
    static const float tau = config::GetFloat("walk_view_height_tau", 0.35f);
    if (g_heldValid.load()) {
        heldQ = q.QuadPart;
        if (g_heldYValid.load()) {  // standing: the held height is in use
            vy = g_heldY.load();
            have = true;
            filtering = false;
            return false;
        }
        if (!have || fabsf(vy - gameY) > 0.6f) vy = gameY;
        have = true;
        filtering = true;
        vy = Ease(vy, gameY, dt, tau);
        *y = vy;
        return true;
    }
    if (!filtering) { have = false; return false; }  // after a standing hold: as before
    const float since = (float)(q.QuadPart - heldQ) / (float)freq.QuadPart;
    vy = Ease(vy, gameY, dt, 0.1f);
    if (since > 0.6f || fabsf(vy - gameY) > 0.6f || fabsf(vy - gameY) < 0.002f) {
        have = filtering = false;
        return false;
    }
    *y = vy;
    return true;
}
void CombatNudgeAmount(float a) { g_nudgeAmt = a; Log("combat: recovery nudge %.2f", a); }
void CombatAttackHold(bool on) { g_attackHold = on; Log("combat: attack position hold (no lunge) %s", on ? "ON" : "OFF"); }
bool CombatSprinting() { return g_sprinting.load(); }
bool CombatBusy() { return g_busy.load(); }
ULONGLONG CombatIdleMs() {
    const ULONGLONG t = g_idleSince.load();
    if (g_busy.load()) return 0;
    return t ? GetTickCount64() - t : 1000000ull;  // never attacked yet: long idle
}
void CombatTrace(int frames) { g_trace = frames; }
// D-154 (owner: "R3 on a stunned enemy: Kratos sprints to catch him, but in VR he doesn't always - when I move my head
// closer during the sprint it executes"): the no-hold window after R3 was a fixed 1.2 s - a longer approach met an attack
// hold that held Kratos' position. Now it lasts until the finisher's sync move has started and ended (or 3.5 s), and
// ends at once when the player swings (then the R3 was a lock-on). The first presses also trace the moves (combattrace)
void CombatFinisherPressed() {
    const ULONGLONG now = GetTickCount64();
    g_finisherAt = now;
    g_finisherUntil = now + 3500;
    static int traces = 0;
    if (traces < 6) {
        ++traces;
        g_trace = 260;  // ~3 s of moves / positions
    }
    Log("combat: R3 pressed (finisher / lock-on) - no attack holds until the finisher is done (max 3.5 s)");
}
void CombatFinisherCancel() {
    if (g_finisherUntil.load() > GetTickCount64() && !HeroInSyncMove()) g_finisherUntil = 0;
}
bool CombatInOwnFollowUp() { return g_ownFollowUp.load(); }
unsigned long long CombatLastAttackStartMs() { return g_lastAttackAt.load(); }
void CombatHitRadius(float m) {
    g_hitRadius = m;
    Log("combat: hit radius %.2f m (Kratos' volume tests so far %llu, hits %llu)", m, (unsigned long long)g_volHeroTests.load(),
        (unsigned long long)g_volHeroHits.load());
}
void CombatHitRadiusFist(float m) {
    g_hitRadiusFist = m;
    config::SetFloat("hit_radius_fist", m);
    Log("combat: fist hit radius %.2f m (weapon %d, axe in hand %d)", m, CurrentWeapon(), (int)AxeInHand());
}
void CombatTsProbe(bool on) {
    static bool installed = false;
    if (on && !installed) {
        installed = true;
        Log("combat: time-scale probe installed %d", (int)DetourInstall(kTsSet, (void*)&hk_TsSet, (void**)&o_tsSet));
    }
    g_tsProbeLogs = 0;
    g_tsProbeOn = on;
    Log("combat: time-scale probe %s", on ? "ON (stacks of slot dips below 0.6, 80 max)" : "off");
}
void CombatHitProbe(bool on) {
    static bool installed = false;
    if (on && !installed) {
        installed = true;
        const bool a = DetourInstall(kProbe0, (void*)&hk_p0, (void**)&o_p0);
        const bool b = DetourInstall(kProbe1, (void*)&hk_p1, (void**)&o_p1);
        const bool c = DetourInstall(kProbe2, (void*)&hk_p2, (void**)&o_p2);
        const bool d = DetourInstall(kProbe3, (void*)&hk_p3, (void**)&o_p3);
        Log("combat: hit probes installed %d %d %d %d", (int)a, (int)b, (int)c, (int)d);
    }
    if (!on) {
        for (int i = 0; i < 4; ++i)
            Log("combat: probe %s called %llu times", kProbeName[i], (unsigned long long)g_probeCount[i].load());
        return;
    }
    for (int i = 0; i < 4; ++i) { g_probeLogs[i] = 0; g_probeCount[i] = 0; }
    Log("combat: hit probes armed (stacks of the first 40 calls each)");
}
void CombatRequestAnimate() {
    g_animateReqMove = ActiveMoveHash();
    g_animateReq = GetTickCount64();
}

float CombatHoldWeight() { return g_holdW.load(); }
bool CombatArmsFromAnimation() { return g_armsAnim.load(); }
float CombatBodyLift() { return g_lift.load(); }
void CombatReportRise(float rise) { g_rise = rise; }
void CombatCameraOffset(float out[3]) {
    AcquireSRWLockShared(&g_camLock);
    for (int i = 0; i < 3; ++i) out[i] = g_camOff[i];
    ReleaseSRWLockShared(&g_camLock);
}
void CombatEnable(bool on) {
    g_on = on;
    Log("combat: F1 body rules %s", on ? "on" : "off");
}

// D-080 diagnostic: melee hits never reached the hooked per-hit handler (owner session: 200 held attacks, 0 hits there;
// the 23 logged hits were thrown-axe hits). A melee hit's hit pause should slow Kratos' creature time scale for a few
// frames: during a held attack (and 0.6 s after) any time-scale slot other than ours (5) dropping below 0.6 is logged
// with the time into the attack - candidate melee-hit timing from the owner's sessions.
void HitPauseWatch(bool holding, ULONGLONG now) {
    static ULONGLONG lastHold = 0, lastLog = 0;
    static int logs = 0;
    if (holding) lastHold = now;
    if (!lastHold || now - lastHold > 600 || logs >= 300) return;
    uint8_t* obj = HeroObject();
    if (!obj || IsBadReadPtr(obj + 0x220, 8)) return;
    uint8_t* cr = *(uint8_t**)(obj + 0x220);
    if (!cr || IsBadReadPtr(cr + 0x308, 0x28)) return;
    const float* s = (const float*)(cr + 0x308);
    for (int i = 0; i < 10; ++i) {
        if (i == 5 || !(s[i] < 0.6f) || s[i] < 0.f) continue;
        if (now - lastLog < 120) return;
        lastLog = now;
        ++logs;
        const ULONGLONG t0 = g_attackStartMs.load();
        Log("combat: time-scale slot %d = %.2f (hit pause?) %llu ms into the attack | slots %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
            i, s[i], t0 ? (unsigned long long)(now - t0) : 0ull, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9]);
        return;
    }
}

// ---- D-097 interaction context (owner list: doors, chests, carrying the boar / Atreus / a crystal, the snow walk with
// the axe on the ground, riding the "giant" (ogre), Atreus behind the shield, the vine choke, dragging, chains,
// climbing ...): which hands follow the ANIMATION, whether the body keeps the game's facing (does not turn with the
// head) and whether the right grip / trigger are plain R1 / R2 (the button presses those scenes ask for). From the
// hero's animation names (r_heroa00.as families: env* environment interactions - chests envContainer*, chains
// envChainPulley*, cranks / wheels / levers, push blocks; nar* / poi* / scn* narrative scenes; cs* combat-sync scenes -
// the ogre ride csJotunnRageKill* (Light / Heavy = R1 / R2), Modi / Baldur shield and choke csMMA* / csChiselMMA* /
// csChokeMMA*; navCarryBoar* / navCarrySon* / navCarryCrystal* / navCarryTreeTrunk*; navSnowBlindShuffle*; navClimb*;
// hitTrapFreyaVines*; freyaDoor*; baldurDragon*; envThrowable* / navCombatThrowableAim* / attThrowable*) and the game's
// sync-move flag. Every new combination is logged ("context: ..."), so missed scenes show their names.
// explicitHands (D-098): a per-hand rule (carries, throwables) - the game plays these as sync moves, which must not make
// them ANIMATE (both arms from the animation)
struct InteractCtx { bool auth[2] = {false, false}; bool bodyLock = false, buttons = false, throwable = false, explicitHands = false; const char* why = ""; };
std::atomic<bool> g_ctxAuth[2] = {false, false}, g_ctxBodyLock{false}, g_ctxButtons{false}, g_ctxThrowable{false}, g_ctxExplicit{false};
static InteractCtx ClassifyContext(const char* prim, const char* anim, bool sync) {
    InteractCtx c;
    char lp[128], la[128];  // lower-cased once (keys below are lower case)
    {
        size_t i = 0;
        for (; prim && prim[i] && i < sizeof(lp) - 1; ++i) lp[i] = (char)tolower((unsigned char)prim[i]);
        lp[i] = 0;
        for (i = 0; anim && anim[i] && i < sizeof(la) - 1; ++i) la[i] = (char)tolower((unsigned char)anim[i]);
        la[i] = 0;
    }
    auto has = [&](const char* k) { return strstr(lp, k) != nullptr || strstr(la, k) != nullptr; };
    auto starts = [&](const char* k) { const size_t n = strlen(k); return strncmp(lp, k, n) == 0 || strncmp(la, k, n) == 0; };
    auto both = [&](bool lock, bool buttons, const char* why) {
        c.auth[0] = c.auth[1] = true;
        c.bodyLock = lock;
        c.buttons = buttons;
        c.why = why;
    };
    const bool attackName = starts("att") || starts("def");
    // D-100 (owner: "I hit an enemy, the axe stayed in place on the enemy where I hit it, and I couldn't hit again for
    // a few seconds"; owner log: a connecting light axe swing turns into the combat-sync follow-up move
    // csaxelightcombo01blendlight / anim csglobalmeleestickf01 - the axe sticking in the enemy - which the cs* scene
    // rule below made "both hands animated, plain buttons, no swings"): Kratos' own attack follow-ups are combat, not
    // scenes - no context (the Rage grabs / beatdowns csRage*, the ogre ride, the Modi / Baldur scenes keep theirs)
    if (starts("csaxe") || starts("csblade") || starts("csglobalmelee") || has("meleestick") || starts("csbare") ||
        starts("cskick") || starts("csspartan") || starts("cschain"))
        return c;
    // D-193: a scripted object weapon (the tree-trunk bat of the Baldur fight) - body locked to the game's facing.
    // D-194 (owner: "I misspoke - in the tree trunk scene I should NOT have motion controls"): both hands from the
    // animation, plain buttons (grip = R1, trigger = R2)
    if (starts("atttreebat")) {
        both(true, true, "scripted object weapon (tree-trunk bat)");
        return c;
    }
    // a throwable in the hand: it rides the animated right hand; swings throw it (no fist needed), trigger = R2
    if (has("throwable") || has("crystalsprintthrow")) {
        c.auth[1] = true;
        c.throwable = true;
        c.explicitHands = true;
        c.why = "throwable in hand";
        return c;
    }
    // carries (per hand, owner)
    if (has("carryboar") || has("witchintro022")) { both(false, false, "carrying the boar"); c.explicitHands = true; return c; }
    if (has("carryson") || has("songapcarry")) { both(false, false, "carrying Atreus"); c.explicitHands = true; return c; }
    if (has("carrycrystal") || (has("crystal") && !attackName)) { c.auth[0] = true; c.why = "carrying a crystal (left)"; c.explicitHands = true; return c; }
    if (has("bifrost")) { c.auth[0] = true; c.why = "the Bifrost crystal (left)"; c.explicitHands = true; return c; }
    if (has("snowblind")) { c.auth[1] = true; c.why = "snow walk, axe on the ground (right)"; c.explicitHands = true; return c; }
    if (has("carrytreetrunk")) { c.auth[0] = true; c.why = "carrying a tree trunk (left)"; c.explicitHands = true; return c; }
    // riding an ogre ("the giant") / the dragon: no motion control, body still, R1 / R2 plain
    if (has("jotunn") || has("dragon")) { both(true, true, "riding (ogre / dragon)"); return c; }
    // climbing, chains, cranks, levers, pushing / dragging: hands on the object, body keeps the game's facing
    if (!attackName && (has("climb") || has("chainpulley") || has("crank") || has("lever") || has("winch") ||
                        has("push") || has("pull") || has("drag") || has("debrislift"))) {
        both(true, true, "climbing / chain / crank / lever / push / drag");
        return c;
    }
    // doors, chests, narrative / scene / combat-sync moves, the vine trap, sync moves
    if (starts("env") || starts("nar") || starts("cs") || starts("poi") || starts("scn") || starts("freya") ||
        has("door") || has("vines") || sync) {
        both(true, true, sync ? "sync move" : "interaction / scene");
        return c;
    }
    return c;
}
static void ContextTick(bool f1, const char* prim, const char* anim, bool sync) {
    InteractCtx c;
    if (f1) c = ClassifyContext(prim, anim, sync);
    // held 300 ms after a context ends: the animation changes between parts of one interaction (enter / idle / exit)
    static ULONGLONG lastOn[2] = {0, 0}, lastLock = 0, lastButtons = 0, lastThrow = 0, lastExplicit = 0;
    const ULONGLONG now = GetTickCount64();
    auto hold = [&](bool on, ULONGLONG& last) { if (on) last = now; return on || (f1 && last && now - last < 300); };
    g_ctxAuth[0] = hold(c.auth[0], lastOn[0]);
    g_ctxAuth[1] = hold(c.auth[1], lastOn[1]);
    g_ctxBodyLock = hold(c.bodyLock, lastLock);
    g_ctxButtons = hold(c.buttons, lastButtons);
    g_ctxThrowable = hold(c.throwable, lastThrow);
    g_ctxExplicit = hold(c.explicitHands, lastExplicit);
    if (!f1) return;
    // log each new animation combination with its decision (capped)
    static std::string lastKey;
    static int logs = 0;
    std::string pb(prim ? prim : ""), ab(anim ? anim : "");
    pb = pb.substr(0, pb.find('#'));
    ab = ab.substr(0, ab.find('#'));
    const std::string key = pb + "|" + ab + (sync ? "|S" : "");
    if (key != lastKey) {
        lastKey = key;
        const bool any = c.auth[0] || c.auth[1] || c.bodyLock || c.buttons || c.throwable;
        static std::string lastAnyKey;
        if ((any || !lastAnyKey.empty()) && logs < 400) {
            ++logs;
            Log("context: anim '%s' move '%s'%s -> hands %s%s, body %s, %s%s", pb.c_str(), ab.c_str(), sync ? " (sync move)" : "",
                c.auth[0] ? "L" : "", c.auth[1] ? "R" : "", c.bodyLock ? "keeps the game's facing" : "follows the head",
                c.buttons ? "grip/trigger = R1/R2" : c.throwable ? "swings throw" : "gestures", any ? (std::string(" (") + c.why + ")").c_str() : "");
        }
        lastAnyKey = any ? key : std::string();
    }
}
bool ContextHandAuthored(int h) { return h >= 0 && h < 2 && g_ctxAuth[h].load(); }
bool ContextBodyLocked() { return g_ctxBodyLock.load(); }
bool ContextPlainButtons() { return g_ctxButtons.load(); }
bool ContextThrowable() { return g_ctxThrowable.load(); }

void CombatTick() {
    static bool cfg = false;
    if (!cfg) { cfg = true; g_on = config::GetFloat("combat_f1", 1.f) > 0.5f; }
    InstallHitLog();
    InstallHitRadius();
    HitPauseWatch(CombatHoldWeight() > 0.001f, GetTickCount64());
    static LARGE_INTEGER freq{}, last{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER nowq;
    QueryPerformanceCounter(&nowq);
    float dt = last.QuadPart ? (float)(nowq.QuadPart - last.QuadPart) / (float)freq.QuadPart : 1.f / 90.f;
    if (dt <= 0.f || dt > 0.1f) dt = 1.f / 90.f;
    last = nowq;
    const ULONGLONG now = GetTickCount64();

    const bool f1 = g_on.load() && FpMode() == 1;
    const char* anim = ActiveMoveAnim();
    Policy p = f1 && ActiveMoveHash() ? Classify(anim, HeroInSyncMove()) : Policy::None;

    char prim[96];
    HeroPrimaryAnim(prim, sizeof(prim));
    ContextTick(FpMode() == 1, prim, ActiveMoveHash() ? anim : "", HeroInSyncMove());  // D-097 (the move name only while one plays)
    {   // D-200: the axe-stuck follow-up plays to its end
        static const float holdMs = config::GetFloat("stuck_hold_ms", 0.f);  // D-201: off by default (owner: "feels kinda worse")
        auto stuckName = [](const char* a) {
            return a && (StartsWithI(a, "csaxelight") || StartsWithI(a, "csglobalmeleestick"));
        };
        const bool stuck = f1 && holdMs > 0.f && ((ActiveMoveHash() && stuckName(anim)) || stuckName(prim));
        static ULONGLONG since = 0;
        static int logs = 0;
        if (stuck && !since) {
            since = now;
            if (logs++ < 30) Log("combat: axe stuck in the target - the stick held centred while Kratos pulls it out (D-200)");
        }
        if (!stuck) since = 0;
        g_blockMove = stuck && (float)(now - since) < holdMs;
    }
    // D-098 (owner: "carrying the log, my right hand should be motion controlled, the left one not"; owner log: the carry
    // navCarryTreeTrunk* is a sync move -> ANIMATE -> BOTH arms from the animation): a per-hand context rule wins
    if (p == Policy::Animate && g_ctxExplicit.load()) p = Policy::None;
    // D-062 trackers (every mode; only F1 uses them)
    {
        static char lastPrim[96] = {};
        if (prim[0] && strcmp(prim, lastPrim) != 0) {
            strcpy_s(lastPrim, prim);
            const bool was = g_inRage.load();
            if (ContainsI(prim, "rage")) g_inRage = !ContainsI(prim, "rageexit");
            else if (StartsWithI(prim, "nav")) g_inRage = false;
            if (g_inRage.load() != was) Log("combat: Spartan Rage %s (%s)", g_inRage.load() ? "ON" : "off", prim);
            const bool sprint = StartsWithI(prim, "navbrutalrun");
            if (sprint != g_sprinting.load()) g_sprinting = sprint;
        }
        static bool attPrev = false;
        static int attLogs = 0;
        const bool attNow = ActiveMoveHash() && IsAttackAnim(anim);
        if (attNow && !attPrev) g_lastAttackAt = now;  // D-154
        if (attNow && !attPrev && attLogs < 200) {
            ++attLogs;
            Log("combat: attack started %s", anim);
        }
        attPrev = attNow;
        {   // D-154: one of Kratos' own attack follow-ups (D-100's list: the axe stuck in the enemy, ...)
            auto st = [&](const char* k) { return (anim && StartsWithI(anim, k)) || (prim && StartsWithI(prim, k)); };
            const bool fu = st("csaxe") || st("csblade") || st("csglobalmelee") || st("csbare") || st("cskick") ||
                            st("csspartan") || st("cschain") || (anim && ContainsI(anim, "meleestick"));
            g_ownFollowUp = fu;
            // the finisher window: done once a sync move has started and ended
            static bool sawSync = false;
            if (now < g_finisherUntil.load()) {
                if (HeroInSyncMove()) sawSync = true;
                else if (sawSync) {
                    sawSync = false;
                    g_finisherUntil = now;
                    Log("combat: finisher done %llu ms after R3", (unsigned long long)(now - g_finisherAt.load()));
                }
            } else {
                sawSync = false;
            }
        }
        // the "delay stance" after an attack is the game's pause-combo window (LIVE trace: R1 during
        // navBareDelayStanceEnter01 -> attShieldBackHandedUpper, not the first attack): busy too
        // (the Blades' version: navLightBladeCombatStanceIdleRight, ~4.3 s - R1 in it = the pause variant attbladeslight00)
        const bool stance = ContainsI(prim, "delaystance") || ContainsI(anim, "delaystance") ||
                            ContainsI(prim, "combatstance") || ContainsI(anim, "combatstance");
        g_inStance = stance;
        g_heroIdle = prim[0] && ContainsI(prim, "idle") && !stance;
        const bool busy = (ActiveMoveHash() && IsAttackAnim(anim)) || IsAttackAnim(prim) || HeroInSyncMove() || stance;
        if (g_trace.load() > 0) {
            g_trace.fetch_sub(1);
            float tsx = 0.f, tsy = 0.f;
            input::PlayerLeftStick(&tsx, &tsy);
            float rp[3] = {}, ry = 0.f;
            RenderRoot(rp, &ry);
            Mat4 nk{};
            StableJoint(kNeckHash, &nk);
            float ey[3];
            FpEye(ey);
            // D-077: the controller as the game left it (before this tick's write) and the game-built root
            float ctl[2] = {NAN, NAN}, gr[3] = {NAN, NAN, NAN}, gry = 0.f;
            if (float* c = ControllerPos()) { ctl[0] = c[0]; ctl[1] = c[2]; }
            GameRoot(gr, &gry);
            Log("combattrace: move %016llx '%s' prim '%s' busy %d nudge %.2f stick %.2f %.2f | root %.3f %.3f %.3f yaw %.1f neck %.3f %.3f %.3f"
                " eye %.3f %.3f %.3f ctl %.3f %.3f game %.3f %.3f held %d %.3f %.3f", (unsigned long long)ActiveMoveHash(), anim, prim,
                (int)busy, g_nudge.load(), tsx, tsy, rp[0], rp[1], rp[2], ry * 57.2958f, nk.m[3][0], nk.m[3][1], nk.m[3][2], ey[0],
                ey[1], ey[2], ctl[0], ctl[1], gr[0], gr[2], (int)g_heldValid.load(), g_heldX.load(), g_heldZ.load());
        }
        if (busy != g_busy.load()) {
            g_busy = busy;
            if (!busy) g_idleSince = now;
        }
    }
    // D-062 animate request: the first attack move that starts after a button-triggered request plays in full
    {
        static bool locked = false;
        static ULONGLONG lockT = 0, noAttSince = 0;
        const ULONGLONG req = g_animateReq.load();
        if (!locked && req && now - req < 700 && ActiveMoveHash() && ActiveMoveHash() != g_animateReqMove.load() &&
            IsAttackAnim(anim)) {
            locked = true;
            lockT = now;
            noAttSince = 0;
            g_animateReq = 0;
            Log("combat: button move -> full animation (%s)", anim);
        } else if (req && now - req >= 700) {
            g_animateReq = 0;
        }
        if (locked) {
            const bool att = ActiveMoveHash() && IsAttackAnim(anim);
            if (att) noAttSince = 0;
            else if (!noAttSince) noAttSince = now;
            if ((noAttSince && now - noAttSince > 250) || now - lockT > 6000) locked = false;
            else if (f1 && p != Policy::None) p = Policy::Animate;
        }
    }
    // G1: hold the controller's horizontal position while a HELD-body attack plays (owner: no lunge / slide). Not for
    // evades, grabs / finishers, and not for full-animation moves (owner: "those are supposed to be sliding"): an
    // attack that turns out ANIMATE mid-way is released at once.
    // D-064 (owner): (1) the player keeps walking during a held attack - the held position moves with the left stick
    // (game-camera frame, attack_move_speed at full stick, max 3 m per attack); (2) after a held attack the game plays a
    // "delay stance" (with its sound) when the stick rests - walking avoided it (owner) because stick input sends the
    // recovery into locomotion: from nudge_after_ms into the attack until nudge_ms after it a small forward push is fed
    // while the player's stick rests, the position still held (no step); (3) the wind-up of a held attack plays at
    // attack_windup_boost x for attack_windup_ms (like the axe throw's time boost): the hit follows the swing sooner.
    {
        static bool holding = false, animated = false, boosted = false;
        static float hp[2], hp0[2], moved = 0.f, movedY = 0.f, savedTs = 1.f, heldYaw = 0.f, reachPeak = 0.f;
        static bool recaptureY = false;  // D-084
        static ULONGLONG attT0 = 0, endT = 0;
        static uint64_t holds = 0;
        // LIVE rootpos: Kratos walks 2.86 m/s at full stick, 1.36 m/s at half
        static const float moveSpeedCap = config::GetFloat("attack_move_speed", 5.0f);  // D-077: a cap (m/s)
        static const float nudgeAfter = config::GetFloat("nudge_after_ms", 300.f), nudgeMs = config::GetFloat("nudge_ms", 450.f);
        const float nudgeAmt = g_nudgeAmt.load() >= 0.f ? g_nudgeAmt.load() : config::GetFloat("recovery_nudge", 0.9f);
        // D-151: the wind-up boost lasts until the hit spheres go live (was a fixed 120 ms at 2.5x)
        static const float boost = config::GetFloat("attack_windup_boost", 4.0f);
        const bool bladesFree = f1 && (IsBladesFreeAnim(anim) || IsBladesFreeAnim(prim));
        g_bladesFree = bladesFree;
        // D-080 (owner: "the R3 finisher triggers some animation but doesn't execute the enemy; it should be full
        // animation, no motion controls, unfrozen body"): a sync move (finisher) or the 1.2 s after an R3 press is the
        // game's alone - no hold starts, and a running hold (an attack's tail kept writing Kratos' position / facing
        // every frame, LIVE owner log: HOLD re-entered during csaxelightcombo01blendlight) is released at once
        const bool syncMove = f1 && (HeroInSyncMove() || now < g_finisherUntil.load());
        const bool attack = f1 && !syncMove && !bladesFree &&
                            ((ActiveMoveHash() && IsAttackAnim(anim) && !StartsWithI(anim, "def")) ||
                             (IsAttackAnim(prim) && !StartsWithI(prim, "def")));
        float sx = 0.f, sy = 0.f;
        input::PlayerLeftStick(&sx, &sy);
        const float sm = sqrtf(sx * sx + sy * sy);
        float* cp = attack || holding ? ControllerPos() : nullptr;
        // D-077 (owner: "the head alignment when moving and attacking"): LIVE walk-attack trace - running 4.1 m/s, then the
        // attack moved the held spot at a fixed 2.9 m/s, and after it the game restarted the run from 0.1 m/s (~0.7 s to
        // get back to speed). Now the held spot carries the game's own velocity from before the attack, steered by the
        // stick and eased; after the attack, while the player walks on, it keeps driving the position until the game's own
        // run has caught up (handoff), then lets go
        static float gvel[2] = {0.f, 0.f}, vel[2] = {0.f, 0.f}, lastGp[2] = {0.f, 0.f}, refSpeed = 0.f, sm0 = 0.f;
        static bool haveLastGp = false, handoff = false, firstTick = false;
        static ULONGLONG handoffT = 0;
        static float lastWritten[2] = {0.f, 0.f};
        static int catchN = 0;
        static float prevRead[2] = {0.f, 0.f}, prevStep[2] = {0.f, 0.f}, blockDir[2] = {0.f, 0.f};
        static bool havePrevRead = false, blocked = false;
        static int staticN = 0;
        {
            float gp[3], gy;
            if (!holding && GameRoot(gp, &gy) && dt > 1e-4f) {
                if (haveLastGp) {
                    const float k = 1.f - expf(-dt / 0.08f);
                    for (int i = 0; i < 2; ++i) {
                        const float d = (i ? gp[2] - lastGp[1] : gp[0] - lastGp[0]) / dt;
                        gvel[i] += (fminf(fmaxf(d, -8.f), 8.f) - gvel[i]) * k;
                    }
                }
                lastGp[0] = gp[0]; lastGp[1] = gp[2];
                haveLastGp = true;
            } else if (holding) {
                haveLastGp = false;
            }
        }
        // the stick's target velocity: the game's speed at the attack start when the player was already moving, else a
        // speed curve measured LIVE (half stick 1.4 m/s, full 4.1 m/s)
        auto steer = [&]() {
            float tv[2] = {0.f, 0.f};
            if (sm > 0.2f) {
                const float cy = g_camYaw.load(), s = sinf(cy), c = cosf(cy);
                const float dx = (sx * c - sy * s) / sm, dz = (-sx * s - sy * c) / sm;  // right (c,-s), forward (-s,-c)
                float speed;
                if (refSpeed > 0.5f) speed = refSpeed * fminf(sm / fmaxf(sm0, 0.3f), 1.2f);
                else speed = sm < 0.5f ? 2.8f * sm : sm < 0.8f ? 1.4f + (sm - 0.5f) * 5.f : 2.9f + (sm - 0.8f) * 6.f;
                speed = fminf(speed, moveSpeedCap);
                tv[0] = dx * speed; tv[1] = dz * speed;
            }
            const float k = 1.f - expf(-dt / 0.12f);
            for (int i = 0; i < 2; ++i) vel[i] += (tv[i] - vel[i]) * k;
            if (blocked) {  // no motion into the direction the game held Kratos back in (sideways stays)
                const float into = vel[0] * blockDir[0] + vel[1] * blockDir[1];
                if (into > 0.f) { vel[0] -= into * blockDir[0]; vel[1] -= into * blockDir[1]; }
            }
            hp[0] += vel[0] * dt;
            hp[1] += vel[1] * dt;
            moved += sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) * dt;
            movedY += sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) * dt;
        };
        auto release = [&](const char* why) {
            holding = false;
            handoff = false;
            // D-081 (owner, reach 2.5 m back: "it teleports me when doing attacks, only the axe"): LIVE every release
            // 0.43-0.77 m off the held spot was "game blocked" - the first tail frame still had the game body lunged AHEAD
            // (the axe lunges furthest), read as a push back, let go there, > 0.6 m = no ease = a jump. The lunge (game
            // body ahead along the attack's facing) is put back on the held spot first; a real push back (behind, a
            // ledge / wall) keeps the game's position
            if (cp && g_attackHold.load() && !animated && !syncMove) {
                const float dx = cp[0] - hp[0], dz = cp[2] - hp[1];
                const float ahead = -(dx * sinf(heldYaw) + dz * cosf(heldYaw));
                if (ahead > 0.02f) { cp[0] = hp[0]; cp[2] = hp[1]; }
            }
            // the view eases from the held spot onto the game's position (where the game has Kratos this frame; a real
            // jump - over 0.6 m - still snaps); only when the view was on the held spot
            if (cp && g_heldValid.load()) {
                const float ex = hp[0] - cp[0], ez = hp[1] - cp[2];
                if (ex * ex + ez * ez < 0.36f) {
                    LARGE_INTEGER q;
                    QueryPerformanceCounter(&q);
                    g_relOffX = ex;
                    g_relOffZ = ez;
                    g_relQpc = q.QuadPart;
                }
            }
            float gp[3], gy;
            if (holds <= 400 && GameRoot(gp, &gy))
                Log("combat: attack over - %.2f m from the held spot, walked %.2f m (dir %.2f %.2f, camera forward %.2f %.2f), "
                    "game facing %.1f deg vs held %.1f, reach used %.2f m (%s), %d hit(s), %.2f s game time",
                    sqrtf((gp[0] - hp[0]) * (gp[0] - hp[0]) + (gp[2] - hp[1]) * (gp[2] - hp[1])), moved, hp[0] - hp0[0],
                    hp[1] - hp0[1], -sinf(g_camYaw.load()), -cosf(g_camYaw.load()), gy * 57.2958f, heldYaw * 57.2958f,
                    reachPeak, why, g_attackHits.load(), g_attackGameT.load());
            reachPeak = 0.f;
            g_attackStartMs = 0;
            // D-079 (owner: after an attack, moving, "the axe and the Blades misalign, then settle back" - not reproduced
            // with the harness's simulated hands): the weapons / hands vs the controllers per frame after such releases
            static int autoTraces = 0;
            if (autoTraces < 25 && config::GetFloat("wtrace_after_attacks", 1.f) > 0.5f) {
                ++autoTraces;
                WeaponTrace(160);  // ~1.8 s: covers starting to walk right after the attack
            }
        };
        if (attack && cp) {
            if (!holding || endT) {  // a new attack (or the next one right after the last)
                // D-079: every attack (also the next one of a spam chain, started while the last is still held) gets its
                // own clock - LIVE gatespam: the chained attacks inherited the first one's game time and were fast-
                // forwarded 74 ms after their start again
                g_attackGameT = 0.f;
                g_attackStartMs = now;
                if (!holding) {
                    firstTick = true;
                    g_attackHits = 0;
                    havePrevRead = false;
                    blocked = false;
                    staticN = 0;
                    hp[0] = hp0[0] = cp[0]; hp[1] = hp0[1] = cp[2]; moved = 0.f; movedY = 0.f;
                    vel[0] = gvel[0]; vel[1] = gvel[1];  // D-077: the game's own velocity carries into the attack
                    refSpeed = sqrtf(gvel[0] * gvel[0] + gvel[1] * gvel[1]);
                    sm0 = sm;
                } else if (movedY >= 0.05f && sm < 0.15f && sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) < 0.15f) {
                    // D-084 (owner: "when you move and attack and then stand still it still goes up and down when doing
                    // attacks - I have to walk without attacking, then stand still"): the held height only applied while
                    // the hold had not moved 5 cm, counted over the WHOLE hold - chained attacks keep one hold, so after
                    // any walking every later attack bobbed (the game root's vertical attack motion) until a walk without
                    // an attack ended the hold. An attack that starts while the player stands takes its height again
                    movedY = 0.f;
                    recaptureY = true;
                }
                handoff = false;
                // D-066 (owner: "the attacks must go where I'm looking and where Kratos' body looks"): the game's facing
                // (controller+0x430, radians, LIVE ctldump = the game root yaw) is set to the view, turned about the
                // eye pivot like the drawn body (the eye point - and so the view - does not move), and then held for the
                // attack (the game turned Kratos during attacks: the eye swung "a little and came back" - owner)
                if (g_attackAim.load()) {
                    float* yp = cp + (0x430 - 0x410) / 4;
                    const float gy0 = yp[0], vy = ViewYaw();
                    float kR = 0.f, kB = 0.f;
                    BodyPivot(&kR, &kB);
                    const float ex = hp[0] + cosf(gy0) * kR + sinf(gy0) * kB, ez = hp[1] - sinf(gy0) * kR + cosf(gy0) * kB;
                    hp[0] = ex - (cosf(vy) * kR + sinf(vy) * kB);
                    hp[1] = ez - (-sinf(vy) * kR + cosf(vy) * kB);
                    if (!endT) { hp0[0] = hp[0]; hp0[1] = hp[1]; }
                    heldYaw = vy;
                    yp[0] = vy;
                    cp[0] = hp[0];
                    cp[2] = hp[1];
                    if (holds <= 80)
                        Log("combat: attack aimed at the view (game facing %.1f -> %.1f deg, turned about the eye)", gy0 * 57.2958f,
                            vy * 57.2958f);
                }
                holding = true;
                animated = p == Policy::Animate;
                attT0 = now;
                endT = 0;
                ++holds;
            }
            if (p == Policy::Animate) animated = true;
            if (g_attackHold.load() && !animated) {
                // walking while attacking (D-077: the game's velocity carried and eased). Not on the attack's first tick:
                // the controller read there already holds this frame's step (LIVE wkd0: the extra step put the view one
                // frame ahead - an 8 m/s frame, ~6 cm forward jolt, at every walking attack's start)
                // D-077 blocked (LIVE wkd2, ledge): the game stopped accepting our position - its own read stayed on the
                // edge while we stepped on (own step -0.06, -0.13 ... -0.33 m along our walk), so the view walked 0.33 m
                // past where Kratos stood (and would walk into walls). The game's read not moving while we step, and
                // behind us, for 2 frames: no more motion into that direction for this attack (sliding along it stays)
                if (havePrevRead) {
                    const float sl = sqrtf(prevStep[0] * prevStep[0] + prevStep[1] * prevStep[1]);
                    const float rx = cp[0] - prevRead[0], rz = cp[2] - prevRead[1];
                    const float behind = sl > 1e-4f ? ((cp[0] - hp[0]) * prevStep[0] + (cp[2] - hp[1]) * prevStep[1]) / sl : 0.f;
                    staticN = sl > 0.02f && sqrtf(rx * rx + rz * rz) < 0.25f * sl && behind < -0.03f ? staticN + 1 : 0;
                    if (staticN >= 2 && !blocked) {
                        blocked = true;
                        blockDir[0] = prevStep[0] / sl;
                        blockDir[1] = prevStep[1] / sl;
                        if (holds <= 80) Log("combat: the game holds Kratos back (%.2f m behind the held spot) - no walking that way", -behind);
                    }
                }
                prevRead[0] = cp[0];
                prevRead[1] = cp[2];
                const float hx0 = hp[0], hz0 = hp[1];
                if (!firstTick) steer();
                firstTick = false;
                prevStep[0] = hp[0] - hx0;
                prevStep[1] = hp[1] - hz0;
                havePrevRead = true;
                // D-075 reach (owner: "make the hits register up to 2.5 m"): the drawn body and the view stay on the held
                // spot (CombatHeldRoot / firstperson.cpp), but the game's own position may follow the attack's lunge up to
                // attack_reach m, so its hitboxes reach the enemies; it snaps back at the end of the attack (tail below)
                // D-076: default 0 (owner: enemies "float 2 m in front" during attacks = they are hit / aligned against the
                // game's invisible lunged body; and the teleport). gowvr.ini attack_reach=2.5 brings the reach back.
                // D-081 (owner: "please do the 2.5 m reach like I told you"): default 2.5 again (D-076 had set 0 because
                // of the enemies floating in front; the snap back at the attack's end stays)
                if (g_reach.load() < -0.5f) g_reach = config::GetFloat("attack_reach", 2.5f);
                const float reach = g_reach.load();
                const float lx = cp[0] - hp[0], lz = cp[2] - hp[1], ll = sqrtf(lx * lx + lz * lz);
                if (reach <= 0.f) {
                    cp[0] = hp[0];
                    cp[2] = hp[1];
                } else if (ll > reach) {
                    cp[0] = hp[0] + lx * reach / ll;
                    cp[2] = hp[1] + lz * reach / ll;
                }
                if (ll > reachPeak) reachPeak = ll;
            }
        } else if (holding) {
            if (!endT) {
                endT = now;
                // a full-animation move (the kick) keeps its slide, but its stance afterwards is cut too: from where the
                // move ended, the recovery tail below holds the position and pushes like after a held attack (LIVE: 102
                // stance frames after spartankickrandom)
                if (animated && cp && g_attackHold.load()) {
                    float gp[3], gy;
                    if (holds <= 80 && GameRoot(gp, &gy))
                        Log("combat: full-animation move over - %.2f m game movement", sqrtf((gp[0] - hp0[0]) * (gp[0] - hp0[0]) +
                            (gp[2] - hp0[1]) * (gp[2] - hp0[1])));
                    hp[0] = hp0[0] = cp[0];
                    hp[1] = hp0[1] = cp[2];
                    animated = false;
                }
            }
            // the recovery window: keep the position (and the held body) while the push ends the attack and the game's
            // run start / stop that it triggers plays out (LIVE: nudge 0.9 -> attack cut after ~10 frames ->
            // axe_run_start -> axe_run_stop ~1.7 s -> idle; 0.35 / 0.6 did not cut it), unless the player walks on
            const bool runStartStop = ContainsI(prim, "run_st") || ContainsI(prim, "walk_st") || ContainsI(prim, "runstop") ||
                                      ContainsI(prim, "walkstop");
            if (syncMove) release("sync move / finisher: the game's movement");
            else if (animated || !g_attackHold.load() || !cp) release(animated ? "full animation: game movement" : "hold off");
            else if (sm > 0.25f || handoff) {
                // D-076: the game position (lunged up to attack_reach) is put back on the held spot before letting go.
                // D-077 handoff: the held spot keeps moving at the carried velocity; the game's own step this frame
                // (where it put Kratos minus where we wrote him) is watched, and once its run has caught up (3 frames at
                // >= 85 % of our speed), or the player stops, or after 1.5 s, the position is handed over
                // D-096 (owner: "during some attacks it still teleports me / misaligns my head, mostly walking and
                // attacking"; owner log: "handoff - the game holds Kratos back (145.85 m/s against our 3.87)" -> "attack
                // over - 2.55 m from the held spot ... (game blocked)"): on the handoff's FIRST frame the game's position
                // still holds the whole reach slack (the lunged / standing game body vs the held spot that walked on, up to
                // attack_reach), read as the game's own step = a huge push back -> released there -> the view jumped up to
                // 2.5 m. That frame now only puts the game on the held spot; the push-back test starts with the next
                // frame (the game's real answer to our position)
                const bool firstHandoff = !handoff;
                if (!handoff) {
                    handoff = true;
                    handoffT = now;
                    catchN = 0;
                    lastWritten[0] = hp[0];
                    lastWritten[1] = hp[1];
                }
                if (firstHandoff) {
                    cp[0] = hp[0];
                    cp[2] = hp[1];
                }
                const float ox = firstHandoff ? 0.f : cp[0] - lastWritten[0], oz = firstHandoff ? 0.f : cp[2] - lastWritten[1];
                const float own = dt > 1e-4f ? sqrtf(ox * ox + oz * oz) / dt : 0.f;
                const float ours0 = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
                // the game pushing back against our motion (a rock / wall / ledge it stops Kratos at - LIVE: walk stop
                // mid-handoff, then a 0.22-0.43 m snap back and a fall) or playing a stop / fall: hand over now, at the
                // game's own (resolved) position, instead of pushing on
                const float back = ours0 > 0.3f && dt > 1e-4f ? -(ox * vel[0] + oz * vel[1]) / (ours0 * dt) : 0.f;
                const bool blocked = back > 0.5f || ContainsI(prim, "stop") || ContainsI(prim, "fall") || ContainsI(prim, "land");
                if (blocked) {
                    if (holds <= 80)
                        Log("combat: handoff - the game holds Kratos back (%.2f m/s against our %.2f, %s)", back, ours0, prim);
                    release("game blocked");
                } else {
                    steer();
                    const float ours = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
                    catchN = own >= 0.85f * ours ? catchN + 1 : 0;
                    cp[0] = hp[0];
                    cp[2] = hp[1];
                    lastWritten[0] = hp[0];
                    lastWritten[1] = hp[1];
                    if (catchN >= 3 || sm < 0.2f || now - handoffT > 1500)
                        release(catchN >= 3 ? "handed over to the game's run" : sm < 0.2f ? "player stopped" : "handoff timeout");
                }
            }
            else if ((runStartStop || g_inStance.load()) && now - endT < 2500) { cp[0] = hp[0]; cp[2] = hp[1]; }
            else if ((float)(now - endT) >= nudgeMs) release("position held");
            else { cp[0] = hp[0]; cp[2] = hp[1]; }
            // the body stays held too (no running legs in place) - not in the handoff, where Kratos really runs
            if (holding && !handoff && !syncMove && p == Policy::None) p = Policy::Hold;
        }
        // time scale (one owner for both boosts): the wind-up boost at a held attack's start, and D-067 spam: a new swing
        // queued while a held attack plays fast-forwards the rest of it (the owner: "attacks spammable - the motion
        // starts the animation again"; the next attack is still the combo's first, and it starts at normal speed)
        {
            // D-075 (owner: "spam the frozen attacks like Goku"): the whole held attack plays at attack_timescale (the body
            // is frozen, so only its timing shows: the hit lands sooner, the next attack can start sooner); a queued swing
            // fast-forwards the rest at attack_spam_timescale
            static const float ffScale = config::GetFloat("attack_spam_timescale", 20.f);
            static const float baseScale = config::GetFloat("attack_timescale", 2.0f);
            if (endT || !holding || animated) g_ffAt = 0;  // the old attack is over: never fast-forward the new one
            static float applied = 1.f;
            if (holding && g_attackStartMs.load()) g_attackGameT = g_attackGameT.load() + dt * applied;
            // D-079 (owner: hits register ~20 %): LIVE owner session - about half the axe swings in a burst fast-forwarded
            // the playing attack at 20x within 78-130 ms of its start; at 20x one frame plays ~0.33 s of the animation, so
            // its CombatCollision On..Off window (the only time the hit spheres are live) was skipped - no hit possible.
            // The fast-forward now waits until the attack has played attack_spam_after_s (0.5 s game time) - past the
            // light attacks' hit window - and only then plays the rest at 20x
            static const float ffAfter = config::GetFloat("attack_spam_after_s", 0.5f);
            const ULONGLONG ff = g_ffAt.load();
            const bool ffOn = ff && now - ff < 1500 && holding && !animated && !endT && ffScale > 1.01f &&
                              g_attackGameT.load() >= ffAfter;
            float want = 1.f;
            // D-156 (owner: "bring back the faster axe stuck animation"): Kratos' own attack follow-ups (D-100's cs list -
            // the axe stuck in the enemy after a connecting light swing, ~0.45 s) play at cs_followup_timescale (2x)
            static const float followScale = config::GetFloat("cs_followup_timescale", 2.0f);
            if (f1 && g_ownFollowUp.load() && followScale > 1.01f) want = followScale;
            if (holding && !animated && !endT && baseScale > 1.01f) want = baseScale;
            // D-151 (owner: "I do the motion and the enemy gets hit ~1 second later"; owner log D-148 build: gesture ->
            // attack start 2-140 ms, but start -> hit 110 ms (light axe), 235 ms (punch), 780 ms (the Spartan kick a hard
            // punch gives - a full-animation move, never sped up), and the heavy axe's long wind-up - most of the owner's
            // swings are heavies, peaks 6-10 m/s): the WIND-UP (attack start until Kratos' hit spheres go live, i.e. the
            // game first tests his attack volume against a target - CombatCollision On, D-079 / D-081) plays at
            // attack_windup_boost (4x), then the attack goes on at its normal held speed so the hit window is never
            // skipped (D-079). No target near (no volume test): the boost ends after attack_windup_max_s game time (0.9).
            // The bare-handed heavy kick (ANIMATE) gets the wind-up boost too (attack_windup_kick 0 = not).
            {
                static const float windupCap = config::GetFloat("attack_windup_max_s", 0.9f);
                static const bool windupKick = config::GetFloat("attack_windup_kick", 1.f) > 0.5f;
                static ULONGLONG windupFor = 0, windupLogged = 0;
                static int windupLogs = 0;
                // D-152 (owner: "the axe is still meh - the hit lands ~half a second after the motion"; owner log D-151:
                // light axe gate -> start 54 ms (the D-148 peak wait, gone with heavies off), start -> hit spheres live
                // 79-156 ms at 4x - the wind-up is 0.34-0.47 s of animation, and a game frame is ~20 ms in VR, so 4x
                // still takes ~5 frames). Per attack animation the game time at which its hit spheres went live is learned
                // (the lowest seen); the wind-up up to one fast frame before that plays at attack_windup_fast (10x), the
                // last part at attack_windup_boost (4x) - the hit window is still entered at 4x / held speed, never skipped
                struct LiveAt { uint64_t h; float t; };
                static LiveAt liveAt[48] = {};
                static int liveN = 0;
                static float windupPrevT = 0.f;
                static uint64_t windupAnim = 0;
                static const float fastBoost = config::GetFloat("attack_windup_fast", 10.f);
                uint64_t ah = 1469598103934665603ull;
                if (anim)
                    for (const char* c = anim; *c && *c != '#'; ++c) ah = (ah ^ (uint8_t)tolower((unsigned char)*c)) * 1099511628211ull;
                float learned = -1.f;
                for (int i = 0; i < liveN; ++i)
                    if (liveAt[i].h == ah) { learned = liveAt[i].t; break; }
                const ULONGLONG st = g_attackStartMs.load();
                const bool kick = anim && StartsWithI(anim, "spartankick");
                const bool live = st && g_heroVolAt.load() >= st;
                const bool capped = g_attackGameT.load() >= windupCap;
                const bool windup = holding && !endT && st && !live && !capped && boost > 1.01f &&
                                    (!animated || (windupKick && kick));
                if (windup) {
                    const float gt = g_attackGameT.load();
                    const float margin = fastBoost * fmaxf(dt, 0.011f) + 0.03f;  // one fast frame + a little
                    const bool fast = fastBoost > boost && learned > 0.f && gt < learned - margin;
                    // D-202 (owner: on the Draugr "the axe passes through with no reaction at all: no sound, no flinch,
                    // no damage"; owner logs: about half the light axe swings "wind-up over ... no target tested, time
                    // cap" - the game never tested Kratos' attack volume during them): the last part of the wind-up ran at
                    // 4x until the hit spheres were seen live - at ~20 ms VR frames 0.08 s of animation per frame, about a
                    // light swing's whole CombatCollision window, so it could open and close between two of the game's
                    // collision steps (D-079 found the same at 20x). From one 4x frame before the learned live time (or
                    // 0.25 s game time when none is learned yet) the wind-up plays at most attack_window_step_s (0.03 s)
                    // of animation per frame - the window always gets collision steps. 0 = as before
                    static const float windowStep = config::GetFloat("attack_window_step_s", 0.03f);
                    const float slowFrom = learned > 0.f ? learned - (boost * fmaxf(dt, 0.011f) + 0.02f) : 0.25f;
                    const bool approach = windowStep > 0.f && !fast && gt >= slowFrom;
                    if (approach)  // (below the held attack's base speed too - D-075's 2x resumes once the window is seen)
                        want = fminf(boost, fmaxf(1.f, windowStep / fmaxf(dt, 0.005f)));
                    else
                        want = fmaxf(want, fast ? fastBoost : boost);
                    windupFor = st;
                    windupPrevT = gt;  // the last game time before the spheres went live (conservative)
                    windupAnim = ah;
                } else if (windupFor && windupFor == st && windupLogged != st) {
                    windupLogged = st;
                    float was = -1.f;
                    if (live && windupAnim) {
                        int k = -1;
                        for (int i = 0; i < liveN; ++i)
                            if (liveAt[i].h == windupAnim) { k = i; break; }
                        if (k < 0 && liveN < 48) { k = liveN++; liveAt[k] = {windupAnim, windupPrevT}; }
                        else if (k >= 0) { was = liveAt[k].t; if (windupPrevT < liveAt[k].t) liveAt[k].t = windupPrevT; }
                    }
                    if (windupLogs++ < 80)
                        Log("combat: wind-up over %llu ms after the attack start (%.2f s game time) - %s%s", (unsigned long long)(now - st),
                            g_attackGameT.load(), live ? "hit spheres live" : capped ? "no target tested, time cap" : "attack ended",
                            was > 0.f ? " (fast part learned for this attack)" : live ? " (learned now - faster next time)" : "");
                }
            }
            if (ffOn) want = fmaxf(want, ffScale);
            // D-076: a swing waiting on the post-attack stance fast-forwards the stance (instead of a block tap)
            const ULONGLONG fs = g_ffStanceAt.load();
            if (f1 && g_inStance.load() && fs && now - fs < 300 && ffScale > 1.01f) want = fmaxf(want, ffScale);
            // D-088 (owner: "remove the animation of unequipping the axe and the Blades in F1"): the put-away moves
            // (nav[Combat]Sheath{Axe,Blades}ToBare) play at unequip_timescale (15x) - the weapon is on the back in a few
            // frames. At most 150 ms per put-away (a move name lingering must not leave Kratos fast), and the game
            // body is held where it was meanwhile (a walk at 15x would lurch ~2 m)
            {
                static const float unequipScale = config::GetFloat("unequip_timescale", 15.f);
                static bool unequipOn = false;
                static ULONGLONG unequipT0 = 0;
                static float unequipPos[2] = {0.f, 0.f};
                const bool unequipAnim = f1 && unequipScale > 1.01f && anim &&
                                         (ContainsI(anim, "SheathAxeToBare") || ContainsI(anim, "SheathBladesToBare"));
                if (unequipAnim && !unequipOn && !unequipT0) {
                    if (float* c = ControllerPos()) {
                        unequipPos[0] = c[0];
                        unequipPos[1] = c[2];
                        unequipOn = true;
                        unequipT0 = now;
                        Log("combat: put-away (%s) played at %.0fx", anim, unequipScale);
                    }
                }
                if (unequipOn && (!unequipAnim || now - unequipT0 > 150)) unequipOn = false;
                if (!unequipAnim) unequipT0 = 0;  // the next put-away may fast-forward again
                if (unequipOn) {
                    want = fmaxf(want, unequipScale);
                    if (float* c = ControllerPos()) { c[0] = unequipPos[0]; c[2] = unequipPos[1]; }
                }
            }
            if (want != applied) {
                (void)savedTs;
                if (HeroTimeScaleWish(0, want)) applied = want;  // D-203: through the arbiter (owner 0 = combat)
                static int tsLogs = 0;
                if (tsLogs++ < 60) {
                    float cur = -1.f;
                    if (uint8_t* obj = HeroObject())
                        if (uint8_t* cr = *(uint8_t**)(obj + 0x220)) cur = *(float*)(cr + 0x308 + 5 * 4);
                    Log("combat: time scale -> %.1f (slot reads %.2f)%s", want, cur, ffOn && want == ffScale ? " - next swing queued, fast-forward" : "");
                }
            }
            boosted = applied != 1.f;
        }
        // D-065: the drawn body is put on the held spot; the game root's per-frame deviation from it is measured (what the
        // owner saw as "the body moves a little") and logged at the release
        const bool locked = holding && !animated && g_attackHold.load();
        // D-066: the facing held too - D-077: not in the handoff (the game turns Kratos into his run like when walking;
        // the view then uses the game's facing again, continuous from the held one)
        if (locked && !handoff && cp && g_attackAim.load()) cp[(0x430 - 0x410) / 4] = heldYaw;
        g_heldYaw = heldYaw;
        g_heldYawValid = locked && !handoff && g_attackAim.load();
        // height: LIVE stilltrace - with x/z locked the drawn root still bobbed 5-12 cm (the attacks' vertical root
        // motion); the drawn height stays at the attack's start while the player stands (walking: the ground's height)
        // D-084: movedY = moved since the height was last taken (an attack starting while standing takes it again)
        {
            static bool haveY = false;
            float gp[3], gy;
            if (!locked) haveY = false;
            else if ((!haveY || recaptureY) && GameRoot(gp, &gy)) { g_heldY = gp[1]; haveY = true; recaptureY = false; }
            g_heldYValid = locked && haveY && movedY < 0.05f;
        }
        g_heldX = hp[0];
        g_heldZ = hp[1];
        g_heldValid = locked;
        {
            static float maxDev = 0.f;
            float gp[3], gy;
            if (locked && GameRoot(gp, &gy)) {
                const float dv = sqrtf((gp[0] - hp[0]) * (gp[0] - hp[0]) + (gp[2] - hp[1]) * (gp[2] - hp[1]));
                if (dv > maxDev) maxDev = dv;
            } else if (!holding && maxDev > 0.f) {
                if (holds <= 80) Log("combat: while held, the game's root was up to %.3f m off the held spot (drawn body locked)", maxDev);
                maxDev = 0.f;
            }
        }
        // the push: from nudge_after_ms into the attack while it plays, and after it only while the stance shows
        g_nudge = holding && !animated && g_attackHold.load() &&
                  ((!endT && (float)(now - attT0) >= nudgeAfter) || (endT && g_inStance.load())) ? nudgeAmt : 0.f;
    }

    static Policy lastP = Policy::None;
    static ULONGLONG holdSeen = 0;
    if (p != lastP) {
        Log("combat: %s -> %s (%s)", PolicyName(lastP), PolicyName(p), anim && anim[0] ? anim : "-");
        lastP = p;
    }
    // HOLD weight: on at once, kept through short gaps between combo hits, faded out over hold_blend_ms
    static const float graceMs = config::GetFloat("hold_grace_ms", 300.f), blendMs = config::GetFloat("hold_blend_ms", 250.f);
    if (p == Policy::Hold) holdSeen = now;
    float w = 0.f;
    if (holdSeen && f1) {
        const float since = (float)(now - holdSeen);
        w = since <= graceMs ? 1.f : fmaxf(0.f, 1.f - (since - graceMs) / blendMs);
        if (w <= 0.f) holdSeen = 0;
    }
    g_holdW = w;

    // leaps under HOLD: the animated neck rises above the held one -> lift body and view (vertical only)
    // D-065 (owner: the body must stay completely still in held attacks): the leap lift is off by default (leaps are
    // full-animation moves now); gowvr.ini leap_rise=0.3 brings it back
    static const float leapThr = config::GetFloat("leap_rise", 99.f);
    static float lift = 0.f;
    const float rise = w > 0.f ? g_rise.load() : 0.f;
    const float liftTarget = rise > leapThr ? rise : 0.f;
    lift = Ease(lift, liftTarget, dt, liftTarget > lift ? 0.06f : 0.12f);
    if (fabsf(lift) < 0.002f && liftTarget == 0.f) lift = 0.f;
    g_lift = lift;
    static float liftPeak = 0.f;
    if (lift > liftPeak) liftPeak = lift;
    if (lift == 0.f && liftPeak > 0.f) {
        Log("combat: leap under HOLD, body and view lifted up to %.2f m", liftPeak);
        liftPeak = 0.f;
    }

    // ANIMATE: arms from the animation; the view follows the animated neck's translation relative to its start
    const bool anim8 = p == Policy::Animate;
    g_armsAnim = anim8;
    // D-162 (owner: "during climbing the hands are a bit too far forward, and the head too - when I turn F1 off and on
    // while climbing the head goes a bit back and the hands line up"): climbing is a sync move (ANIMATE), so the view
    // followed the animated neck relative to where it was when the climb STARTED - standing on the ground, before the
    // entry move brought the body to the wall: the whole climb was seen from that standing->hanging difference ahead of
    // the eye point (and the hand targets with it - they hang off the same base). F1 off / on mid-climb took the start
    // again on the wall. Climbing moves (navclimb*, navropevertical*) follow no neck offset: the fixed eye point on the
    // climbing body, as after that re-enable
    const bool climbing = (anim && (StartsWithI(anim, "navclimb") || StartsWithI(anim, "navropevertical"))) ||
                          StartsWithI(prim, "navclimb") || StartsWithI(prim, "navropevertical");
    static bool haveStart = false;
    static float n0[3] = {0.f, 0.f, 0.f};
    static float off[3] = {0.f, 0.f, 0.f};
    float target[3] = {0.f, 0.f, 0.f};
    float rp[3], ry;
    Mat4 nk;
    // D-164: in every body-locked context the view goes to the body's current eye itself (firstperson.cpp)
    // D-181: nor in the boat (its view follows the seated eye itself, firstperson.cpp)
    if (anim8 && !climbing && !ContextBodyLocked() && !InBoat() && RenderRoot(rp, &ry) && StableJoint(kNeckHash, &nk)) {
        const float s = sinf(ry), c = cosf(ry);
        const float dx = nk.m[3][0] - rp[0], dy = nk.m[3][1] - rp[1], dz = nk.m[3][2] - rp[2];
        const float l[3] = {dx * c - dz * s, dy, dx * s + dz * c};  // body frame: right, up, back
        if (!haveStart) { memcpy(n0, l, sizeof(n0)); haveStart = true; }
        const float d[3] = {l[0] - n0[0], l[1] - n0[1], l[2] - n0[2]};
        target[0] = d[0] * c + d[2] * s;  // back to world (rendered facing)
        target[1] = d[1];
        target[2] = -d[0] * s + d[2] * c;
    } else {
        haveStart = false;
    }
    static const float camTau = config::GetFloat("animate_cam_tau", 0.12f);
    for (int i = 0; i < 3; ++i) off[i] = Ease(off[i], target[i], dt, camTau);
    AcquireSRWLockExclusive(&g_camLock);
    g_camOff[0] = off[0];
    g_camOff[1] = off[1] + lift;
    g_camOff[2] = off[2];
    ReleaseSRWLockExclusive(&g_camLock);
}

}  // namespace gowvr::fp
