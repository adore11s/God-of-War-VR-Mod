#include "mutate.h"
#include "../core/addr.h"
#include "r1.h"
#include "../core/detour.h"
#include "../core/log.h"
#include "../core/mat.h"
#include "headcam.h"
#include "sharpui.h"
#include "../fp/firstperson.h"
#include "../fp/body.h"
#include "../fp/arms.h"
#include "../fp/combat.h"
#include "../fp/walls.h"
#include "../core/config.h"
#include "../input/vrmap.h"
#include "../input/turning.h"
#include "../vr/xr.h"
#include "../diag/crashlog.h"
#include <cmath>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace gowvr::stereo {
namespace {

// tools/re/hookgen.py output for GoW.exe a3b83b87...
constexpr HookSite kBuildRenderViewTable{"BuildRenderViewTable", 0x14045b600, 5,
    {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x05, 0xc7, 0x2f, 0x8f}};
constexpr HookSite kViewUpdate{"ViewUpdate", 0x1404e81a0, 7,
    {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x10, 0x49, 0x89, 0x6b, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48}};

constexpr HookSite kRenderViewBuild{"RenderViewBuild", 0x1404d25b0, 5,
    {0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x57, 0x49, 0x8d, 0xab, 0x18, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec}};
constexpr uint64_t kMainRenderViewVa = 0x142448450;  // RenderView kind 0 (0x590 bytes), CAMERA_FINDINGS.md

constexpr uint64_t kViewTableVa = 0x1425314f0;  // 0xa0-byte entries (CAMERA_FINDINGS.md)
constexpr uint64_t kViewAPtrVa = 0x142d4e5d8;
constexpr int kEntryStride = 0xa0;
constexpr int kFrameMainIndex = 0x4ce0;

using PFN_Build = void(__fastcall*)(uint8_t* frameCtx);
using PFN_ViewUpdate = void(__fastcall*)(uint8_t* view);
PFN_Build o_Build = nullptr;
PFN_ViewUpdate o_ViewUpdate = nullptr;
uint8_t* g_base = nullptr;

using PFN_RVBuild = void(__fastcall*)(uint8_t* rv, int kind, uint8_t* view, uint64_t p4);
PFN_RVBuild o_RVBuild = nullptr;

enum Seam { kNone, kTable, kView, kRenderView };
std::atomic<int> g_seam{kNone};
std::atomic<int> g_framesLeft{0};
std::atomic<bool> g_readbackDue{false};
float g_yawRad = 0, g_offX = 0;
uint64_t g_applied = 0;

template <class T> T* At(uint64_t staticVa) { return (T*)((uint8_t*)GameVa(staticVa)); }

Mat4 Load(const void* p) { Mat4 m; memcpy(&m, p, sizeof(m)); return m; }
void Store(void* p, const Mat4& m) { memcpy(p, &m, sizeof(m)); }

const Mat4* g_deltaOverride = nullptr;  // full local-space delta (head-tracked eye), replaces offX/yaw when set
Mat4 Delta() { return g_deltaOverride ? *g_deltaOverride : Mul(LocalOffsetX(g_offX), LocalYaw(g_yawRad)); }

// Camera->world with pitch and roll removed (world up +Y assumed; logged by SetEyePoses): the head supplies them.
Mat4 Level(const Mat4& c) {
    float bx = c.m[2][0], bz = c.m[2][2];  // back axis projected on the horizontal plane
    float n = sqrtf(bx * bx + bz * bz);
    if (n < 1e-4f) return c;
    bx /= n; bz /= n;
    Mat4 r = c;
    r.m[2][0] = bx; r.m[2][1] = 0.f; r.m[2][2] = bz;
    r.m[1][0] = 0.f; r.m[1][1] = 1.f; r.m[1][2] = 0.f;
    r.m[0][0] = bz; r.m[0][1] = 0.f; r.m[0][2] = -bx;  // X = Y x Z
    return r;
}

// Mutates a (cam->world C, view*proj VP) pair consistently: P = C*VP, C' = D*C, VP' = inv(C')*P.
void Apply(uint8_t* camWorld, uint8_t* viewProj, uint8_t* viewOut, uint8_t* vpvOut, const uint8_t* viewport) {
    Mat4 C = Load(camWorld), VP = Load(viewProj);
    Mat4 P = Mul(C, VP);
    Mat4 C2 = Mul(Delta(), C);
    Mat4 V2 = RigidInverse(C2);
    Mat4 VP2 = Mul(V2, P);
    Store(camWorld, C2);
    Store(viewProj, VP2);
    if (viewOut) Store(viewOut, V2);
    if (vpvOut && viewport) Store(vpvOut, Mul(VP2, Load(viewport)));
}

void __fastcall hk_Build(uint8_t* frameCtx) {
    o_Build(frameCtx);
    if (g_seam != kTable) {
        if (g_readbackDue.exchange(false)) {
            int idx = *(int*)(frameCtx + kFrameMainIndex);
            uint8_t* e = At<uint8_t>(kViewTableVa) + idx * kEntryStride;
            uint8_t* viewA = *At<uint8_t*>(kViewAPtrVa);
            Log("mutate: readback after window: table C == viewA C ? %d", memcmp(e, viewA + 0x150, 64) == 0);
        }
        return;
    }
    int idx = *(int*)(frameCtx + kFrameMainIndex);
    if (idx < 0 || idx > 8) return;
    uint8_t* e = At<uint8_t>(kViewTableVa) + idx * kEntryStride;
    if (g_applied == 0) Log("mutate[table]: first apply idx=%d entry=%p yaw=%.3f rad offx=%.4f", idx, e, g_yawRad, g_offX);
    Apply(e + 0x00, e + 0x40, nullptr, nullptr, nullptr);
    ++g_applied;
    if (--g_framesLeft <= 0) {
        g_seam = kNone;
        g_readbackDue = true;
        Log("mutate[table]: window over after %llu frames", (unsigned long long)g_applied);
    }
}

std::atomic<bool> g_freeze{false};
uint8_t g_frozen[0x1d0];  // view A +0x10 .. +0x1e0 snapshot
bool g_haveFrozen = false;

// Head camera (owner report: shadows cut off behind Kratos). Shadow cascades, culling and secondary views are fitted
// from view A before the frame-graph kick, so the head-centred camera with the VR frustum is written into view A right
// after View::Update; the original game camera is kept (g_viewAOrig) as the base for the per-eye RenderView builds.
bool g_headCamOn = false;
bool g_armsDiag = false;
// ---- camera steering (camera yaw sync). In gameplay first person the VIEW turns only
// with the headset and the player's own right stick: God of War's camera swings behind Kratos on its own, and a view
// following it turned the world around the player (owner: "turning is broken"). The game camera still decides where
// the axe throw and Atreus' shots go, so a virtual right stick steers it toward where the head looks:
//   - while aiming (LT): yaw and pitch, so the throw lands where the player looks;
//   - otherwise only with gowvr.ini camsync=1 (in God of War right-stick flicks switch lock-on targets).
// Stick law (proportional, full stick a few degrees off, no fighting the player's own stick) plus its guards:
// a spin detector (camera runs away while the physical head is still -> pause) and a stuck detector (the camera does
// not answer, e.g. a scripted camera -> pause). The stick directions are learned from the player's own stick use.
struct CamSync {
    bool init = false, loaded = false, stickHeld = false;
    float renderYaw = 0.f, camPrev = 0.f, pitchPrev = 0.f, hmdPrev = 0.f;
    float sign = 0.f, signY = 1.f, learn = 0.f, learnY = 0.f;  // stick +X / +Y -> camera yaw / pitch direction
    float prevErr = 0.f, prevErrY = 0.f, spin = 0.f, stuck = 0.f, badY = 0.f;
    double pauseUntil = 0.0, lastT = 0.0;
    uint64_t trips = 0;
    // snap turning: how far the game camera still has to turn to face the view again (radians, the view yaw's sense) -
    // a snap turns only the view (owner: "after two 90 deg snaps the axe throw goes 45 - 90 deg off": throws follow the
    // game camera, which was up to 180 deg behind - and at 180 deg the aim steering could not tell left from right)
    float alignLeft = 0.f, alignStuck = 0.f;
    double alignSince = 0.0;
    uint64_t aligns = 0;
};
CamSync g_cs;
std::atomic<bool> g_camSyncOn{false};                 // steering outside aiming (gowvr.ini camsync)
float g_csFull = 8.f, g_csMin = 0.18f, g_csDead = 1.f;  // degrees to full stick, least stick that moves, deadband
float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
double Seconds() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
float StickFor(float errDeg, float sign) {
    const float a = fabsf(errDeg);
    if (sign == 0.f || a <= g_csDead) return 0.f;
    const float m = fminf((a - g_csDead) / g_csFull, 1.f);
    return sign * (errDeg > 0.f ? 1.f : -1.f) * (g_csMin + (1.f - g_csMin) * m);
}
// Returns the yaw the first-person view is built on this frame (the game camera's yaw while inactive).
float CamSyncYaw(float camYaw, float camPitch, float hmdYaw, float hmdPitch, bool active, bool aiming) {
    if (!g_cs.loaded) {
        g_cs.loaded = true;
        g_cs.sign = config::GetFloat("camsync_sign", 0.f);
        g_cs.signY = config::GetFloat("camsync_sign_y", 1.f) >= 0.f ? 1.f : -1.f;
        g_camSyncOn = config::GetFloat("camsync", 0.f) > 0.5f;
        g_csFull = fmaxf(config::GetFloat("camsync_full_deg", 8.f), 1.f);
        g_csMin = config::GetFloat("camsync_min", 0.18f);
        g_csDead = config::GetFloat("camsync_dead_deg", 1.f);
    }
    const double now = Seconds();
    const float dt = (float)fmin(fmax(now - g_cs.lastT, 0.001), 0.1);
    g_cs.lastT = now;
    if (!active || !g_cs.init) {
        input::SnapTakeSteps();  // (pushes outside gameplay first person do nothing)
        g_cs.renderYaw = g_cs.camPrev = camYaw;
        g_cs.pitchPrev = camPitch;
        g_cs.hmdPrev = hmdYaw;
        g_cs.init = active;
        g_cs.spin = g_cs.stuck = g_cs.badY = 0.f;
        input::SetCameraSyncStick(0.f, 0.f);
        return camYaw;
    }
    const float d = WrapPi(camYaw - g_cs.camPrev), dp = camPitch - g_cs.pitchPrev;
    g_cs.camPrev = camYaw;
    g_cs.pitchPrev = camPitch;
    const float hmdRate = fabsf(WrapPi(hmdYaw - g_cs.hmdPrev)) / dt * 57.2958f;  // deg/s of the physical head
    g_cs.hmdPrev = hmdYaw;
    const vr::InputSnapshot in = vr::XrInput();
    // snap turning (input/turning.cpp, the VR settings panel): a push turns the view's own yaw at once - and the drawn
    // body with it - the way the stick turns the camera in smooth turning (camsync_sign); the game camera is left alone,
    // and the stick's X is not "the player turns" then (the view does not follow the camera's own swings)
    const bool snap = input::SnapMode();
    if (const int n = input::SnapTakeSteps()) {
        const float dy = (g_cs.sign != 0.f ? g_cs.sign : -1.f) * (float)n * input::SnapStepRad();
        g_cs.renderYaw = WrapPi(g_cs.renderYaw + dy);
        fp::BodySnapYaw(dy);
        g_cs.alignLeft += dy;  // the game camera follows in the background (below)
        g_cs.alignSince = now;
        g_cs.alignStuck = 0.f;
        static int snapLogs = 0;
        if (snapLogs++ < 5) Log("turning: snap %+d x %.0f deg (view and body turned %.0f deg)", n, input::SnapStepRad() * 57.2958f, dy * 57.2958f);
    }
    const float rx = snap ? 0.f : in.hand[1].stick[0], ry = in.hand[1].stick[1];
    g_cs.stickHeld = g_cs.stickHeld ? (fabsf(rx) > 0.05f || fabsf(ry) > 0.05f) : (fabsf(rx) > 0.15f || fabsf(ry) > 0.15f);
    if (g_cs.stickHeld || fabsf(d) > 0.35f) {  // the player turns (or a quick turn / cut): the view turns with it
        if (fabsf(rx) > 0.05f || fabsf(d) > 0.35f) g_cs.renderYaw = WrapPi(g_cs.renderYaw + d);
        if (fabsf(rx) > 0.5f && fabsf(d) < 0.35f) {
            g_cs.learn += d * (rx > 0.f ? 1.f : -1.f);
            if (fabsf(g_cs.learn) > 0.6f) {
                const float s = g_cs.learn > 0.f ? 1.f : -1.f;
                if (s != g_cs.sign) {
                    g_cs.sign = s;
                    config::SetFloat("camsync_sign", s);
                    Log("camsync: learned stick direction %+.0f (right stick +X %s the camera yaw)", s, s > 0.f ? "increases" : "decreases");
                }
                g_cs.learn = 0.f;
            }
        }
        if (fabsf(ry) > 0.5f && fabsf(rx) < 0.3f && fabsf(dp) < 0.2f) {
            g_cs.learnY += dp * (ry > 0.f ? 1.f : -1.f);
            if (fabsf(g_cs.learnY) > 0.25f) {
                const float s = g_cs.learnY > 0.f ? 1.f : -1.f;
                if (s != g_cs.signY) {
                    g_cs.signY = s;
                    config::SetFloat("camsync_sign_y", s);
                    Log("camsync: learned pitch stick direction %+.0f", s);
                }
                g_cs.learnY = 0.f;
            }
        }
        g_cs.spin = g_cs.stuck = g_cs.badY = 0.f;
        input::SetCameraSyncStick(0.f, 0.f);
        return g_cs.renderYaw;
    }
    // the snap's camera catch-up: what the camera turned this frame comes off; done when it got there, after 3 s, or
    // when the camera does not answer (lock-on, a scripted camera: steering there would switch lock-on targets)
    const float csSign = g_cs.sign != 0.f ? g_cs.sign : -1.f;
    if (g_cs.alignLeft != 0.f) {
        const float before = g_cs.alignLeft;
        g_cs.alignLeft -= d;
        const float camRateA = fabsf(d) / dt * 57.2958f;
        g_cs.alignStuck = camRateA < 5.f ? g_cs.alignStuck + dt : 0.f;
        if (before * g_cs.alignLeft <= 0.f || fabsf(g_cs.alignLeft) < 0.03f || now - g_cs.alignSince > 3.0 ||
            g_cs.alignStuck > 0.3f) {
            if (++g_cs.aligns <= 5 || g_cs.aligns % 50 == 0)
                Log("camsync: snap catch-up %s (%.0f deg left, %llu)",
                    g_cs.alignStuck > 0.3f ? "stopped - the camera does not answer" : "done", g_cs.alignLeft * 57.2958f,
                    (unsigned long long)g_cs.aligns);
            g_cs.alignLeft = 0.f;
        }
    }
    if (g_cs.alignLeft != 0.f && !aiming && now >= g_cs.pauseUntil) {
        g_cs.prevErr = WrapPi(g_cs.renderYaw + hmdYaw - camYaw) * 57.2958f;
        input::SetCameraSyncStick(StickFor(g_cs.alignLeft * 57.2958f, csSign), 0.f);
        return g_cs.renderYaw;
    }
    const bool steerYaw = aiming || g_camSyncOn.load();
    float err = WrapPi(g_cs.renderYaw + hmdYaw - camYaw) * 57.2958f;  // head's world yaw vs the game camera
    // a snap's catch-up still running: the far side of 180 deg is the way the snap went, not the shorter-looking one
    if (g_cs.alignLeft != 0.f && fabsf(err) > 90.f && err * g_cs.alignLeft < 0.f) err += g_cs.alignLeft > 0.f ? 360.f : -360.f;
    const float errY = (hmdPitch - camPitch) * 57.2958f;                   // head pitch vs the game camera's
    if (!steerYaw || now < g_cs.pauseUntil) {
        g_cs.spin = g_cs.stuck = g_cs.badY = 0.f;
        g_cs.prevErr = err;
        g_cs.prevErrY = errY;
        input::SetCameraSyncStick(0.f, 0.f);
        return g_cs.renderYaw;
    }
    float x = StickFor(err, g_cs.sign);
    float y = aiming ? StickFor(errY, g_cs.signY) : 0.f;
    const float camRate = fabsf(d) / dt * 57.2958f;
    // spin detector: the camera keeps turning, the physical head is still and the error does not close
    if (fabsf(x) > 0.5f && camRate > 30.f && hmdRate < 35.f && fabsf(err) >= fabsf(g_cs.prevErr) - 0.1f) g_cs.spin += dt;
    else g_cs.spin = 0.f;
    // stuck detector: steering hard, the camera does not answer (scripted camera, lock-on)
    if (fabsf(x) > 0.8f && fabsf(err) > 30.f && camRate < 5.f) g_cs.stuck += dt;
    else g_cs.stuck = 0.f;
    if (g_cs.spin > 0.25f || g_cs.stuck > 1.f) {
        ++g_cs.trips;
        g_cs.pauseUntil = now + (g_cs.spin > 0.25f ? 0.5 : 2.0);
        if (g_cs.trips <= 20 || g_cs.trips % 100 == 0)
            Log("camsync: %s (error %.0f deg, camera %.0f deg/s, head %.0f deg/s) -> steering paused (%llu)",
                g_cs.spin > 0.25f ? "camera runs away" : "camera does not answer", err, camRate, hmdRate,
                (unsigned long long)g_cs.trips);
        g_cs.spin = g_cs.stuck = 0.f;
        x = y = 0.f;
    }
    // pitch: the stick direction is only learned from the player's own stick; a pitch error that grows while steering
    // means the direction is inverted in the game's settings -> flip it
    if (fabsf(y) > 0.5f && fabsf(errY) > 8.f && fabsf(errY) > fabsf(g_cs.prevErrY) + 0.05f) g_cs.badY += dt;
    else g_cs.badY = 0.f;
    if (g_cs.badY > 0.3f) {
        g_cs.signY = -g_cs.signY;
        config::SetFloat("camsync_sign_y", g_cs.signY);
        Log("camsync: pitch error grows while steering -> pitch stick direction flipped to %+.0f", g_cs.signY);
        g_cs.badY = 0.f;
        y = 0.f;
    }
    g_cs.prevErr = err;
    g_cs.prevErrY = errY;
    static uint64_t steered = 0;
    if (++steered % 90 == 1)  // live evidence for tuning (how fast God of War's camera answers the stick)
        Log("camsync: %s err %.1f deg pitchErr %.1f deg stick(%.2f %.2f) camera %.0f deg/s sign(%+.0f %+.0f) (%llu frames)",
            aiming ? "aim" : "always", err, errY, x, y, camRate, g_cs.sign, g_cs.signY, (unsigned long long)steered);
    input::SetCameraSyncStick(x, y);
    return g_cs.renderYaw;
}
void CamSyncEnable(bool on) { g_camSyncOn = on; config::SetFloat("camsync", on ? 1.f : 0.f); g_cs.init = false; Log("camsync: %s (outside aiming)", on ? "on" : "off"); }
  // diagnostic: arms driven without the head camera
// Headset aiming (owner: "implement headset based throwing of the axe"): while aiming in gameplay first person the
// head camera is also written into view B, the gameplay-facing view (26 read sites: traversal prompts, look-at,
// animation, Lua - CAMERA_FINDINGS.md), so the game's aim reads where the head looks without waiting for the stick
// steering. Position and orientation of the head (eye midpoint); the game's projection is kept.
constexpr uint64_t kViewBPtrVa = 0x142d4e5e0;
std::atomic<bool> g_aimHead{false};
Mat4 g_headWorld;           // last frame's head camera (view A after the head camera), game thread
// D-147: the same with view A's projection tangents, for the enemy health bars (read on the animation job threads)
SRWLOCK g_uiCamLock = SRWLOCK_INIT;
Mat4 g_uiCam{};
float g_uiCamTan[2] = {0.f, 0.f};
ULONGLONG g_uiCamAt = 0;

uint64_t g_aimViewBWrites = 0;
// Owner: "when i throw something it throws it a bit lower than it should" -> the aim view is pitched up by this
// (degrees, gowvr.ini aim_pitch_deg, Alt+F5/F6 -/+0.5).
float g_aimPitchDeg = -1000.f;
bool g_fpBaseValid = false;  // this frame's first-person base (set at View::Update)
Mat4 g_fpBase;
void HeadCameraIntoViewA(uint8_t* view);

// D-083 (owner: "make the UI on the screen like 75% size"): in the headset the UI covered the whole eye image, its edges
// out at the borders of the view. The 2D views C / D (UI; ortho, CAMERA_FINDINGS.md) get their clip x/y scaled about the
// centre: projection and view x projection columns 0/1, the inverse projection rows 0/1, and the two viewport products
// rebuilt from them. Stereo frames only (the flat 2D screen keeps the full UI). Trade-off: markers the UI places over
// 3D points (prompts, enemy bars) move toward the centre by the same factor.
constexpr uint64_t kViewCPtrVa = 0x142d4e5e8, kViewDPtrVa = 0x142d4e5f0;
constexpr uint64_t kInPauseMenuVa = 0x1422e795b;  // byte set by the Lua binding inPauseMenu (0x140906a40)
constexpr uint64_t kVideoHandleVa = 0x142d4df18;   // the Bink player's open video (BinkOpen result, 0 = none)
constexpr uint64_t kVideoClosingVa = 0x142d1b5d4;  // its closing-frame count (0 while it plays)
float g_uiScale = -1.f;
int g_screen2DMode = -1;
std::atomic<bool> g_screen2D{false};
std::atomic<uint64_t> g_viewAUpdates{0};  // D-083 loadwatch
uint64_t g_uiScaled = 0, g_uiUpdates = 0;
// VR check (log): why a UI view update was not scaled here, and the flat screen's signals (UiDiagText). On another
// game build a view laid out differently would show up as "not orthographic" / never marked instead of scaled.
uint64_t g_uiSkipCam = 0, g_uiSkipPersp = 0, g_uiSharp = 0;
bool g_lastPause = false, g_lastLoading = false, g_lastFaded = false, g_lastVideo = false;
float g_lastFadeA = 0.f;

// D-102 (owner: "make the aiming icon bigger in F3 and F1 mode"): the UI is one layer (D-101), so in the first-person
// modes it gets its own scale - ui_scale_fp 0.5 (third person keeps ui_scale 0.35). In F1 the Combat HUD (health / rage /
// runic bars) is hidden anyway (CombatHudTick), so mainly the reticle, prompts and subtitles grow. "uiscalefp <s>"
float g_uiScaleFp = -1.f;
void UiScaleView(uint8_t* view) {
    if (g_uiScale < 0.f) g_uiScale = config::GetFloat("ui_scale", 0.35f);  // D-094b: 0.35 (owner; 0.75, 0.55, 0.4 were too big)
    if (g_uiScaleFp < 0.f) g_uiScaleFp = config::GetFloat("ui_scale_fp", 0.5f);
    ++g_uiUpdates;
    const int fpm = fp::FpMode();
    // D-103 (owner: "the bigger aiming icon also in normal, non F1 / F3"): the reticle shows only while aiming, so in
    // third person the UI takes the first-person scale while the aim trigger (left) is held and 400 ms after (the reticle
    // fades out); the HUD bars on screen grow with it while aiming (one UI layer, D-101)
    static ULONGLONG aimSeen = 0;
    {
        const vr::InputSnapshot in = vr::XrInput();
        if (in.focused && in.hand[0].active && in.hand[0].trigger > 0.25f) aimSeen = GetTickCount64();
    }
    const bool aiming = aimSeen && GetTickCount64() - aimSeen < 400;
    const float s = (fpm == 1 || fpm == 2 || aiming) && g_uiScaleFp > 0.05f ? g_uiScaleFp : g_uiScale;
    if (!g_headCamOn || g_screen2D.load()) { ++g_uiSkipCam; return; }
    float* P = (float*)(view + 0xd0);
    if (P[11] != 0.f || P[15] != 1.f) { ++g_uiSkipPersp; return; }  // orthographic only (a perspective P has P23 = -1)
    // D-104 sharp HUD: the UI is drawn unscaled into the mod's own target and pasted in scaled (sharpui.cpp)
    if (SharpUiMarkView(view)) { ++g_uiSharp; return; }
    if (s <= 0.05f || fabsf(s - 1.f) < 1e-3f) return;
    float* VP = (float*)(view + 0x50);
    float* iP = (float*)(view + 0x190);
    // the headset's vertical stretch undone (UiVerticalFix; the orthographic P gives the UI's width / height)
    const float sy = s * UiVerticalFix(fabsf(P[0]) > 1e-9f ? fabsf(P[5] / P[0]) : 0.f);
    for (int r = 0; r < 4; ++r) {
        P[r * 4 + 0] *= s; P[r * 4 + 1] *= sy;
        VP[r * 4 + 0] *= s; VP[r * 4 + 1] *= sy;
        iP[r] /= s; iP[4 + r] /= sy;
    }
    const Mat4 vp = Load(view + 0x1d0);
    const Mat4 a = Mul(Load(view + 0x50), vp), b = Mul(Load(view + 0xd0), vp);
    memcpy(view + 0x90, &a, sizeof(a));
    memcpy(view + 0x110, &b, sizeof(b));
    if (++g_uiScaled <= 3 || g_uiScaled % 5000 == 0)
        Log("ui: 2D view %p drawn at %.2f (scaled %llu of %llu updates)", (void*)view, s, (unsigned long long)g_uiScaled,
            (unsigned long long)g_uiUpdates);
}

// D-102 (owner: "in F1 hide the health / Spartan Rage / runic bars so they only show on some action"): the game's own HUD
// option Combat HUD ("Display Kratos and Atreus combat information"; Lua SetUserCombatHUDSetting 0x140763130 -> byte
// 0x141078feb; LIVE harness 2026-10-04: 0 Off, 1 On, 2 Touch - a direct write hides / shows the bars at once, and with
// Touch a touchpad (Back) tap shows them for a few seconds). Entering F1 sets f1_combat_hud (default 2 Touch: hidden
// until called; -1 leaves the game's setting); leaving F1 puts the player's own value back (kept in gowvr.ini
// combat_hud_restore, so a setting saved by the game while in F1 is not taken as the player's)
void CombatHudTick() {
    static int want = -2;
    if (want == -2) want = (int)config::GetFloat("f1_combat_hud", 2.f);
    uint8_t* v = (uint8_t*)GameVa(0x141078febull);
    // D-218: while the left hand is held at the head the byte shows 1 (On); `real` is the setting underneath it - what
    // the F1 rules below read and change, and what the byte goes back to when the hand comes down
    static bool peekOn = false;
    static uint8_t peekReal = 1;
    uint8_t real = peekOn ? peekReal : *v;
    if (want >= 0 && want <= 2) {
        static bool applied = false;
        const bool f1 = fp::FpMode() == 1;
        if (f1 && !applied) {
            applied = true;
            const int cur = real;
            if (cur != want && cur <= 2) config::SetFloat("combat_hud_restore", (float)cur);
            real = (uint8_t)want;
            Log("hud: F1 - Combat HUD %d -> %d (%s)", cur, want, want == 2 ? "Touch: hidden until called" : want == 0 ? "Off" : "On");
        } else if (!f1 && applied) {
            applied = false;
            const int restore = (int)config::GetFloat("combat_hud_restore", 1.f);
            if (real == want) {
                real = (uint8_t)restore;
                Log("hud: F1 off - Combat HUD back to %d", restore);
            }
        }
    }
    const bool peek = input::HudPeek();
    if (peek) {
        if (!peekOn) Log("hud: left hand at the head - Combat HUD shown (setting %d kept underneath, D-218)", (int)real);
        peekOn = true;
        peekReal = real;
        if (*v != 1) *v = 1;
    } else {
        if (peekOn) Log("hud: left hand down - Combat HUD back to %d (D-218)", (int)real);
        peekOn = false;
        if (*v != real) *v = real;
    }
}

// D-107 (owner: "make the cursor move to the centre of the view - in the flat game it's on the right"; "the cursor is
// head tracked but it's not showing where I shoot"). LIVE capture in the owner's game, the "help Atreus aim at the deer"
// sequence: the game draws its crosshair at UI (0.623, 0.5) and aims through it; its aim view (view B) is zoomed to
// tan 0.296 x 0.167 (33 deg), the eye images are tan 2.30 x 1.60 (133 deg). With the head as view B the arrow went ~4 deg
// right of where the head looks, and the HUD showed the crosshair elsewhere again. Now view B is also turned so the ray
// through the game's reticle IS the head's (aim_pitch_deg-pitched) forward, and the sharp HUD draws the reticle there:
// at the centre of the view (raised by aim_pitch_deg) - cursor, head and shot agree. Reticle spots (UI uv): gameplay
// (axe; LIVE hudshot) ui_reticle_x / _y 0.5 / 0.4366, radius ui_reticle_r 0.045; cinematic aim sections (Atreus'
// crosshair) ui_reticle_cine_x / _y / _r 0.623 / 0.5 / 0.06. Only while the sharp HUD runs (otherwise the reticle is not
// drawn at the centre, so the shot is left as before); gowvr.ini aim_reticle_align 0 turns it off.
struct AimRet { float srcU, srcV, srcR, dstU, dstV; };
AimRet g_aimRet{0.5f, 0.4366f, 0.045f, 0.5f, 0.5f};
std::atomic<ULONGLONG> g_aimRetTick{0};
float EyeTanYForAim();
void AlignReticleToHead(uint8_t* view, Mat4* hw) {
    static int align = -1;
    if (align < 0) align = config::GetFloat("aim_reticle_align", 1.f) > 0.5f ? 1 : 0;
    if (!align || !SharpUiActive()) return;
    static float g[6] = {-1.f};
    if (g[0] < 0.f) {
        g[0] = config::GetFloat("ui_reticle_x", 0.5f);
        g[1] = config::GetFloat("ui_reticle_y", 0.4366f);
        g[2] = config::GetFloat("ui_reticle_r", 0.0225f);  // D-152: half (was 0.045)
        g[3] = config::GetFloat("ui_reticle_cine_x", 0.623f);
        g[4] = config::GetFloat("ui_reticle_cine_y", 0.5f);
        g[5] = config::GetFloat("ui_reticle_cine_r", 0.03f);  // D-152: half (was 0.06)
    }
    const bool cine = fp::InCinematic();
    const float u = cine ? g[3] : g[0], v = cine ? g[4] : g[1], r = cine ? g[5] : g[2];
    const float* P = (const float*)(view + 0xd0);  // view B's own projection (kept): P00 = 1/tanX, P11 = 1/tanY
    if (P[0] <= 0.f || P[5] <= 0.f) return;
    // the reticle ray in view B's camera space (looks down -Z): (x, y, -1)
    const float x = (u - 0.5f) * 2.f / P[0], y = (0.5f - v) * 2.f / P[5];
    // Q turns that ray onto -Z (row vectors: yaw about Y, then pitch about X); view B = Q * pitched head
    const float a = atanf(x), L = sqrtf(1.f + x * x), b = atan2f(-y, L);
    Mat4 Ry{}, Rx{};
    Ry.m[0][0] = cosf(a); Ry.m[0][2] = -sinf(a); Ry.m[1][1] = 1.f; Ry.m[2][0] = sinf(a); Ry.m[2][2] = cosf(a); Ry.m[3][3] = 1.f;
    Rx.m[0][0] = 1.f; Rx.m[1][1] = cosf(b); Rx.m[1][2] = sinf(b); Rx.m[2][1] = -sinf(b); Rx.m[2][2] = cosf(b); Rx.m[3][3] = 1.f;
    *hw = Mul(Mul(Ry, Rx), *hw);
    // where that is in the eye image: the head's forward raised by aim_pitch_deg
    const float ty = EyeTanYForAim();
    g_aimRet = {u, v, r, 0.5f, ty > 0.f ? 0.5f - tanf(g_aimPitchDeg * 0.0174533f) / (2.f * ty) : 0.5f};
    g_aimRetTick = GetTickCount64();
    static uint64_t n = 0;
    if (++n == 1 || n % 900 == 0)
        Log("aim: reticle (%.3f %.3f)%s aligned to the head - view B turned %.1f deg yaw %.1f deg pitch (tan %.3f x %.3f); "
            "drawn at (%.3f %.3f)",
            u, v, cine ? " cinematic" : "", -a * 57.2958f, b * 57.2958f, 1.f / P[0], 1.f / P[5], g_aimRet.dstU,
            g_aimRet.dstV);
}

void __fastcall hk_ViewUpdate(uint8_t* view) {
    o_ViewUpdate(view);
    if (view == *At<uint8_t*>(kViewAPtrVa)) {  // D-083: latched once per frame (the kick and xr.cpp read it)
        ++g_viewAUpdates;
        CombatHudTick();  // D-102
        diag::PassTableWatch();  // D-220g
        if (g_screen2DMode < 0) g_screen2DMode = config::GetFloat("screen_2d_menus", 1.f) > 0.5f ? 1 : 0;
        // loading screens (owner: "make loadings be in the 2D floating space too"): LIVE loadwatch through a Continue
        // load - the tip / spinning-logo screen is drawn by ordinary stereo kicks, none of the game's flags tested
        // marks it, but Kratos is not posed at all for its whole length (no hero world pass, ~570 frames) while he is
        // posed every frame on the title scene and in gameplay (F1: eyesync even forces his pass each frame; cutscenes
        // not measured). screen_2d_loading_frames frames without a hero pass = a loading screen. (A pause stops his
        // passes too: the 2D screen then stays ~10 frames after the pause menu closes.)
        // (LIVE: single hero passes happen inside a load - left only after 10 posed frames in a row, no on/off flash)
        // D-085 (owner: "loading starts, ~1 s full VR, then 2D, and again ~1 s VR before it loads up"): those seconds are
        // the game's full-screen FADES around the load - 0x1422ae730..74c are two fade overlay colours (RGBA; "Quitting
        // to main menu" sets the first to opaque black 0,0,0,1), LIVE loadwatch: alpha 74c = 1 through the fade-out
        // before the load and the fade-in after it, alpha 73c = 1 for ~2 s after that - while Kratos is still / again
        // posed. A screen covered by a fade (alpha >= 0.9) is a 2D screen too; Kratos-not-posed now after 10 frames
        // (was 30). IsInCinematicMode: his passes may pause while a cutscene shows others (not measured) - there the
        // no-pose rule waits 240 frames. A fade left on for 1200 posed frames in a row is ignored (stuck overlay).
        static uint64_t lastHeroPasses = 0;
        static int noHero = 0, heroRun = 0;
        static bool loading = false;
        const uint64_t hp = fp::HeroWorldPassCount();
        if (hp != lastHeroPasses) { noHero = 0; ++heroRun; } else { ++noHero; heroRun = 0; }
        lastHeroPasses = hp;
        static const int loadFrames = (int)config::GetFloat("screen_2d_loading_frames", 10.f);
        const int needNoHero = fp::InCinematic() ? (loadFrames > 0 ? 240 : 0) : loadFrames;
        if (needNoHero > 0 && noHero >= needNoHero) loading = true;
        else if (needNoHero <= 0 || heroRun >= 10) loading = false;
        const float fadeA = fmaxf(*At<float>(0x1422ae74c), *At<float>(0x1422ae73c));
        static int fadePosed = 0;
        const bool fadeCover = fadeA >= 0.9f && fadeA <= 1.01f;
        fadePosed = fadeCover && heroRun > 0 ? fadePosed + 1 : 0;
        static const bool fadeOn = config::GetFloat("screen_2d_fades", 1.f) > 0.5f;
        const bool faded = fadeOn && fadeCover && fadePosed < 1200;
        const bool pause = *At<uint8_t>(kInPauseMenuVa) != 0;
        // a full-screen video (owner + a tester: the PlayStation Studios logo at startup was stretched over the whole
        // view): the game's one Bink player (video_bink: BinkOpen -> 0x142d4df18, cleared after its 3 closing frames
        // counted in 0x142d1b5d4). The title scene behind the logo already poses Kratos, so the loading rule switched
        // to stereo. "Playing" as the game itself tests it: handle set and not closing. (The other videos are the skill
        // previews in the menus - a 2D screen anyway.)
        const bool video = *At<uint64_t>(kVideoHandleVa) != 0 && *At<int32_t>(kVideoClosingVa) == 0;
        g_lastPause = pause; g_lastLoading = loading; g_lastFaded = faded; g_lastFadeA = fadeA; g_lastVideo = video;
        const bool on = g_headCamOn &&
                        (g_screen2DMode == 2 || (g_screen2DMode == 1 && (pause || loading || faded || video)));
        if (on != g_screen2D.load())
            Log("ui: %s", on ? (pause ? "2D screen (pause menu): flat game camera, one kick"
                                      : video ? "2D screen (video playing): flat game camera, one kick"
                                      : loading ? "2D screen (loading: Kratos not posed): flat game camera, one kick"
                                      : faded ? "2D screen (screen faded out): flat game camera, one kick"
                                                : "2D screen (forced)")
                             : "2D screen off: stereo");
        g_screen2D = on;
    }
    if (view == *At<uint8_t*>(kViewCPtrVa) || view == *At<uint8_t*>(kViewDPtrVa)) UiScaleView(view);
    if (g_freeze && view == *At<uint8_t*>(kViewAPtrVa)) {
        if (!g_haveFrozen) {
            memcpy(g_frozen, view + 0x10, sizeof(g_frozen));
            g_haveFrozen = true;
            Log("mutate: view A frozen (cam pos %.4f %.4f %.4f)", *(float*)(view + 0x180), *(float*)(view + 0x184),
                *(float*)(view + 0x188));
        }
        memcpy(view + 0x10, g_frozen, sizeof(g_frozen));
        // fall through: a view-seam mutation may be applied on top of the frozen pose
    }
    if (view == *At<uint8_t*>(kViewAPtrVa)) {
        fp::BodyPostAnim();
        if (!g_headCamOn) {
            // armsdiag: drive the arms in the flat third-person view (simulated hands) so the hands can be inspected
            Mat4 b, hands[2];
            bool valid[2] = {false, false};
            bool on = g_armsDiag && fp::DiagBodyBase(&b);
            if (on) HeadcamHandsWorld(b, hands, valid);
            fp::ArmsUpdate(on, on ? hands : nullptr, on ? valid : nullptr);
            fp::BodyFollowYaw(0.f, false, false);  // VR off: hand the facing back to the game
        }
    }
    if (g_headCamOn && !g_screen2D.load() && view == *At<uint8_t*>(kViewAPtrVa)) HeadCameraIntoViewA(view);
    if (g_aimHead.load() && view == *At<uint8_t*>(kViewBPtrVa)) {
        if (g_aimPitchDeg < -999.f) g_aimPitchDeg = config::GetFloat("aim_pitch_deg", 3.f);
        Mat4 hw = g_headWorld;  // pitch up about the camera's right axis (it looks down -Z)
        const float ap = g_aimPitchDeg * 0.0174533f, ca = cosf(ap), sa = sinf(ap);
        for (int j = 0; j < 3; ++j) {
            const float y = g_headWorld.m[1][j], z = g_headWorld.m[2][j];
            hw.m[1][j] = y * ca + z * sa;
            hw.m[2][j] = z * ca - y * sa;
        }
        AlignReticleToHead(view, &hw);  // D-107
        Mat4 d = Mul(hw, RigidInverse(Load(view + 0x150)));
        g_deltaOverride = &d;
        Apply(view + 0x150, view + 0x50, view + 0x10, view + 0x90, view + 0x1d0);
        g_deltaOverride = nullptr;
        if (++g_aimViewBWrites == 1 || g_aimViewBWrites % 900 == 0)
            Log("aim: head camera written into view B (%llu frames)", (unsigned long long)g_aimViewBWrites);
    }
    if (g_seam != kView || view != *At<uint8_t*>(kViewAPtrVa)) return;
    if (g_applied == 0) Log("mutate[view]: first apply view=%p yaw=%.3f rad offx=%.4f", view, g_yawRad, g_offX);
    Apply(view + 0x150, view + 0x50, view + 0x10, view + 0x90, view + 0x1d0);
    ++g_applied;
    if (--g_framesLeft <= 0) {
        g_seam = kNone;
        g_readbackDue = true;
        Log("mutate[view]: window over after %llu frames", (unsigned long long)g_applied);
    }
}

// Camera delivery by parameter: the RenderView builder reads the engine view it is given; hand it a modified copy of
// view A instead of view A itself. View A (and its gameplay/job readers) never sees the delta.
alignas(16) uint8_t g_viewCopy[0x380];
// View A write-back (owner report: fog/smoke drawn as flat walls in both eyes): systems inside the kick besides the
// RenderView builder (particle billboarding, fog volume) read view A directly, so each eye's camera is also written into
// view A. The original of this frame is kept (g_viewAOrig) so eye 1 is built from it, not from eye 0's result; if no
// View::Update ran since (view A still equals what we last wrote), the previous original is reused.
bool g_viewAWrite = true;
alignas(16) uint8_t g_viewAOrig[0x380];
alignas(16) uint8_t g_viewALast[0x380];
bool g_viewAOrigValid = false;

// Per-eye offset set by the R1 double kick around each kick (eye camera through the same parameter override).
std::atomic<bool> g_eyeOn{false};
float g_eyeOffX = 0.f, g_eyeYaw = 0.f;
int g_eyeIndex = 0;
bool g_eyePoseMode = false, g_eyeLevel = true;
Mat4 g_eyeDelta[2];
float g_eyeTan[2][2] = {{0, 0}, {0, 0}};
float g_fovTan[2] = {0, 0};  // requested symmetric tanX/tanY for the eye views (0 = keep the game's projection)
float EyeTanYForAim() { return g_fovTan[1]; }

// Rewrites every projection-derived field of a view copy for a symmetric frustum (tanX, tanY), keeping near/far and the
// reverse-Z terms. Layout (LIVE-ext dump of view A, 2026-09-27): +0x10 view, +0x50 view*proj, +0x90 view*proj*viewport,
// +0xd0 proj (P00=1/tanX, P11=1/tanY), +0x110 proj*viewport, +0x190 inverse proj (tanX, tanY on the diagonal),
// +0x1d0 viewport, +0x250 tanX,tanY,.., +0x260/+0x270 side-plane normals (-k,0,-tanX*k)/(0,-k,-tanY*k) with
// k=1/sqrt(1+tanX^2), +0x280/+0x290 tanX/tanY splats.
void OverrideProjection(uint8_t* v, float tx, float ty) {
    float* P = (float*)(v + 0xd0);
    P[0] = 1.f / tx;
    P[5] = 1.f / ty;
    float* Pi = (float*)(v + 0x190);
    Pi[0] = tx;
    Pi[5] = ty;
    Mat4 vp = Load(v + 0x1d0);
    Store(v + 0x110, Mul(Load(v + 0xd0), vp));
    Mat4 VP = Mul(Load(v + 0x10), Load(v + 0xd0));
    Store(v + 0x50, VP);
    Store(v + 0x90, Mul(VP, vp));
    float* t = (float*)(v + 0x250);
    t[0] = tx;
    t[1] = ty;
    float k = 1.f / sqrtf(1.f + tx * tx);
    float* nx = (float*)(v + 0x260);
    nx[0] = -k; nx[1] = 0.f; nx[2] = -tx * k;
    float* ny = (float*)(v + 0x270);
    ny[0] = 0.f; ny[1] = -k; ny[2] = -ty * k;
    float* sx = (float*)(v + 0x280);
    float* sy = (float*)(v + 0x290);
    for (int i = 0; i < 4; ++i) { sx[i] = tx; sy[i] = ty; }
}  // rendered tanX, tanY per eye (from the built RenderView projection)
uint8_t g_eyeHist[2][128];
bool g_eyeHistValid[2] = {false, false};
std::atomic<int> g_rvProbe{0};
int g_rvProbeEye = 1;
uint8_t g_rvSnap[2][0x590];

std::atomic<int> g_camLog{0};

void __fastcall hk_RVBuild(uint8_t* rv, int kind, uint8_t* view, uint64_t p4) {
    if (g_camLog > 0 && kind == 0 && rv == At<uint8_t>(kMainRenderViewVa)) {
        const float* c = (const float*)(view + 0x150);
        Log("camlog: eyeOn=%d eye=%d view=%p T(%.4f %.4f %.4f) Z(%.4f %.4f %.4f) tid=%lu", g_eyeOn.load(), g_eyeIndex,
            view, c[12], c[13], c[14], c[8], c[9], c[10], GetCurrentThreadId());
        --g_camLog;
    }
    if (g_eyeOn && kind == 0 && rv == At<uint8_t>(kMainRenderViewVa) && view == *At<uint8_t*>(kViewAPtrVa)) {
        if (g_viewAWrite && g_eyePoseMode && g_viewAOrigValid) memcpy(view, g_viewAOrig, sizeof(g_viewAOrig));
        memcpy(g_viewCopy, view, sizeof(g_viewCopy));
        float savedOff = g_offX, savedYaw = g_yawRad;
        g_offX = g_eyeOffX;
        g_yawRad = g_eyeYaw;
        Mat4 d;
        if (g_eyePoseMode) {  // head-tracked eye: C' = E_eye * Level(C) expressed as one local delta on C
            Mat4 c = Load(g_viewCopy + 0x150);
            Mat4 base = (g_headCamOn && g_fpBaseValid) ? g_fpBase : (g_eyeLevel ? Level(c) : c);
            Mat4 lv = Mul(base, RigidInverse(c));
            Mat4 e = g_eyeDelta[g_eyeIndex];
            // D-161 (owner: "it knows when I go into the wall with the headset but doesn't push me out"): D-160 pulled
            // back only view A's head camera (center); the headset's eye views are built here from their own poses
            // relative to the same base - they get the same wall shift (F1 gameplay; zero otherwise)
            if (g_headCamOn && g_fpBaseValid) fp::WallsEyeShift(e);
            d = Mul(e, lv);
            g_deltaOverride = &d;
        }
        Apply(g_viewCopy + 0x150, g_viewCopy + 0x50, g_viewCopy + 0x10, g_viewCopy + 0x90, g_viewCopy + 0x1d0);
        g_deltaOverride = nullptr;
        if (g_eyePoseMode && g_fovTan[0] > 0.f && g_fovTan[1] > 0.f) OverrideProjection(g_viewCopy, g_fovTan[0], g_fovTan[1]);
        if (g_viewAWrite && g_eyePoseMode) {
            memcpy(view, g_viewCopy, sizeof(g_viewCopy));
            memcpy(g_viewALast, g_viewCopy, sizeof(g_viewALast));
        }
        g_offX = savedOff;
        g_yawRad = savedYaw;
        // Per-eye banked temporal state: the builder copies current (+0x80 cam->world, +0x240 view*proj) into previous
        // (+0xc0, +0x100) before computing the new frame, so seed "current" with THIS eye's last values; the engine then
        // derives a correct per-eye previous camera for velocity/TAA/FSR reprojection (rvprobe receipt 2026-09-27).
        int eye = g_eyeIndex;
        if (g_eyeHistValid[eye]) {
            memcpy(rv + 0x80, g_eyeHist[eye], 64);
            memcpy(rv + 0x240, g_eyeHist[eye] + 64, 64);
        }
        o_RVBuild(rv, kind, g_viewCopy, p4);
        const float* proj = (const float*)rv;  // +0x0 projection: P00 = 1/tanX, P11 = 1/tanY
        if (proj[0] != 0.f && proj[5] != 0.f) { g_eyeTan[eye][0] = 1.f / proj[0]; g_eyeTan[eye][1] = 1.f / proj[5]; }
        memcpy(g_eyeHist[eye], rv + 0x80, 64);
        memcpy(g_eyeHist[eye] + 64, rv + 0x240, 64);
        g_eyeHistValid[eye] = true;
        if (g_rvProbe > 0) {  // diagnostic: find RenderView fields that carry the previous build's current matrices
            int pe = g_rvProbeEye ^= 1;
            memcpy(g_rvSnap[pe], rv, 0x590);
            if (pe == 1) {
                char line[1024];
                int len = sprintf_s(line, "rvprobe: eye1 blocks equal to eye0 blocks:");
                for (int a = 0; a + 64 <= 0x590; a += 16)
                    for (int b = 0; b + 64 <= 0x590; b += 16)
                        if (memcmp(g_rvSnap[1] + a, g_rvSnap[0] + b, 64) == 0 &&
                            memcmp(g_rvSnap[1] + a, g_rvSnap[1] + b, 64) != 0 && len < 980)
                            len += sprintf_s(line + len, sizeof(line) - len, " eye1+%x=eye0+%x", a, b);
                Log("%s", line);
                --g_rvProbe;
            }
        }
        return;
    }
    if (g_seam != kRenderView || kind != 0 || rv != At<uint8_t>(kMainRenderViewVa) ||
        view != *At<uint8_t*>(kViewAPtrVa)) {
        o_RVBuild(rv, kind, view, p4);
        return;
    }
    memcpy(g_viewCopy, view, sizeof(g_viewCopy));
    if (g_applied == 0) Log("mutate[renderview]: first apply rv=%p view=%p yaw=%.3f rad offx=%.4f", rv, view, g_yawRad, g_offX);
    Apply(g_viewCopy + 0x150, g_viewCopy + 0x50, g_viewCopy + 0x10, g_viewCopy + 0x90, g_viewCopy + 0x1d0);
    o_RVBuild(rv, kind, g_viewCopy, p4);
    ++g_applied;
    if (--g_framesLeft <= 0) {
        g_seam = kNone;
        Log("mutate[renderview]: window over after %llu frames", (unsigned long long)g_applied);
    }
}

// D-159: the sound listener = the head in F1 / F3 (owner: "the 8D audio doesn't work as I wanted - I want it based on
// where I look: standing still, a voice is always in my left ear even when I look at it"). D-157 put the head into view
// B, but the listener never reads view B. STATIC (decompile + GoW.exe disassembly): sub_1405f4c00, once per frame, takes
// the listener object DAT_1422c31a8 (+0x00 an offset matrix, +0x40 the result, +0x80 the camera object - the same one
// views A / B are fed from), puts offset x camera world at +0x40 and calls Wwise SetListenerPosition sub_140bc1820
// (AkListenerPosition*, listener index) for listeners 0 and 1 with front = normalize(+0x60 row) with z negated, top =
// (0, 1, 0), position = (-x, y, z) (Wwise space = the game's mirrored in x). In F1 / F3 that camera is the game's
// third-person camera, which does not turn with the headset - sounds kept their side whatever way the player looked.
// The hook hands Wwise the head camera instead (eye midpoint: view A's camera-to-world after the head write, the same
// convention - it looks down -Z, row 2 = back), built by the game's own formula. Checked in the binary: the function
// reads only rcx (the struct) and edx (the index), nothing from the caller's stack, so a C hook is exact. The game's
// own distance checks (listener +0x70) are left alone. gowvr.ini audio_head_f1 / audio_head_f3 0 = the game's listener.
constexpr HookSite kSetListenerPos{"SetListenerPosition", 0x140bc1820, 6,
    {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x70, 0x8b, 0xfa, 0x0f, 0x29, 0x74, 0x24}};
using PFN_SetListenerPos = int(__fastcall*)(const float* pos, uint32_t index);
PFN_SetListenerPos o_setListenerPos = nullptr;
int g_audioHeadF1 = -1, g_audioHeadF3 = -1;
// the head as an AkListenerPosition (front, top, position in Wwise space), by the game's own formula
bool HeadListenerPose(float p[9]) {
    Mat4 hw;
    float tx, ty;
    if (!HeadCameraForUi(&hw, &tx, &ty)) return false;
    // the back row's horizontal part gives the facing; looking almost straight down / up it vanishes - then the up
    // row's horizontal part does (looking down the up row points forward, looking up it points back)
    float bx = hw.m[2][0], bz = hw.m[2][2];
    if (bx * bx + bz * bz < 0.04f) {
        const float s = hw.m[2][1] > 0.f ? -1.f : 1.f;
        bx = s * hw.m[1][0];
        bz = s * hw.m[1][2];
    }
    const float l = sqrtf(bx * bx + bz * bz);
    const float* t = hw.m[3];
    if (!(l > 1e-3f) || !std::isfinite(l) || !std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) return false;
    const float q[9] = {bx / l, 0.f, -bz / l, 0.f, 1.f, 0.f, -t[0], t[1], t[2]};
    memcpy(p, q, sizeof(q));
    return true;
}
int __fastcall hk_SetListenerPos(const float* pos, uint32_t index) {
    const int fpm = fp::FpMode();
    if (pos && ((fpm == 1 && g_audioHeadF1 > 0) || (fpm == 2 && g_audioHeadF3 > 0))) {
        float p[9];
        if (HeadListenerPose(p)) {
            static uint64_t n = 0;
            if (index == 0 && (++n == 1 || n % 5400 == 0)) {
                Log("audio: listener = head (F%d, %llu frames) - head front %.2f %.2f pos %.2f %.2f %.2f | game's camera front "
                    "%.2f %.2f pos %.2f %.2f %.2f",
                    fpm == 1 ? 1 : 3, (unsigned long long)n, p[0], p[2], p[6], p[7], p[8], pos[0], pos[2], pos[6], pos[7], pos[8]);
            }
            return o_setListenerPos(p, index);
        }
    }
    return o_setListenerPos(pos, index);
}

// D-162 (owner: "8D audio - Kratos' grunts and attacks should not be 8D in F1"): with the listener on the head (D-159)
// Kratos' own sounds came from his emitters - his head / mouth joint, his weapons - a few cm to a metre around the
// headset, so they panned as the player turned the head. STATIC (decompile + disassembly): the game's per-emitter update
// sub_1405efac0(emitter) (one argument: only rcx is read, nothing from the caller's stack) takes the emitter's world (its
// own link, or the joint emitter+0x2a2 of its owner emitter+0x28 - the object at +0x238 is the skeleton) and queues Wwise
// SetPosition messages (type 0xd, 0x30 B: game object id emitter+0x228, AkSoundPosition front / top / position); the
// message processor (sub_140c14fd0 case 0xd) applies them in order. Wwise's own SetPosition is sub_140bc1db0(id,
// AkSoundPosition*) (reads rcx / rdx only). After the game's update, an emitter owned by Kratos (or his axe / shield /
// Blades while not thrown) gets one more SetPosition - 0.25 m straight in front of the head listener, facing as the head -
// so it is heard centred, in both ears. F1 only; gowvr.ini audio_self_centre 0 = off.
constexpr HookSite kEmitterUpdate{"SoundEmitterUpdate", 0x1405efac0, 5,
    {0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x49, 0x8d, 0xab, 0x38, 0xfc, 0xff, 0xff, 0x48, 0x81, 0xec, 0xb8}};
using PFN_EmitterUpdate = void(__fastcall*)(uint8_t* e);
PFN_EmitterUpdate o_emitterUpdate = nullptr;
using PFN_SoundSetPosition = int(__fastcall*)(uint64_t gameObj, const float* pos);
constexpr uint64_t kSoundSetPositionVa = 0x140bc1db0;
int g_selfCentre = -1;
void __fastcall hk_EmitterUpdate(uint8_t* e) {
    o_emitterUpdate(e);
    if (!e || g_selfCentre <= 0 || g_audioHeadF1 <= 0 || fp::FpMode() != 1) return;
    // the game's own guard (sub_1405efac0 returns at once without it) and Wwise's message queue (DAT_14242e268)
    if (!*At<uint8_t>(0x1422e36ea) || !*At<void*>(0x14242e268)) return;
    uint8_t* owner = *(uint8_t**)(e + 0x28);
    if (!owner || !fp::IsKratosSoundOwner(owner)) return;
    const uint64_t id = *(const uint64_t*)(e + 0x228);
    if (id == 0 || id == ~0ull) return;
    float p[9];
    if (!HeadListenerPose(p)) return;
    static const float ahead = config::GetFloat("audio_self_ahead", 0.25f);
    p[6] += p[0] * ahead;  // Wwise space: front and position alike
    p[7] += p[1] * ahead;
    p[8] += p[2] * ahead;
    ((PFN_SoundSetPosition)At<uint8_t>(kSoundSetPositionVa))(id, p);
    static std::atomic<uint64_t> n{0};
    const uint64_t k = ++n;
    if (k == 1 || k % 20000 == 0)
        Log("audio: Kratos' own sound centred in front of the head (game object %llx, %llu updates)", (unsigned long long)id,
            (unsigned long long)k);
}

}  // namespace

