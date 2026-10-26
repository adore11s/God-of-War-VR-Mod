#include "vpad.h"
#include "vrmap.h"
#include "../fp/combat.h"
#include "../core/log.h"
#include "../core/patch.h"
#include "../core/config.h"
#include "../vr/xr.h"
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gowvr::input {
namespace {

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
PFN_XInputGetState o_GetState = nullptr;

std::mutex g_mu;
bool g_enabled = false;
XINPUT_GAMEPAD g_hold{};
double g_holdUntil = 0;
DWORD g_packet = 1;
XINPUT_GAMEPAD g_lastSent{};
std::atomic<SHORT> g_playerLX{0}, g_playerLY{0};
std::atomic<uint64_t> g_polls{0};
std::atomic<DWORD> g_pollThread{0};
std::atomic<BYTE> g_gameLT{0};  // D-078: the left trigger the game last received (aiming)
std::atomic<WORD> g_gameButtons{0};  // D-080: the buttons the game last received (L1 = block)
// D-245 (owner: "when using mouse or gamepad I can't do head-based aiming"): the aim / look state of the devices the VR
// mapping does not see
std::atomic<ULONGLONG> g_extLookAt{0};   // a physical pad's right stick or the mouse turned the camera
std::atomic<uint8_t> g_realPsL2{0};      // a real PlayStation pad's L2 (0..255)
std::atomic<ULONGLONG> g_realPsAt{0};
void NoteLook(float deflection) { if (deflection > 0.25f) g_extLookAt = GetTickCount64(); }

SHORT Axis(float v) {
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    return (SHORT)lrintf(v * 32767.f);
}
BYTE Trig(float v) { return (BYTE)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * 255.f); }

SHORT AddAxis(SHORT a, SHORT b) {
    int s = (int)a + (int)b;
    return (SHORT)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// D-111: which pad the game sees. Xbox (F10 since D-217, default): the virtual pad is XInput user 0 (as always). PlayStation (F11):
// the same virtual pad (VR controllers + any XInput pad merged) is the game's own PlayStation pad of user 1 (libScePad,
// scePadRead) - the game then shows PlayStation button prompts - and XInput user 0 reads neutral.
std::atomic<int> g_padMode{0};       // 0 Xbox, 1 PlayStation
std::atomic<bool> g_psEverOn{false};  // the virtual PlayStation pad was used this session (stays connected, neutral)
std::atomic<bool> g_psUnavailable{false};  // libScePad imports not all found: Xbox only
bool PsMode() { return g_padMode.load(std::memory_order_relaxed) == 1; }
std::atomic<bool> g_hudTouch{false};       // D-262
std::atomic<int> g_psHandle{-1};  // the game's handle of user 1's PlayStation pad (real, or the mod's virtual one)
// the PlayStation mode takes the pads from XInput only once the game has opened its PlayStation pad (it reads it then);
// before / without that (libScePad not initialised) XInput stays the game's pad - never no input at all
bool PsActive() { return PsMode() && g_psHandle.load() >= 0; }

// The virtual pad over `g` (a real pad's state, or zero): holds, the VR controllers (merged like a second pad), the
// D-064 nudge, and the trackers of what the game received. Caller holds g_mu and has checked g_enabled.
void BuildPad(XINPUT_GAMEPAD& g);

// D-216: XInputGetStateEx (ordinal 100, the undocumented one with the Guide button) and XInputGetCapabilities - another
// build of the game (Epic) may read the pad through them instead
PFN_XInputGetState o_GetStateEx = nullptr;
using PFN_XInputGetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
PFN_XInputGetCapabilities o_GetCaps = nullptr;

// D-287 (owner, a Valve Index player: "with Xbox controls nothing works on my Index controllers - the hands move and the
// panel pointer works, in game nothing; PlayStation fixes it"): the Xbox mode reaches the game only through its import
// slots (XInput, and GetForegroundWindow - the game's Xbox pad reader DxPad::vfunc_3 drops every pad unless it is in
// front; its PlayStation reader has no such check). Another module can write its own hook over ours later - Steam
// Input does it for a game when it drives a controller it knows (the Index controllers are Steam Input devices; PS VR2
// / Quest controllers are not) - and its virtual pad, idle while the controllers serve the mod's VR session, is all the
// game reads. ImportWatch (1 Hz) takes such a slot back and calls that hook from ours (*Above): the game still gets
// what that module gives, then the VR pad merged in. A call coming back into our hook from it (its saved original is
// our hook) goes straight to the real function (t_xiDepth) - no loop, no second merge.
enum { kAboveGetState, kAboveSetState, kAboveGetCaps, kAboveGetStateEx, kAboveFg, kAboveCount };
std::atomic<void*> g_above[kAboveCount] = {};
thread_local int t_xiDepth = 0;
template <class Fn, class... A> auto CallAbove(int i, Fn real, A... a) -> decltype(real(a...)) {
    if (Fn above = (Fn)g_above[i].load(std::memory_order_relaxed)) {
        ++t_xiDepth;
        const auto r = above(a...);
        --t_xiDepth;
        return r;
    }
    return real(a...);
}
DWORD WINAPI XiNone(DWORD, XINPUT_STATE*) { return ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI XiNoCaps(DWORD, DWORD, XINPUT_CAPABILITIES*) { return ERROR_DEVICE_NOT_CONNECTED; }

DWORD MergeState(DWORD user, XINPUT_STATE* st, DWORD rc);
DWORD WINAPI hk_XInputGetState(DWORD user, XINPUT_STATE* st) {
    if (t_xiDepth) return o_GetState(user, st);  // D-287: back from the hook above ours
    return MergeState(user, st, CallAbove(kAboveGetState, o_GetState, user, st));
}
DWORD WINAPI hk_XInputGetStateEx(DWORD user, XINPUT_STATE* st) {
    const PFN_XInputGetState real = o_GetStateEx ? o_GetStateEx : &XiNone;
    if (t_xiDepth) return real(user, st);
    return MergeState(user, st, CallAbove(kAboveGetStateEx, real, user, st));
}
DWORD WINAPI hk_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    const PFN_XInputGetCapabilities real = o_GetCaps ? o_GetCaps : &XiNoCaps;
    if (t_xiDepth) return real(user, flags, c);
    const DWORD rc = CallAbove(kAboveGetCaps, real, user, flags, c);
    if (user != 0 || rc == ERROR_SUCCESS || !c) return rc;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_enabled || PsActive()) return rc;
    }
    ZeroMemory(c, sizeof(*c));  // the virtual pad: a standard gamepad on user 0
    c->Type = XINPUT_DEVTYPE_GAMEPAD;
    c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    c->Gamepad.wButtons = 0xF3FF;
    c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
    c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = 0x7FFF;
    return ERROR_SUCCESS;
}

