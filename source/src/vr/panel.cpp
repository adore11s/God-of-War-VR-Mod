// VR settings panel (panel.h). Drawn with GDI into a 32-bit DIB (redrawn only when a value or the hovered button
// changes), shown by xr.cpp as its own quad layer to the right of the flat screen, turned toward the head.
#include "panel.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../input/vpad.h"
#include "../frame/rendersize.h"
#include "../stereo/mutate.h"
#include "../fp/firstperson.h"
#include "../fp/arms.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gowvr::vr {
namespace {

// panel pixels (D-253: 11 rows of kRow + the hints; D-283 12, D-284 13, D-286 15, D-288 16, D-289 14; D-292 13 rows and two
// hint lines: 122 + 13 x 120 + 116)
constexpr int kW = 1000, kH = 1798;
// D-253: the first row's label, the row pitch (D-288: 16 rows - 120: label 40 + 4 + button 64 + 12, the panel as tall
// as with 15 rows of 128)
constexpr int kTop = 122, kRow = 120, kLabelH = 40, kButtonY = 44, kButtonH = 64;
constexpr float kPanelW = 1.2f;             // metres (D-253: 11 rows; D-289: 14 rows of 120 px, as tall as 13 of 128 were)
constexpr float kGap = 0.15f;               // between the screen and the panel
constexpr int kCursorPx = 64;

const float kSpeeds[] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
const int kSnaps[] = {15, 30, 45, 60, 90};
const int kSizes[] = {1440, 1728, 2048, 2304, 2560, 2816};

enum Action {
    kNone, kTurnSmooth, kTurnSnap, kStepDown, kStepUp, kPadXbox, kPadPs, kWallsOn, kWallsOff, kSizeDown, kSizeUp,
    kFinisherFp, kFinisherTp, kAfrOn, kAfrOff, kMotionOn, kMotionOff, kF1On, kF1Off, kF3On, kF3Off,
    kBarsHead, kBarsHand, kAimHold, kAimPress, kHandDown, kHandUp
};
struct Button { RECT r; Action a; const wchar_t* text; bool selected; };

// GDI surface (render thread)
HDC g_dc = nullptr;
HBITMAP g_bmp = nullptr;
uint32_t* g_bits = nullptr;
HFONT g_fTitle = nullptr, g_fLabel = nullptr, g_fButton = nullptr, g_fHint = nullptr;
uint32_t g_out[kW * kH];
uint32_t g_cursor[kCursorPx * kCursorPx];
bool g_cursorMade = false;

// D-286: 40 (it was 24 = 12 rows: D-284's 13th row, VR on / off, had lost its buttons)
Button g_buttons[40];
int g_nButtons = 0;
Action g_hover = kNone;
bool g_needDraw = true;
char g_lastState[128] = "";

// pointing (render thread writes, pad thread reads)
std::atomic<bool> g_capture[2] = {false, false};
bool g_armed[2] = {true, true};
uint64_t g_clicks = 0;

template <class T, size_t N> int Nearest(const T (&list)[N], float v) {
    int best = 0;
    for (int i = 1; i < (int)N; ++i)
        if (fabsf((float)list[i] - v) < fabsf((float)list[best] - v)) best = i;
    return best;
}

bool Snap() { return config::GetFloat("turn_snap", 0.f) > 0.5f; }
bool Walls() { return config::GetFloat("walls_f1", 1.f) > 0.5f; }
// owner 2026-10-18: finishers / executions in first person (as before) or third person (the game's own camera;
// fp/firstperson.cpp FinisherScene)
bool FinisherTp() { return config::GetFloat("finisher_third_person", 1.f) > 0.5f; }
bool Afr() { return config::GetFloat("afr_rendering", 0.f) > 0.5f; }         // D-241
bool Motion() { return config::GetFloat("motion_controls", 1.f) > 0.5f; }    // D-244 (fp::MotionControls reads it)
// (D-289, owner: "remove the distant model detail and hide aiming icon options from VR settings" - D-283's "Distant model
// detail" and D-286's "Axe aiming icon" rows are gone; gowvr.ini lod_scale / aim_icon_hide still work)
// D-284 (xr.cpp, mutate.cpp read it live); D-291 (owner: "make the left hand UI default for everyone"): on by default
bool BarsHand() { return config::GetFloat("hud_bars_hand", 1.f) > 0.5f; }
bool AimPress() { return config::GetFloat("aim_toggle", 0.f) > 0.5f; }          // D-286 (input/vrmap.cpp)

void Apply(Action a) {
    switch (a) {
        case kTurnSmooth: config::SetFloat("turn_snap", 0.f); break;
        case kTurnSnap: config::SetFloat("turn_snap", 1.f); break;
        case kStepDown:
        case kStepUp: {
            const int d = a == kStepUp ? 1 : -1;
            if (Snap()) {
                int i = Nearest(kSnaps, config::GetFloat("snap_turn_degrees", 45.f)) + d;
                i = i < 0 ? 0 : i >= (int)(sizeof(kSnaps) / sizeof(kSnaps[0])) ? (int)(sizeof(kSnaps) / sizeof(kSnaps[0])) - 1 : i;
                config::SetFloat("snap_turn_degrees", (float)kSnaps[i]);
            } else {
                int i = Nearest(kSpeeds, config::GetFloat("turn_speed", 1.f)) + d;
                i = i < 0 ? 0 : i >= (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0])) ? (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0])) - 1 : i;
                config::SetFloat("turn_speed", kSpeeds[i]);
            }
            break;
        }
        case kPadXbox: input::SetPadMode(0); break;
        case kPadPs: input::SetPadMode(1); break;
        case kWallsOn: config::SetFloat("walls_f1", 1.f); break;
        case kWallsOff: config::SetFloat("walls_f1", 0.f); break;
        case kFinisherFp: config::SetFloat("finisher_third_person", 0.f); break;
        case kFinisherTp: config::SetFloat("finisher_third_person", 1.f); break;
        case kAfrOn: config::SetFloat("afr_rendering", 1.f); break;
        case kAfrOff: config::SetFloat("afr_rendering", 0.f); break;
        case kMotionOn: config::SetFloat("motion_controls", 1.f); break;
        case kMotionOff: config::SetFloat("motion_controls", 0.f); break;
        case kBarsHead: config::SetFloat("hud_bars_hand", 0.f); break;  // D-284
        case kBarsHand: config::SetFloat("hud_bars_hand", 1.f); break;
        case kAimHold:  // D-286 config::SetFloat("aim_toggle", 0.f); break;
        case kAimPress: config::SetFloat("aim_toggle", 1.f); break;
        // D-288 (a Meta Quest 2 player: "when I hold the axe with my hand pointing forward its top points backward, as if
        // turned 90 degrees - maybe hand rotation offsets in the VR settings"): F5 / F6's wrist pitch for this headset's
        // controllers (gowvr.ini wristpitch_<psvr2 | quest | other>), 5 degrees a click
        case kHandDown: fp::ArmsAdjustPitch(-5.f); break;
        case kHandUp: fp::ArmsAdjustPitch(5.f); break;
        case kF1On: fp::SetGameplayFP(true); break;    // D-253 (the same switch as O / B at the head, D-252)
        case kF1Off: fp::SetGameplayFP(false); break;
        case kF3On: fp::SetCutsceneFP(true); break;
        case kF3Off: fp::SetCutsceneFP(false); break;
        case kSizeDown:
        case kSizeUp: {
            // "Auto" (the size the runtime asks for, rendersize.cpp) is the step above the largest size
            const int count = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
            if (const int autoPx = frame::AutoRenderSize()) {
                if (a == kSizeUp) break;
                int i = count - 1;
                while (i > 0 && kSizes[i] >= autoPx) --i;  // the largest size below Auto
                config::SetFloat("render_size_auto", 0.f);
                config::SetFloat("render_size", (float)kSizes[i]);
                break;
            }
            int i = Nearest(kSizes, config::GetFloat("render_size", 2048.f)) + (a == kSizeUp ? 1 : -1);
            if (i >= count && frame::RuntimeEyeSizeKnown()) { config::SetFloat("render_size_auto", 1.f); break; }
            i = i < 0 ? 0 : i >= count ? count - 1 : i;
            config::SetFloat("render_size", (float)kSizes[i]);
            break;
        }
        default: return;
    }
    g_needDraw = true;
    Log("panel: %d -> turning %s, speed %.0f%%, snap %.0f deg, pad %s, walls %s, picture %.0f%s, finishers %s, "
        "rendering %s, motion controls %s", (int)a,
        Snap() ? "snap" : "smooth", config::GetFloat("turn_speed", 1.f) * 100.f,
        config::GetFloat("snap_turn_degrees", 45.f), input::PadMode() ? "PlayStation" : "Xbox", Walls() ? "on" : "off",
        config::GetFloat("render_size", 2048.f), frame::AutoRenderSize() ? " (auto)" : "",
        FinisherTp() ? "third person" : "first person", Afr() ? "AFR" : "native", Motion() ? "on" : "off");
}

