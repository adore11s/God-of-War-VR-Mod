#pragma once
// Square VR picture (owner 2026-10-17, as in the Beyond: Two Souls mod 0.17.12): while VR runs the game renders
// N x N (gowvr.ini render_size, the launcher asks) whatever the monitor's resolution, and in Windowed mode its window
// on the monitor is a small square (gowvr.ini desktop_window). The headset's eyes are close to square, so a 16:9
// frame wasted width and stretched the game's 2D UI vertically (a Quest 3 tester: "the apple of Idun pickup
// stretched the whole screen").
#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;

namespace gowvr::frame {

// DllMain: GoW.exe's imports of GetClientRect / GetWindowPlacement (only the game's own calls)
void InstallRenderSize();
// Render thread, each Present: the game window (the swapchain's), square on while VR runs, the small window kept
void RenderSizeTick(IDXGISwapChain* sc, bool vrRunning);
// Render thread: the game frame size a stereo (flatFrame false) or flat 2D screen frame should have now - a frame of
// another size is from the switch between them (the game resizes a frame or two later) and is not shown. False = any.
bool RenderSizeExpected(bool flatFrame, uint32_t* w, uint32_t* h);
// xr.cpp, each session: the larger side of the eye image the runtime asks for. The picture is that size (gowvr.ini
// render_size_auto=1, the default) instead of render_size.
void SetRuntimeEyeSize(int px);
// the automatic picture size in use now, 0 = render_size is
int AutoRenderSize();
// a session told its eye image size (the panel offers "Auto")
bool RuntimeEyeSizeKnown();
// VR summary / VR check line (log)
void RenderSizeDiagText(char* out, size_t n);

}  // namespace gowvr::frame