DWORD MergeState(DWORD user, XINPUT_STATE* st, DWORD rc) {
    uint64_t n = ++g_polls;
    DWORD tid = GetCurrentThreadId();
    if (g_pollThread.exchange(tid) != tid && n < 50) Log("vpad: XInputGetState polled user=%lu rc=%lu thread=%lu", user, rc, tid);
    if (user != 0 || !st) return rc;
    if (rc == ERROR_SUCCESS)  // D-245: a real pad's right stick, before the VR pad is merged in
        NoteLook(fmaxf(fabsf(st->Gamepad.sThumbRX / 32767.f), fabsf(st->Gamepad.sThumbRY / 32767.f)));
    std::lock_guard<std::mutex> lk(g_mu);
    if (PsActive()) {
        // D-111: the pads go to the game through its PlayStation pad (hk_scePadRead); XInput user 0 stays connected but
        // neutral, so switching modes never shows the game's "controller disconnected" message
        if (rc == ERROR_SUCCESS || g_enabled) {
            ZeroMemory(st, sizeof(*st));
            st->dwPacketNumber = g_packet;
            return ERROR_SUCCESS;
        }
        return rc;
    }
    if (!g_enabled) {
        g_gameLT = rc == ERROR_SUCCESS ? st->Gamepad.bLeftTrigger : (BYTE)0;
        g_gameButtons = rc == ERROR_SUCCESS ? st->Gamepad.wButtons : (WORD)0;
        return rc;
    }
    if (rc != ERROR_SUCCESS) {
        ZeroMemory(st, sizeof(*st));
        rc = ERROR_SUCCESS;
    }
    BuildPad(st->Gamepad);
    // Games skip unchanged packet numbers; advance ours whenever the merged state changes.
    if (memcmp(&st->Gamepad, &g_lastSent, sizeof(st->Gamepad)) != 0) {
        ++g_packet;
        g_lastSent = st->Gamepad;
    }
    st->dwPacketNumber = g_packet;
    return rc;
}

