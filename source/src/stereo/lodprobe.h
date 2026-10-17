#pragma once
// D-108: the game's LOD selector (sub_14046f100) - probe and scale. See lodprobe.cpp.

namespace gowvr::stereo {

void InstallLodHook();     // DLL load (no thread is in the selector yet)
void LodProbe(int calls);  // command "lodprobe [calls]": log the cameras / scales / LODs the selector is called with
void LodScale(float s);    // command "lodscale <f>", gowvr.ini lod_scale (1 = the game's)

}  // namespace gowvr::stereo
