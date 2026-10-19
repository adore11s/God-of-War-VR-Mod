#include "walls.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../vr/xr.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// D-160 (owner: "when in F1 mode you can't go through the walls with your head and your hands can't go through the
// walls. Also implement rumble once you touch walls"). In F1 the view is the headset offset from a fixed eye point on
// Kratos (view = center x base, firstperson.cpp / mutate.cpp) and the hands are the controllers - Kratos' capsule stops
// at walls, the player's head and hands did not. Both are now cast against the world with the game's own ray cast:
//   sub_14053e780 (ADDR-021, STATIC: decompile + disassembly of the caller at 0x140797a55): rcx = start float4, rdx = end
//   float4, r8b = mode (0 = ray), xmm3 = radius 0, stack +0x20 / +0x28 = 0.f, +0x30 = query (filter), +0x38 = result
//   (0x70 B: +0x00 hit point, +0x20 normal, +0x5c fraction), +0x40 = flags byte, +0x48 = a zeroed 0x100 B stats block;
//   eax = number of hits (the closest is kept). It runs the broadphase (world DAT_1422ae7e0[query +0x58]) and the narrow
//   phase sub_1401536c0 itself, with a scratch allocator from the game's thread-local block (**(TLS[0] + 0x28)).
//   The query is the player's own environment query as logged by D-158 (caller 0x14053f14e: +0x08 2 = environment
//   bodies, +0x0c 4 = bodies that block the player, +0x18 all materials, +0x20 0x40000000 excluded, +0x28 5) without its
//   callbacks / ignored objects (environment only: Kratos is never in it).
// Guards: the game's thread-local allocator must exist on the calling thread (the view update runs on the main thread,
// which makes physics queries itself - D-158 log), world 0 must exist, the call sits in __try (an exception turns walls
// off for the session and is logged), each hit is checked to lie on the ray (otherwise counted as an anomaly; 40 of
// them without a valid hit turn walls off - a wrong reading of the result), hits at the ray's very start are ignored
// (the start inside geometry). gowvr.ini walls_f1 0 = off, wall_rumble 0 = no rumble (1 = full).

