// VR settings panel (panel.h). Drawn with GDI into a 32-bit DIB (redrawn only when a value or the hovered button
// changes), shown by xr.cpp as its own quad layer to the right of the flat screen, turned toward the head.
#include "panel.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../input/vpad.h"
#include "../diag/cmd.h"
#include "../stereo/mutate.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gowvr::vr {
namespace {

constexpr int kW = 1000, kH = 1150;         // panel pixels
constexpr float kPanelW = 1.3f;             // metres
constexpr float kGap = 0.15f;               // between the screen and the panel
constexpr int kCursorPx = 64;

const float kSpeeds[] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
const int kSnaps[] = {15, 30, 45, 60, 90};
const int kSizes[] = {1440, 1728, 2048, 2304, 2560, 2816};

enum Action {
    kNone, kTurnSmooth, kTurnSnap, kStepDown, kStepUp, kPadXbox, kPadPs, kWallsOn, kWallsOff, kSizeDown, kSizeUp,
    kVrOn, kVrOff
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

Button g_buttons[16];
int g_nButtons = 0;
Action g_hover = kNone;
bool g_needDraw = true;
char g_lastState[96] = "";

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
        case kVrOn: break;  // (VR is on while the panel shows)
        case kVrOff: diag::RequestVr(false); break;  // as F9 - F9 turns it back on
        case kSizeDown:
        case kSizeUp: {
            int i = Nearest(kSizes, config::GetFloat("render_size", 2048.f)) + (a == kSizeUp ? 1 : -1);
            i = i < 0 ? 0 : i >= (int)(sizeof(kSizes) / sizeof(kSizes[0])) ? (int)(sizeof(kSizes) / sizeof(kSizes[0])) - 1 : i;
            config::SetFloat("render_size", (float)kSizes[i]);
            break;
        }
        default: return;
    }
    g_needDraw = true;
    Log("panel: %d -> turning %s, speed %.0f%%, snap %.0f deg, pad %s, walls %s, picture %.0f", (int)a,
        Snap() ? "snap" : "smooth", config::GetFloat("turn_speed", 1.f) * 100.f,
        config::GetFloat("snap_turn_degrees", 45.f), input::PadMode() ? "PlayStation" : "Xbox", Walls() ? "on" : "off",
        config::GetFloat("render_size", 2048.f));
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
    if (g_nButtons < 16) g_buttons[g_nButtons++] = {{x, y, x + w, y + h}, a, text, selected};
}

// The rows: label, then two choices or a - value + stepper
void Layout(wchar_t* stepText, wchar_t* sizeText) {
    g_nButtons = 0;
    const bool snap = Snap();
    int y = 130 + 52;
    AddButton(40, y, 440, 72, kTurnSmooth, L"Smooth", !snap);
    AddButton(520, y, 440, 72, kTurnSnap, L"Snap", snap);
    y += 140;
    AddButton(40, y, 140, 72, kStepDown, L"\x2212", false);
    AddButton(820, y, 140, 72, kStepUp, L"+", false);
    if (snap) swprintf_s(stepText, 32, L"%.0f\x00b0", config::GetFloat("snap_turn_degrees", 45.f));
    else swprintf_s(stepText, 32, L"%.0f %%", config::GetFloat("turn_speed", 1.f) * 100.f);
    y += 140;
    const bool ps = input::PadMode() != 0;
    AddButton(40, y, 440, 72, kPadXbox, L"Xbox", !ps);
    AddButton(520, y, 440, 72, kPadPs, L"PlayStation", ps);
    y += 140;
    const bool walls = Walls();
    AddButton(40, y, 440, 72, kWallsOn, L"On", walls);
    AddButton(520, y, 440, 72, kWallsOff, L"Off", !walls);
    y += 140;
    AddButton(40, y, 140, 72, kSizeDown, L"\x2212", false);
    AddButton(820, y, 140, 72, kSizeUp, L"+", false);
    y += 140;
    AddButton(40, y, 440, 72, kVrOn, L"On", true);  // the panel only shows in VR
    AddButton(520, y, 440, 72, kVrOff, L"Off", false);
    const int n = (int)config::GetFloat("render_size", 2048.f);
    swprintf_s(sizeText, 32, L"%d x %d", n, n);
}

void Draw() {
    if (!MakeSurface()) return;
    wchar_t stepText[32], sizeText[32];
    Layout(stepText, sizeText);
    const COLORREF bg = RGB(40, 40, 42), title = RGB(178, 28, 28), button = RGB(66, 66, 70), hover = RGB(96, 96, 102),
                   selected = RGB(178, 28, 28), selectedHover = RGB(210, 50, 50), white = RGB(245, 245, 245),
                   label = RGB(200, 200, 205), hint = RGB(150, 150, 155);
    Fill({0, 0, kW, kH}, bg);
    Fill({0, 0, kW, 100}, title);
    Text(L"VR SETTINGS", {40, 0, kW - 40, 100}, g_fTitle, white, DT_LEFT | DT_VCENTER);
    const wchar_t* labels[] = {L"Turning (first person)", Snap() ? L"Snap angle" : L"Turning speed",
                               L"Controller buttons", L"Wall collision (first person)", L"VR picture size (per eye)",
                               L"VR on / off  (Crashing during cutscene)"};
    for (int i = 0; i < 6; ++i) Text(labels[i], {40, 130 + i * 140, kW - 40, 130 + i * 140 + 48}, g_fLabel, label, DT_LEFT | DT_VCENTER);
    // the value boxes of the two stepper rows
    for (int row : {1, 4}) {
        const int y = 130 + row * 140 + 52;
        Fill({200, y, 800, y + 72}, RGB(30, 30, 32), 14);
        Text(row == 1 ? stepText : sizeText, {200, y, 800, y + 72}, g_fButton, white, DT_CENTER | DT_VCENTER);
    }
    for (int i = 0; i < g_nButtons; ++i) {
        const Button& b = g_buttons[i];
        const bool hv = b.a == g_hover;
        Fill(b.r, b.selected ? (hv ? selectedHover : selected) : (hv ? hover : button), 14);
        Text(b.text, b.r, g_fButton, white, DT_CENTER | DT_VCENTER);
    }
    Text(L"Point a controller at an option and pull its trigger.", {40, kH - 158, kW - 40, kH - 116}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    Text(L"VR off: press F9 on the keyboard to turn it back on.", {40, kH - 112, kW - 40, kH - 70}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    Text(L"A new picture size takes effect when you leave the menu.", {40, kH - 66, kW - 40, kH - 24}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
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
            float d[3];
            const float fwd[3] = {0.f, 0.f, -1.f};
            Rotate(hs.aim.q, fwd, d);
            const float dn = d[0] * nrm[0] + d[1] * nrm[1] + d[2] * nrm[2];
            if (dn < -1e-3f) {  // pointing at the panel's front
                const float t = ((c[0] - hs.aim.p[0]) * nrm[0] + (c[1] - hs.aim.p[1]) * nrm[1] + (c[2] - hs.aim.p[2]) * nrm[2]) / dn;
                if (t > 0.f && t < 20.f) {
                    const float q[3] = {hs.aim.p[0] + d[0] * t, hs.aim.p[1] + d[1] * t, hs.aim.p[2] + d[2] * t};
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
    char state[96];
    sprintf_s(state, "%d %.2f %.0f %d %d %.0f", Snap() ? 1 : 0, config::GetFloat("turn_speed", 1.f),
              config::GetFloat("snap_turn_degrees", 45.f), input::PadMode(), Walls() ? 1 : 0,
              config::GetFloat("render_size", 2048.f));
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
