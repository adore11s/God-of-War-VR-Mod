#include "movelog.h"
#include "../core/addr.h"
#include "body.h"
#include "../core/log.h"
#include "../core/config.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace gowvr::fp {
namespace {

constexpr uint64_t kNeckHash = 0xf14d9a553fdad7ccull;
constexpr uint64_t kShoulderR = 0xe783988515993319ull;  // joint 418
constexpr uint64_t kShoulderL = 0x05c2b5ca793d4bf2ull;  // joint 423

// Upper-body facing: yaw of the left->right shoulder line (a chest axis flipped through vertical when Kratos leans
// into a swing, which read as ~180 deg "twist" on every attack).
bool ShoulderYaw(float* yaw) {
    Mat4 r, l;
    if (!StableJoint(kShoulderR, &r) || !StableJoint(kShoulderL, &l)) return false;
    const float x = r.m[3][0] - l.m[3][0], z = r.m[3][2] - l.m[3][2];
    if (x * x + z * z < 0.01f) return false;
    *yaw = atan2f(x, z);
    return true;
}

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_active{0};
std::atomic<bool> g_sync{false};
char g_activeAnim[2][96] = {};  // double buffer: readers get a complete string
std::atomic<int> g_animBuf{0};
char g_primAnim[4][96] = {};    // the hero's primary animation right now (ring: readers copy it at once)
std::atomic<int> g_primBuf{0};
std::unordered_map<uint64_t, std::string> g_names;
bool g_namesTried = false;

uint8_t* Base() { return (uint8_t*)GetModuleHandleW(nullptr); }

std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void LoadNames() {
    g_namesTried = true;
    FILE* f = nullptr;
    _wfopen_s(&f, (GameDir() + L"gowvr_movenames.txt").c_str(), L"rb");
    if (!f) { Log("movelog: gowvr_movenames.txt not found - moves are logged by hash"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        const uint64_t h = JointHash(line);
        auto it = g_names.find(h);
        // prefer a CamelCase spelling (move names) over the lower-case animation spelling of the same string
        if (it == g_names.end() || (it->second[0] >= 'a' && line[0] >= 'A' && line[0] <= 'Z')) g_names[h] = line;
    }
    fclose(f);
    Log("movelog: %zu names loaded", g_names.size());
}

float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

uint8_t* Creature() {
    uint8_t* obj = HeroObject();
    if (!obj || IsBadReadPtr(obj + 0x220, 8)) return nullptr;
    uint8_t* cr = *(uint8_t**)(obj + 0x220);
    if (!cr || IsBadReadPtr(cr, 0x3b90) || *(int32_t*)(cr + 0x14) != 2) return nullptr;
    return cr;
}

// The hero creature's move player and its active move instance (read-only; null-safe).
uint8_t* MovePlayer() {
    uint8_t* cr = Creature();
    if (!cr) return nullptr;
    uint8_t* mp = *(uint8_t**)(cr + 0x3a70);
    if (!mp || IsBadReadPtr(mp, 0x50)) return nullptr;
    return mp;
}

// GetPrimaryAnimationName (0x1405fa590): object+0x178 -> anim (-0x20) -> sub_14047b6f0(anim, 0) -> +0xf0 -> +0x90 -> +8.
int AnimNameSeh(uint8_t* obj, char* out, size_t cap) {
    __try {
        uint8_t* a = *(uint8_t**)(obj + 0x178);
        if (!a || a == (uint8_t*)0x20) return 0;
        a -= 0x20;
        using PFN = uint8_t*(__fastcall*)(uint8_t*, unsigned);
        uint8_t* e = ((PFN)((uint8_t*)GameVa(0x14047b6f0ull)))(a, 0);
        if (!e || !*(uint8_t**)(e + 0xf0)) return 0;
        uint8_t* nm = *(uint8_t**)(*(uint8_t**)(e + 0xf0) + 0x90);
        if (!nm) return 0;
        const char* s = (const char*)(nm + 8);
        size_t i = 0;
        for (; i + 1 < cap && s[i] && s[i] >= 0x20 && s[i] < 0x7f; ++i) out[i] = s[i];
        out[i] = 0;
        return (int)i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Track {
    uint64_t hash = 0;
    ULONGLONG t0 = 0;
    float p0[3] = {}, yaw0 = 0.f, prevYaw = 0.f, yawNet = 0.f, yawAbs = 0.f;
    float travelMax = 0.f, riseMax = 0.f, dropMax = 0.f;
    int twistAxis = 0;                // the chest axis that is most horizontal at the start (its yaw is meaningful)
    float twist0 = 0.f, twistMax = 0.f;
    float neck0[3] = {}, neckMax = 0.f, neckRise = 0.f, neckDrop = 0.f;
    float pct = 0.f;
    bool haveBody = false;
    char anim[96] = {};
    bool animIsAttack = false;
};

// Yaw of one of the chest joint's axes (row k), and the index of its most horizontal axis.
float AxisYaw(const Mat4& m, int k) { return atan2f(m.m[k][0], m.m[k][2]); }
int FlattestAxis(const Mat4& m) {
    int best = 0;
    float bh = -1.f;
    for (int k = 0; k < 3; ++k) {
        const float h = m.m[k][0] * m.m[k][0] + m.m[k][2] * m.m[k][2];
        if (h > bh) { bh = h; best = k; }
    }
    return best;
}
Track g_cur;
FILE* g_csv = nullptr;

// Body-frame (x right, y up, z back) position of p relative to a frame origin/yaw ("back" axis convention).
void BodyLocal(const float* p, const float* o, float yaw, float out[3]) {
    const float vx = p[0] - o[0], vz = p[2] - o[2], s = sinf(yaw), c = cosf(yaw);
    out[0] = vx * c - vz * s;
    out[1] = p[1] - o[1];
    out[2] = vx * s + vz * c;
}

void Finish(ULONGLONG now) {
    if (!g_cur.hash) return;
    const char* nm = MoveName(g_cur.hash);
    char hx[24];
    sprintf_s(hx, "%016llx", (unsigned long long)g_cur.hash);
    const unsigned dur = (unsigned)(now - g_cur.t0);
    Log("move: end   %-44s %5u ms  travel %.2f m  root rise %.2f drop %.2f  yaw net %+.0f total %.0f  shoulder twist %.0f deg  "
        "neck moved %.2f (up %.2f down %.2f) m  (%s, last %.0f%%)", g_cur.anim[0] ? g_cur.anim : (nm ? nm : hx), dur,
        g_cur.travelMax, g_cur.riseMax, g_cur.dropMax, g_cur.yawNet * 57.2958f, g_cur.yawAbs * 57.2958f,
        g_cur.twistMax * 57.2958f, g_cur.neckMax, g_cur.neckRise, g_cur.neckDrop, hx, g_cur.pct * 100.f);
    if (!g_csv) {
        _wfopen_s(&g_csv, (GameDir() + L"gowvr_logs\\moves.csv").c_str(), L"ab");
        if (g_csv) fprintf(g_csv, "tick,name,hash,dur_ms,travel_m,rise_m,drop_m,yaw_net_deg,yaw_total_deg,chest_twist_deg,neck_moved_m,neck_up_m,neck_down_m,anim\n");
    }
    if (g_csv) {
        fprintf(g_csv, "%llu,%s,%s,%u,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%s\n", (unsigned long long)g_cur.t0, nm ? nm : "",
                hx, dur, g_cur.travelMax, g_cur.riseMax, g_cur.dropMax, g_cur.yawNet * 57.2958f, g_cur.yawAbs * 57.2958f,
                g_cur.twistMax * 57.2958f, g_cur.neckMax, g_cur.neckRise, g_cur.neckDrop, g_cur.anim);
        fflush(g_csv);
    }
    g_cur = Track{};
}

}  // namespace

const char* MoveName(uint64_t hash) {
    if (!g_namesTried) LoadNames();
    auto it = g_names.find(hash);
    return it == g_names.end() ? nullptr : it->second.c_str();
}

uint64_t ActiveMoveHash() { return g_active.load(); }
const char* ActiveMoveAnim() { return g_activeAnim[g_animBuf.load()]; }
void HeroPrimaryAnim(char* out, size_t n) { strcpy_s(out, n, g_primAnim[g_primBuf.load()]); }
bool HeroInSyncMove() { return g_sync.load(); }

void MoveLogEnable(bool on) {
    g_on = on;
    if (on && !g_namesTried) LoadNames();
    if (!on) Finish(GetTickCount64());
    Log("movelog: %s", on ? "on" : "off");
}

void MoveLogTick() {
    static bool cfg = false;
    if (!cfg) { cfg = true; if (config::GetFloat("move_log", 0.f) > 0.5f) MoveLogEnable(true); }
    uint8_t* mp = MovePlayer();
    uint64_t active = 0;
    float pct = 0.f;
    if (mp) {
        const int n = *(int32_t*)(mp + 0x48);
        for (int i = 0; i < n && i < 8; ++i) {
            uint8_t* inst = *(uint8_t**)(mp + 8 + (size_t)i * 8);
            if (!inst || IsBadReadPtr(inst, 0x130)) continue;
            if (*(uint8_t*)(inst + 0x128) & 4) {
                active = *(uint64_t*)(inst + 0xd8);
                pct = *(float*)(inst + 0x88);
                break;
            }
        }
    }
    g_active = active;
    const ULONGLONG now = GetTickCount64();
    if (uint8_t* cr = Creature())
        g_sync = (*(uint8_t*)(cr + 0x3b81) & 1) != 0 || *(uint8_t**)(cr + 0x3b60) != cr + 0x3b60;
    else
        g_sync = false;
    // the hero's primary animation, every other frame (sprint / Rage locomotion live in one long move, so the active
    // move's name alone cannot tell them)
    {
        static int tick = 0;
        if ((++tick & 1) == 0) {
            char a[96] = {};
            if (uint8_t* obj = HeroObject()) AnimNameSeh(obj, a, sizeof(a));
            const int nb = (g_primBuf.load() + 1) & 3;
            strcpy_s(g_primAnim[nb], a);
            g_primBuf = nb;
        }
    }
    // the active move's animation name, for the F1 body rules (combat.cpp): sampled until an attack/evade name shows
    // (the first frames of a move can still report the previous animation)
    {
        static uint64_t animHash = 0;
        static ULONGLONG animT0 = 0;
        static bool animFinal = false;
        if (active != animHash) {
            animHash = active;
            animT0 = now;
            animFinal = false;
            const int nb = 1 - g_animBuf.load();
            g_activeAnim[nb][0] = 0;
            g_animBuf = nb;
        }
        if (active && !animFinal) {
            char a[96] = {};
            if (uint8_t* obj = HeroObject()) AnimNameSeh(obj, a, sizeof(a));
            if (a[0]) {
                const bool attack = _strnicmp(a, "att", 3) == 0 || _strnicmp(a, "def", 3) == 0 ||
                                    _strnicmp(a, "spartan", 7) == 0;
                if (attack || now - animT0 < 150 || !g_activeAnim[g_animBuf.load()][0]) {
                    const int nb = 1 - g_animBuf.load();
                    strcpy_s(g_activeAnim[nb], a);
                    g_animBuf = nb;
                }
                if (attack || now - animT0 > 300) animFinal = true;
            }
        }
    }
    if (!g_on.load()) return;
    float gp[3], gy = 0.f, rp[3], ry = 0.f;
    const bool haveRoot = GameRoot(gp, &gy) && RenderRoot(rp, &ry);
    if (active != g_cur.hash) {
        Finish(now);
        if (active) {
            g_cur.hash = active;
            g_cur.t0 = now;
            if (uint8_t* obj = HeroObject()) AnimNameSeh(obj, g_cur.anim, sizeof(g_cur.anim));
            const char* nm = MoveName(active);
            Log("move: start %-40s (%016llx, anim %s)", nm ? nm : "?", (unsigned long long)active,
                g_cur.anim[0] ? g_cur.anim : "-");
            if (haveRoot) {
                memcpy(g_cur.p0, gp, sizeof(gp));
                g_cur.yaw0 = g_cur.prevYaw = gy;
                Mat4 nk;
                float sy;
                if (ShoulderYaw(&sy)) g_cur.twist0 = WrapPi(sy - ry);
                if (StableJoint(kNeckHash, &nk)) BodyLocal(&nk.m[3][0], rp, ry, g_cur.neck0);
                g_cur.haveBody = true;
            }
        }
    }
    if (g_cur.hash && !g_cur.animIsAttack) {
        char a[96] = {};
        if (uint8_t* obj = HeroObject()) AnimNameSeh(obj, a, sizeof(a));
        if (a[0] && (!g_cur.anim[0] || _strnicmp(a, "att", 3) == 0 || now - g_cur.t0 < 120)) {
            strcpy_s(g_cur.anim, a);
            g_cur.animIsAttack = _strnicmp(a, "att", 3) == 0;
        }
    }
    if (!g_cur.hash || !haveRoot || !g_cur.haveBody) return;
    g_cur.pct = pct;
    const float dx = gp[0] - g_cur.p0[0], dz = gp[2] - g_cur.p0[2], dy = gp[1] - g_cur.p0[1];
    g_cur.travelMax = fmaxf(g_cur.travelMax, sqrtf(dx * dx + dz * dz));
    g_cur.riseMax = fmaxf(g_cur.riseMax, dy);
    g_cur.dropMax = fmaxf(g_cur.dropMax, -dy);
    const float dyaw = WrapPi(gy - g_cur.prevYaw);
    g_cur.yawNet += dyaw;
    g_cur.yawAbs += fabsf(dyaw);
    g_cur.prevYaw = gy;
    Mat4 nk;
    float sy;
    if (ShoulderYaw(&sy)) g_cur.twistMax = fmaxf(g_cur.twistMax, fabsf(WrapPi(sy - ry - g_cur.twist0)));
    if (StableJoint(kNeckHash, &nk)) {
        float l[3];
        BodyLocal(&nk.m[3][0], rp, ry, l);
        const float ex = l[0] - g_cur.neck0[0], ey = l[1] - g_cur.neck0[1], ez = l[2] - g_cur.neck0[2];
        g_cur.neckMax = fmaxf(g_cur.neckMax, sqrtf(ex * ex + ey * ey + ez * ez));
        g_cur.neckRise = fmaxf(g_cur.neckRise, ey + (gp[1] - g_cur.p0[1]));
        g_cur.neckDrop = fmaxf(g_cur.neckDrop, -(ey + (gp[1] - g_cur.p0[1])));
    }
}

}  // namespace gowvr::fp