bool MakeSurface() {
    if (g_dc) return g_bits != nullptr;
    g_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bmp || !bits) { Log("panel: DIB section failed"); return false; }
    g_bits = (uint32_t*)bits;
    SelectObject(g_dc, g_bmp);
    auto font = [](int px, int weight) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    g_fTitle = font(54, FW_BOLD);
    g_fLabel = font(36, FW_SEMIBOLD);
    g_fButton = font(38, FW_SEMIBOLD);
    g_fHint = font(28, FW_NORMAL);
    SetBkMode(g_dc, TRANSPARENT);
    return true;
}

void Fill(const RECT& r, COLORREF c, int round = 0) {
    HBRUSH b = CreateSolidBrush(c);
    if (round > 0) {
        HGDIOBJ ob = SelectObject(g_dc, b), op = SelectObject(g_dc, GetStockObject(NULL_PEN));
        RoundRect(g_dc, r.left, r.top, r.right + 1, r.bottom + 1, round, round);
        SelectObject(g_dc, ob);
        SelectObject(g_dc, op);
    } else {
        FillRect(g_dc, &r, b);
    }
    DeleteObject(b);
}

void Text(const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    DrawTextW(g_dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

void AddButton(int x, int y, int w, int h, Action a, const wchar_t* text, bool selected) {
    if (g_nButtons < (int)(sizeof(g_buttons) / sizeof(g_buttons[0]))) g_buttons[g_nButtons++] = {{x, y, x + w, y + h}, a, text, selected};
}

// D-253 (owner: "add First Person (walking) and First Person (cutscenes) on / off, and reorder the VR settings - it's all
// over the place; rendering methods next to each other, no labels needed"): one row table - first person (walking,
// cutscenes, finishers, motion controls, walls, turning), the controller buttons, the picture (size, rendering), VR.
// A row is a label, then two choices or "- value +"
struct Row {
    const wchar_t* label;
    Action a, b;            // the two choices, or the stepper's down / up
    const wchar_t *ta, *tb; // their texts (nullptr: a stepper)
    bool selA;              // the first choice is the selected one (two choices only)
    int value;              // stepper: 0 = turning step, 1 = picture size, 2 = hand angle
};
int Rows(Row* r) {
    const bool snap = Snap();
    int n = 0;
    r[n++] = {L"First person (walking)", kF1On, kF1Off, L"On", L"Off", fp::GameplayFP(), 0};
    r[n++] = {L"First person (cutscenes)", kF3On, kF3Off, L"On", L"Off", fp::CutsceneFP(), 0};
    r[n++] = {L"Finishers perspective", kFinisherFp, kFinisherTp, L"First person", L"Third person", !FinisherTp(), 0};
    r[n++] = {L"Motion controls (first person)", kMotionOn, kMotionOff, L"On", L"Off", Motion(), 0};
    r[n++] = {L"Hand angle (tilt the hands and the axe)", kHandDown, kHandUp, nullptr, nullptr, false, 2};  // D-288
    r[n++] = {L"Health bars (first person)", kBarsHead, kBarsHand, L"Head", L"Left hand", !BarsHand(), 0};  // D-284
    r[n++] = {L"Aiming mode (left trigger)", kAimHold, kAimPress, L"Hold", L"Press", !AimPress(), 0};
    r[n++] = {L"Wall collision (first person)", kWallsOn, kWallsOff, L"On", L"Off", Walls(), 0};
    r[n++] = {L"Turning (first person)", kTurnSmooth, kTurnSnap, L"Smooth", L"Snap", !snap, 0};
    r[n++] = {snap ? L"Snap angle" : L"Turning speed", kStepDown, kStepUp, nullptr, nullptr, false, 0};
    r[n++] = {L"Controller buttons", kPadXbox, kPadPs, L"Xbox", L"PlayStation", input::PadMode() == 0, 0};
    r[n++] = {L"VR picture size (per eye)", kSizeDown, kSizeUp, nullptr, nullptr, false, 1};
    r[n++] = {L"Rendering method", kAfrOff, kAfrOn, L"Native", L"AFR", !Afr(), 0};
    // (D-292, owner: "remove On and Off for VR in the VR settings panel" - F9 still switches VR)
    return n;
}
constexpr int kMaxRows = 13;

void Layout(wchar_t* stepText, wchar_t* sizeText, wchar_t* handText) {
    g_nButtons = 0;
    Row rows[20];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const int y = kTop + i * kRow + kButtonY;
        if (r.ta) {
            AddButton(40, y, 440, kButtonH, r.a, r.ta, r.selA);
            AddButton(520, y, 440, kButtonH, r.b, r.tb, !r.selA);
        } else {
            AddButton(40, y, 140, kButtonH, r.a, L"\x2212", false);
            AddButton(820, y, 140, kButtonH, r.b, L"+", false);
        }
    }
    swprintf_s(handText, 32, L"%.0f\x00b0", fp::ArmsWristPitch());
    if (Snap()) swprintf_s(stepText, 32, L"%.0f\x00b0", config::GetFloat("snap_turn_degrees", 45.f));
    else swprintf_s(stepText, 32, L"%.0f %%", config::GetFloat("turn_speed", 1.f) * 100.f);
    if (const int autoPx = frame::AutoRenderSize()) {
        swprintf_s(sizeText, 32, L"Auto (%d x %d)", autoPx, autoPx);
    } else {
        const int px = (int)config::GetFloat("render_size", 2048.f);
        swprintf_s(sizeText, 32, L"%d x %d", px, px);
    }
}

void Draw() {
    if (!MakeSurface()) return;
    wchar_t stepText[32], sizeText[32], handText[32];
    Layout(stepText, sizeText, handText);
    const COLORREF bg = RGB(40, 40, 42), title = RGB(178, 28, 28), button = RGB(66, 66, 70), hover = RGB(96, 96, 102),
                   selected = RGB(178, 28, 28), selectedHover = RGB(210, 50, 50), white = RGB(245, 245, 245),
                   label = RGB(200, 200, 205), hint = RGB(150, 150, 155);
    Fill({0, 0, kW, kH}, bg);
    Fill({0, 0, kW, 100}, title);
    Text(L"VR SETTINGS", {40, 0, kW - 40, 100}, g_fTitle, white, DT_LEFT | DT_VCENTER);
    Row rows[20];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const int y = kTop + i * kRow;
        Text(rows[i].label, {40, y, kW - 40, y + kLabelH}, g_fLabel, label, DT_LEFT | DT_VCENTER);
        if (!rows[i].ta) {  // the stepper's value box
            const int by = y + kButtonY;
            Fill({200, by, 800, by + kButtonH}, RGB(30, 30, 32), 14);
            Text(rows[i].value == 1 ? sizeText : rows[i].value == 2 ? handText : stepText, {200, by, 800, by + kButtonH},
                 g_fButton, white, DT_CENTER | DT_VCENTER);
        }
    }
    for (int i = 0; i < g_nButtons; ++i) {
        const Button& b = g_buttons[i];
        const bool hv = b.a == g_hover;
        Fill(b.r, b.selected ? (hv ? selectedHover : selected) : (hv ? hover : button), 14);
        Text(b.text, b.r, g_fButton, white, DT_CENTER | DT_VCENTER);
    }
    Text(L"Point a controller at an option and pull its trigger.", {40, kH - 106, kW - 40, kH - 66}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    // D-253 (owner: "instead of the picture-size line: for a quick switch between first person cutscenes or walking
    // press O / Y near your head") - the right upper button: PS VR2 O, Meta / Index B (D-252)
    Text(L"Quick first-person switch: press \x25CB (PS VR2) / B near your head.", {40, kH - 62, kW - 40, kH - 22}, g_fHint,
         hint, DT_LEFT | DT_VCENTER);
    GdiFlush();
    for (int i = 0; i < kW * kH; ++i) g_out[i] = g_bits[i] | 0xff000000u;  // GDI leaves alpha 0: opaque
}

void MakeCursor() {
    if (g_cursorMade) return;
    g_cursorMade = true;
    const float c = (kCursorPx - 1) * 0.5f;
    for (int y = 0; y < kCursorPx; ++y)
        for (int x = 0; x < kCursorPx; ++x) {
            const float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c)) / c;  // 0 centre .. 1 edge
            float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
            if (d < 0.55f) { a = 1.f; r = g = b = 1.f; }                       // white dot
            else if (d < 0.85f) { a = 1.f; r = 0.75f; g = 0.12f; b = 0.12f; }  // red ring
            else if (d < 1.f) { a = (1.f - d) / 0.15f; r = 0.75f * a; g = b = 0.12f * a; }  // soft edge (premultiplied)
            auto u8 = [](float v) { return (uint32_t)lrintf(fminf(fmaxf(v, 0.f), 1.f) * 255.f); };
            g_cursor[y * kCursorPx + x] = u8(a) << 24 | u8(r) << 16 | u8(g) << 8 | u8(b);
        }
}

void Rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.f * (y * v[2] - z * v[1]), ty = 2.f * (z * v[0] - x * v[2]), tz = 2.f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// D-263 (owner: "my hand was jittering even in the VR menu panel"): the pointer ray came straight from the controller
// (the drawn hands' D-239 smoothing never covered it), and 2 m away a tremble of the hand is a jumping dot. It now goes
// through the same One Euro filter (hand_smoothing, hand_smooth_cutoff 3 Hz, hand_smooth_beta 6): still at rest, a fast
// sweep passes. The direction's change counts at the panel's ~2 m
void SmoothAim(int hand, float p[3], float d[3]) {
    static bool have[2] = {false, false};
    static float sp[2][3], sd[2][3];
    static LARGE_INTEGER last[2] = {}, qf{};
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    if (config::GetFloat("hand_smoothing", 1.f) < 0.5f) { have[hand] = false; return; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = last[hand].QuadPart ? (float)(now.QuadPart - last[hand].QuadPart) / (float)qf.QuadPart : 0.f;
    last[hand] = now;
    if (!have[hand] || dt <= 0.f || dt > 0.1f) {
        memcpy(sp[hand], p, sizeof(sp[hand]));
        memcpy(sd[hand], d, sizeof(sd[hand]));
        have[hand] = true;
        return;
    }
    static const float minCut = config::GetFloat("hand_smooth_cutoff", 3.f), beta = config::GetFloat("hand_smooth_beta", 6.f);
    float dp2 = 0.f, dd2 = 0.f;
    for (int i = 0; i < 3; ++i) {
        dp2 += (p[i] - sp[hand][i]) * (p[i] - sp[hand][i]);
        dd2 += (d[i] - sd[hand][i]) * (d[i] - sd[hand][i]);
    }
    const float speed = (sqrtf(dp2) + 2.f * sqrtf(dd2)) / dt;
    const float cutoff = minCut + beta * speed;
    const float a = 1.f / (1.f + 1.f / (6.2831853f * cutoff * dt));
    for (int i = 0; i < 3; ++i) {
        sp[hand][i] += (p[i] - sp[hand][i]) * a;
        sd[hand][i] += (d[i] - sd[hand][i]) * a;
    }
    const float n = sqrtf(sd[hand][0] * sd[hand][0] + sd[hand][1] * sd[hand][1] + sd[hand][2] * sd[hand][2]);
    if (n > 1e-6f)
        for (int i = 0; i < 3; ++i) sd[hand][i] /= n;
    memcpy(p, sp[hand], sizeof(sp[hand]));
    memcpy(d, sd[hand], sizeof(sd[hand]));
}

}  // namespace

