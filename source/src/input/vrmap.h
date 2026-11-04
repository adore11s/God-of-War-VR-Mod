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
// D-311: the left hand is held at the left side of the head (gameplay first person, Compass = Head) - the compass is
// shown meanwhile (stereo/mutate.cpp). Any thread.
bool LeftHandAtHead();
// D-293: controllers without finger sensing - the fists are switched on with Y and no weapon is out: both hands are drawn
// closed. Any thread.
bool FistsClosed();
// D-307: the grip (with its hysteresis; the Index's squeeze force, D-305) is held on this hand (0 left, 1 right). Any thread.
bool GripHeld(int h);

}  // namespace gowvr::input