bool InstallMutationHooks() {
    static bool done = false;
    if (done) return true;
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    bool a = DetourInstall(kBuildRenderViewTable, (void*)&hk_Build, (void**)&o_Build);
    bool b = DetourInstall(kViewUpdate, (void*)&hk_ViewUpdate, (void**)&o_ViewUpdate);
    bool c = DetourInstall(kRenderViewBuild, (void*)&hk_RVBuild, (void**)&o_RVBuild);
    done = a && b && c;
    if (g_audioHeadF1 < 0) {  // D-159 (not needed for the view: a failure only leaves the game's listener)
        g_audioHeadF1 = config::GetFloat("audio_head_f1", 1.f) > 0.5f ? 1 : 0;
        g_audioHeadF3 = config::GetFloat("audio_head_f3", 1.f) > 0.5f ? 1 : 0;
        if (g_audioHeadF1 || g_audioHeadF3)
            Log("audio: listener hook %s (the head is the listener in%s%s)",
                DetourInstall(kSetListenerPos, (void*)&hk_SetListenerPos, (void**)&o_setListenerPos) ? "installed" : "FAILED",
                g_audioHeadF1 ? " F1" : "", g_audioHeadF3 ? " F3" : "");
        g_selfCentre = config::GetFloat("audio_self_centre", 1.f) > 0.5f ? 1 : 0;  // D-162
        if (g_selfCentre && g_audioHeadF1)
            Log("audio: Kratos' own sounds centred in F1 - emitter hook %s",
                DetourInstall(kEmitterUpdate, (void*)&hk_EmitterUpdate, (void**)&o_emitterUpdate) ? "installed" : "FAILED");
    }
    return done;
}

