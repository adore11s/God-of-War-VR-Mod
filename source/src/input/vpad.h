#pragma once
#include <cstdint>

namespace gowvr::input {

// Virtual XInput pad on user index 0, merged over any real pad. Installed by IAT-patching GoW.exe's
// XINPUT1_4 ordinal 2 (XInputGetState).
void InstallVirtualPad();

// Present the virtual pad as connected (so the game sees a controller even with none plugged in).
void VpadEnable(bool on);
// Hold a state for `ms` milliseconds, then return to neutral. Sticks are -1..1, triggers 0..1.
// Buttons use XINPUT_GAMEPAD_* bits. A new hold replaces the current one.
// D-064: the left stick the game got last (all pads merged, game-camera space, -1..1), before the mod's recovery nudge
void PlayerLeftStick(float* x, float* y);
float GameLeftTrigger();  // D-078: the left trigger (aim) the game last received, 0..1
bool GameBlockHeld();     // D-080: L1 (block / shield) in the buttons the game last received
void VpadHold(uint16_t buttons, float lx, float ly, float rx, float ry, float lt, float rt, int ms);
uint64_t VpadPolls();

// Focus override: while on, the game's GetForegroundWindow() returns its own window, so input is accepted when the
// desktop focus is elsewhere (headset play). The window is captured at swap-chain creation.
void SetGameWindow(struct HWND__* w);
void FocusOverride(bool on);
void FocusKeepAlive();  // re-assert window activation (command thread, 1 Hz)
// Keyboard fallback for menus: WM_KEYDOWN/WM_KEYUP posted to the game window.
void PostKey(int vk, int ms);
// The same, one edge (key down or up) without waiting - for the pad thread (the four-face-button hold = Esc)
bool PostKeyEdge(int vk, bool down);

// D-111: which pad the game sees - 0 Xbox (XInput user 0, F10), 1 PlayStation (the game's own PlayStation pad through
// libScePad, F11: PlayStation button prompts). Saved in gowvr.ini pad_mode. The game's rumble (either pad) goes to the
// VR controllers.
void SetPadMode(int mode);
int PadMode();

}  // namespace gowvr::input
