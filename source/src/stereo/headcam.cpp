// Head-tracked eye cameras (M1: 6DoF head tracking on top of the game camera, gamepad play).
// OpenXR LOCAL-space eye poses -> local-space rigid deltas relative to a re-center origin (yaw + position of the head
// at re-center), scaled to game units, applied per eye by the RenderView parameter override (mutate.cpp).
// Axis match (STATIC, CAMERA_FINDINGS.md): engine camera space is right-handed, +Y up, looks down -Z (P23 = -1),
// identical to OpenXR view space, so orientation and position carry over without an axis swap.
#include "headcam.h"
#include "mutate.h"
#include "../core/log.h"
#include "../core/config.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace gowvr::stereo {
namespace {

std::atomic<bool> g_recenter{true};
SRWLOCK g_recenterLock = SRWLOCK_INIT;
vr::EyeViews g_recenterAt;  // D-085: the latest located head pose when the recenter was requested
float g_scale = 1.f;
bool g_level = true;
float g_testYawRad = 0.f;
float g_testMotion = 0.f;
bool g_hmdFov = true;
float g_marginRad = 5.f * 0.0174533f;  // frustum margin beyond the headset FoV: no black edges when the compositor
                                       // reprojects a late frame to a fast head turn (owner report)
int g_predictFrames = 1;               // locate the head for the display time of the frame being prepared
bool g_prepValid = false;
Mat4 g_prepD[2], g_prepCenter;
float g_prepTx = 0, g_prepTy = 0;
vr::EyeViews g_prepUsed;  // false: keep the game's own frustum (diagnostic `hmdfov off`)  // diagnostic: emulated head motion amplitude (yaw rad; translation = 0.15 m per unit)  // diagnostic synthetic head yaw (headcfg 3rd arg, degrees)
Mat4 g_originInv;
std::atomic<int> g_recenterCount{0};
std::atomic<bool> g_lockPos{false};
float g_headT[3] = {0, 0, 0};  // head centre (origin space, unscaled) of the last prepare
uint64_t g_lastSerial = 0;
uint64_t g_prepared = 0, g_stale = 0;
// D-110 (code audit M3): the controller grips located for the same predicted display time as the eyes of this prepare
vr::Pose g_prepGrip[2];
bool g_prepGripValid[2] = {false, false};

Mat4 FromPose(const vr::Pose& p) {
    const float x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
    // column-vector rotation R; row-vector matrix rows = columns of R (images of the local axes)
    const float R[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    Mat4 m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] = R[j][i];
    m.m[3][0] = p.p[0];
    m.m[3][1] = p.p[1];
    m.m[3][2] = p.p[2];
    m.m[3][3] = 1.f;
    return m;
}

// D-275 (owner, a Pimax Crystal Light player: "when the main menu shows up everything looks doubled - on my PC it looks
// normal ... I can only play with one eye closed"; "the game looks normal on the settings menu (2D screen) and with the
// SteamVR overlay on" - both are the flat screen): Pimax headsets report each eye's view turned outward (canted
// displays). The eyes were rendered with their own turned orientations, but the game builds part of each frame once,
// from view A - the mod's head camera, which took eye 0's orientation - so the other eye's lighting / shadows / screen-
// space passes disagree with it (the double vision games show on canted headsets without "parallel projections").
// Now, when the two eye views are turned more than 0.5 deg apart, both are rendered along the HEAD's orientation (the
// two averaged) with each eye's frustum widened to cover its own turned one, and submitted that way - the runtime maps
// them onto the turned displays (what Pimax's "Parallel Projections" does). Headsets with parallel views (PS VR2,
// Quest, Index) are untouched. gowvr.ini parallel_eyes: 1 auto (default), 2 always, 0 off
static void QMul(const float a[4], const float b[4], float o[4]) {  // (x, y, z, w)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void QRot(const float q[4], const float v[3], float o[3]) {
    const float t[3] = {2.f * (q[1] * v[2] - q[2] * v[1]), 2.f * (q[2] * v[0] - q[0] * v[2]), 2.f * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
void ParallelViews(vr::EyeViews& v) {
    static const int mode = (int)config::GetFloat("parallel_eyes", 1.f);
    float a[4], b[4];
    memcpy(a, v.pose[0].q, sizeof(a));
    memcpy(b, v.pose[1].q, sizeof(b));
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.f) { for (float& x : b) x = -x; dot = -dot; }
    const float apart = 2.f * acosf(fminf(dot, 1.f));
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("headcam: the eye views are turned %.1f deg apart%s (D-275, parallel_eyes %d)", apart * 57.2958f,
            apart > 0.0087f ? " (canted displays) - rendered parallel, along the head" : "", mode);
    }
    if (mode <= 0 || (mode == 1 && apart <= 0.0087f)) return;
    float h[4] = {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
    const float n = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + h[3] * h[3]);
    if (n < 1e-6f) return;
    for (float& x : h) x /= n;
    const float hc[4] = {-h[0], -h[1], -h[2], h[3]};
    for (int e = 0; e < 2; ++e) {
        float r[4];  // eye frame -> head frame
        QMul(hc, v.pose[e].q, r);
        const vr::Fov f = v.fov[e];
        const float xs[3] = {tanf(f.left), 0.f, tanf(f.right)}, ys[3] = {tanf(f.down), 0.f, tanf(f.up)};
        float lo = 1e9f, hi = -1e9f, dn = 1e9f, up = -1e9f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                if (i == 1 && j == 1) continue;  // the edges' corners and middles
                const float d[3] = {xs[i], ys[j], -1.f};
                float o[3];
                QRot(r, d, o);
                if (o[2] > -1e-3f) continue;  // (never for real frustums)
                const float tx = o[0] / -o[2], ty = o[1] / -o[2];
                lo = fminf(lo, tx); hi = fmaxf(hi, tx); dn = fminf(dn, ty); up = fmaxf(up, ty);
            }
        if (!(lo < hi) || !(dn < up)) continue;
        v.fov[e] = {atanf(lo), atanf(hi), atanf(up), atanf(dn)};
        memcpy(v.pose[e].q, h, sizeof(h));
    }
}

