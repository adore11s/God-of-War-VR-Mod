#pragma once
// D-104 sharp HUD: the game draws its UI unscaled into a mod-owned transparent target, which is mip-filtered and pasted
// into each eye image at the HUD scale (ui_scale) - with the aiming reticle region at its own scale.
#include <cstdint>
#include <d3d11.h>

namespace gowvr::stereo {

void SharpUiInstall(ID3D11DeviceContext* ctx);  // device creation (after cbprobe): immediate-context hooks
// Game thread, from UiScaleView: true = this 2D UI view is drawn unscaled and marked for the composite (the caller then
// does not scale it); false = sharp HUD off / not working - the caller scales the view as before
bool SharpUiMarkView(uint8_t* view);
// Render thread, right before an eye image is copied from the backbuffer and at Present: paste this eye's HUD
void SharpUiFinish(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb);
void SharpUiOnResize();              // swapchain ResizeBuffers: drop our view of the backbuffer
void SharpUiEnable(bool on);         // command "sharpui on|off", gowvr.ini sharp_ui
void SharpUiReticleScale(float s);   // command "uireticle <s>", gowvr.ini ui_reticle_scale
void SharpUiProbe(int frames);       // command "sharpui probe [frames]": where the UI view data goes (log)
void SharpUiHudShot();              // command "sharpui hudshot": HUD target read back (log + shots/hud_target.png)
void SharpUiCensus();               // D-145 command "sharpui census": one frame's UI draws logged (also every 15 s in F1)
void SharpUiReticleObjectsMoved();  // D-149: the aiming icon's UI objects were placed for the aim point (any thread)
float SharpUiReticleScaleNow();     // D-149: ui_reticle_scale (the aiming icon's size, x its native size)
bool SharpUiActive();                // the HUD is drawn by the sharp HUD now (on, hooks complete, not fallen back)
void SharpUiPresentTick();           // Present (render thread), before SharpUiFinish

}  // namespace gowvr::stereo