void BuildPad(XINPUT_GAMEPAD& g) {
    XINPUT_GAMEPAD v{};
    if (NowMs() < g_holdUntil) v = g_hold;
    XINPUT_GAMEPAD c{};
    if (VrGamepad(&c)) {  // motion controllers merge like a second pad
        v.wButtons |= c.wButtons;
        v.bLeftTrigger = std::max(v.bLeftTrigger, c.bLeftTrigger);
        v.bRightTrigger = std::max(v.bRightTrigger, c.bRightTrigger);
        v.sThumbLX = AddAxis(v.sThumbLX, c.sThumbLX);
        v.sThumbLY = AddAxis(v.sThumbLY, c.sThumbLY);
        v.sThumbRX = AddAxis(v.sThumbRX, c.sThumbRX);
        v.sThumbRY = AddAxis(v.sThumbRY, c.sThumbRY);
    }
    g.wButtons |= v.wButtons;
    g.bLeftTrigger = std::max(g.bLeftTrigger, v.bLeftTrigger);
    g.bRightTrigger = std::max(g.bRightTrigger, v.bRightTrigger);
    g.sThumbLX = AddAxis(g.sThumbLX, v.sThumbLX);
    g.sThumbLY = AddAxis(g.sThumbLY, v.sThumbLY);
    g.sThumbRX = AddAxis(g.sThumbRX, v.sThumbRX);
    g.sThumbRY = AddAxis(g.sThumbRY, v.sThumbRY);
    // D-064: the player's own stick (before the recovery nudge) for the held-attack movement (combat.cpp)
    g_playerLX = g.sThumbLX;
    g_playerLY = g.sThumbLY;
    if (fp::CombatBlockMove()) { g.sThumbLX = 0; g.sThumbLY = 0; }  // D-200 (the mod still sees the player's stick above)
    g_gameLT = g.bLeftTrigger;
    // D-064: F1 attack recovery -> locomotion like walking (no post-attack "delay stance" and its sound): a small forward
    // push while the player's stick rests (the position is held meanwhile, so Kratos does not step)
    if (const float nudge = fp::CombatStickNudge(); nudge > 0.f && abs(g.sThumbLX) < 8000 && abs(g.sThumbLY) < 8000)
        g.sThumbLY = Axis(nudge);
    g_gameButtons = g.wButtons;
}

// ---- D-111 rumble: the game's vibration requests -> the VR controllers (vr::XrRumble keeps them going per frame) ------
std::atomic<float> g_rumbleLarge{0.f}, g_rumbleSmall{0.f};
void SetRumble(float heavy, float light, const char* from) {
    heavy = heavy < 0.f ? 0.f : heavy > 1.f ? 1.f : heavy;
    light = light < 0.f ? 0.f : light > 1.f ? 1.f : light;
    g_rumbleLarge = heavy;
    g_rumbleSmall = light;
    static std::atomic<int> logs{0};
    if ((heavy > 0.f || light > 0.f) && logs.fetch_add(1) < 20) Log("vpad: rumble from %s heavy %.2f light %.2f", from, heavy, light);
    vr::XrRumble(heavy, light);
}

// XInputSetState (XINPUT1_4 ordinal 3): the Xbox rumble. The virtual pad is user 0 - its rumble goes to the controllers
// (and on to a real pad as before).
using PFN_XInputSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
PFN_XInputSetState o_SetState = nullptr;
DWORD WINAPI hk_XInputSetState(DWORD user, XINPUT_VIBRATION* v) {
    if (t_xiDepth) return o_SetState(user, v);  // D-287
    DWORD rc = CallAbove(kAboveSetState, o_SetState, user, v);
    if (user == 0 && v && !PsActive()) {
        SetRumble(v->wLeftMotorSpeed / 65535.f, v->wRightMotorSpeed / 65535.f, "Xbox pad");
        bool enabled;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            enabled = g_enabled;
        }
        if (enabled) rc = ERROR_SUCCESS;  // the virtual pad is connected even with no real one
    }
    return rc;
}

// ---- D-111 the game's PlayStation pad (libScePad, imported by GoW.exe; STATIC sm::JediPad, sm_core_009 / 010) ---------
// JediPad::vfunc_1 opens users 1..4 once at startup (scePadOpen(user, 0, 0, 0)) and keeps only valid handles; vfunc_3
// reads each with scePadRead(handle, buf, 64) into 120-byte samples: +0 buttons (bit 0 Create, 1 L3, 2 R3, 3 Options,
// 4 Up, 5 Right, 6 Down, 7 Left, 8 L2, 9 R2, 10 L1, 11 R1, 12 Triangle, 13 Circle, 14 Cross, 15 Square, 20 touchpad
// click, 31 intercepted), +4..7 sticks LX LY RX RY (0..255, 128 centre, Y down), +8 / +9 L2 / R2 pressure, +0x0c
// orientation quaternion, +0x34 touch count, +0x4c connected, +0x50 timestamp; a sample with buttons or a stick off
// 0x70..0x90 marks the PlayStation pad as the one in use (the game's prompt style). scePadGetControllerType == 2 is
// asked by vfunc_19. scePadSetVibration(handle, {byte0, byte1}) = the two motors.
// User 1's handle carries the virtual pad; when there is no PlayStation pad the real open fails, and a handle of ours is
// returned so the game reads it from the start (the mode can be switched at any time).
constexpr int kFakePsHandle = 0x7a0001;
constexpr int kPsSample = 0x78;
std::atomic<bool> g_psFake{false};
using PFN_scePadOpen = int (*)(int, int, int, const void*);
using PFN_scePadRead = int (*)(int, void*, int);
using PFN_scePadGetControllerType = int (*)(int, int*);
using PFN_scePadHandleCall = int (*)(int, void*, void*, void*);  // the handle + whatever else (forwarded untouched)
PFN_scePadOpen o_scePadOpen = nullptr;
PFN_scePadRead o_scePadRead = nullptr;
PFN_scePadGetControllerType o_scePadGetControllerType = nullptr;
PFN_scePadHandleCall o_scePadSetVibration = nullptr, o_scePadSetVibrationMode = nullptr, o_scePadSetLightBar = nullptr,
                     o_scePadResetLightBar = nullptr;