void Recenter(const vr::EyeViews& v) {
    Mat4 e0 = FromPose(v.pose[0]);
    // head yaw from the back (+Z image) axis projected on the horizontal plane; head position = eye midpoint
    float bx = e0.m[2][0], bz = e0.m[2][2], n = sqrtf(bx * bx + bz * bz);
    if (n < 1e-4f) { bx = 0; bz = 1; } else { bx /= n; bz /= n; }
    Mat4 o{};
    o.m[0][0] = bz;  o.m[0][2] = -bx;
    o.m[1][1] = 1.f;
    o.m[2][0] = bx;  o.m[2][2] = bz;
    for (int j = 0; j < 3; ++j) o.m[3][j] = 0.5f * (v.pose[0].p[j] + v.pose[1].p[j]);
    o.m[3][3] = 1.f;
    g_originInv = RigidInverse(o);
    ++g_recenterCount;
    Log("headcam: re-centered at head (%.3f %.3f %.3f) yaw back=(%.3f, %.3f)", o.m[3][0], o.m[3][1], o.m[3][2], bx, bz);
}

}  // namespace

// diagnostic "fplook <pitch down deg> <yaw left deg> [back m] [up m]": turns / moves the head view in head space
// (harness screenshots of the waist and the axe without a headset); "fplook 0 0" turns it off. Not saved.
std::atomic<float> g_lookPitch{0.f}, g_lookYaw{0.f}, g_lookBack{0.f}, g_lookUp{0.f};
void FpLook(float pitchDeg, float yawDeg, float back, float up) {
    g_lookPitch = pitchDeg * 0.0174533f;
    g_lookYaw = yawDeg * 0.0174533f;
    g_lookBack = back;
    g_lookUp = up;
    Log("fplook: pitch down %.0f deg, yaw left %.0f deg, back %.2f m, up %.2f m", pitchDeg, yawDeg, back, up);
}

