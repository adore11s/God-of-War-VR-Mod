// Motion controllers -> the game's own gamepad funnel (XInput, merged by vpad.cpp). Playbook ch.03: physical transport
// (vr/xr.cpp) and game semantics (here) stay separate; neutral on any ownership loss; radial deadzones; trigger
// thresholds above a resting controller's reading; hysteresis on grips.
#include "vrmap.h"
#include "../core/addr.h"
#include "../vr/xr.h"
#include "../core/log.h"
#include "../core/config.h"
#include "../fp/body.h"
#include "../fp/firstperson.h"
#include "../fp/combat.h"
#include "../fp/arms.h"
#include "../stereo/mutate.h"
#include "vpad.h"
#include "turning.h"
#include "../vr/panel.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cmath>

namespace gowvr::input {
namespace {

std::atomic<bool> g_on{false};
std::atomic<float> g_camSyncX{0.f}, g_camSyncY{0.f};
std::atomic<float> g_locoYaw{0.f};

// ---- physical swings (playbook ch.02 #motion-gestures): room-space velocity on our own clock, speed + direction gate,
// hysteresis re-arm and a cooldown sized to the game's attack animation. Gameplay F1 only (owner rule: outside F1 the
// controllers are a plain gamepad). One detector per hand; F1 combat gestures (docs/F1_COMBAT_PLAN.md step 3):
//   right swing -> R1; right swing with the right trigger held -> R2; big downward chop -> R2
//   D-148: a HARD swing / punch (peak hand speed >= heavy_speed / heavy_speed_fist) -> R2 instead of R1 (either hand)
//   right trigger held with the hand above the head (charge_ms) -> R2 held until the trigger is released or the hand
//     swings down (charged heavies: Executioner's Cleave, Guardian's Justice)
//   fast back-and-forth waving (grind_reversals, default 4, in 0.8 s) -> R1 held while it lasts (Grinding Storm / Guardian Sweep)
//   left swing / punch while the axe is not in the hand (bare-handed, Blades, Rage) -> R1
//   both hands above the head, then both down fast together -> R2 (Raging Smash, Chaos Slam)
//   both grips held, both hands above the head for 0.8 s -> L3+R3 (Spartan Rage)
//   aiming (left trigger): right throw gesture -> R1 + fp::ThrowStart (D-028); the right trigger stays a direct R2
struct Swing {
    bool armed = true;
    float prev[3] = {0, 0, 0};
    ULONGLONG prevT = 0, cooldownUntil = 0, pressUntil = 0, slowSince = 0;
    WORD button = 0;
    // D-148: a swing that passed the gate waits (at most heavy_decide_ms) for its peak speed: light (R1) or heavy (R2)
    bool pend = false;
    ULONGLONG pendAt = 0;
    float pendPeak = 0.f, pendThr = 0.f;
    const char* pendLight = "";
    const char* pendHeavy = "";
    // recent controller samples (fresh ones only): speed over ~60 ms and path length over ~200 ms
    struct S { float p[3]; ULONGLONG t; } hist[48];
    int head = 0, count = 0;
    // kinematics of the last fresh sample
    float v[3] = {0, 0, 0}, speed = 0.f, path = 0.f, along = 0.f;
    // grind: direction reversals at speed
    float peakDir[3] = {0, 0, 0};
    ULONGLONG rev[8] = {};
    int revN = 0;
    ULONGLONG grindUntil = 0, aboveSince = 0, lastAbove = 0;
    float fireDir[3] = {0, 0, 0};  // D-075: direction of the last swing that fired (reversal re-arms)
    ULONGLONG firedAt = 0;
    void Fired(ULONGLONG now) {
        firedAt = now;
        for (int i = 0; i < 3; ++i) fireDir[i] = speed > 1e-3f ? v[i] / speed : 0.f;
    }
    // D-084 (owner: "the way I bring back my weapon from the swing triggers another attack"): a motion against the
    // direction of the swing that fired, within swing_return_ms of it, is the arm coming back - never an attack. (The
    // D-075 reversal re-arm made exactly that motion count as a new swing once it was fast enough.)
    bool Returning(ULONGLONG now, ULONGLONG ms) const {
        if (!firedAt || now - firedAt > ms || speed < 1e-3f) return false;
        return (v[0] * fireDir[0] + v[1] * fireDir[1] + v[2] * fireDir[2]) / speed < -0.2f;
    }
};
Swing g_swing, g_swingL;
std::atomic<uint64_t> g_swings{0}, g_heavy{0};

float HeadY() {
    vr::EyeViews ev = vr::XrLatestViews();
    return ev.valid ? 0.5f * (ev.pose[0].p[1] + ev.pose[1].p[1]) : 1e9f;
}

// Updates the hand's velocity window; returns true on a fresh sample.
bool Kinematics(Swing& s, const vr::HandState& H, ULONGLONG now, bool fresh) {
    if (!H.active || !H.gripValid) { s.prevT = 0; s.speed = 0.f; return false; }
    const float* p = H.grip.p;
    bool upd = false;
    if (fresh && s.prevT && now > s.prevT) {
        Swing::S& cur = s.hist[s.head];
        memcpy(cur.p, p, sizeof(cur.p));
        cur.t = now;
        s.head = (s.head + 1) % 48;
        if (s.count < 48) ++s.count;
        auto at = [&](int back) -> const Swing::S& { return s.hist[(s.head - 1 - back + 96) % 48]; };
        int k60 = 0;
        while (k60 + 1 < s.count && now - at(k60 + 1).t <= 60) ++k60;
        float path = 0.f;
        for (int k = 0; k + 1 < s.count && now - at(k + 1).t <= 200; ++k) {
            const float* a = at(k).p; const float* b = at(k + 1).p;
            path += sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }
        const float dt = k60 > 0 ? (float)(now - at(k60).t) / 1000.f : 1.f;
        const float* o = at(k60).p;
        for (int i = 0; i < 3; ++i) s.v[i] = k60 > 0 ? (p[i] - o[i]) / dt : 0.f;
        s.speed = sqrtf(s.v[0] * s.v[0] + s.v[1] * s.v[1] + s.v[2] * s.v[2]);
        s.path = path;
        // the controller's pointing direction (-Z of the grip frame) must lead the motion: a swing, not a retraction
        const float* q = H.grip.q;  // x,y,z,w
        const float fx = -(2 * (q[0] * q[2] + q[1] * q[3])), fy = -(2 * (q[1] * q[2] - q[0] * q[3])),
                    fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
        s.along = s.speed > 1e-3f ? (s.v[0] * fx + s.v[1] * fy + s.v[2] * fz) / s.speed : 0.f;
        // grind reversals: at > 1.5 m/s the motion turned against the last fast direction
        if (s.speed > 1.5f) {
            const float d = (s.v[0] * s.peakDir[0] + s.v[1] * s.peakDir[1] + s.v[2] * s.peakDir[2]) / s.speed;
            if (d < -0.5f) {
                s.rev[s.revN++ % 8] = now;
            }
            for (int i = 0; i < 3; ++i) s.peakDir[i] = s.v[i] / s.speed;
        }
        // D-075 (owner: hits "very inconsistent", can't spam): re-armed by a short slow moment (80 ms under 1.2 m/s, was
        // 150 ms under 1.0 - continuous back-and-forth swinging never got it) OR by reversing against the last swing
        if (s.speed < 1.2f) { if (!s.slowSince) s.slowSince = now; }
        else s.slowSince = 0;
        if (!s.armed && s.slowSince && now - s.slowSince >= 80) s.armed = true;
        if (!s.armed && s.speed > 0.6f && now - s.firedAt > 70 &&
            (s.v[0] * s.fireDir[0] + s.v[1] * s.fireDir[1] + s.v[2] * s.fireDir[2]) / s.speed < -0.2f)
            s.armed = true;
        upd = true;
    }
    if (fresh) {  // only a new controller sample moves the velocity window (the game polls faster than 90 Hz)
        memcpy(s.prev, p, sizeof(s.prev));
        s.prevT = now;
    }
    // above the head (for charge / slam / roar)
    const float hy = HeadY();
    if (p[1] > hy) { if (!s.aboveSince) s.aboveSince = now; s.lastAbove = now; }
    else s.aboveSince = 0;
    return upd;
}

int Reversals(const Swing& s, ULONGLONG now, ULONGLONG windowMs) {
    int n = 0;
    for (ULONGLONG t : s.rev) if (t && now - t <= windowMs) ++n;
    return n;
}

bool OverShoulder(const vr::HandState& H, float* behindOut = nullptr, float* upOut = nullptr, float* rightOut = nullptr);

// ---- D-062 combat rework (owner attack review 2026-09-30, docs/F1_ATTACK_REVIEW.md) ----
// Attack gate: every attack press waits until the previous attack / evade has ended (plus combo_gap_ms) so the game
// always starts the FIRST attack of its combo (no combos, no dodge attacks); while the game's sprint plays the left
// stick is released first (no sprint attacks: the sprint stops, then the normal attack). Latest press wins; a press not
// sendable within 1.5 s is dropped. Throws (aimed) and the charge are not gated here.
enum class Mode { Bare, Axe, Blades, Rage };
Mode CurMode() {
    if (fp::CombatInRage()) return Mode::Rage;
    if (fp::CurrentWeapon() == 2) return Mode::Blades;
    if (fp::AxeInHand()) return Mode::Axe;
    return Mode::Bare;  // also with the axe thrown
}
struct Pending { WORD button = 0; bool rt = false, animate = false; ULONGLONG at = 0; const char* what = ""; };
Pending g_pend;
ULONGLONG g_sendUntil = 0, g_sendStart = 0;
WORD g_sendBtn = 0;
bool g_sendRt = false;
std::atomic<uint64_t> g_gateSent{0}, g_gateDropped{0};
void QueueAttack(WORD button, bool rt, bool animate, const char* what, ULONGLONG now) {
    g_pend = Pending{button, rt, animate, now, what};
    fp::CombatFinisherCancel();  // D-154: a swing after R3 - it was a lock-on
}

// stance_cancel: 1 = a short block (L1) tap, 2 = a short left-stick nudge back, 0 = wait the stance out
std::atomic<int> g_stanceMode{-1};
// D-076 (owner: "after doing the same attack twice the shield activates for a few seconds"): the block tap (mode 1)
// raised the shield bare-handed. Default now mode 3: the stance is fast-forwarded (creature time scale, combat.cpp)
int StanceCancelMode() {
    if (g_stanceMode.load() < 0) g_stanceMode = (int)config::GetFloat("stance_cancel", 3.f);
    return g_stanceMode.load();
}
void StanceCancel(XINPUT_GAMEPAD* g, ULONGLONG now) {
    static ULONGLONG since = 0, lastNow = 0;
    if (StanceCancelMode() == 3) { fp::CombatStanceFastForward(); return; }
    if (now - lastNow > 200) since = now;  // a new stance episode
    lastNow = now;
    if (now - since > 120) return;          // pressed for the first 120 ms only
    if (StanceCancelMode() == 1) g->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    else if (StanceCancelMode() == 2) g->sThumbLY = -12000;
}
void GateTick(XINPUT_GAMEPAD* g, ULONGLONG now) {
    static const ULONGLONG gap = (ULONGLONG)config::GetFloat("combo_gap_ms", 60.f);
    if (g_pend.at) {
        if (now - g_pend.at > 2500) {
            ++g_gateDropped;
            Log("vrmap: %s dropped (still busy after 2.5 s)", g_pend.what);
            g_pend = Pending{};
        } else if (fp::CombatSprinting()) {
            g->sThumbLX = g->sThumbLY = 0;  // stop the sprint first
            g->wButtons &= ~XINPUT_GAMEPAD_LEFT_THUMB;
        } else if (fp::CombatInStance()) {
            // the post-attack "delay stance" (pause-combo window, ~4 s standing still): end it first, or the press
            // becomes the game's pause combo
            StanceCancel(g, now);
        } else if (fp::CombatBusy()) {
            fp::CombatFastForward();  // D-067 spam: a held attack's rest plays fast, then this one starts
        } else if (fp::CombatIdleMs() >= gap && now - g_sendUntil >= 150) {
            // (the 200 ms after a press: the attack takes a few frames to show as busy)
            g_sendBtn = g_pend.button;
            g_sendRt = g_pend.rt;
            // D-076: a short press (max 50 ms, released as soon as the attack shows): the attack runs at 2-2.5x, so a
            // 90 ms press lasted 200+ ms of game time = "R1 held" -> Guardian Sweep (attShieldLegCutter, the shield sweep
            // the owner saw "activate out of nowhere"), and for weapons the hold-R1 specials
            g_sendUntil = now + 50;
            g_sendStart = now;
            if (g_pend.animate) fp::CombatRequestAnimate();
            if (++g_gateSent <= 40)
                Log("vrmap: %s -> %s%s (waited %llu ms)", g_pend.what, g_pend.rt ? "R2" : "R1",
                    g_pend.animate ? " [full animation]" : "", (unsigned long long)(now - g_pend.at));
            g_pend = Pending{};
        }
    }
    if (now < g_sendUntil && now - g_sendStart >= 20 && fp::CombatBusy()) g_sendUntil = 0;  // the attack started: let go
    if (now < g_sendUntil) {
        g->wButtons |= g_sendBtn;
        if (g_sendRt) g->bRightTrigger = 255;
    }
}

// ---- D-068 (owner: bare-handed and in Rage, punches only while the grips are fully held - fists; no attack otherwise)
// A punch counts only while that hand's grip is squeezed past fist_squeeze (0.85). The left grip is also the block (L1):
// bare-handed / in Rage the shield comes up only after the left fist has been held shield_delay_ms (150) and not within
// 400 ms of a left punch, so closing the fist to punch does not raise the shield (or the Rage stomp) first.
extern bool g_gripHeld[2];
ULONGLONG g_gripLSince = 0, g_lastLeftPunch = 0;
float FistSqueeze() { static const float f = config::GetFloat("fist_squeeze", 0.85f); return f; }
// D-076 (owner: fists "when I have both fingers on the grip buttons - not pressing, 90 % of the finger detection on L1 +
// R1"): a hand is a fist when its middle finger (on the PSVR2 grip / L1 / R1) is curled >= fist_curl (0.9, the runtime's
// finger sensing) or its grip is pressed; punching needs BOTH hands to be fists
bool FistHand(const vr::HandState& H) {
    // D-077: LIVE owner session - fingers resting on the grips gave a middle-finger curl of 0.65-0.73 (never 0.9): 0.6
    // D-079 (owner: "make the finger detection lower"): LIVE 143 samples - fingers resting on the grip (not pressed):
    // middle / ring / little 0.45-0.58 (pressed 0.64-0.73, off the grip <= 0.24; the index pulling the trigger alone
    // drags the middle to ~0.35 but ring / little stay ~0.1-0.2). The mean of the three >= fist_curl 0.42.
    // D-080: LIVE owner session (252 punch motions missed, ~220 of them "fingers not on the grip"): resting values hover
    // 0.40-0.52 (often just under 0.42), off the grip 0.00-0.02 -> 0.25 (the trigger-only coupling stays ~0.21)
    static const float curl = config::GetFloat("fist_curl", 0.25f);
    if (!H.active) return false;
    // A Quest 3 tester (Virtual Desktop): "the fist hits don't register" - Meta's controllers have no finger sensing on
    // the grip (the log: "no finger data", curls 0), so fingers resting there were never seen and a fist needed the grip
    // pressed past fist_squeeze (0.85) - nearly the left shield's squeeze. Without finger data a light squeeze
    // (fist_squeeze_nofingers, 0.25) is the fist; the left shield still needs the firm grip (g_gripHeld, past 0.55, held)
    static const float lightSqueeze = config::GetFloat("fist_squeeze_nofingers", 0.25f);
    if (!H.fingersValid) return H.squeeze >= lightSqueeze;
    const float three = (H.fingerCurl[2] + H.fingerCurl[3] + H.fingerCurl[4]) / 3.f;
    return H.squeeze >= FistSqueeze() || three >= curl;
}
bool LeftShieldActive(const vr::HandState& L, ULONGLONG now, Mode mode) {
    if (!g_gripHeld[0] || OverShoulder(L)) return false;
    if (mode != Mode::Bare && mode != Mode::Rage) return true;
    static const ULONGLONG delay = (ULONGLONG)config::GetFloat("shield_delay_ms", 150.f);
    return g_gripLSince && now - g_gripLSince >= delay && now - g_lastLeftPunch >= 400;
}

// D-144 (owner: "in hand combat, if I swing too much on the Y axis it must not attack - I do upward hooks with the shield
// and they come out as fist attacks"): bare-handed / in Rage a hand moving mostly up or down (|vertical speed| above
// punch_vertical_max of its speed; default 0.7 = steeper than ~44 deg) is no punch - either hand. Weapons keep their
// chops / overhead swings
static bool TooVertical(const Swing& w, bool fists) {
    static const float k = config::GetFloat("punch_vertical_max", 0.7f);
    return fists && k > 0.f && k < 1.f && w.speed > 1e-3f && fabsf(w.v[1]) > k * w.speed;
}

// D-148 (owner: "depending on how hard you swing or hit with the axe, fists, Blades it decides light (R1) or heavy (R2)"):
// the gate still fires early in the motion (D-064), but the button is chosen at the swing's PEAK speed: as soon as the
// hand passes heavy_speed (weapons, default 6.0 m/s) / heavy_speed_fist (bare-handed and Rage, 5.5) -> R2 at once;
// otherwise R1 the moment the hand slows below 90 % of its peak, at the latest heavy_decide_ms (80) after the gate - a
// light swing waits a few samples at most. (Owner logs D-142..D-144: swings pass the gate at 2.5-6 m/s and peak 3-10 m/s.)
// gowvr.ini heavy_by_strength 0 = the old rules (trigger-held swing / chop heavies). The axe takes part too, although
// D-086 removed its trigger heavies (those stay off): the hard swing is the owner's new heavy.
// The whole rule hangs on ONE switch (owner: "make it easy to reverse"): gowvr.ini heavy_by_strength (1 on, 0 = exactly
// the pre-D-148 behaviour - that code path is kept untouched), live command "heavystrength on|off" (saved to the ini).
std::atomic<int> g_heavyByStrength{-1};  // -1 = not read from the ini yet
struct StrengthCfg { bool on; float weapon, fist; ULONGLONG decideMs; };
static StrengthCfg Strength() {
    static const float weapon = config::GetFloat("heavy_speed", 6.0f), fist = config::GetFloat("heavy_speed_fist", 5.5f);
    static const ULONGLONG decideMs = (ULONGLONG)config::GetFloat("heavy_decide_ms", 80.f);
    // D-152 (owner: "revert whatever I wanted with heavy attacks, I don't want them anymore"): off by default
    if (g_heavyByStrength.load() < 0) g_heavyByStrength = config::GetFloat("heavy_by_strength", 0.f) > 0.5f ? 1 : 0;
    return StrengthCfg{g_heavyByStrength.load() == 1, weapon, fist, decideMs};
}
static void ResolveStrength(Swing& w, int hand, ULONGLONG now, ULONGLONG lightCool) {
    if (!w.pend) return;
    if (w.speed > w.pendPeak) w.pendPeak = w.speed;
    const bool heavy = w.pendPeak >= w.pendThr;
    if (!heavy && w.speed >= 0.9f * w.pendPeak && now - w.pendAt < Strength().decideMs) return;  // still speeding up
    w.pend = false;
    QueueAttack(heavy ? 0 : XINPUT_GAMEPAD_RIGHT_SHOULDER, heavy, false, heavy ? w.pendHeavy : w.pendLight, now);
    w.cooldownUntil = heavy ? (w.cooldownUntil > now + 300 ? w.cooldownUntil : now + 300) : now + lightCool;
    ++(heavy ? g_heavy : g_swings);
    vr::XrHaptic(hand, heavy ? 0.9f : 0.6f, heavy ? 0.09f : 0.05f);
    static int logs = 0;
    if (logs++ < 120)
        Log("vrmap: %s hand peak %.2f m/s (heavy from %.2f) %llu ms after the gate -> %s", hand ? "right" : "left",
            w.pendPeak, w.pendThr, (unsigned long long)(now - w.pendAt), heavy ? "HEAVY (R2)" : "light (R1)");
}

float g_aimTrigger = 0.f;  // D-204: the left trigger's value (logged with the swings)
void UpdateSwing(const vr::HandState& R, const vr::HandState& L, XINPUT_GAMEPAD* g, ULONGLONG now, bool fresh,
                 bool aiming, bool gripR, bool gripL) {
    // D-064 (owner: "a bit of delay between the motion and the attack - predict it like the axe throw"): swings fire
    // early in the motion like the aimed throw (was 3.0 m/s over 0.40 m, i.e. near the end of the swing)
    // D-085 (owner: "all the attacks are still too easy to trigger, but don't make it too hard"): 2.8 m/s over 0.32 m
    // (was 2.4 / 0.26; D-064's late-firing values were 3.0 / 0.40) - roughly halfway back. Punches / the left hand keep
    // their relative factors
    static float swingSpeed = config::GetFloat("swing_speed", 2.8f), swingTravel = config::GetFloat("swing_travel", 0.32f);
    // D-028/D-029: an aimed throw fires early in the arm's forward motion; must go forward along the view
    static float throwSpeed = config::GetFloat("throw_speed", 2.5f), throwTravel = config::GetFloat("throw_travel", 0.25f);
    static float chargeMs = config::GetFloat("charge_ms", 350.f);
    static const ULONGLONG returnMs = (ULONGLONG)config::GetFloat("swing_return_ms", 700.f);  // D-084
    static const ULONGLONG lightCool = (ULONGLONG)config::GetFloat("swing_cooldown_ms", 300.f);
    // D-086 (owner: "remove the heavy attack with the axe - when you press R2 - I don't want it"): with the axe in F1 no
    // swing / chop is heavy (always a light attack), no overhead charge and no two-handed slam (both R2). Aimed L2 + R2
    // and the runic L1 + R2 stay. gowvr.ini f1_axe_heavy=1 brings the axe heavies back
    static const bool axeHeavy = config::GetFloat("f1_axe_heavy", 0.f) > 0.5f;
    const bool axeInHand = fp::AxeInHand();
    const bool trigR = R.active && R.trigger > 0.5f;
    const Mode mode = CurMode();
    const bool noAxeHeavy = mode == Mode::Axe && !axeHeavy;  // D-086
    const bool weapon = mode == Mode::Axe || mode == Mode::Blades;
    const bool shield = gripL && LeftShieldActive(L, now, mode);  // the left grip = L1 (block), see VrGamepad
    // D-068: punches need a fist (D-097: not with a throwable in the hand - the swing throws it)
    const bool fists = (mode == Mode::Bare || mode == Mode::Rage) && !fp::ContextThrowable();
    // D-079 (owner: "it's supposed to be if one hand is that finger holding, not both at the same time"): each hand
    // punches when ITS fingers rest on its grip
    // D-080: the finger sensing drops out mid-punch (LIVE: R 0.51 -> 0.02 within 18 ms while punching): a hand stays a
    // fist for fist_hold_ms (700) after its fingers last read as on the grip
    static ULONGLONG fistSeen[2] = {0, 0};
    static const ULONGLONG fistHold = (ULONGLONG)config::GetFloat("fist_hold_ms", 700.f);
    // Owner 2026-10-18 (Meta Quest, no finger sensing): squeezing the left grip lightly for a fist raised the shield by
    // accident - without finger data the RIGHT grip alone (a light squeeze) makes both hands fists, and the left grip is
    // only the shield
    // D-240 (owner: "for Meta Quest and controllers without finger sensing: instead of the right grip being squeezed,
    // HOLD the Y button and it closes both fists - but it must not close the left shield"): no finger data on either
    // hand -> both fists while the left upper button (Y) is held; the grips no longer make fists there (the left grip
    // is only the shield). gowvr.ini fist_on_y 0 = the right-grip squeeze as before
    static const bool fistOnY = config::GetFloat("fist_on_y", 1.f) > 0.5f;
    const bool noFingers = fistOnY && !L.fingersValid && !R.fingersValid;
    const bool fistR0 = noFingers ? L.b : FistHand(R);
    if (L.fingersValid ? FistHand(L) : fistR0) fistSeen[0] = now;
    if (fistR0) fistSeen[1] = now;
    const bool fistL = fistSeen[0] && now - fistSeen[0] <= fistHold, fistR = fistSeen[1] && now - fistSeen[1] <= fistHold;
    const bool fistsReady = fistR || fistL;
    {
        static int lastReady = 0;
        static int logs = 0;
        const int ready = (fistL ? 1 : 0) | (fistR ? 2 : 0);
        if (fists && ready != lastReady && logs < 120) {
            ++logs;
            Log("vrmap: fists L %s R %s (L squeeze %.2f fingers %.2f %.2f %.2f%s | R squeeze %.2f fingers %.2f %.2f %.2f%s)",
                fistL ? "READY" : "open", fistR ? "READY" : "open", L.squeeze, L.fingerCurl[2], L.fingerCurl[3], L.fingerCurl[4],
                L.fingersValid ? "" : " no finger data", R.squeeze, R.fingerCurl[2], R.fingerCurl[3], R.fingerCurl[4],
                R.fingersValid ? "" : " no finger data");
        }
        if (fists) lastReady = ready;
        // D-077: the finger values every 2 s while fists matter (to tune fist_curl from the owner's sessions)
        static ULONGLONG lastFingerLog = 0;
        static int fingerLogs = 0;
        if (fists && now - lastFingerLog >= 2000 && fingerLogs < 400) {
            lastFingerLog = now;
            ++fingerLogs;
            Log("vrmap: fingers L squeeze %.2f curl %.2f %.2f %.2f %.2f %.2f | R squeeze %.2f curl %.2f %.2f %.2f %.2f %.2f | fists %s",
                L.squeeze, L.fingerCurl[0], L.fingerCurl[1], L.fingerCurl[2], L.fingerCurl[3], L.fingerCurl[4], R.squeeze,
                R.fingerCurl[0], R.fingerCurl[1], R.fingerCurl[2], R.fingerCurl[3], R.fingerCurl[4], fistsReady ? "READY" : "open");
        }
    }
    // D-077: L2 + R2 held with the Blades = the game's L2+R2 spin (no move change): the Blades are free while it plays
    if (aiming && trigR && mode == Mode::Blades) fp::BladeAimHeld();
    Swing& s = g_swing;
    const bool upd = Kinematics(s, R, now, fresh);
    Kinematics(g_swingL, L, now, fresh);

    // ---- D-062 button moves (full animation): right trigger / right grip presses
    static bool trigPrev = false, gripPrev = false;
    const bool trigEdge = trigR && !trigPrev, gripEdge = gripR && !gripPrev;
    trigPrev = trigR;
    gripPrev = gripR;
    if (aiming && trigEdge && mode == Mode::Blades) fp::BladeThrowStart();  // D-063: L2 + R2 Blades throw
    if (!aiming && trigEdge) {
        if (mode == Mode::Bare && !shield) QueueAttack(0, true, true, "kick (right trigger)", now);
        else if (shield)
            QueueAttack(0, true, true, mode == Mode::Bare ? "shield + R2 (left grip + trigger)"
                                     : mode == Mode::Rage ? "Rage L1 + R2 (left grip + trigger)"
                                                          : "runic heavy (left grip + trigger)", now);
    }
    if (!aiming && gripEdge && shield && mode != Mode::Bare && !fp::AxeThrown() && !OverShoulder(R))
        QueueAttack(XINPUT_GAMEPAD_RIGHT_SHOULDER, false, true,
                    mode == Mode::Rage ? "Rage L1 + R1 (both grips)" : "runic light (both grips)", now);

    // ---- right hand: swings / throws
    if (upd) {
        // D-055: the aimed throw also with the Blades drawn (owner: "make the throw of the blade possible like before":
        // L2 + throwing motion); L2 + R1 is the game's Blades ranged attack. The axe-only release boost is skipped.
        // D-062: not while the right trigger is held (L2 + R2 = the heavy throw only)
        const bool bladesDrawn = fp::CurrentWeapon() == 2;
        const bool aimThrow = aiming && (axeInHand || bladesDrawn) && !trigR;
        const float minSpeed = aimThrow ? throwSpeed : swingSpeed, minTravel = aimThrow ? throwTravel : swingTravel;
        const bool downward = s.v[1] < -0.6f * s.speed;
        float viewFwd = 0.f;
        if (aimThrow && s.speed > 1e-3f) {
            vr::EyeViews ev = vr::XrLatestViews();
            if (ev.valid) {
                const float* hq = ev.pose[0].q;
                float hx = -2.f * (hq[0] * hq[2] + hq[3] * hq[1]), hz = -(1.f - 2.f * (hq[0] * hq[0] + hq[1] * hq[1]));
                const float hn = sqrtf(hx * hx + hz * hz);
                if (hn > 1e-3f) viewFwd = (s.v[0] * hx + s.v[2] * hz) / (hn * s.speed);
            }
        }
        // D-079 (owner: "fist motions aren't triggering that good"): a punch only had to lead with the controller's pointing
        // direction like a weapon swing (along > 0.3) - hooks (sideways) and uppercuts (up) failed it. Bare / Rage: any
        // direction but back toward the body (the retraction, along < punch_along -0.3)
        static const float punchAlong = config::GetFloat("punch_along", -0.3f);
        const float alongMin = fists && !aiming ? punchAlong : 0.3f;
        // (D-093 rule reverted - owner: "revert the swings as they were")
        const bool swingGate = s.armed && now >= s.cooldownUntil && s.speed > minSpeed && s.path > minTravel &&
                               (aimThrow ? viewFwd > 0.6f : (s.along > alongMin || downward)) && (!fists || fistR || aiming);
        if (swingGate && !aimThrow && TooVertical(s, fists && !aiming)) {
            static int vertLogs = 0;
            if (vertLogs++ < 40)
                Log("vrmap: right hand moving mostly up / down (vy %.2f of %.2f m/s) - no punch", s.v[1], s.speed);
        } else if (swingGate && !aimThrow && s.Returning(now, returnMs)) {
            static int retLogs = 0;
            if (retLogs++ < 40)
                Log("vrmap: right hand coming back %llu ms after its swing (speed %.2f) - not an attack",
                    (unsigned long long)(now - s.firedAt), s.speed);
        } else if (swingGate) {
            // D-148: by strength (not aimed): the chop rule is replaced; a trigger-held swing stays heavy (not the axe, D-086)
            const bool byStrength = Strength().on && !aimThrow && !aiming;
            const bool chop = !byStrength && !aimThrow && s.speed > minSpeed * 1.35f && downward && s.path > minTravel * 1.25f;
            // D-062 heavy: axe / Blades = trigger held + swing (or a chop), Rage = trigger held + punch, bare = never
            // (the bare-hand heavy is the kick on the trigger itself)
            const bool heavy = !aimThrow && !(noAxeHeavy && !aiming) && (weapon ? (chop || trigR) : mode == Mode::Rage ? trigR : false);
            // D-067 / D-075 spam: short cooldowns (was 900 / 450 ms); the gate fast-forwards a playing held attack
            // D-084: light 300 ms (was 150; swing_cooldown_ms)
            s.cooldownUntil = now + (aimThrow ? 250 : heavy ? 300 : lightCool);
            s.armed = false;
            s.Fired(now);
            bool pended = false;
            if (!aiming && weapon && shield) {
                // D-062: with a weapon and the shield up, runic attacks come from the buttons only
            } else if (aiming) {  // throws and aimed attacks: pressed at once (D-028 timing), not gated
                if (aimThrow && axeInHand) fp::ThrowStart();
                else if (aimThrow && bladesDrawn) fp::BladeThrowStart();  // D-063
                s.button = heavy ? 0 : XINPUT_GAMEPAD_RIGHT_SHOULDER;
                s.pressUntil = now + 90;
            } else if (byStrength && !heavy) {
                s.pend = true;
                s.pendAt = now;
                s.pendPeak = s.speed;
                s.pendThr = fists ? Strength().fist : Strength().weapon;
                const bool bash = mode == Mode::Bare && shield;
                s.pendLight = bash ? "shield bash (left grip + swing)" : fists ? "light punch" : "light swing";
                s.pendHeavy = bash ? "shield + heavy (left grip + hard swing)" : fists ? "heavy punch (hard)" : "heavy swing (hard)";
                pended = true;
                ResolveStrength(s, 1, now, lightCool);  // already past the heavy speed: at once
            } else {
                QueueAttack(heavy ? 0 : XINPUT_GAMEPAD_RIGHT_SHOULDER, heavy, false,
                            heavy ? (chop ? "heavy swing (chop)" : "heavy swing (trigger)")
                                  : mode == Mode::Bare && shield ? "shield bash (left grip + swing)" : "light swing", now);
            }
            if (!pended) {
                ++(heavy ? g_heavy : g_swings);
                vr::XrHaptic(1, heavy ? 0.9f : 0.6f, heavy ? 0.09f : 0.05f);
            }
            if (g_swings + g_heavy <= 20 || aimThrow)
                Log("vrmap: swing speed=%.2f path=%.2f along=%.2f vy=%.2f fwd=%.2f trig=%d aimTrig=%.2f mode=%d shield=%d -> %s",
                    s.speed, s.path, s.along, s.v[1], viewFwd, (int)trigR, g_aimTrigger, (int)mode, (int)shield,
                    aimThrow ? (axeInHand ? "F1 aimed throw" : "F1 aimed Blades throw") : pended ? "light or heavy by its peak"
                    : chop && heavy ? "heavy (chop)" : heavy ? "heavy (trigger)" : "light");
        }
    }
    ResolveStrength(s, 1, now, lightCool);  // D-148: decided at the swing's peak
    if (now < s.pressUntil) {
        if (s.button) g->wButtons |= s.button;
        else g->bRightTrigger = 255;
    }

    // ---- charge: trigger held with the hand above the head -> R2 held (released with the trigger or a down swing)
    // D-062: axe / Blades only (the bare-hand charge is not in F1), and only as a fresh attack (gate rule)
    static bool charging = false;
    if (!aiming && weapon && !noAxeHeavy && !shield && trigR && s.aboveSince && now - s.aboveSince >= (ULONGLONG)chargeMs && !charging &&
        !fp::CombatBusy()) {
        charging = true;
        vr::XrHaptic(1, 0.4f, 0.12f);
        Log("vrmap: charge (trigger held overhead) -> R2 held");
    }
    if (charging && (!trigR || aiming || (s.speed > 2.0f && s.v[1] < -0.6f * s.speed))) {
        charging = false;
        Log("vrmap: charge released");
    }
    if (charging) g->bRightTrigger = 255;

    // ---- grind: fast back-and-forth waving -> R1 held while it lasts
    // D-062: off by default (holding R1 starts the game's special follow-ups; the owner wants single attacks only)
    static const int grindRev = (int)config::GetFloat("grind_reversals", 4.f);  // 3 = two quick normal swings
    static const bool grindOn = config::GetFloat("grind_f1", 0.f) > 0.5f;
    if (grindOn && !aiming && Reversals(s, now, 800) >= grindRev && s.speed > 1.2f) {
        if (now >= s.grindUntil) Log("vrmap: grind (waving) -> R1 held");
        s.grindUntil = now + 200;
    }
    if (now < s.grindUntil) g->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

    // ---- left hand: punches / swings while the axe is not in the hand (bare-handed, Blades, Rage)
    Swing& l = g_swingL;
    // D-068: bare-handed / in Rage a left punch needs the left fist and overrides the (delayed) shield; with a weapon the
    // left hand swings as before while the shield is down
    static const float punchAlongL = config::GetFloat("punch_along", -0.3f);
    const bool leftGate = !axeInHand && !aiming && (fists ? fistL : !shield) && L.active && l.armed && now >= l.cooldownUntil &&
                          l.speed > swingSpeed * 0.85f && l.path > swingTravel * 0.6f &&
                          (l.along > (fists ? punchAlongL : 0.3f) || l.v[1] < -0.6f * l.speed) && !l.Returning(now, returnMs);
    if (leftGate && TooVertical(l, fists)) {
        static int vertLogsL = 0;
        if (vertLogsL++ < 40)
            Log("vrmap: left hand moving mostly up / down (vy %.2f of %.2f m/s) - no punch", l.v[1], l.speed);
    } else if (leftGate) {
        l.armed = false;
        l.cooldownUntil = now + lightCool;
        l.Fired(now);
        if (fists) g_lastLeftPunch = now;  // the shield stays down for this punch
        if (Strength().on) {  // D-148: light or heavy by the punch's peak speed
            l.pend = true;
            l.pendAt = now;
            l.pendPeak = l.speed;
            l.pendThr = fists ? Strength().fist : Strength().weapon;
            l.pendLight = "left punch / swing";
            l.pendHeavy = "left heavy punch / swing (hard)";
            ResolveStrength(l, 0, now, lightCool);
        } else {
            QueueAttack(XINPUT_GAMEPAD_RIGHT_SHOULDER, false, false, "left punch / swing", now);
            vr::XrHaptic(0, 0.6f, 0.05f);
            Log("vrmap: left punch/swing speed=%.2f path=%.2f -> R1", l.speed, l.path);
        }
    }
    ResolveStrength(l, 0, now, lightCool);  // D-148

    // ---- two-handed slam: both hands were above the head, now both going down fast together -> R2
    // D-062: with a weapon or in Rage (bare-handed the R2 is the kick, a button move)
    static ULONGLONG slamCool = 0;
    if (((weapon && !noAxeHeavy) || mode == Mode::Rage) && !aiming && now >= slamCool && R.active && L.active && s.lastAbove &&
        l.lastAbove && now - s.lastAbove < 600 && now - l.lastAbove < 600 && s.v[1] < -2.0f && l.v[1] < -2.0f) {
        slamCool = now + 1200;
        QueueAttack(0, true, false, "two-handed slam", now);
        s.cooldownUntil = now + 900;
        vr::XrHaptic(0, 0.9f, 0.1f);
        vr::XrHaptic(1, 0.9f, 0.1f);
        Log("vrmap: two-handed slam -> R2");
    }

    // ---- Spartan Rage "roar": both grips, both hands above the head for 0.8 s -> L3+R3
    static ULONGLONG roarUntil = 0, roarCool = 0;
    if (now >= roarCool && gripR && gripL && s.aboveSince && l.aboveSince && now - s.aboveSince >= 800 &&
        now - l.aboveSince >= 800) {
        roarCool = now + 3000;
        roarUntil = now + 150;
        vr::XrHaptic(0, 1.f, 0.2f);
        vr::XrHaptic(1, 1.f, 0.2f);
        Log("vrmap: rage roar (both fists overhead) -> L3+R3");
    }
    if (now < roarUntil) g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;

    // D-079 diagnostic: bare / Rage, a fast hand motion (peak > 1.8 m/s) that ended without a punch is logged with what
    // stopped it - to tune the punch motion from the owner's sessions
    if (fists && !aiming) {
        struct Miss { bool in = false, fired = false, fist = false, armed = false, cool = false, vert = false; float sp = 0, path = 0, along = 0; };
        static Miss miss[2];
        static int missLogs = 0;
        const Swing* sw[2] = {&l, &s};
        const bool fist[2] = {fistL, fistR};
        const float need = swingSpeed;
        for (int h = 0; h < 2; ++h) {
            Miss& m = miss[h];
            const Swing& w = *sw[h];
            if (w.speed > 1.8f) {
                if (!m.in) m = Miss{true};
                if (w.firedAt == now) m.fired = true;
                if (w.speed > m.sp) {
                    m.sp = w.speed; m.path = w.path; m.along = w.along;
                    m.fist = fist[h]; m.armed = w.armed || w.firedAt == now; m.cool = now < w.cooldownUntil;
                    m.vert = TooVertical(w, true);
                }
            } else if (m.in && w.speed < 1.0f) {
                if (!m.fired && missLogs < 300) {
                    ++missLogs;
                    const float speedNeed = h ? need : need * 0.85f, pathNeed = h ? swingTravel : swingTravel * 0.6f;
                    Log("vrmap: %s punch MISSED - peak %.2f m/s (need %.2f) path %.2f (need %.2f) along %.2f (need > %.2f)%s%s%s%s",
                        h ? "right" : "left", m.sp, speedNeed, m.path, pathNeed, m.along, punchAlongL,
                        m.fist ? "" : " | fingers not on the grip", m.armed ? "" : " | not re-armed", m.cool ? " | cooldown" : "",
                        m.vert ? " | mostly up / down (no punch)" : "");
                }
                m = Miss{};
            }
        }
    }
}
bool g_gripHeld[2] = {false, false};  // (declared extern above UpdateSwing)

// ---- over-the-shoulder grab = draw / put away the Leviathan axe (owner request): the right grip closing while the
// right hand is behind the head's face plane and above the chest (where the axe hangs). Draw = a short L2 tap (aiming
// raises the axe into the hand without attacking); put away = the sheathe button (gowvr.ini sheathe_button, XInput
// mask, default D-pad down). The drawn state is tracked here: a grab toggles it, an attack means drawn.
struct Draw { bool prevGrip = false, drawn = false; ULONGLONG pressUntil = 0, cooldownUntil = 0; WORD button = 0; BYTE trig = 0; };

// The game's action -> pad button table (STATIC, decompile input_002.c sub_14073fa40 / sub_1407519c0): action names at
// 0x140e0fd10 (0 Aim, 1 HeavyAttack, 2 Block, 3 LightAttack, 4 AxeRecall, 5 Interact, 6 Evade, 7 SonAction, 8 Map,
// 9 Sprint, 10 LockOn, 11 Pause, 12 ArrowType, 13 Sheathe, 14 QuickTurn, 15 Blades), the player's mapping as bytes at
// 0x142506658 + action (runtime, from the button settings), physical order L2 R2 L1 R1 Triangle Circle Cross Square
// Select L3 R3 Start Up Right Down Left. Returned as an XInput button mask, or a trigger (lt/rt) for L2/R2.
WORD ActionButton(int action, BYTE* lt, BYTE* rt) {
    *lt = *rt = 0;
    const uint8_t* table = (uint8_t*)GameVa(0x142506658ull);
    const int phys = table[action];
    static const WORD kMask[16] = {0, 0, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT};
    if (phys == 0) { *lt = 255; return 0; }
    if (phys == 1) { *rt = 255; return 0; }
    return phys < 16 ? kMask[phys] : 0;
}
Draw g_draw;
std::atomic<uint64_t> g_draws{0};

// Owner 2026-10-18: either hand draws either weapon - the SHOULDER decides: over the RIGHT shoulder = equip / put away
// the axe, over the LEFT shoulder = the Blades of Chaos. A draw needs the hand behind the head (OverShoulder); the side is
// which side of the head's middle the hand is on. The right grip still recalls the thrown axe - not while it reaches
// over the left shoulder for the Blades.
// D-043 / D-044: the Blades are the game's Blades action (default D-pad left), a toggle - pressed with the Blades in
// hand it puts them away (LIVE: Blades -> D-pad left -> Bare). The current weapon comes from the game
// (fp::CurrentWeapon, the hash GetCurrentWeapon compares), so nothing is guessed.
struct BladesDraw { ULONGLONG pressUntil = 0, cooldownUntil = 0; WORD button = 0; BYTE trig = 0; };
BladesDraw g_blades;
bool g_prevGripL = false;
enum class Shoulder { None, Right, Left };
Shoulder ShoulderOf(const vr::HandState& H, float* behind, float* up) {
    float right = 0.f;
    if (!OverShoulder(H, behind, up, &right)) return Shoulder::None;
    return right >= 0.f ? Shoulder::Right : Shoulder::Left;
}
const char* HandName(int hand) { return hand == 1 ? "right" : "left"; }

void NoSwingAfterDraw(ULONGLONG now) {
    // bringing the hands forward from the back is not an attack / punch
    if (g_swing.cooldownUntil < now + 700) g_swing.cooldownUntil = now + 700;
    if (g_swingL.cooldownUntil < now + 900) g_swingL.cooldownUntil = now + 900;
}

void AxeToggle(int hand, float behind, float up, ULONGLONG now) {
    // the real state: is the axe on the hand (attachment check in fp/body.cpp)?
    g_draw.drawn = !fp::AxeInHand();
    BYTE lt = 0, rt = 0;
    // D-102 (owner: "when I reach over my back it needs to EQUIP the axe, not call it - the Blades equip very well"): with
    // the axe on the back the game's own Sheathe / draw binding (action 13, D-pad right) - LIVE harness 2026-10-04:
    // sheathed -> one press -> the axe drawn in the hand ~0.25 s later, the game's equip; the recall (action 4) only while
    // the Blades are out (it swaps them for the axe)
    const int action = g_draw.drawn ? (fp::CurrentWeapon() == 2 ? 4 : 13) : 13;
    fp::AxeRequest(g_draw.drawn);
    g_draw.button = ActionButton(action, &lt, &rt);
    g_draw.trig = (BYTE)((lt ? 1 : 0) | (rt ? 2 : 0));
    Log("vrmap: %s -> game action %d = pad buttons 0x%04x triggers %d (table byte %d)",
        g_draw.drawn ? "draw" : "put away", action, g_draw.button, g_draw.trig,
        (int)((uint8_t*)GameVa(0x142506658ull))[action]);
    g_draw.pressUntil = now + (g_draw.drawn ? 200 : 400);
    g_draw.cooldownUntil = now + 900;
    NoSwingAfterDraw(now);
    ++g_draws;
    vr::XrHaptic(hand, 0.6f, 0.08f);
    Log("vrmap: %s hand over the right shoulder -> %s axe (behind %.2f m, up %.2f m)", HandName(hand),
        g_draw.drawn ? "draw" : "put away", behind, up);
}

void BladesToggle(int hand, float behind, float up, ULONGLONG now) {
    const int w = fp::CurrentWeapon();
    BYTE lt = 0, rt = 0;
    g_blades.button = ActionButton(15, &lt, &rt);
    g_blades.trig = (BYTE)((lt ? 1 : 0) | (rt ? 2 : 0));
    g_blades.pressUntil = now + 200;
    g_blades.cooldownUntil = now + 900;
    NoSwingAfterDraw(now);
    vr::XrHaptic(hand, 0.6f, 0.08f);
    Log("vrmap: %s hand over the left shoulder -> %s the Blades (weapon was %d, pad buttons 0x%04x, behind %.2f m, up %.2f m)",
        HandName(hand), w == 2 ? "put away" : "draw", w, g_blades.button, behind, up);
}

// a grip press behind the head: the shoulder it is over picks the weapon
void ShoulderGrab(const vr::HandState& H, int hand, ULONGLONG now) {
    float behind = 0.f, up = 0.f;  // metres behind the face plane (horizontal) / above the eyes
    switch (ShoulderOf(H, &behind, &up)) {
    case Shoulder::Right:
        if (now >= g_draw.cooldownUntil) AxeToggle(hand, behind, up, now);
        break;
    case Shoulder::Left:
        if (now >= g_blades.cooldownUntil) BladesToggle(hand, behind, up, now);
        break;
    default: break;
    }
}

void UpdateDraw(const vr::HandState& R, const vr::HandState& L, bool gripHeld, bool gripHeldL, XINPUT_GAMEPAD* g,
                ULONGLONG now) {
    const bool edge = gripHeld && !g_draw.prevGrip;
    g_draw.prevGrip = gripHeld;
    const bool edgeL = gripHeldL && !g_prevGripL;
    g_prevGripL = gripHeldL;
    if (edge) {
        float rb = 0.f, ru = 0.f;
        if (fp::AxeThrown() && ShoulderOf(R, &rb, &ru) != Shoulder::Left) {
            if (now >= g_draw.cooldownUntil) {
                // owner: with the axe thrown, the right grip calls it back (same as the left controller's recall button)
                BYTE lt = 0, rt = 0;
                fp::AxeRequest(true);
                g_draw.button = ActionButton(4, &lt, &rt);
                g_draw.trig = (BYTE)((lt ? 1 : 0) | (rt ? 2 : 0));
                g_draw.pressUntil = now + 200;
                g_draw.cooldownUntil = now + 600;
                vr::XrHaptic(1, 0.5f, 0.06f);
                Log("vrmap: right grip with the axe thrown -> recall (pad buttons 0x%04x)", g_draw.button);
            }
        } else {
            ShoulderGrab(R, 1, now);
        }
    }
    if (edgeL) ShoulderGrab(L, 0, now);
    if (now < g_draw.pressUntil) {
        g->wButtons |= g_draw.button;
        if (g_draw.trig & 1) g->bLeftTrigger = 255;
        if (g_draw.trig & 2) g->bRightTrigger = 255;
    }
    if (now < g_blades.pressUntil) {
        g->wButtons |= g_blades.button;
        if (g_blades.trig & 1) g->bLeftTrigger = 255;
        if (g_blades.trig & 2) g->bRightTrigger = 255;
    }
}
// D-043: hand behind the head's face plane and not below the chest - the over-shoulder grab zone, for either hand
bool OverShoulder(const vr::HandState& H, float* behindOut, float* upOut, float* rightOut) {
    if (!H.gripValid) return false;
    vr::EyeViews v = vr::XrLatestViews();
    if (!v.valid) return false;
    float head[3], d[3];
    for (int i = 0; i < 3; ++i) head[i] = 0.5f * (v.pose[0].p[i] + v.pose[1].p[i]);
    for (int i = 0; i < 3; ++i) d[i] = H.grip.p[i] - head[i];
    const float* q = v.pose[0].q;
    float fx = -2.f * (q[0] * q[2] + q[3] * q[1]), fz = -(1.f - 2.f * (q[0] * q[0] + q[1] * q[1]));
    const float n = sqrtf(fx * fx + fz * fz);
    if (n < 1e-3f) return false;
    fx /= n; fz /= n;
    const float behind = -(d[0] * fx + d[2] * fz), up = d[1];
    if (behindOut) *behindOut = behind;
    if (upOut) *upOut = up;
    if (rightOut) *rightOut = d[0] * -fz + d[2] * fx;  // to the head's right (its forward turned 90 deg)
    return behind > 0.02f && up > -0.35f;
}

uint64_t g_lastSerial = 0;
ULONGLONG g_lastSerialTick = 0;

SHORT Axis(float v) { return (SHORT)lrintf((v > 1.f ? 1.f : v < -1.f ? -1.f : v) * 32767.f); }

// radial deadzone, rescaled, angle preserved, magnitude clamped to 1
void Stick(const float* in, float dz, SHORT* x, SHORT* y) {
    float r = sqrtf(in[0] * in[0] + in[1] * in[1]);
    if (r <= dz) { *x = *y = 0; return; }
    float s = fminf((r - dz) / (1.f - dz), 1.f) / r;
    *x = Axis(in[0] * s);
    *y = Axis(in[1] * s);
}

BYTE Trigger(float v, float threshold) {
    if (v <= threshold) return 0;
    return (BYTE)lrintf(fminf((v - threshold) / (1.f - threshold), 1.f) * 255.f);
}

// D-218: the left hand held at the head (combat HUD shown); written by VrGamepad, read by the HUD tick (stereo/mutate.cpp)
std::atomic<bool> g_hudPeek{false};
std::atomic<ULONGLONG> g_hudPeekAt{0};

}  // namespace

// D-218: only while the controller mapping keeps running (a paused headset / VR off never leaves the bars forced on)
bool HudPeek() { return g_hudPeek.load() && GetTickCount64() - g_hudPeekAt.load() < 300; }

bool VrGamepad(XINPUT_GAMEPAD* g) {
    if (!g_on.load()) return false;
    vr::InputSnapshot s = vr::XrInput();
    ULONGLONG now = GetTickCount64();
    const bool fresh = s.serial != g_lastSerial;
    if (fresh) { g_lastSerial = s.serial; g_lastSerialTick = now; }
    if (!s.focused || now - g_lastSerialTick > 250) {  // stale or not focused: neutral, and forget held grips
        g_gripHeld[0] = g_gripHeld[1] = false;
        return false;
    }
    *g = XINPUT_GAMEPAD{};
    const vr::HandState& L = s.hand[0];
    const vr::HandState& R = s.hand[1];
    // D-079 (owner: "press both thumbsticks -> F1 mode, hold them -> F3 mode, as on/off switches"): both stick clicks
    // together - released before stick_combo_hold_ms (700) toggles gameplay first person (F1), held that long toggles
    // cutscene first person (F3). While the combo is down neither click reaches the game (no sprint, no lock-on / D-pad,
    // no L3+R3 Spartan Rage); it ends when both are up. A lone left click (sprint) waits 60 ms for the right one, and a
    // shorter click is sent afterwards as a 100 ms pulse, so the combo never taps sprint.
    // D-094 (owner: "when Spartan Rage is ready (L3+R3), pressing both thumbsticks must not turn me into F1"): with the
    // Rage meter full (hero meter "Blood", fp::CombatRageReady) or in Rage (L3+R3 = leave it) the combo is the game's
    // L3+R3 for as long as both are down - no F1 / F3 toggle
    // D-097 (owner: "make the two thumbsticks not activate F1 and F3 anymore"): both stick clicks are the game's L3+R3
    // (Spartan Rage in / out) for as long as both are down; the combo still keeps either click from being a sprint,
    // a lock-on, a D-pad chord or a Blade flip. F1 / F3 moved to the right trigger near the head (below)
    static bool combo = false, comboRage = false;
    static ULONGLONG lClickSince = 0, l3PulseUntil = 0;
    static bool lClickSent = false;
    const bool lc = L.active && L.stickClick, rc = R.active && R.stickClick;
    {
        if (lc && rc) {
            if (!combo) {
                combo = true;
                comboRage = true;
                float rv = -1.f, rm = -1.f;
                const bool meter = fp::CombatRageMeter(&rv, &rm);
                Log("vrmap: both sticks down -> L3+R3 to the game (Rage meter %s %.1f / %.1f, in Rage %d)", meter ? "read" : "NOT read",
                    rv, rm, (int)fp::CombatInRage());
            }
        } else if (combo && !lc && !rc) {
            combo = false;
            comboRage = false;
        }
        // D-087 (owner: "press a thumbstick: Kratos flips that Blade and holds it upside down - left stick only the left
        // Blade"): F1 with the Blades - a lone left click (not the combo) with the stick resting flips the left Blade on
        // release; the click still reaches the game as before (a sprint needs the stick pushed anyway)
        static bool lClickCombo = false, lClickMoved = false;
        if (lc) {
            if (!lClickSince) { lClickSince = now; lClickSent = false; lClickCombo = false; lClickMoved = false; }
            if (fmaxf(fabsf(L.stick[0]), fabsf(L.stick[1])) > 0.3f) lClickMoved = true;
        } else {
            if (lClickSince && !lClickSent && !combo) l3PulseUntil = now + 100;  // a quick sprint click
            if (lClickSince && !lClickCombo && !lClickMoved && fp::FpMode() == 1 && CurMode() == Mode::Blades) {
                fp::ArmsBladeFlip(0);
                vr::XrHaptic(0, 0.4f, 0.04f);
            }
            lClickSince = 0;
        }
        if (combo) { lClickSent = true; lClickCombo = true; }  // this click belongs to the combo: never a sprint / flip
    }
    // D-097 (owner: "the right VR controller trigger near your head - that's F1, and if you hold the trigger near your
    // head it turns F3"): the right trigger pressed while the right hand is at the head (within head_zone_m 0.25 m of
    // the eyes, not more than 0.10 m behind the face plane - the over-shoulder grab zone is further back - and between
    // 0.25 m below and 0.10 m above the eyes) starts the gesture: released before head_hold_ms (700) toggles gameplay
    // first person (F1), held that long toggles cutscene first person (F3, once). While the gesture runs the trigger
    // never reaches the game (no R2, no heavy / charge, no Freezing Throw)
    {
        static bool headG = false, headHeld = false, trigDown = false;
        static ULONGLONG headSince = 0;
        static const float zone = config::GetFloat("head_zone_m", 0.25f);
        static const ULONGLONG headHoldMs = (ULONGLONG)config::GetFloat("head_hold_ms", 700.f);
        const bool trigNow = R.active && (trigDown ? R.trigger > 0.30f : R.trigger > 0.60f);
        const bool trigEdge = trigNow && !trigDown;
        trigDown = trigNow;
        if (trigEdge && !headG && R.gripValid) {
            vr::EyeViews v = vr::XrLatestViews();
            if (v.valid) {
                float d[3];
                for (int i = 0; i < 3; ++i) d[i] = R.grip.p[i] - 0.5f * (v.pose[0].p[i] + v.pose[1].p[i]);
                const float* q = v.pose[0].q;
                float fx = -2.f * (q[0] * q[2] + q[3] * q[1]), fz = -(1.f - 2.f * (q[0] * q[0] + q[1] * q[1]));
                const float fn = sqrtf(fx * fx + fz * fz);
                if (fn > 1e-3f) {
                    fx /= fn; fz /= fn;
                    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    const float behind = -(d[0] * fx + d[2] * fz), up = d[1];
                    if (dist < zone && behind < 0.10f && up > -0.25f && up < 0.10f) {
                        headG = true;
                        headHeld = false;
                        headSince = now;
                        vr::XrHaptic(1, 0.3f, 0.03f);
                        Log("vrmap: right trigger at the head (%.2f m from the eyes, behind %.2f, up %.2f) - F1 on release, F3 if held",
                            dist, behind, up);
                    }
                }
            }
        }
        if (headG) {
            if (trigNow) {
                if (!headHeld && now - headSince >= headHoldMs) {
                    headHeld = true;
                    fp::SetCutsceneFP(!fp::CutsceneFP());
                    vr::XrHaptic(1, 0.8f, 0.15f);
                    Log("vrmap: right trigger held at the head -> F3 (cutscene first person) %s", fp::CutsceneFP() ? "ON" : "off");
                }
            } else {
                if (!headHeld) {
                    fp::SetGameplayFP(!fp::GameplayFP());
                    vr::XrHaptic(1, 0.6f, 0.06f);
                    Log("vrmap: right trigger at the head -> F1 (gameplay first person) %s", fp::GameplayFP() ? "ON" : "off");
                }
                headG = false;
            }
            s.hand[1].trigger = 0.f;  // (R refers to it) the gesture's trigger is not the game's
        }
    }
    // D-218 (owner: "when I move my left hand to my head and hold it there, show my health, Spartan Rage and runic
    // bars"): the left hand within head_peek_m (0.22 m) of the eyes - in front of / beside the head (not more than 0.10 m
    // behind the face plane: the over-shoulder Blades grab is further back), between 0.30 m below and 0.15 m above
    // the eyes - for head_peek_ms (250) shows the combat HUD until it leaves (0.05 m hysteresis). No button involved;
    // the hand's buttons still reach the game. A light buzz on the left controller when the bars come up
    {
        static const float zone = config::GetFloat("head_peek_m", 0.22f);
        static const ULONGLONG dwell = (ULONGLONG)config::GetFloat("head_peek_ms", 250.f);
        static ULONGLONG since = 0;
        bool in = false;
        float dist = 0.f;
        if (L.gripValid) {
            vr::EyeViews v = vr::XrLatestViews();
            if (v.valid) {
                float d[3];
                for (int i = 0; i < 3; ++i) d[i] = L.grip.p[i] - 0.5f * (v.pose[0].p[i] + v.pose[1].p[i]);
                const float* q = v.pose[0].q;
                float fx = -2.f * (q[0] * q[2] + q[3] * q[1]), fz = -(1.f - 2.f * (q[0] * q[0] + q[1] * q[1]));
                const float fn = sqrtf(fx * fx + fz * fz);
                if (fn > 1e-3f) {
                    fx /= fn; fz /= fn;
                    dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    const float behind = -(d[0] * fx + d[2] * fz), up = d[1];
                    const float lim = g_hudPeek.load() ? zone + 0.05f : zone;
                    in = dist < lim && behind < 0.10f && up > -0.30f && up < 0.15f;
                }
            }
        }
        if (!in) since = 0;
        else if (!since) since = now;
        const bool peek = in && now - since >= dwell;
        g_hudPeekAt = now;
        if (peek != g_hudPeek.load()) {
            g_hudPeek = peek;
            if (peek) vr::XrHaptic(0, 0.25f, 0.03f);
            Log("vrmap: left hand %s the head (%.2f m) - combat HUD %s (D-218)", peek ? "held at" : "away from", dist,
                peek ? "shown" : "back to the game's setting");
        }
    }
    // D-097: scenes that ask for plain button presses (the ogre ride, doors / chests, cranks, combat-sync scenes ...)
    // D-164 (owner: "on the flat cinema screen - loading / options - I can't press R1"): in F1 the grip / trigger are
    // swing gestures, not R1 / R2; the 2D screen (pause, options, loading) takes plain buttons like the plain-button scenes
    // (grip = R1 / L1, trigger = R2, no gestures)
    const bool plain = fp::FpMode() == 1 && (fp::ContextPlainButtons() || stereo::Screen2DFrame());
    for (int h = 0; h < 2; ++h) {  // grip hysteresis: press above 0.55, release below 0.40
        float q = s.hand[h].squeeze;
        g_gripHeld[h] = s.hand[h].active && (g_gripHeld[h] ? q > 0.40f : q > 0.55f);
    }
    if (!g_gripHeld[0]) g_gripLSince = 0;
    else if (!g_gripLSince) g_gripLSince = now;
    // D-204 (owner: "after throwing the axe a lot, the attack motion doesn't trigger attacks - I wait, unequip and equip
    // and then it usually works"; owner log: the axe caught and thrown again about every second from 861 s on, no aiming
    // in the game, the swings "-> F1 aimed throw" with the right trigger free): aiming was any left-trigger pull past
    // 0.25 - a finger resting on the (resisting) PSVR2 trigger reads that - so every swing was an aimed throw. Now one aim
    // state with hysteresis: on past aim_on 0.45, off below aim_off 0.30 - the game's L2, the aimed-trigger R2 rule and the
    // swing classifier all use it
    static const float aimOn = config::GetFloat("aim_on", 0.45f), aimOff = config::GetFloat("aim_off", 0.30f);
    static bool aimLatch = false;
    {
        const float lt = L.active ? L.trigger : 0.f;
        if (!aimLatch && lt > aimOn) aimLatch = true;
        else if (aimLatch && lt < aimOff) aimLatch = false;
    }
    g_aimTrigger = L.active ? L.trigger : 0.f;
    if (L.active) {
        float ls[2] = {L.stick[0], L.stick[1]};
        // D-235 (owner: "in the settings flat screen the left stick's input is reversed - horizontal is vertical and
        // vertical is horizontal - PSVR2, and Quest 3 users too"): the rotation is set only while the first-person view is
        // built (mutate.cpp), so on a 2D screen (menus, settings) the LAST gameplay F1 value stayed - about 90 deg with
        // the head turned that way swapped the axes. Only in gameplay F1 and not on a 2D screen now
        const bool locoOn = fp::FpMode() == 1 && !stereo::Screen2DFrame();
        if (float a = g_locoYaw.load(); a != 0.f && locoOn) {  // forward = the view direction (first person)
            static const float sgn = config::GetFloat("loco_sign", 1.f) >= 0.f ? 1.f : -1.f;
            float ca = cosf(a * sgn), sa = sinf(a * sgn);
            ls[0] = L.stick[0] * ca - L.stick[1] * sa;
            ls[1] = L.stick[0] * sa + L.stick[1] * ca;
        }
        Stick(ls, 0.15f, &g->sThumbLX, &g->sThumbLY);
        g->bLeftTrigger = aimLatch ? Trigger(L.trigger, 0.25f) : 0;        // aim (D-204: only while aiming)
        // shield; in F1 not while the left hand reaches over the shoulder for the Blades (D-043)
        // D-068: bare-handed / in Rage (F1) the shield waits a moment after the fist closes (see LeftShieldActive)
        if (fp::FpMode() == 1 && !plain ? LeftShieldActive(L, now, CurMode()) : g_gripHeld[0]) g->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (L.a) g->wButtons |= XINPUT_GAMEPAD_X;                          // Atreus
        // D-240: bare-handed / in Rage without finger sensing, Y held is the fists - not the game's Y (unless the axe is out
        // of the hand, thrown: then it recalls as always)
        const bool yIsFists = config::GetFloat("fist_on_y", 1.f) > 0.5f && !L.fingersValid && !R.fingersValid &&
                              fp::FpMode() == 1 && (CurMode() == Mode::Bare || CurMode() == Mode::Rage) && !fp::AxeThrown();
        if (L.b && !yIsFists) g->wButtons |= XINPUT_GAMEPAD_Y;             // recall axe / triangle
        static bool lbPrev = false;
        if (L.b && !lbPrev) fp::AxeRequest(true);                          // a recall: the axe may land in the IK hand
        lbPrev = L.b;
        // sprint (D-079: not while the both-sticks combo is down; a lone click after 60 ms or as a pulse, see above)
        if (lc && !combo && !lClickSent && now - lClickSince >= 60) lClickSent = true;
        if ((lc && !combo && lClickSent) || now < l3PulseUntil) g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (L.trackpadForce > 0.6f) g->wButtons |= XINPUT_GAMEPAD_START;   // options
        if (L.menu) g->wButtons |= XINPUT_GAMEPAD_START;  // D-236: the controller's own menu button (Quest left) = Start / Options
    }
    if (R.active) {
        // F1 gameplay: smooth turning at turn_speed, or snap turning (turning.cpp; the VR settings panel)
        const bool f1Turn = fp::FpMode() == 1 && !plain;
        float rs[2] = {R.stick[0], R.stick[1]};
        if (f1Turn) rs[0] = input::TurnStickX(R.stick[0]);
        Stick(rs, 0.15f, &g->sThumbRX, &g->sThumbRY);
        if (fabsf(R.stick[0]) < 0.15f && fabsf(R.stick[1]) < 0.15f) {  // camera steering only while the player is off it
            g->sThumbRX = Axis(g_camSyncX.load());
            g->sThumbRY = Axis(g_camSyncY.load());
        }
        // heavy attack: outside F1 the plain trigger; in F1 the trigger modifies swings (heavy) and charges overhead,
        // except while aiming (Freezing Throw / Burst = L2 + R2 directly)
        // D-097: in the plain-button scenes and with a throwable in the hand the trigger is R2 as well
        if (fp::FpMode() != 1 || plain || fp::ContextThrowable() || aimLatch)
            g->bRightTrigger = Trigger(R.trigger, 0.25f);
        // F1: right grip = holding the axe, attacks come from physical swings (UpdateSwing). Outside F1 the mod is the
        // plain third-person game (owner, 2026-09-29): no gestures, the right grip is simply R1 (light attack).
        // D-097: the plain-button scenes too (the ogre ride's R1, mashing at doors / chests)
        if ((fp::FpMode() != 1 || plain) && g_gripHeld[1]) g->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (R.a) g->wButtons |= XINPUT_GAMEPAD_A;                          // interact / jump
        if (R.b) g->wButtons |= XINPUT_GAMEPAD_B;                          // dodge
        if (R.trackpadForce > 0.6f) g->wButtons |= XINPUT_GAMEPAD_BACK;    // map
        // D-035 D-pad (every mode; motion controllers have none - owner: "how do I equip the blades?"): hold the right
        // stick click and push the right stick = D-pad in that direction. Game defaults (action table 0x142506658,
        // LIVE): Up = arrow type, Right = sheathe, Down = quick turn, Left = Blades. A click released without pushing
        // = R3 (lock on), sent on release.
        static bool chord = false, used = false;
        static ULONGLONG r3Until = 0;
        // D-205 (owner: "I couldn't do any attacks with my axe for ~10 s - I was swinging but nothing happened"; owner log:
        // "right stick click + push -> D-pad right (sheathe)" at 382.3 s, then 16 s of swings judged as open-hand punches,
        // 6 such chords in the session - 2 sheathes, 4 quick turns - all while turning): the chord took a click and a
        // push in either order, so pressing the stick down a little while turning with it fired the D-pad. Now the chord
        // needs the stick CENTRED when the click starts (then push while holding); a click that starts while the stick
        // is pushed is neither D-pad nor R3
        static bool fromCentre = false;
        if (combo) used = true;  // D-079: a click of the both-sticks combo is never R3 / D-pad
        if (R.stickClick) {
            if (!chord) {
                chord = true;
                used = combo;
                fromCentre = fmaxf(fabsf(R.stick[0]), fabsf(R.stick[1])) < 0.35f;
                if (!fromCentre) {
                    used = true;  // (no R3 on release either)
                    static int accLogs = 0;
                    if (accLogs++ < 20) Log("vrmap: right stick clicked while pushed (turning) - ignored, no D-pad / R3 (D-205)");
                }
            }
            const float x = R.stick[0], y = R.stick[1];
            if (!combo && fromCentre && fmaxf(fabsf(x), fabsf(y)) > 0.6f) {
                const WORD dp = fabsf(x) > fabsf(y) ? (x > 0.f ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT)
                                                    : (y > 0.f ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN);
                if (!used) Log("vrmap: right stick click + push -> D-pad %s", dp == XINPUT_GAMEPAD_DPAD_LEFT ? "left (Blades)"
                    : dp == XINPUT_GAMEPAD_DPAD_RIGHT ? "right (sheathe)" : dp == XINPUT_GAMEPAD_DPAD_UP ? "up" : "down");
                used = true;
                g->wButtons |= dp;
            }
            if (fromCentre) g->sThumbRX = g->sThumbRY = 0;  // no turning while picking a direction (D-205: a deliberate chord only)
        } else if (chord) {
            chord = false;
            if (!used) {
                r3Until = now + 150;
                fp::CombatFinisherPressed();  // D-080: a stun finisher must not meet an attack hold
                // D-087: F1 with the Blades - the plain right click (not the D-pad chord, not the combo) also flips the
                // right Blade; R3 still goes to the game (lock-on / finisher)
                if (fp::FpMode() == 1 && CurMode() == Mode::Blades) {
                    fp::ArmsBladeFlip(1);
                    vr::XrHaptic(1, 0.4f, 0.04f);
                }
            }
        }
        if (now < r3Until) g->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;     // lock on
    }
    if (combo && comboRage && lc && rc) g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;  // D-094
    // D-087: a flipped Blade goes back to the normal grip when the Blades are put away / switched (or F1 ends)
    if (fp::FpMode() != 1 || fp::CurrentWeapon() != 2) fp::ArmsBladeFlipReset();
    // motion gestures (swings, aimed throws, over-shoulder grab, grip recall) exist only in gameplay F1 (D-097: not in the
    // plain-button scenes, where the grip / trigger are R1 / R2)
    if (fp::FpMode() == 1 && !plain) {
        UpdateSwing(R, L, g, now, fresh, aimLatch, g_gripHeld[1], g_gripHeld[0]);
        GateTick(g, now);
        if (R.active || L.active) UpdateDraw(R, L, R.active && g_gripHeld[1], L.active && g_gripHeld[0], g, now);
    } else {
        g_prevGripL = g_gripHeld[0];
        g_blades.pressUntil = 0;
        g_pend = Pending{};  // D-062: no queued F1 attack survives leaving F1
        g_sendUntil = 0;
        g_swing.prevT = 0;  // no stale velocity when F1 comes back
        g_swingL.prevT = 0;
        g_draw.prevGrip = g_gripHeld[1];
        g_draw.pressUntil = 0;
    }
    // D-162 (owner: "for the PlayStation and Xbox schemes: pressing all 4 buttons - A X Y B / cross square triangle circle -
    // triggers the Options button; the PSVR2 Sense controllers don't have it"): all four face buttons held = Options
    // (Start) for as long as they are; the face buttons themselves go nowhere from then until all four are released (no
    // jump / dodge / Atreus / recall from the chord). (The Index trackpad press stays Options too.)
    // Owner 2026-10-17: a TAP of the four opens the game menu (map, skills, inventory: View / touchpad), HOLDING them
    // (chord_hold_ms, 600) the screen the keyboard's Esc opens (owner: "when you hold it it goes to the screen when you
    // press Esc" - not Options' screen): Esc is posted to the game window, released 0.1 s later. The tap is sent when the
    // four are released, as a short press of View.
    {
        static bool chordOn = false, holdSent = false;
        static ULONGLONG chordSince = 0, pulseUntil = 0, escUpAt = 0;
        if (escUpAt && now >= escUpAt) {
            escUpAt = 0;
            PostKeyEdge(VK_ESCAPE, false);
        }
        static WORD pulse = 0;
        static const ULONGLONG holdMs = (ULONGLONG)fmaxf(config::GetFloat("chord_hold_ms", 600.f), 100.f);
        const bool all4 = L.active && R.active && L.a && L.b && R.a && R.b;
        if (all4 && !chordOn) {
            chordOn = true;
            holdSent = false;
            chordSince = now;
        }
        if (chordOn) {
            if (!L.a && !L.b && !R.a && !R.b) {
                chordOn = false;
                if (!holdSent) {
                    // D-237 (owner: "tapping all four face buttons = the three-lines button (Start / Options), holding
                    // them = the game menu (map, skills, inventory)"): the tap is Start now (was View, 2026-10-17)
                    pulse = XINPUT_GAMEPAD_START;
                    pulseUntil = now + 120;
                    Log("vrmap: all four face buttons tapped -> Start / Options (the three-lines button)");
                }
            } else {
                g->wButtons &= (WORD)~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y);
                if (!holdSent && all4 && now - chordSince >= holdMs) {
                    holdSent = true;
                    // D-237: holding them is the game menu (View / touchpad) - the Esc screen it opened is gone
                    pulse = XINPUT_GAMEPAD_BACK;
                    pulseUntil = now + 150;
                    Log("vrmap: all four face buttons held -> the game menu (View / touchpad)");
                }
            }
        }
        if (now < pulseUntil) g->wButtons |= pulse;
    }
    // the VR settings panel: a hand pointing at it clicks with its trigger - not the game's L2 / R2 then
    if (vr::PanelCapturesTrigger(0)) g->bLeftTrigger = 0;
    if (vr::PanelCapturesTrigger(1)) g->bRightTrigger = 0;
    return true;
}