bool IsPsHandle(int h) { return h >= 0 && h == g_psHandle.load(); }

int hk_scePadOpen(int user, int type, int index, const void* param) {
    int h = o_scePadOpen(user, type, index, param);
    if (user == 1) {
        if (h < 0) {
            Log("vpad: scePadOpen(user 1) = 0x%x (no PlayStation pad): the mod's virtual one is handle 0x%x", (unsigned)h,
                (unsigned)kFakePsHandle);
            h = kFakePsHandle;
            g_psFake = true;
        } else {
            Log("vpad: scePadOpen(user 1) = %d (a PlayStation pad): the virtual pad merges into it", h);
        }
        g_psHandle = h;
    }
    return h;
}

uint8_t StickByte(SHORT v, bool invert) {
    float f = (float)v / 32767.f;
    if (invert) f = -f;
    const int b = (int)lrintf(f * 127.5f + 127.5f);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

// XInput gamepad -> the PlayStation sample's buttons (map = touchpad click, as on PlayStation)
uint32_t PsButtons(const XINPUT_GAMEPAD& g) {
    uint32_t b = 0;
    const WORD w = g.wButtons;
    if (w & XINPUT_GAMEPAD_A) b |= 0x4000;               // Cross
    if (w & XINPUT_GAMEPAD_B) b |= 0x2000;               // Circle
    if (w & XINPUT_GAMEPAD_X) b |= 0x8000;               // Square
    if (w & XINPUT_GAMEPAD_Y) b |= 0x1000;               // Triangle
    if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= 0x400;    // L1
    if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= 0x800;   // R1
    if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= 0x100;   // L2
    if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= 0x200;  // R2
    if (w & XINPUT_GAMEPAD_LEFT_THUMB) b |= 0x2;         // L3
    if (w & XINPUT_GAMEPAD_RIGHT_THUMB) b |= 0x4;        // R3
    if (w & XINPUT_GAMEPAD_START) b |= 0x8;              // Options
    if (w & XINPUT_GAMEPAD_BACK) b |= 0x100000;          // touchpad click (map)
    if (w & XINPUT_GAMEPAD_DPAD_UP) b |= 0x10;
    if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= 0x20;
    if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= 0x40;
    if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= 0x80;
    return b;
}

// a neutral connected sample (or a disconnected one: sticks centred all the same - an off-centre stick counts as use)
void PsNeutral(uint8_t* s, bool connected) {
    memset(s, 0, kPsSample);
    s[4] = s[5] = s[6] = s[7] = 0x80;
    *(float*)(s + 0x18) = 1.f;  // orientation w
    s[0x4c] = connected ? 1 : 0;
    *(uint64_t*)(s + 0x50) = (uint64_t)(NowMs() * 1000.0);
    s[0x68] = connected ? 1 : 0;
}

// the virtual pad merged into a sample: buttons OR, each stick / trigger the one further from rest
void PsMerge(uint8_t* s, const XINPUT_GAMEPAD& g) {
    *(uint32_t*)s |= PsButtons(g);
    const uint8_t v[4] = {StickByte(g.sThumbLX, false), StickByte(g.sThumbLY, true), StickByte(g.sThumbRX, false),
                          StickByte(g.sThumbRY, true)};
    for (int i = 0; i < 4; ++i)
        if (abs((int)v[i] - 128) > abs((int)s[4 + i] - 128)) s[4 + i] = v[i];
    s[8] = std::max(s[8], g.bLeftTrigger);
    s[9] = std::max(s[9], g.bRightTrigger);
    s[0x4c] = 1;
    if (!s[0x68]) s[0x68] = 1;
}

int hk_scePadRead(int h, void* data, int count) {
    if (!IsPsHandle(h) || !data || count < 1) return o_scePadRead(h, data, count);
    const bool fake = g_psFake.load();
    if (!PsMode()) {
        if (!fake) return o_scePadRead(h, data, count);  // a real PlayStation pad, as it is
        // no PlayStation pad and the Xbox mode: our handle reads disconnected - or, once the PlayStation mode was used
        // this session, connected and idle (so the switch back shows no "controller disconnected")
        PsNeutral((uint8_t*)data, g_psEverOn.load());
        return 1;
    }
    g_psEverOn = true;
    int n = fake ? 0 : o_scePadRead(h, data, count);
    if (n > 0) {  // D-245: the real PlayStation pad's own right stick (+6 / +7, 128 centre) and L2 (+8)
        const uint8_t* s = (const uint8_t*)data + (size_t)(n - 1) * kPsSample;
        NoteLook(fmaxf(fabsf(s[6] - 128.f), fabsf(s[7] - 128.f)) / 127.f);
        g_realPsL2 = s[8];
        g_realPsAt = GetTickCount64();
    }
    XINPUT_GAMEPAD g{};
    {
        XINPUT_STATE xs{};
        if (o_GetState && o_GetState(0, &xs) == ERROR_SUCCESS) {  // a real Xbox pad merges in
            g = xs.Gamepad;
            NoteLook(fmaxf(fabsf(g.sThumbRX / 32767.f), fabsf(g.sThumbRY / 32767.f)));  // D-245
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_enabled) BuildPad(g);
        else {
            g_gameLT = g.bLeftTrigger;
            g_gameButtons = g.wButtons;
        }
    }
    if (n <= 0) {
        PsNeutral((uint8_t*)data, true);
        n = 1;
    }
    for (int i = 0; i < n; ++i) PsMerge((uint8_t*)data + (size_t)i * kPsSample, g);
    // D-262: the HUD peek as a finger resting on the touchpad - the sample's touch data (+0x34 count, +0x3c / +0x3e x / y,
    // +0x40 id; STATIC sub_14057b990 turns a count > 0 into the pad's FingerOnTouchpad, the touchpad 1920 x 943 by
    // default) - so the game's Touch combat HUD comes up its own way (timer 0x141079150, EVT_TOUCH_STARTED / _ENDED)
    if (g_hudTouch.load()) {
        for (int i = 0; i < n; ++i) {
            uint8_t* s = (uint8_t*)data + (size_t)i * kPsSample;
            if (s[0x34]) continue;  // a real finger already
            s[0x34] = 1;
            *(uint16_t*)(s + 0x3c) = 960;
            *(uint16_t*)(s + 0x3e) = 471;
            s[0x40] = 0x7f;
        }
    }
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 3) Log("vpad: PlayStation pad read (handle 0x%x, %d sample(s), %s)", (unsigned)h, n, fake ? "virtual" : "real + virtual");
    return n;
}