namespace gowvr::fp {
namespace {

using PFN_RayCast = int(__fastcall*)(const float* start, const float* end, uint64_t mode, float radius, float a5, float a6,
                                     void* query, void* result, uint8_t* flags, void* stats);
constexpr uint64_t kRayCastVa = 0x14053e780;
constexpr uint64_t kWorldsVa = 0x1422ae7e0;

uint8_t* G(uint64_t va) {
    static uint8_t* const b = (uint8_t*)GetModuleHandleW(nullptr);
    return (uint8_t*)GameVa(va);
}

std::atomic<bool> g_dead{false};
uint64_t g_casts = 0, g_hits = 0, g_clampHead = 0, g_clampHand = 0, g_anomaly = 0, g_valid = 0, g_skipThread = 0;
double g_costUs = 0.0;

bool Enabled() {
    // read each time: the VR settings panel switches it (gowvr.ini walls_f1)
    return config::GetFloat("walls_f1", 1.f) > 0.5f && !g_dead.load();
}

bool GameThreadReady() {
    // sub_14053e780: mov rax, gs:[0x58]; mov rax, [rax]; mov r9, [rax + 0x28]; mov rax, [r9] - the scratch allocator
    uint8_t** tls = (uint8_t**)__readgsqword(0x58);
    if (!tls || !tls[0]) return false;
    void** p = *(void***)(tls[0] + 0x28);
    return p && *p;
}

// no C++ objects in here (__try)
int RawCast(const float* s, const float* e, void* q, void* r, uint8_t* flags, void* stats, DWORD* code) {
    __try {
        return ((PFN_RayCast)G(kRayCastVa))(s, e, 0, 0.f, 0.f, 0.f, q, r, flags, stats);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

struct Hit {
    float p[3];
    float n[3];
};

// true + hit filled when the segment a -> b hits a wall
bool Cast(const float a[3], const float b[3], Hit* hit) {
    if (!GameThreadReady()) { ++g_skipThread; return false; }
    uint8_t* world = *(uint8_t**)G(kWorldsVa);
    if (!world || !*(void**)world) return false;
    alignas(16) uint8_t q[0x80] = {};
    *(uint32_t*)(q + 0x08) = 2;           // body category: environment
    *(uint32_t*)(q + 0x0c) = 4;           // the body collides with: the player
    *(uint32_t*)(q + 0x10) = 0xffffffffu; // groups: all
    *(uint64_t*)(q + 0x18) = ~0ull;       // materials: all ...
    *(uint64_t*)(q + 0x20) = 0x40000000;  // ... but this one (as the player's query)
    *(uint32_t*)(q + 0x28) = 5;           // body flags (as the player's query)
    *(uint16_t*)(q + 0x2c) = 0xffff;
    *(uint16_t*)(q + 0x2e) = 0xffff;
    alignas(16) float s[4] = {a[0], a[1], a[2], 1.f}, e[4] = {b[0], b[1], b[2], 1.f};
    alignas(16) uint8_t r[0x80] = {};
    *(float*)(r + 0x5c) = 1.f;
    *(int32_t*)(r + 0x60) = -1;
    r[0x64] = 0xff;
    uint8_t flags = 0;
    alignas(16) uint8_t stats[0x100] = {};
    static LARGE_INTEGER qf{};
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    DWORD code = 0;
    const int n = RawCast(s, e, q, r, &flags, stats, &code);
    QueryPerformanceCounter(&t1);
    ++g_casts;
    g_costUs += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)qf.QuadPart;
    if (n < 0) {
        g_dead = true;
        Log("walls: the game's ray cast raised exception 0x%08lx - walls off for this session", code);
        return false;
    }
    if (n == 0) return false;
    ++g_hits;
    memcpy(hit->p, r, 12);
    memcpy(hit->n, r + 0x20, 12);
    return true;
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// from -> to, kept `margin` short of a wall in between: true + out (the allowed point) and depth (how far `to` was in)
bool Clamp(const float from[3], const float to[3], float margin, float out[3], float* depth, const char* what) {
    float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float L = sqrtf(Dot(d, d));
    if (!(L > 0.02f) || L > 3.f) return false;  // no offset, or not a sane one (teleport / recenter frame)
    const float u[3] = {d[0] / L, d[1] / L, d[2] / L};
    const float ext = L + margin;
    const float end[3] = {from[0] + u[0] * ext, from[1] + u[1] * ext, from[2] + u[2] * ext};
    Hit h;
    if (!Cast(from, end, &h)) return false;
    const float rel[3] = {h.p[0] - from[0], h.p[1] - from[1], h.p[2] - from[2]};
    const float along = Dot(rel, u);
    const float px = rel[0] - u[0] * along, py = rel[1] - u[1] * along, pz = rel[2] - u[2] * along;
    const float perp = sqrtf(px * px + py * py + pz * pz);
    if (!std::isfinite(along) || !std::isfinite(perp) || perp > 0.25f || along < -0.05f || along > ext + 0.05f) {
        ++g_anomaly;
        if (g_anomaly <= 8)
            Log("walls: %s hit off the ray (along %.3f of %.3f m, %.3f m beside it) - point %.2f %.2f %.2f normal %.2f %.2f "
                "%.2f - ignored",
                what, along, ext, perp, h.p[0], h.p[1], h.p[2], h.n[0], h.n[1], h.n[2]);
        if (g_anomaly >= 40 && g_valid == 0) {
            g_dead = true;
            Log("walls: %llu hits and none on the ray - the result is not read right; walls off for this session",
                (unsigned long long)g_anomaly);
        }
        return false;
    }
    ++g_valid;
    if (along < 0.03f) return false;  // the start itself sits in geometry
    const float allowed = fmaxf(0.f, along - margin);
    if (allowed >= L) return false;
    for (int i = 0; i < 3; ++i) out[i] = from[i] + u[i] * allowed;
    *depth = L - allowed;
    static int logs = 0;
    if (logs < 12) {
        ++logs;
        Log("walls: %s at a wall %.2f m from the start (it was %.2f m out; kept %.2f m) - normal %.2f %.2f %.2f", what,
            along, L, allowed, h.n[0], h.n[1], h.n[2]);
    }
    return true;
}

// contact rumble. D-161 (owner: "make vibrations very subtle, no vibration when I hit my head into a wall, only once when
// I touch the wall - not the whole time I hold it"): one short faint tap on the hand's controller when its contact
// starts (wall_rumble = its strength, default 0.12; 0 = none), nothing while it stays on the wall, nothing for the
// head. A contact ends after 300 ms off the wall (no repeat taps from hovering at the surface).
struct Contact {
    ULONGLONG lastAt = 0;
    bool on = false;
};
Contact g_head, g_hand[2];

void Touch(Contact& c, int hand) {
    static const float k = config::GetFloat("wall_rumble", 0.12f);
    const ULONGLONG now = GetTickCount64();
    if (!c.on && hand >= 0 && k > 0.f) vr::XrHaptic(hand, fminf(1.f, k), 0.025f);
    c.on = true;
    c.lastAt = now;
}

void Release(Contact& c) {
    if (c.on && GetTickCount64() - c.lastAt > 300) c.on = false;  // a short gap is still the same contact
}

// D-161: the head's shift (base frame) for the rendered eyes - written by the view update, read by the eye builds
SRWLOCK g_shiftLock = SRWLOCK_INIT;
float g_shift[3] = {0.f, 0.f, 0.f};
ULONGLONG g_shiftAt = 0;
void SetShift(const float s[3]) {
    AcquireSRWLockExclusive(&g_shiftLock);
    for (int i = 0; i < 3; ++i) g_shift[i] = s ? s[i] : 0.f;
    g_shiftAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_shiftLock);
}

void Report() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!last) { last = now; return; }
    if (now - last < 60000) return;
    last = now;
    Log("walls: last 60 s - %llu casts (%.1f us each), %llu hits, %llu valid, %llu anomalies, head kept back %llu frames, "
        "hands %llu, no game thread %llu",
        (unsigned long long)g_casts, g_casts ? g_costUs / (double)g_casts : 0.0, (unsigned long long)g_hits,
        (unsigned long long)g_valid, (unsigned long long)g_anomaly, (unsigned long long)g_clampHead,
        (unsigned long long)g_clampHand, (unsigned long long)g_skipThread);
    g_casts = g_hits = g_clampHead = g_clampHand = g_skipThread = 0;
    g_costUs = 0.0;
}

}  // namespace