bool PanelFrame(const InputSnapshot& in, const Pose& screen, float screenW, const float head[3], PanelLayer* out) {
    if (!stereo::PauseMenuOpen()) { PanelHidden(); return false; }
    // to the right of the screen, the same height, turned toward the head (yaw only)
    float right[3];
    const float x1[3] = {1.f, 0.f, 0.f};
    Rotate(screen.q, x1, right);
    const float off = 0.5f * screenW + kGap + 0.5f * kPanelW;
    const float c[3] = {screen.p[0] + right[0] * off, screen.p[1], screen.p[2] + right[2] * off};
    const float tx = head[0] - c[0], tz = head[2] - c[2];  // panel -> head: the quad's +Z
    const float yaw = atan2f(tx, tz);
    Pose p{};
    p.q[0] = 0.f; p.q[1] = sinf(0.5f * yaw); p.q[2] = 0.f; p.q[3] = cosf(0.5f * yaw);
    memcpy(p.p, c, sizeof(c));
    const float h = kPanelW * (float)kH / (float)kW;
    // pointing: each hand's aim ray against the panel's plane
    const float nrm[3] = {sinf(yaw), 0.f, cosf(yaw)};       // +Z
    const float rgt[3] = {cosf(yaw), 0.f, -sinf(yaw)};      // +X
    Action hover = kNone;
    bool cursor = false;
    float hitP[3] = {};
    for (int hand = 1; hand >= 0; --hand) {  // the right hand first
        const HandState& hs = in.hand[hand];
        bool hit = false;
        if (hs.active && hs.aimValid) {
            float d[3], ap[3] = {hs.aim.p[0], hs.aim.p[1], hs.aim.p[2]};
            const float fwd[3] = {0.f, 0.f, -1.f};
            Rotate(hs.aim.q, fwd, d);
            SmoothAim(hand, ap, d);  // D-263
            const float dn = d[0] * nrm[0] + d[1] * nrm[1] + d[2] * nrm[2];
            if (dn < -1e-3f) {  // pointing at the panel's front
                const float t = ((c[0] - ap[0]) * nrm[0] + (c[1] - ap[1]) * nrm[1] + (c[2] - ap[2]) * nrm[2]) / dn;
                if (t > 0.f && t < 20.f) {
                    const float q[3] = {ap[0] + d[0] * t, ap[1] + d[1] * t, ap[2] + d[2] * t};
                    const float lx = (q[0] - c[0]) * rgt[0] + (q[2] - c[2]) * rgt[2], ly = q[1] - c[1];
                    const float u = lx / kPanelW + 0.5f, v = 0.5f - ly / h;
                    if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f) {
                        hit = true;
                        if (!cursor) {
                            cursor = true;
                            memcpy(hitP, q, sizeof(q));
                            const int px = (int)(u * kW), py = (int)(v * kH);
                            for (int i = 0; i < g_nButtons; ++i) {
                                const RECT& r = g_buttons[i].r;
                                if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) hover = g_buttons[i].a;
                            }
                            // the trigger: a pull (past 0.75, again after below 0.35) clicks the hovered button
                            if (hs.trigger < 0.35f) g_armed[hand] = true;
                            if (g_armed[hand] && hs.trigger > 0.75f) {
                                g_armed[hand] = false;
                                if (hover != kNone) { ++g_clicks; Apply(hover); }
                            }
                        }
                    }
                }
            }
        }
        g_capture[hand] = hit;
        if (!hit && hs.trigger < 0.35f) g_armed[hand] = true;
    }
    // redraw when a value or the hovered button changed (values: also changed with F10 / F11 or a gowvr.ini edit)
    char state[128];
    sprintf_s(state, "%d %.2f %.0f %d %d %.0f %d %d %d %d %d %d %d %d %.0f", Snap() ? 1 : 0, config::GetFloat("turn_speed", 1.f),
              config::GetFloat("snap_turn_degrees", 45.f), input::PadMode(), Walls() ? 1 : 0,
              config::GetFloat("render_size", 2048.f), frame::AutoRenderSize(), FinisherTp() ? 1 : 0, Afr() ? 1 : 0,
              Motion() ? 1 : 0, fp::GameplayFP() ? 1 : 0, fp::CutsceneFP() ? 1 : 0, BarsHand() ? 1 : 0, AimPress() ? 1 : 0,
              fp::ArmsWristPitch());
    if (hover != g_hover || strcmp(state, g_lastState) != 0 || g_needDraw) {
        g_hover = hover;
        strcpy_s(g_lastState, state);
        g_needDraw = false;
        Draw();
        out->dirty = true;
        static int shown = 0;
        if (!shown++) Log("panel: VR settings panel shown (%dx%d px, %.2f m wide, right of the screen)", kW, kH, kPanelW);
    } else {
        out->dirty = false;
    }
    out->pose = p;
    out->w = kPanelW;
    out->h = h;
    out->pixels = g_out;
    out->pw = kW;
    out->ph = kH;
    out->cursor = cursor;
    if (cursor) {
        out->cursorPose = p;
        for (int j = 0; j < 3; ++j) out->cursorPose.p[j] = hitP[j] + nrm[j] * 0.01f;  // just in front of the panel
        out->cursorSize = 0.035f;
    }
    return true;
}

void PanelHidden() {
    g_capture[0] = g_capture[1] = false;
    g_hover = kNone;
    g_needDraw = true;
}

const uint32_t* PanelCursorPixels() {
    MakeCursor();
    return g_cursor;
}

bool PanelCapturesTrigger(int hand) { return g_capture[hand & 1].load(std::memory_order_relaxed); }

}  // namespace gowvr::vr