int hk_scePadGetControllerType(int h, int* type) {
    if (!IsPsHandle(h) || !type) return o_scePadGetControllerType(h, type);
    if (!g_psFake.load()) {
        const int rc = o_scePadGetControllerType(h, type);
        if (rc >= 0 && *type != 0) return rc;  // a real pad's own type
    }
    *type = PsMode() ? 2 : 0;  // 2 = DualSense (the PSVR2 Sense controllers' family)
    return 0;
}

int hk_scePadSetVibration(int h, void* param, void* a, void* b) {
    if (IsPsHandle(h) && PsMode() && param) {
        const uint8_t* m = (const uint8_t*)param;
        SetRumble(m[0] / 255.f, m[1] / 255.f, "PlayStation pad");
    }
    if (IsPsHandle(h) && g_psFake.load()) return 0;
    return o_scePadSetVibration(h, param, a, b);
}
// the other per-handle calls: never into libScePad with our handle
int hk_scePadSetVibrationMode(int h, void* a, void* b, void* c) {
    return IsPsHandle(h) && g_psFake.load() ? 0 : o_scePadSetVibrationMode(h, a, b, c);
}
int hk_scePadSetLightBar(int h, void* a, void* b, void* c) {
    return IsPsHandle(h) && g_psFake.load() ? 0 : o_scePadSetLightBar(h, a, b, c);
}
int hk_scePadResetLightBar(int h, void* a, void* b, void* c) {
    return IsPsHandle(h) && g_psFake.load() ? 0 : o_scePadResetLightBar(h, a, b, c);
}

// Focus override: GoW drops pad input unless GetForegroundWindow() is its window. In VR the desktop focus is
// irrelevant (the player is in the headset), so the mod answers the game's own focus query while the override is on.
using PFN_GetForegroundWindow = HWND(WINAPI*)();
PFN_GetForegroundWindow o_GetForegroundWindow = nullptr;
std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<bool> g_focusOverride{false};

HWND WINAPI hk_GetForegroundWindow() {
    HWND game = g_gameWindow.load();
    if (g_focusOverride && game) return game;
    if (t_xiDepth) return o_GetForegroundWindow();  // D-287
    return CallAbove(kAboveFg, o_GetForegroundWindow);
}

}  // namespace

