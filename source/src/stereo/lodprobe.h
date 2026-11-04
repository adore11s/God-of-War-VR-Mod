#pragma once
// D-108: the game's LOD selector (sub_14046f100) - probe and scale. See lodprobe.cpp.

namespace gowvr::stereo {

void InstallLodHook();     // DLL load (no thread is in the selector yet)
void LodProbe(int calls);  // command "lodprobe [calls]": log the cameras / scales / LODs the selector is called with
void LodScale(float s);    // command "lodscale <f>", gowvr.ini lod_scale (1 = the game's)
// D-296: the model streaming request list (4096 per frame in the game) made stream_list_scale x bigger. DLL load only
// (noThreads: no game thread exists yet - its code is patched)
void InstallStreamLists(bool noThreads);
// D-296 game thread, r1.cpp's kick hook: at each frame's start (frameStart) and after each kick - the frame's peak demand
// on the model list and the texture request buffer, logged once a minute
void StreamReqNote(bool frameStart);
// D-297: the game's model + texture streaming heap (4096 MB) made bigger by the card's video memory. DLL load (a detour
// on the game's memory setup, which runs later on its main thread)
void InstallTextureHeap();
// D-299 / D-300: VR texture detail - extra mip levels the game is asked for in VR (0 = the game's own .. 3), and whether
// that is the automatic choice (gowvr.ini vr_texture_detail -1); the VR settings panel's "Texture detail" row
int VrTextureDetailLevel();
bool VrTextureDetailAuto();
// D-303: fp/body.cpp's skinning palette hook, for each drawn character (the skinned draw's render object): in VR its
// textures are asked at full size while it is drawn (gowvr.ini vr_character_textures)
void CharacterTextures(void* renderObject);

}  // namespace gowvr::stereo
