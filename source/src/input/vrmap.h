#pragma once

struct _XINPUT_GAMEPAD;

namespace gowvr::input {

// Current motion-controller state mapped to a gamepad (false = neutral / not available). Any thread.
bool VrGamepad(struct _XINPUT_GAMEPAD* out);
void VrControllersEnable(bool on);
void SetHeavyByStrength(bool on);  // D-148: command "heavystrength on|off", gowvr.ini heavy_by_strength
void GateTest(bool rt, bool animate, int holdMs);
void SetStanceCancel(int mode);
void GateSpam(int n, int intervalMs);  // diagnostic "gatespam <n> <ms>"  // "stancecancel 0|1|2"  // diagnostic "gate r1|r2 [anim]" (D-062 attack gate via the virtual pad)
// Camera steering: virtual right stick (x = yaw, y = pitch) steering the game camera toward the head;
// applied only while the player is not using the right stick. -1..1, 0 = off.
void SetCameraSyncStick(float x, float y = 0.f);
// Gameplay first person: rotate the left stick by this yaw (radians) so "forward" walks where the player looks
// (the game moves relative to its own camera). 0 = off.
void SetLocomotionYaw(float rad);
// D-218: the left hand is held at the head - show the combat HUD (health / Spartan Rage / runic / talisman) meanwhile.
// Any thread.
bool HudPeek();

}  // namespace gowvr::input