void MutateArm(const char* seam, const char* kind, float amount, int frames) {
    g_seam = kNone;
    g_yawRad = !strcmp(kind, "yaw") ? amount * 3.14159265f / 180.f : 0.f;
    g_offX = !strcmp(kind, "offx") ? amount : 0.f;
    g_applied = 0;
    g_framesLeft = frames;
    g_seam = !strcmp(seam, "table") ? kTable : !strcmp(seam, "view") ? kView : !strcmp(seam, "rv") ? kRenderView : kNone;
    Log("mutate: armed seam=%s kind=%s amount=%.4f frames=%d", seam, kind, amount, frames);
}

void MutateOff() {
    g_seam = kNone;
    Log("mutate: off");
}

void Freeze(bool on) {
    if (on) g_haveFrozen = false;
    g_freeze = on;
    Log("mutate: freeze %s", on ? "on" : "off");
}

const uint8_t* FrozenCamWorld() { return g_haveFrozen ? g_frozen + (0x150 - 0x10) : nullptr; }

void RvProbe(int n) {
    g_rvProbeEye = 1;
    g_rvProbe = n;
}

void SetEyeIndex(int eye) { g_eyeIndex = eye & 1; }

void ResetEyeHistory() { g_eyeHistValid[0] = g_eyeHistValid[1] = false; }
void SetEyePoses(bool on, const Mat4* eye0, const Mat4* eye1, bool level) {
    static bool logged = false;
    if (on) {
        g_eyeDelta[0] = *eye0;
        g_eyeDelta[1] = *eye1;
        g_eyeLevel = level;
        if (!logged && *At<uint8_t*>(kViewAPtrVa)) {
            const float* c = (const float*)(*At<uint8_t*>(kViewAPtrVa) + 0x150);
            Log("headcam: view A camera rows X(%.3f %.3f %.3f) Y(%.3f %.3f %.3f) Z(%.3f %.3f %.3f) T(%.2f %.2f %.2f)",
                c[0], c[1], c[2], c[4], c[5], c[6], c[8], c[9], c[10], c[12], c[13], c[14]);
            logged = true;
        }
    }
    g_eyePoseMode = on;
    g_eyeOn = on;
}