void WallsHead(Mat4& center, const Mat4& base) {
    if (!Enabled()) return;
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("walls: F1 head / hands against walls on (the game's ray cast; walls_f1, wall_rumble)");
    }
    Report();
    const Mat4 H = Mul(center, base);
    const float eye[3] = {base.m[3][0], base.m[3][1], base.m[3][2]};  // Kratos' eye point - inside his capsule
    const float head[3] = {H.m[3][0], H.m[3][1], H.m[3][2]};
    float out[3], depth = 0.f;
    static const float margin = config::GetFloat("wall_head_margin", 0.15f);  // the near plane stays out of the wall
    if (!Clamp(eye, head, margin, out, &depth, "head")) {
        Release(g_head);
        SetShift(nullptr);
        return;
    }
    ++g_clampHead;
    // the world shift into center's frame (base rows: right / up / back)
    const float dw[3] = {out[0] - head[0], out[1] - head[1], out[2] - head[2]};
    float dl[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 3; ++i) {
        const float l2 = base.m[i][0] * base.m[i][0] + base.m[i][1] * base.m[i][1] + base.m[i][2] * base.m[i][2];
        if (l2 > 1e-6f) dl[i] = (dw[0] * base.m[i][0] + dw[1] * base.m[i][1] + dw[2] * base.m[i][2]) / l2;
        center.m[3][i] += dl[i];
    }
    SetShift(dl);  // D-161: the same shift for both rendered eyes
    Touch(g_head, -1);  // D-161: no rumble for the head
}