// D-216 (owner: "make the Epic Games version work"): the Steam GoW.exe imports XInputGetState / SetState from
// XINPUT1_4.dll by ordinal (2 / 3) and the hooks patched exactly those slots. Another build can import another XInput DLL
// (xinput1_3, xinput9_1_0), by name, delay-loaded, or look the functions up at run time - then the VR controllers would
// do nothing while everything else worked (a VR mod for another game hit exactly this on its Epic build). Every route is
// caught: any xinput*.dll import slot (ordinal or name, normal or delay-load), and GetProcAddress for xinput functions.
struct XiFn { uint16_t ord; const char* name; void* hook; void** orig; };
XiFn g_xi[] = {
    {2, "XInputGetState", (void*)&hk_XInputGetState, (void**)&o_GetState},
    {3, "XInputSetState", (void*)&hk_XInputSetState, (void**)&o_SetState},
    {4, "XInputGetCapabilities", (void*)&hk_XInputGetCapabilities, (void**)&o_GetCaps},
    {100, nullptr, (void*)&hk_XInputGetStateEx, (void**)&o_GetStateEx},
};
bool IsXInputModule(HMODULE m) {
    char p[MAX_PATH];
    if (!m || !GetModuleFileNameA(m, p, MAX_PATH)) return false;
    const char* b = strrchr(p, '\\');
    return _strnicmp(b ? b + 1 : p, "xinput", 6) == 0;
}
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
PFN_GetProcAddress o_GetProcAddress = nullptr;
FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR n) {
    FARPROC r = o_GetProcAddress(m, n);
    if (!r || !n || !IsXInputModule(m)) return r;
    for (XiFn& f : g_xi) {
        const bool match = IS_INTRESOURCE(n) ? (uint16_t)(uintptr_t)n == f.ord : (f.name && strcmp(n, f.name) == 0);
        if (!match) continue;
        if (*f.orig && *f.orig != (void*)r) return r;  // already bound to another XInput DLL: leave this one alone
        *f.orig = (void*)r;
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 10)
            Log("vpad: the game looked up xinput %s at run time - hooked (D-216)", f.name ? f.name : "ordinal 100");
        return (FARPROC)f.hook;
    }
    return r;
}

using PFN_GetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
PFN_GetRawInputData o_GetRawInputData = nullptr;
UINT WINAPI hk_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header) {
    const UINT r = o_GetRawInputData(h, cmd, data, size, header);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        const RAWINPUT* in = (const RAWINPUT*)data;
        if (in->header.dwType == RIM_TYPEMOUSE && !(in->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            labs(in->data.mouse.lLastX) + labs(in->data.mouse.lLastY) >= 2)
            g_extLookAt = GetTickCount64();  // D-245: the mouse turns the camera
    }
    return r;
}

void InstallVirtualPad() {
    HMODULE exe = GetModuleHandleW(nullptr);
    int slots[4];
    for (int i = 0; i < 4; ++i) slots[i] = PatchImportAny(exe, "xinput", g_xi[i].ord, g_xi[i].name, g_xi[i].hook, g_xi[i].orig);
    const int gpa = PatchImportAny(exe, "kernel32", 0, "GetProcAddress", (void*)&hk_GetProcAddress, (void**)&o_GetProcAddress) +
                    PatchImportAny(exe, "api-ms-win-core-libraryloader", 0, "GetProcAddress", (void*)&hk_GetProcAddress,
                                   (void**)&o_GetProcAddress);
    const bool ok = slots[0] > 0, rs = slots[1] > 0;
    // D-245: the mouse's movement as the game reads it (raw input) - turning the camera with the mouse
    const bool ri = PatchImport(exe, "USER32.dll", "GetRawInputData", (void*)&hk_GetRawInputData, (void**)&o_GetRawInputData);
    Log("vpad: raw input (mouse look) %s (D-245)", ri ? "watched" : "NOT watched");
    bool fg = PatchImport(exe, "USER32.dll", "GetForegroundWindow", (void*)&hk_GetForegroundWindow,
                          (void**)&o_GetForegroundWindow);
    Log("vpad: XInput import slots GetState=%d SetState=%d GetCapabilities=%d GetStateEx=%d, run-time lookups (GetProcAddress) "
        "%s, GetForegroundWindow=%d (D-216)", slots[0], slots[1], slots[2], slots[3], gpa ? "watched" : "NOT watched", fg);
    if (!ok && !gpa) Log("vpad: WARNING - no way to the game's XInput found: the VR controllers may not reach the game");
    // D-111: rumble (Xbox) and the game's PlayStation pad. DllMain: before the game opens its pads.
    const char* lib = "libScePad.dll";
    const bool p0 = PatchImport(exe, lib, "scePadOpen", (void*)&hk_scePadOpen, (void**)&o_scePadOpen);
    const bool p1 = PatchImport(exe, lib, "scePadRead", (void*)&hk_scePadRead, (void**)&o_scePadRead);
    const bool p2 = PatchImport(exe, lib, "scePadGetControllerType", (void*)&hk_scePadGetControllerType,
                                (void**)&o_scePadGetControllerType);
    const bool p3 = PatchImport(exe, lib, "scePadSetVibration", (void*)&hk_scePadSetVibration, (void**)&o_scePadSetVibration);
    const bool p4 = PatchImport(exe, lib, "scePadSetVibrationMode", (void*)&hk_scePadSetVibrationMode,
                                (void**)&o_scePadSetVibrationMode);
    const bool p5 = PatchImport(exe, lib, "scePadSetLightBar", (void*)&hk_scePadSetLightBar, (void**)&o_scePadSetLightBar);
    const bool p6 = PatchImport(exe, lib, "scePadResetLightBar", (void*)&hk_scePadResetLightBar, (void**)&o_scePadResetLightBar);
    Log("vpad: IAT XInputSetState(ord 3)=%d, libScePad open=%d read=%d type=%d vibration=%d mode=%d lightbar=%d/%d", rs, p0,
        p1, p2, p3, p4, p5, p6);
    if (!(p0 && p1 && p2 && p3 && p4 && p5 && p6)) {  // all or nothing: a half-hooked PlayStation pad is not offered
        Log("vpad: PlayStation pad emulation unavailable (libScePad imports not all found)");
        g_psHandle = -1;
        g_psUnavailable = true;
    }
    g_padMode = config::GetFloat("pad_mode", 0.f) > 0.5f && !g_psUnavailable ? 1 : 0;
    Log("vpad: pad mode %s (F10 Xbox / F11 PlayStation)", PsMode() ? "PlayStation" : "Xbox");
}