void CamLog(int n) { g_camLog = n; }

namespace {
void HeadCameraIntoViewA(uint8_t* view) {
    if (!HeadcamPrepare()) return;
    Mat4 center;
    float tx, ty;
    bool level;
    if (!HeadcamCenter(&center, &tx, &ty, &level)) return;
    static int logged = 0;
    if (g_camLog > 0 || logged < 4) {
        const float* o = (const float*)(view + 0x150);
        Log("headcam: viewA update #%d T(%.3f %.3f %.3f) Z(%.3f %.3f %.3f) sameAsLastWritten=%d", logged, o[12], o[13],
            o[14], o[8], o[9], o[10], memcmp(view, g_viewALast, sizeof(g_viewALast)) == 0);
        ++logged;
    }
    memcpy(g_viewAOrig, view, sizeof(g_viewAOrig));
    g_viewAOrigValid = true;
    Mat4 c = Load(view + 0x150);
    // first person (F1/F3): Kratos' eyes as the base; otherwise the game camera (levelled)
    // gameplay first person: the view yaw is detached from the game camera (see CamSync)
    {
        const bool syncActive = fp::FpMode() == 1 && !fp::InBoat() && !fp::InCinematic();
        const float camYaw = atan2f(c.m[2][0], c.m[2][2]);
        const float hmdYaw = atan2f(center.m[2][0], center.m[2][2]);
        // pitch, looking up positive: the camera looks down -Z, so its "back" row points down when it looks up
        const float camPitch = asinf(fmaxf(-1.f, fminf(1.f, -c.m[2][1])));
        const float hmdPitch = asinf(fmaxf(-1.f, fminf(1.f, -center.m[2][1])));
        const vr::InputSnapshot in = vr::XrInput();
        const bool aiming = in.focused && in.hand[0].active && in.hand[0].trigger > 0.3f;  // LT (vrmap: aim)
        const float ry = CamSyncYaw(camYaw, camPitch, hmdYaw, hmdPitch, syncActive, aiming);
        if (syncActive) {
            Mat4 cr = c;  // only the yaw of the "back" row is read in gameplay first person
            cr.m[2][0] = sinf(ry); cr.m[2][2] = cosf(ry);
            g_fpBaseValid = fp::FirstPersonBase(cr, &g_fpBase);
        } else {
            g_fpBaseValid = fp::FirstPersonBase(c, &g_fpBase);
        }
        const bool gameplayFp = syncActive && g_fpBaseValid && fp::FpMode() == 1;
        const float viewYaw = WrapPi(ry + hmdYaw);
        if (gameplayFp) {
            // D-037: the view sits fp_forward in front of the eyes along where the HEAD faces (the body follows the head),
            // not along the stick-only view yaw: with the head turned 90 deg the camera and the hand targets were 0.2 m
            // off to the side of Kratos' eyes (harness: the shoulder under the view, the arms reaching unevenly)
            const float f = fp::ViewForward();
            const float bx = sinf(viewYaw), bz = cosf(viewYaw);
            g_fpBase.m[3][0] += f * (g_fpBase.m[2][0] - bx);
            g_fpBase.m[3][2] += f * (g_fpBase.m[2][2] - bz);
        }
        // walking goes where the player looks and the body turns with the head, continuously. No spine
        // lean: leaning only moves the view (owner: "when you lean you actually lean with your body").
        input::SetLocomotionYaw(gameplayFp ? WrapPi(viewYaw - camYaw) : 0.f);
        fp::SetGameCamYaw(camYaw);  // D-064: the game-camera frame of the stick (movement during held attacks)
        // D-097: riding, climbing, chains, dragging, doors / chests, Atreus behind the shield ...: the body keeps the
        // game's facing instead of turning with the head (owner: "the body shouldn't move")
        fp::BodyFollowYaw(viewYaw, gameplayFp && !fp::ContextBodyLocked(), aiming);
        static const bool viewB = config::GetFloat("aim_viewb", 1.f) > 0.5f;
        // gameplay first person, and third person with VR on (owner: "fix the headtracking throwing axe mechanic in 3rd
        // person"); never in cutscenes or the boat
        const bool thirdPerson = fp::FpMode() == 0 && !fp::InCinematic() && !fp::InBoat();
        // D-106 (owner: "the Atreus bow aiming: the cursor icon is head tracked but it doesn't shoot where I look"):
        // the hunt section with Atreus' bow runs with IsInCinematicMode set the whole time (LIVE log: cinematic=1 from
        // the section's start, no "headset aiming on" while the owner aimed), so aiming there got the game camera.
        // Aiming in a cinematic section (any view mode; not the boat) also takes the head. In a real cutscene the aim
        // trigger does nothing in the game; view B is the head only while it is held.
        const bool cineAim = fp::InCinematic() && !fp::InBoat();
        const bool aimHead = viewB && aiming && (gameplayFp || thirdPerson || cineAim);
        if (aimHead != g_aimHead.load())
            Log("aim: headset aiming %s%s", aimHead ? "on (view B = head)" : "off",
                aimHead && cineAim ? " - cinematic section" : "");
        g_aimHead = aimHead;
    }
    HeadcamLockPosition(false);  // (pinning the view to the head was the wrong model: the body follows the head)
    Mat4 base = g_fpBaseValid ? g_fpBase : (level ? Level(c) : c);
    // D-160: gameplay F1 - the head (and below the hands) stop at walls between Kratos' eye point and them
    const bool wallsOn = g_fpBaseValid && fp::FpMode() == 1 && !fp::InBoat() && !fp::InCinematic();
    if (wallsOn) fp::WallsHead(center, base);
    else fp::WallsIdle();
    Mat4 lv = Mul(base, RigidInverse(c));
    Mat4 d = Mul(center, lv);
    g_deltaOverride = &d;
    Apply(view + 0x150, view + 0x50, view + 0x10, view + 0x90, view + 0x1d0);
    g_deltaOverride = nullptr;
    g_headWorld = Load(view + 0x150);  // head camera (eye midpoint) for view B while aiming
    if (tx > 0.f && ty > 0.f) OverrideProjection(view, tx, ty);
    memcpy(g_viewALast, view, sizeof(g_viewALast));
    {   // D-147
        const float* P = (const float*)(view + 0xd0);
        if (P[0] > 0.f && P[5] > 0.f) {
            AcquireSRWLockExclusive(&g_uiCamLock);
            g_uiCam = g_headWorld;
            g_uiCamTan[0] = 1.f / P[0];
            g_uiCamTan[1] = 1.f / P[5];
            g_uiCamAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_uiCamLock);
        }
    }
    // motion-controlled arms: gameplay first person only (cutscenes stay authored)
    Mat4 hands[2];
    bool valid[2] = {false, false};
    const bool armsOn = g_fpBaseValid && fp::FpMode() == 1 && !fp::InBoat();  // boat: both hands authored
    if (armsOn) HeadcamHandsWorld(g_fpBase, hands, valid);
    if (armsOn && wallsOn) fp::WallsHands(hands, valid, g_fpBase);  // D-160
    fp::ArmsUpdate(armsOn, hands, valid);
}
}  // namespace

