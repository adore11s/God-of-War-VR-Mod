#pragma once
#include <cstdint>

namespace gowvr::stereo {

// Detours the frame-graph kick (sub_140426da0). Stage 1 only observes.
bool R1Install();
// Log a before/after snapshot of the render-frame globals for the next n kicks.
void R1Observe(int frames);
// Zero-delta double kick for n frames; mask selects between-eyes drain/reset steps (see r1.cpp BetweenEyes).
void R1Double(int frames, unsigned mask);
// One-shot .data diff: mode 1 = across kick 1 + between-eyes (next double kick), mode 2 = across one normal frame.
void R1Diff(int mode);
// Load gowvr_logs/r1bank.txt (per-eye state bank; used when the double-kick mask has bit 0x2000).
bool R1BankLoad();
// Per-kick eye camera offsets (metres along camera right, yaw degrees): kick 1 = eye 0, kick 2 = eye 1.
void R1Eyes(bool on, float off0, float off1, float yaw0, float yaw1);
// VR mode: built-in whitelist bank, continuous double kick (mask 0x7e07), head-tracked eyes from OpenXR (headcam.cpp).
void R1Vr(bool on);
// Diagnostic: identical per-frame sideways camera motion for both eyes (temporal-history test).
void R1Wobble(float amp);
void R1FogFix(bool on);
void R1Mask(unsigned mask);
uint64_t R1Kicks();  // frame-graph kicks so far (D-083 loadwatch)
// dlss-vr: fp/body.cpp hk_PaletteBuild, after sub_140471c30 built a skinned instance's palettes - eye 1 gets eye 0's previous
void R1SkinPrevious(uint8_t* inst, void* renderObject, void* curOut, void* prevOut);
int R1KickPhase();   // D-099: main thread: 0 none / 1 in kick 1 (eye 0) / 2 between the eyes / 3 in kick 2 (eye 1) / 4 other kick work (single kick, before kick 1)

}  // namespace gowvr::stereo