// D-287: once a second (command thread) - our import slots taken back from a module that hooked over them; and once a
// minute, while VR runs in the Xbox mode, how often the game read the pad (0 = it reads nothing: the log says so)
void ImportWatch() {
    HMODULE exe = GetModuleHandleW(nullptr);
    struct W { const char* dll; uint16_t ord; const char* name; void* hook; int above; const char* what; };
    const W w[] = {
        {"xinput", g_xi[0].ord, g_xi[0].name, g_xi[0].hook, kAboveGetState, "XInputGetState"},
        {"xinput", g_xi[1].ord, g_xi[1].name, g_xi[1].hook, kAboveSetState, "XInputSetState"},
        {"xinput", g_xi[2].ord, g_xi[2].name, g_xi[2].hook, kAboveGetCaps, "XInputGetCapabilities"},
        {"xinput", g_xi[3].ord, g_xi[3].name, g_xi[3].hook, kAboveGetStateEx, "XInputGetStateEx"},
        {"user32", 0, "GetForegroundWindow", (void*)&hk_GetForegroundWindow, kAboveFg, "GetForegroundWindow"},
    };
    for (auto& a : g_above) {  // a module whose hook we call was unloaded since: not called any more
        void* p = a.load();
        HMODULE m = nullptr;
        if (p && !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCSTR)p, &m)) {
            a = nullptr;
            Log("vpad: a hook above ours (%p) is gone - not called any more (D-287)", p);
        }
    }
    for (const W& x : w) {
        void* foreign = nullptr;
        const int n = ReclaimImportAny(exe, x.dll, x.ord, x.name, x.hook, &foreign);
        if (!n || !foreign) continue;
        const bool changed = g_above[x.above].exchange(foreign) != foreign;
        static std::atomic<int> logs{0};
        if (changed && logs.fetch_add(1) < 20) {
            char mod[MAX_PATH] = "?";
            HMODULE m = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)foreign, &m) && m)
                GetModuleFileNameA(m, mod, MAX_PATH);
            Log("vpad: the game's %s slot was hooked over by %s (%p) - taken back, that hook is called from ours (D-287)",
                x.what, mod, foreign);
        }
    }
    static ULONGLONG last = 0;
    static uint64_t polls0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 60000) return;
    last = now;
    const uint64_t p = g_polls.load(), d = p - polls0;
    polls0 = p;
    bool enabled;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        enabled = g_enabled;
    }
    if (!enabled || PsActive()) return;
    HWND fg = o_GetForegroundWindow ? o_GetForegroundWindow() : nullptr;
    Log("vpad: Xbox pad mode - the game read the pad %llu times in the last minute%s (game window %p, in front %p, focus "
        "override %s; D-287)", (unsigned long long)d, d ? "" : " - NOTHING reaches the game this way", (void*)g_gameWindow.load(),
        (void*)fg, g_focusOverride.load() ? "on" : "off");
}

void SetPadMode(int mode) {
    if (mode == 1 && g_psUnavailable) { Log("vpad: PlayStation pad emulation unavailable - staying Xbox"); return; }
    if (g_padMode.exchange(mode) == mode) return;
    SetRumble(0.f, 0.f, "mode switch");  // a running rumble belongs to the old pad
    config::SetFloat("pad_mode", (float)mode);
    Log("vpad: pad mode -> %s (%s)", mode ? "PlayStation (the game's own PlayStation pad, user 1)" : "Xbox (XInput user 0)",
        mode && g_psFake.load() ? "virtual PlayStation pad" : mode ? "merged into the connected PlayStation pad" : "as before");
}
int PadMode() { return g_padMode.load(); }