void ArmsDiag(bool on) { g_armsDiag = on; Log("arms: diagnostic third-person drive %s", on ? "on" : "off"); }

void CamSync(bool on) { CamSyncEnable(on); }

void AimAdjustPitch(float deltaDeg) {
    if (g_aimPitchDeg < -999.f) g_aimPitchDeg = config::GetFloat("aim_pitch_deg", 3.f);
    g_aimPitchDeg += deltaDeg;
    config::SetFloat("aim_pitch_deg", g_aimPitchDeg);
    Log("aim: pitch bias %.1f deg", g_aimPitchDeg);
}

// D-083 diagnostic "loadwatch on|off" (owner: "loadings are not in the 2D screen"): per presented frame, candidate
// loading-screen signals, logged when they change - the 8 timers sub_1406f3bf0 tests (0x1422ae730..74c), the pause
// menu byte, the byte PassVideo tests with it (0x1422e9cf9), and whether view A updated / the hero posed / kicks ran
std::atomic<bool> g_loadWatch{false};
void LoadWatch(bool on) { g_loadWatch = on; Log("loadwatch %s", on ? "on" : "off"); }
void LoadWatchTick() {
    if (!g_loadWatch.load() || !g_base) return;
    static uint64_t frame = 0, lastVa = 0, lastHp = 0, lastK = 0;
    static char last[160] = "";
    static int logs = 0;
    ++frame;
    const float* t = At<float>(0x1422ae730);
    char tm[80];
    sprintf_s(tm, "%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
    int on = 0;
    for (int i = 0; i < 8; ++i) on |= (t[i] > 0.f) << i;
    const uint64_t va = g_viewAUpdates.load(), hp = fp::HeroWorldPassCount(), k = R1Kicks();
    char s[160];
    sprintf_s(s, "timers %02x pause %d b9cf9 %d viewA %d hero %d kicks %d", on, (int)*At<uint8_t>(0x1422e795b),
              (int)*At<uint8_t>(0x1422e9cf9), va != lastVa, hp != lastHp, (int)(k - lastK));
    lastVa = va; lastHp = hp; lastK = k;
    if (strcmp(s, last) && logs < 600) {
        ++logs;
        Log("loadwatch: frame %llu %s (timer values %s)", (unsigned long long)frame, s, tm);
        strcpy_s(last, s);
    }
}

bool Screen2DFrame() { return g_screen2D.load(); }
void Screen2DMode(int mode) { g_screen2DMode = mode; Log("ui: 2D screen mode %d (0 off, 1 pause menu, 2 always)", mode); }
void UiScale(float s) { g_uiScale = s; config::SetFloat("ui_scale", s); Log("ui: scale %.2f (from the next view update)", s); }
void UiScaleFp(float s) { g_uiScaleFp = s; config::SetFloat("ui_scale_fp", s); Log("ui: first-person scale %.2f", s); }
// D-104: the sharp HUD's paste scale (render thread) - ui_scale; in the first-person modes ui_scale_fp (D-102). While
// aiming only the reticle region grows (sharpui.cpp, ui_reticle_scale), so D-103's whole-UI aim scale is not used here.
// D-107: the reticle while head-aiming (and 400 ms after: the game fades it out) - render thread
bool AimReticleDisplay(float* srcU, float* srcV, float* srcR, float* dstU, float* dstV) {
    const ULONGLONG t = g_aimRetTick.load();
    if (!t || GetTickCount64() - t > 400) return false;
    const AimRet r = g_aimRet;
    *srcU = r.srcU; *srcV = r.srcV; *srcR = r.srcR; *dstU = r.dstU; *dstV = r.dstV;
    return true;
}

bool HeadCameraForUi(Mat4* camToWorld, float* tanX, float* tanY) {
    AcquireSRWLockShared(&g_uiCamLock);
    const bool ok = g_uiCamAt && GetTickCount64() - g_uiCamAt < 250 && g_uiCamTan[0] > 0.f;
    if (ok) { *camToWorld = g_uiCam; *tanX = g_uiCamTan[0]; *tanY = g_uiCamTan[1]; }
    ReleaseSRWLockShared(&g_uiCamLock);
    return ok;
}

bool PauseMenuOpen() { return g_lastPause; }

void UiDiagText(char* out, size_t n) {
    sprintf_s(out, n,
              "UI view updates %llu: scaled %llu, by the sharp HUD %llu, skipped (head camera off / 2D screen) %llu, "
              "skipped (not orthographic) %llu | 2D screen %s (mode %d; pause menu %d, video %d, loading %d, faded %d "
              "alpha %.2f; head camera %d)",
              (unsigned long long)g_uiUpdates, (unsigned long long)g_uiScaled, (unsigned long long)g_uiSharp,
              (unsigned long long)g_uiSkipCam, (unsigned long long)g_uiSkipPersp, g_screen2D.load() ? "ON" : "off",
              g_screen2DMode, g_lastPause ? 1 : 0, g_lastVideo ? 1 : 0, g_lastLoading ? 1 : 0, g_lastFaded ? 1 : 0,
              g_lastFadeA, g_headCamOn ? 1 : 0);
}

// The game lays its 2D UI out in square pixels of its frame, but each eye frame holds the eye frustum (tanX x tanY),
// so in the headset the UI is stretched vertically by (tanY / tanX) * (width / height) - LIVE logs: PSVR2 at 3840x2130
// 1.26, Quest 3 at 2560x1600 1.66 (a tester: "the apple of Idun pickup stretched the whole screen"); with the square
// picture (frame/rendersize) PSVR2 0.70 (owner: "the HUD is a bit squashed on the y axis"), Quest 3 1.04. The UI's
// vertical scale is divided by it. gowvr.ini ui_aspect_fix=0 turns it off.
float UiVerticalFix(float frameAspect) {
    static const bool on = config::GetFloat("ui_aspect_fix", 1.f) > 0.5f;
    if (!on || frameAspect <= 0.f || g_fovTan[0] <= 0.f || g_fovTan[1] <= 0.f) return 1.f;
    const float stretch = g_fovTan[1] / g_fovTan[0] * frameAspect;
    return fminf(fmaxf(1.f / stretch, 0.3f), 3.f);
}

float UiScaleCurrent() {
    const float s = g_uiScale < 0.f ? config::GetFloat("ui_scale", 0.35f) : g_uiScale;
    const float sFp = g_uiScaleFp < 0.f ? config::GetFloat("ui_scale_fp", 0.5f) : g_uiScaleFp;
    const int fpm = fp::FpMode();
    return (fpm == 1 || fpm == 2) && sFp > 0.05f ? sFp : s;
}

void SetHeadCamera(bool on) {
    g_headCamOn = on;
    Log("headcam: head camera in view A %s", on ? "on" : "off");
}

void EyeFrameBegin() {
    if (g_headCamOn) return;  // the original was saved at View::Update
    uint8_t* viewA = *At<uint8_t*>(kViewAPtrVa);
    if (!viewA) { g_viewAOrigValid = false; return; }
    // a fresh View::Update since our last write -> new original; otherwise keep the previous one
    if (!g_viewAOrigValid || memcmp(viewA, g_viewALast, sizeof(g_viewALast)) != 0) memcpy(g_viewAOrig, viewA, sizeof(g_viewAOrig));
    g_viewAOrigValid = true;
}

void EyeViewAWrite(bool on) {
    g_viewAWrite = on;
    Log("headcam: view A write-back %s", on ? "on" : "off");
}

void SetEyeFov(float tanX, float tanY) {
    if (tanX != g_fovTan[0] || tanY != g_fovTan[1])
        Log("headcam: eye frustum tanX=%.3f tanY=%.3f (h %.1f deg, v %.1f deg)", tanX, tanY,
            2.f * atanf(tanX) * 57.2958f, 2.f * atanf(tanY) * 57.2958f);
    g_fovTan[0] = tanX;
    g_fovTan[1] = tanY;
}

bool EyeRenderedTan(int eye, float* tanX, float* tanY) {
    *tanX = g_eyeTan[eye & 1][0];
    *tanY = g_eyeTan[eye & 1][1];
    return *tanX > 0.f && *tanY > 0.f;
}

void SetEyeCamera(bool on, float offX, float yawDeg) {
    g_eyePoseMode = false;
    g_eyeOffX = offX;
    g_eyeYaw = yawDeg * 3.14159265f / 180.f;
    g_eyeOn = on;
}

}  // namespace gowvr::stereo