void SetCameraSyncStick(float x, float y) { g_camSyncX = x; g_camSyncY = y; }
void SetLocomotionYaw(float rad) { g_locoYaw = rad; }

void SetHeavyByStrength(bool on) {  // D-148 switch ("heavystrength on|off")
    g_heavyByStrength = on ? 1 : 0;
    config::SetFloat("heavy_by_strength", on ? 1.f : 0.f);
    Log("vrmap: light / heavy by swing strength %s", on ? "ON (D-148)" : "OFF (the old rules: trigger-held swing / chop heavies)");
}

void VrControllersEnable(bool on) {
    g_on = on;
    Log("vrmap: motion controller input %s", on ? "on" : "off");
}

// diagnostic "gate r1|r2 [anim]" (command thread): the D-062 gate rules through the virtual pad - waits (up to 1.5 s)
// until no attack / evade plays and combo_gap_ms have passed, then presses R1 / R2 (anim: full-animation request)
// diagnostic "gatespam <n> <ms>" (command thread): n light swings, one every <ms>, through the same rules as GateTick
// (queued while busy -> fast-forward request; stance -> block tap; sent when free). Logs each send.
void GateSpam(int n, int intervalMs) {
    static const ULONGLONG gap = (ULONGLONG)config::GetFloat("combo_gap_ms", 60.f);
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG lastSend = 0, nextSwing = t0;
    int swings = 0, sent = 0;
    bool pending = false, cancelled = false;
    while (sent < n && GetTickCount64() - t0 < (ULONGLONG)(n * intervalMs + 4000)) {
        const ULONGLONG now = GetTickCount64();
        if (swings < n && now >= nextSwing) { pending = true; ++swings; nextSwing += intervalMs; }
        if (pending) {
            if (fp::CombatInStance()) {
                if (StanceCancelMode() == 3) fp::CombatStanceFastForward();
                else if (!cancelled) { VpadHold(XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, 0, 0, 0, 0, 100); cancelled = true; }
            } else if (fp::CombatBusy()) {
                fp::CombatFastForward();
            } else if (fp::CombatIdleMs() >= gap && now - lastSend >= 150) {
                VpadHold(XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, 0, 0, 0, 0, 40);  // short, like GateTick (D-076)
                lastSend = now;
                pending = false;
                cancelled = false;
                ++sent;
                Log("gatespam: R1 #%d at %llu ms", sent, (unsigned long long)(now - t0));
            }
        }
        Sleep(5);
    }
}
void SetStanceCancel(int mode) { g_stanceMode = mode; Log("vrmap: delay stance cancel mode %d (1 block tap, 2 stick nudge, 0 wait)", mode); }
void GateTest(bool rt, bool animate, int holdMs) {
    const ULONGLONG t0 = GetTickCount64();
    static const ULONGLONG gap = (ULONGLONG)config::GetFloat("combo_gap_ms", 60.f);
    static ULONGLONG lastSend = 0;
    while (GetTickCount64() - t0 < 1500 && GetTickCount64() - lastSend < 200) Sleep(5);  // same rule as GateTick
    bool cancelled = false;
    while (GetTickCount64() - t0 < 2500 && (fp::CombatBusy() || fp::CombatIdleMs() < gap || fp::CombatSprinting())) {
        if (fp::CombatBusy() && !fp::CombatInStance()) fp::CombatFastForward();  // D-067 spam
        if (fp::CombatInStance() && StanceCancelMode() == 3) {
            if (!cancelled) Log("gate: delay stance -> fast-forward");
            cancelled = true;
            fp::CombatStanceFastForward();
        } else if (fp::CombatInStance() && !cancelled && StanceCancelMode()) {
            cancelled = true;
            Log("gate: delay stance -> cancel by %s", StanceCancelMode() == 1 ? "block tap" : "stick nudge");
            if (StanceCancelMode() == 1) VpadHold(XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, 0, 0, 0, 0, 120);
            else VpadHold(0, 0, -0.37f, 0, 0, 0, 0, 120);
        }
        Sleep(5);
    }
    lastSend = GetTickCount64();
    if (fp::CombatBusy() || fp::CombatSprinting()) { Log("gate: dropped (busy after 2.5 s)"); return; }
    if (animate) fp::CombatRequestAnimate();
    VpadHold(rt ? 0 : XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, 0, 0, 0, rt ? 1.f : 0.f, holdMs);
    Log("gate: %s%s after %llu ms", rt ? "R2" : "R1", animate ? " [full animation]" : "", (unsigned long long)(GetTickCount64() - t0));
}

}  // namespace gowvr::input