// D-217: F10 is now the Xbox pad key. Windows treats F10 as a system key (WM_SYSKEYDOWN) and enters the window's menu
// mode on its release - a windowed game stalls until the next key. The game window is subclassed once and only F10's
// system-key messages are dropped (the mod reads its keys with GetAsyncKeyState); every other message goes on unchanged
WNDPROC g_prevWndProc = nullptr;
LRESULT CALLBACK F10WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp == VK_F10) return 0;
    return CallWindowProcW(g_prevWndProc, h, msg, wp, lp);
}

void SetGameWindow(HWND w) {
    g_gameWindow = w;
    static HWND subclassed = nullptr;
    if (!w || w == subclassed || !IsWindow(w)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return;
    if (subclassed && IsWindow(subclassed)) return;  // one game window at a time (a second chain would lose its proc)
    g_prevWndProc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)&F10WndProc);
    if (g_prevWndProc) {
        subclassed = w;
        Log("vpad: game window subclassed - F10 does not open the window menu (D-217)");
    }
}

void PostKey(int vk, int ms) {
    if (!g_gameWindow) return;
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    PostMessageW(g_gameWindow, WM_KEYDOWN, (WPARAM)vk, 1 | (sc << 16));
    Sleep((DWORD)ms);
    PostMessageW(g_gameWindow, WM_KEYUP, (WPARAM)vk, 1 | (sc << 16) | (1u << 30) | (1u << 31));
}

bool PostKeyEdge(int vk, bool down) {
    HWND w = g_gameWindow.load();
    if (!w) return false;
    const UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    const LPARAM lp = (LPARAM)(1u | (sc << 16) | (down ? 0u : (1u << 30) | (1u << 31)));
    return PostMessageW(w, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp) != 0;
}

// The game keeps its own "active" state from window activation and drops input while inactive; while the override
// is on, re-assert activation (called once per second from the command thread).
void FocusKeepAlive() {
    HWND w = g_gameWindow.load();
    if (!g_focusOverride || !w) return;
    PostMessageW(w, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(w, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(w, WM_SETFOCUS, 0, 0);
}

void FocusOverride(bool on) {
    g_focusOverride = on;
    FocusKeepAlive();
    Log("vpad: focus override %s (game window %p)", on ? "on" : "off", (void*)g_gameWindow.load());
}

void VpadEnable(bool on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_enabled = on;
    Log("vpad: %s", on ? "enabled (user 0 reports connected)" : "disabled");
}

void PlayerLeftStick(float* x, float* y) {
    *x = g_playerLX.load() / 32767.f;
    *y = g_playerLY.load() / 32767.f;
}
float GameLeftTrigger() { return g_gameLT.load() / 255.f; }
bool PlayerAimInput() {
    if (GameLeftTrigger() > 0.3f) return true;  // the VR aim trigger (vrmap's latch) or an Xbox pad's LT
    if (g_realPsL2.load() > 76 && GetTickCount64() - g_realPsAt.load() < 250) return true;  // a PlayStation pad's L2
    static const int key = (int)config::GetFloat("aim_key", (float)VK_LCONTROL);  // owner: the keyboard aims with Left Ctrl
    if (key <= 0 || !(GetAsyncKeyState(key) & 0x8000)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);  // (user32's own - the game's import is the hooked one)
    return pid == GetCurrentProcessId();
}
bool ExternalLookActive() { return GetTickCount64() - g_extLookAt.load() < 150; }
bool GameBlockHeld() { return (g_gameButtons.load() & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0; }
void SetHudTouch(bool on) { g_hudTouch = on; }
bool HudTouchPossible() { return PsMode() && !g_psUnavailable.load() && g_psHandle.load() >= 0; }
void VpadHold(uint16_t buttons, float lx, float ly, float rx, float ry, float lt, float rt, int ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hold = {};
    g_hold.wButtons = buttons;
    g_hold.sThumbLX = Axis(lx);
    g_hold.sThumbLY = Axis(ly);
    g_hold.sThumbRX = Axis(rx);
    g_hold.sThumbRY = Axis(ry);
    g_hold.bLeftTrigger = Trig(lt);
    g_hold.bRightTrigger = Trig(rt);
    g_holdUntil = NowMs() + ms;
    Log("vpad: hold buttons=0x%04x L=(%.2f,%.2f) R=(%.2f,%.2f) T=(%.2f,%.2f) for %d ms (polls so far %llu)", buttons,
        lx, ly, rx, ry, lt, rt, ms, (unsigned long long)g_polls.load());
}

uint64_t VpadPolls() { return g_polls.load(); }

}  // namespace gowvr::input