bool HeadcamPrepare() {
    vr::EyeViews v = vr::XrLatestViews();
    if (!v.valid) return false;
    if (g_predictFrames > 0 && v.period > 0) {  // the frame prepared now is displayed after the one being begun
        vr::EyeViews p;
        if (vr::XrLocateAt(v.displayTime + v.period * g_predictFrames, &p)) {
            p.serial = v.serial;
            p.period = v.period;
            v = p;
        }
    }
    ParallelViews(v);  // D-275 (canted displays: both eyes along the head)
    if (v.serial == g_lastSerial) ++g_stale;
    g_lastSerial = v.serial;
    // D-110: the hands for the same moment as the eyes (they were one frame older: the F1 hands trailed the head)
    if (!vr::XrLocateHandsAt(v.displayTime, g_prepGrip, g_prepGripValid)) g_prepGripValid[0] = g_prepGripValid[1] = false;
    if (g_recenter.exchange(false)) {
        // D-085: the head pose at the request (taken by HeadcamRecenter) - while the 2D screen shows this prepare does not
        // run, and when the stereo view returns the head may have turned since the recenter
        vr::EyeViews at = v;
        AcquireSRWLockExclusive(&g_recenterLock);
        if (g_recenterAt.valid) at = g_recenterAt;
        g_recenterAt = vr::EyeViews{};
        ReleaseSRWLockExclusive(&g_recenterLock);
        ParallelViews(at);  // D-275: the head's yaw, not eye 0's turned one
        Recenter(at);
    }
    Mat4 d[2];
    for (int e = 0; e < 2; ++e) {
        d[e] = Mul(FromPose(v.pose[e]), g_originInv);
        if (g_testYawRad != 0.f) d[e] = Mul(d[e], LocalYaw(g_testYawRad));
        if (g_testMotion != 0.f) {  // pre-multiply in head space: turn and sway like a seated player
            const float t = (float)g_prepared * 0.045f;
            Mat4 m = LocalYaw(g_testMotion * sinf(t));
            m.m[3][0] = 0.15f * g_testMotion * sinf(1.37f * t);
            m.m[3][1] = 0.05f * g_testMotion * sinf(0.71f * t);
            d[e] = Mul(d[e], m);
        }
        for (int j = 0; j < 3; ++j) d[e].m[3][j] *= g_scale;
        if (g_lookPitch.load() != 0.f || g_lookYaw.load() != 0.f || g_lookBack.load() != 0.f || g_lookUp.load() != 0.f) {
            const float p = -g_lookPitch.load(), y = g_lookYaw.load(), cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y);
            Mat4 rx{}, ry{};
            rx.m[0][0] = 1; rx.m[1][1] = cp; rx.m[1][2] = sp; rx.m[2][1] = -sp; rx.m[2][2] = cp; rx.m[3][3] = 1;
            ry.m[0][0] = cy; ry.m[0][2] = -sy; ry.m[1][1] = 1; ry.m[2][0] = sy; ry.m[2][2] = cy; ry.m[3][3] = 1;
            Mat4 L = Mul(rx, ry);
            // orbit: "back" metres behind the turned view (looking at the head), then "up"
            for (int j = 0; j < 3; ++j) L.m[3][j] = g_lookBack.load() * L.m[2][j];
            L.m[3][1] += g_lookUp.load();
            d[e] = Mul(L, d[e]);  // head space
        }
    }
    for (int j = 0; j < 3; ++j) g_headT[j] = 0.5f * (d[0].m[3][j] + d[1].m[3][j]) / (g_scale != 0.f ? g_scale : 1.f);
    if (g_lockPos.load())  // owner: in F1/F3 the head stays pinned to Kratos' head; leaning must not leave the skull
        for (int e = 0; e < 2; ++e)
            for (int j = 0; j < 3; ++j) d[e].m[3][j] -= g_headT[j] * g_scale;
    // Symmetric frustum covering both eyes' (asymmetric) FoV: all screen-space passes keep their symmetric assumptions;
    // the submitted layer reports exactly this FoV, the compositor maps it onto the display.
    float tx = 0.f, ty = 0.f;
    for (int e = 0; e < 2; ++e) {
        tx = fmaxf(tx, fmaxf(tanf(-v.fov[e].left), tanf(v.fov[e].right)));
        ty = fmaxf(ty, fmaxf(tanf(v.fov[e].up), tanf(-v.fov[e].down)));
    }
    if (!g_hmdFov) {  // the game's own projection; report what it renders
        if (!EyeRenderedTan(0, &tx, &ty)) { tx = 0.75f; ty = 0.42f; }
    } else {
        tx = tanf(fminf(atanf(tx) + g_marginRad, 1.45f));
        ty = tanf(fminf(atanf(ty) + g_marginRad, 1.45f));
    }
    g_prepD[0] = d[0];
    g_prepD[1] = d[1];
    g_prepCenter = d[0];  // eyes share orientation; head = midpoint
    for (int j = 0; j < 3; ++j) g_prepCenter.m[3][j] = 0.5f * (d[0].m[3][j] + d[1].m[3][j]);
    g_prepTx = g_hmdFov ? tx : 0.f;
    g_prepTy = g_hmdFov ? ty : 0.f;
    g_prepUsed = v;
    for (int e = 0; e < 2; ++e) {
        g_prepUsed.ownFov[e] = v.fov[e];  // the eye's own frustum: xr.cpp crops the image to it and submits it
        g_prepUsed.fov[e] = {-atanf(tx), atanf(tx), atanf(ty), -atanf(ty)};  // exactly what this frame renders
    }
    g_prepUsed.hasOwnFov = g_hmdFov;
    g_prepValid = true;
    uint64_t k = ++g_prepared;
    if (k <= 2 || k % 900 == 0)
        Log("headcam: frame %llu serial=%llu stale=%llu eye0 delta T(%.3f %.3f %.3f) eye1 T(%.3f %.3f %.3f)",
            (unsigned long long)k, (unsigned long long)v.serial, (unsigned long long)g_stale, d[0].m[3][0],
            d[0].m[3][1], d[0].m[3][2], d[1].m[3][0], d[1].m[3][1], d[1].m[3][2]);
    return true;
}

