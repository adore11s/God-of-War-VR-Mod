#pragma once
// D-182 per-eye TAA history: the game's own temporal anti-aliasing (DLSS off) keeps ONE history image; with both eyes
// rendered every frame each eye blended the OTHER eye's last image - a faint copy of characters / objects offset outward
// (left eye: to the left, right eye: to the right) and shimmer. Each eye now keeps its own history (see taaeyes.cpp).

namespace gowvr::stereo {

void TaaEyesInstall();          // after the device exists (d3d_hooks.cpp, next to SharpUiInstall)
void TaaEyesEnable(bool on);    // gowvr.ini taa_per_eye (default on)

}  // namespace gowvr::stereo