void WallsEyeShift(Mat4& eyeDelta) {
    AcquireSRWLockShared(&g_shiftLock);
    const bool fresh = g_shiftAt && GetTickCount64() - g_shiftAt < 200;
    if (fresh)
        for (int i = 0; i < 3; ++i) eyeDelta.m[3][i] += g_shift[i];
    ReleaseSRWLockShared(&g_shiftLock);
}

void WallsHands(Mat4 hands[2], const bool valid[2], const Mat4& base) {
    if (!Enabled()) return;
    static const float margin = config::GetFloat("wall_hand_margin", 0.06f);
    for (int h = 0; h < 2; ++h) {
        if (!valid[h]) { Release(g_hand[h]); continue; }
        // an approximate shoulder: 0.18 m to the hand's side and 0.25 m below the eye point
        const float side = h ? 0.18f : -0.18f;
        float sh[3];
        for (int i = 0; i < 3; ++i) sh[i] = base.m[3][i] + base.m[0][i] * side - base.m[1][i] * 0.25f;
        const float hand[3] = {hands[h].m[3][0], hands[h].m[3][1], hands[h].m[3][2]};
        float out[3], depth = 0.f;
        if (!Clamp(sh, hand, margin, out, &depth, h ? "right hand" : "left hand")) {
            Release(g_hand[h]);
            continue;
        }
        ++g_clampHand;
        for (int i = 0; i < 3; ++i) hands[h].m[3][i] = out[i];
        Touch(g_hand[h], h);
    }
}

void WallsIdle() {
    g_head.on = g_hand[0].on = g_hand[1].on = false;
    SetShift(nullptr);
}