bool HeadcamArm(vr::EyeViews* used) {
    if (!g_prepValid) return false;
    SetEyeFov(g_prepTx, g_prepTy);
    SetEyePoses(true, &g_prepD[0], &g_prepD[1], g_level);
    if (used) *used = g_prepUsed;
    return true;
}

bool HeadcamCenter(Mat4* center, float* tx, float* ty, bool* level) {
    if (!g_prepValid) return false;
    *center = g_prepCenter;
    *tx = g_prepTx;
    *ty = g_prepTy;
    *level = g_level;
    return true;
}

void HeadcamOverscan(float marginDeg, int predictFrames) {
    g_marginRad = marginDeg * 0.0174533f;
    g_predictFrames = predictFrames;
    Log("headcam: frustum margin %.1f deg, prediction %d frame(s)", marginDeg, predictFrames);
}

bool g_handSim = false;
float g_handSimRoll = 0.f;  // degrees about the pointing axis (diagnostic)  // diagnostic: simulated hands held in front of the chest (grip frame: X right, Y up, -Z fwd)

void HeadcamHandSimRoll(float deg) { g_handSimRoll = deg; }
// diagnostic "handsimmove <amp>": the simulated hands move in circles (amp x 0.15 m) and roll +-amp x 90 deg, like a
// player waving the controllers - exercises the forearm twist and the finger chains without a headset
float g_handSimMove = 0.f;
// "handsimlock on": the simulated hands stay where they are in the room while fplook turns the head (only the head
// turns - the body follows it, the hands do not)
bool g_handSimLock = false;
void HeadcamHandSimLock(bool on) { g_handSimLock = on; Log("headcam: simulated hands %s the head turn", on ? "ignore" : "follow"); }
float g_handSimFwd = 0.38f;  // "handsimfwd <m>": how far in front of the eyes the simulated hands are (reach tests)
void HeadcamHandSimFwd(float m) { g_handSimFwd = m; Log("headcam: simulated hands %.2f m in front", m); }
void HeadcamHandSimMove(float amp) { g_handSimMove = amp; Log("headcam: simulated hand motion %.2f", amp); }

void HeadcamHandSim(bool on) { g_handSim = on; Log("headcam: simulated hands %s", on ? "on" : "off"); }

// D-239 (owner: "someone said I should add hand smoothing - the hands jitter a little"): the controller pose (tracking
// space, before it is put on Kratos) through a One Euro filter per hand - small jitter at rest is smoothed, fast motion
// passes nearly unfiltered (the cutoff rises with speed), so swings keep their timing. Only the drawn hands: the swing
// and gesture detection read the raw controllers. VR settings panel "Hand smoothing"; gowvr.ini hand_smoothing
// (1 = on), hand_smooth_cutoff (Hz at rest, 3) / hand_smooth_beta (cutoff added per m/s, 6)
static void SmoothHand(int h, Mat4& e) {
    static bool have[2] = {false, false};
    static Mat4 f[2];
    static LARGE_INTEGER last[2] = {};
    static LARGE_INTEGER qf{};
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    if (config::GetFloat("hand_smoothing", 1.f) < 0.5f) { have[h] = false; return; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = last[h].QuadPart ? (float)(now.QuadPart - last[h].QuadPart) / (float)qf.QuadPart : 0.f;
    last[h] = now;
    if (!have[h] || dt <= 0.f || dt > 0.1f) { f[h] = e; have[h] = true; return; }
    static const float minCut = config::GetFloat("hand_smooth_cutoff", 3.f), beta = config::GetFloat("hand_smooth_beta", 6.f);
    const float dx = e.m[3][0] - f[h].m[3][0], dy = e.m[3][1] - f[h].m[3][1], dz = e.m[3][2] - f[h].m[3][2];
    // speed: position and the rotation's change (rows' difference ~ angle) at a 0.15 m lever
    float rot = 0.f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rot += (e.m[r][c] - f[h].m[r][c]) * (e.m[r][c] - f[h].m[r][c]);
    const float speed = (sqrtf(dx * dx + dy * dy + dz * dz) + 0.15f * sqrtf(rot * 0.5f)) / dt;
    const float cutoff = minCut + beta * speed;
    const float tau = 1.f / (6.2831853f * cutoff);
    const float a = 1.f / (1.f + tau / dt);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) f[h].m[r][c] += (e.m[r][c] - f[h].m[r][c]) * a;
    OrthonormalizeKeepScale(f[h]);
    e = f[h];
}

