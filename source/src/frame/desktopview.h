#pragma once
// D-329: the desktop window's picture while VR runs (owner 2026-11-15: "make the window resizable; nothing may break,
// the UI or the 2D screens" and "since the sharp UI I cannot see it in the desktop window").
// At Present, AFTER every headset copy (the eye captures, the flat screen's copy, shots) and right before the game's
// Present - so nothing here reaches the headset:
//   1. the HUD pasted in while the headset gets it as its own layer (sharpui.cpp SharpUiDesktopHud);
//   2. the picture fitted into the window with its shape kept (black bars): DXGI stretches the whole backbuffer onto the
//      window's client area, so a square 3D picture (or the player's 16:9 / 16:10 flat screens) in a window of another
//      shape would be squashed. The backbuffer's picture is drawn smaller inside it, so that the stretch gives it back
//      its own shape.
struct IDXGISwapChain;

namespace gowvr::frame {

void DesktopViewOnPresent(IDXGISwapChain* sc);  // render thread, last step before the game's Present
void DesktopViewOnResize();                    // ResizeBuffers: drop our view of the backbuffer first

}  // namespace gowvr::frame
