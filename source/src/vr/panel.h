#pragma once
// VR settings panel (owner 2026-10-17: "when I press settings there will be a new VR menu panel - like the flat screen
// and right next to it to the right; dark grey background, red top"). Shown next to the flat 2D screen while the game's
// pause menu is open. Point with either controller and pull its trigger: turning (smooth + speed / snap + angle),
// controller buttons (Xbox / PlayStation), wall collision, VR picture size - each saved in gowvr.ini at once.
#include "xr.h"
#include <cstdint>

namespace gowvr::vr {

struct PanelLayer {
    Pose pose{};             // the panel quad (LOCAL space)
    float w = 0.f, h = 0.f;  // metres
    const uint32_t* pixels = nullptr;  // pw x ph, 0xAARRGGBB (sRGB) - new content when dirty
    int pw = 0, ph = 0;
    bool dirty = false;
    bool cursor = false;     // a controller points at the panel: the dot
    Pose cursorPose{};
    float cursorSize = 0.f;
};

// Render thread, a flat 2D screen frame: the panel for this frame next to the screen (its pose and size) for the head
// at `head`; false = no panel (the pause menu is closed). Handles pointing and clicks.
bool PanelFrame(const InputSnapshot& in, const Pose& screen, float screenW, const float head[3], PanelLayer* out);
// No panel this frame (not a flat screen frame): pointing state reset
void PanelHidden();
// The 64 x 64 cursor dot (0xAARRGGBB premultiplied, sRGB)
const uint32_t* PanelCursorPixels();
// Pad thread (vrmap): hand 0 left / 1 right points at the panel - its trigger belongs to the panel, not the game
bool PanelCapturesTrigger(int hand);

}  // namespace gowvr::vr