void HeadcamHandsWorld(const Mat4& base, Mat4 out[2], bool valid[2]) {
    if (g_handSim) {
        for (int h = 0; h < 2; ++h) {
            Mat4 e{};
            e.m[0][0] = e.m[1][1] = e.m[2][2] = e.m[3][3] = 1.f;  // identity grip orientation = holding forward, knuckles up
            const float tm = (float)(GetTickCount64() % 1000000ull) * 0.001f;
            float rr = (g_handSimRoll * 0.0174533f + g_handSimMove * 1.5708f * sinf(tm * 2.1f + h)) * (h ? 1.f : -1.f);
            e.m[0][0] = cosf(rr); e.m[0][1] = sinf(rr);
            e.m[1][0] = -sinf(rr); e.m[1][1] = cosf(rr);
            e.m[3][0] = h ? 0.22f : -0.22f;
            e.m[3][1] = -0.28f;
            e.m[3][2] = -g_handSimFwd;
            if (h == 1 && g_handSimFwd < 0.f) e.m[3][1] = 0.05f;  // negative "handsimfwd": right hand up behind the head
            e.m[3][0] +=g_handSimMove * 0.15f * cosf(tm * 3.f + h * 1.3f);
            e.m[3][1] += g_handSimMove * 0.15f * sinf(tm * 3.f + h * 1.3f);
            if (const float y = g_lookYaw.load(); y != 0.f && !g_handSimLock) {  // fplook yaw: player turned with the view
                Mat4 ry{};
                ry.m[0][0] = cosf(y); ry.m[0][2] = -sinf(y); ry.m[1][1] = 1; ry.m[2][0] = sinf(y); ry.m[2][2] = cosf(y);
                ry.m[3][3] = 1;
                e = Mul(e, ry);
            }
            out[h] = Mul(e, base);
            valid[h] = true;
        }
        return;
    }
    vr::InputSnapshot s = vr::XrInput();
    for (int h = 0; h < 2; ++h) {
        valid[h] = s.focused && s.hand[h].active && (g_prepGripValid[h] || s.hand[h].gripValid);
        if (!valid[h]) continue;
        // D-110: the grip predicted for the eyes' display time when located, else this frame's (one frame older)
        Mat4 e = Mul(FromPose(g_prepGripValid[h] ? g_prepGrip[h] : s.hand[h].grip), g_originInv);
        SmoothHand(h, e);  // D-239
        if (g_lockPos.load())
            for (int j = 0; j < 3; ++j) e.m[3][j] -= g_headT[j];  // hands relative to the (pinned) head
        for (int j = 0; j < 3; ++j) e.m[3][j] *= g_scale;
        out[h] = Mul(e, base);
    }
}

void HeadcamRecenter() {
    const vr::EyeViews now = vr::XrLatestViews();  // runtime recenter: called once its new space is in effect
    AcquireSRWLockExclusive(&g_recenterLock);
    g_recenterAt = now;
    ReleaseSRWLockExclusive(&g_recenterLock);
    g_recenter = true;
    vr::XrScreenRecenter();  // D-085: the 2D screen too (the head camera does not run while it shows)
}
int HeadcamRecenterCount() { return g_recenterCount.load(); }
void HeadcamLockPosition(bool on) { g_lockPos = on; }

void HeadcamHmdFov(bool on) {
    g_hmdFov = on;
    Log("headcam: headset frustum %s", on ? "on" : "off (game frustum)");
}

void HeadcamConfig(float scale, bool level, float testYawDeg, float motion) {
    g_testMotion = motion;
    g_scale = scale;
    g_level = level;
    g_testYawRad = testYawDeg * 0.0174533f;
    Log("headcam: scale=%.3f level=%d testYaw=%.1f", scale, level, testYawDeg);
}

}  // namespace gowvr::stereo