// D-230 (owner: "any attack in first person - axe, fists, Blades - if I walk backwards during it I phase through walls
// and objects and fall out of the map; even on stairs"; "ONLY walking backwards, not forward"): the held spot of an F1
// attack is moved by the stick with no collision (combat.cpp steer, D-077), and the game's controller is put onto it -
// at the attack's end when the game body is "ahead" of it (taken for the attack's lunge: walking backwards puts the
// wall-stopped game body ahead) and every frame of the walking-on handoff. A step of the held spot is now cast against
// the world first: Kratos' capsule at three heights (feet + 0.35 / 0.9 / 1.5 m, wall_body_radius 0.3 m kept), sliding
// along a wall it meets, and a ground probe at the end (no ground within 1.2 m below, or ground over 0.4 m above the feet
// = not walkable: no step). Game thread; with walls off (walls_f1 0) or no cast possible, the step is allowed as before.
bool WallsStep(const float from[3], const float to[3], float out[3]) {
    out[0] = to[0]; out[1] = to[1]; out[2] = to[2];
    if (!Enabled() || config::GetFloat("attack_walk_walls", 1.f) < 0.5f) return true;
    {   // no cast possible here (another thread, no world): the step as before - never blocked by a missing cast
        uint8_t* world = *(uint8_t**)G(kWorldsVa);
        if (!GameThreadReady() || !world || !*(void**)world) return true;
    }
    static const float radius = config::GetFloat("wall_body_radius", 0.3f);
    static uint64_t steps = 0, stopped = 0, slid = 0, noGround = 0;
    auto horizontalCast = [&](const float a[3], const float b[3], float* allowed, float n[3]) {
        float d[3] = {b[0] - a[0], 0.f, b[2] - a[2]};
        const float L = sqrtf(d[0] * d[0] + d[2] * d[2]);
        *allowed = L;
        if (L < 1e-4f) return false;
        const float u[3] = {d[0] / L, 0.f, d[2] / L};
        bool any = false;
        for (float hgt : {0.35f, 0.9f, 1.5f}) {
            const float s[3] = {a[0], a[1] + hgt, a[2]};
            const float e[3] = {a[0] + u[0] * (L + radius), a[1] + hgt, a[2] + u[2] * (L + radius)};
            Hit h;
            if (!Cast(s, e, &h)) continue;
            const float along = (h.p[0] - s[0]) * u[0] + (h.p[2] - s[2]) * u[2];
            if (!std::isfinite(along) || along < 0.02f) continue;  // (the start itself inside geometry: not this step's)
            const float ok = fmaxf(0.f, along - radius);
            if (ok < *allowed) {
                *allowed = ok;
                const float nl = sqrtf(h.n[0] * h.n[0] + h.n[2] * h.n[2]);
                n[0] = nl > 1e-3f ? h.n[0] / nl : -u[0];
                n[1] = 0.f;
                n[2] = nl > 1e-3f ? h.n[2] / nl : -u[2];
                any = true;
            }
        }
        return any;
    };
    auto walkable = [&](const float p[3]) {
        const float s[3] = {p[0], from[1] + 0.6f, p[2]}, e[3] = {p[0], from[1] - 1.2f, p[2]};
        Hit h;
        if (!Cast(s, e, &h)) return false;      // no ground below: a ledge / hole
        return h.p[1] - from[1] < 0.4f;          // a step up of at most 0.4 m
    };
    ++steps;
    float allowed = 0.f, n[3] = {0.f, 0.f, 0.f};
    const bool hitWall = horizontalCast(from, to, &allowed, n);
    float p[3] = {to[0], from[1], to[2]};
    if (hitWall) {
        const float dx = to[0] - from[0], dz = to[2] - from[2], L = sqrtf(dx * dx + dz * dz);
        p[0] = from[0] + dx / L * allowed;
        p[2] = from[2] + dz / L * allowed;
        // slide: the rest of the step along the wall (its component into the wall removed)
        float rx = to[0] - p[0], rz = to[2] - p[2];
        const float into = rx * n[0] + rz * n[2];
        if (into < 0.f) { rx -= into * n[0]; rz -= into * n[2]; }
        const float q[3] = {p[0] + rx, from[1], p[2] + rz};
        float a2 = 0.f, n2[3];
        if (rx * rx + rz * rz > 1e-8f && !horizontalCast(p, q, &a2, n2)) { p[0] = q[0]; p[2] = q[2]; ++slid; }
        else ++stopped;
    }
    // the ground probe counts only where it finds the ground under Kratos' feet now (a surface the cast does not see -
    // a boat, a platform - must not stop him)
    static bool groundSeen = false;
    static ULONGLONG groundAt = 0;
    const ULONGLONG nowT = GetTickCount64();
    if (nowT - groundAt > 250) { groundSeen = walkable(from); groundAt = nowT; }
    if (groundSeen && !walkable(p)) {  // the end is no ground to stand on: no step at all
        ++noGround;
        out[0] = from[0]; out[1] = from[1]; out[2] = from[2];
    } else {
        out[0] = p[0]; out[1] = to[1]; out[2] = p[2];
    }
    if ((hitWall || out[0] == from[0]) && (stopped + slid + noGround) % 200 == 1)
        Log("walls: attack walking - %llu steps, %llu stopped at a wall, %llu slid along one, %llu with no ground ahead "
            "(D-230)", (unsigned long long)steps, (unsigned long long)stopped, (unsigned long long)slid,
            (unsigned long long)noGround);
    return !hitWall && out[0] == p[0] && out[2] == p[2] && p[0] == to[0] && p[2] == to[2];
}

bool WallsAttackActive() { return Enabled() && config::GetFloat("attack_walk_walls", 1.f) > 0.5f; }

// D-230: a straight move of Kratos (feet height from[1]) clear of walls - the end of a held attack snaps the game onto
// the held spot only through a clear path
bool WallsClearPath(const float from[3], const float to[3]) {
    if (!Enabled() || config::GetFloat("attack_walk_walls", 1.f) < 0.5f) return true;
    {
        uint8_t* world = *(uint8_t**)G(kWorldsVa);
        if (!GameThreadReady() || !world || !*(void**)world) return true;
    }
    for (float hgt : {0.35f, 0.9f, 1.5f}) {
        const float s[3] = {from[0], from[1] + hgt, from[2]}, e[3] = {to[0], from[1] + hgt, to[2]};
        Hit h;
        if (Cast(s, e, &h)) return false;
    }
    return true;
}

}  // namespace gowvr::fp
