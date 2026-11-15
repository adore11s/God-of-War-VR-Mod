#pragma once
// D-318 / D-328: the welcome windows' DRAWING - the panel's pattern, the built-in Montserrat, the pages' text - shared by
// the mod (vr/panel.cpp: when and where the windows show) and tools/welcome_preview (PNGs of every window from this
// exact code, to check a new or changed window before the headset).
#include <windows.h>
#include <cstdint>

namespace gowvr::vr::welcome {
constexpr int kWW = 1200, kWH = 820;  // the window's pixels
constexpr int kWelPages = 4;          // WELCOME, KO-FI SUPPORTER, FIRST PERSON, VR CONTROLS
extern uint32_t g_wOut[kWW * kWH];    // the last drawn window, 0xAARRGGBB premultiplied
extern RECT g_wBtn;                   // the CONTINUE / START bubble in g_wOut's pixels
uint32_t PatternPixel(int px, int py);  // one pixel of the VR panel's background pattern (0x00RRGGBB)
bool MakeD2D();                         // Direct2D / DirectWrite / WIC and the fonts (once); false = unavailable
void WelcomeDraw(int page, bool hover); // page 0..kWelPages-1 into g_wOut (hover = the bubble pointed at)
}  // namespace gowvr::vr::welcome
