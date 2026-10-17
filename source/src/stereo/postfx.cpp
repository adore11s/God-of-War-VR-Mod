// VR comfort post-effect policy: depth of field off (no in-game setting for it).
// sub_140410fd0(mainRenderView) runs inside the kick right after the main RenderView build (sub_140425e30) and writes
// the post-effect constant block read by PassCombinedPostEffects: 0x141188b50 = DoF enable (1.0/0.0, from the camera
// settings' DoF flag), 0x141188b60 = circle-of-confusion scale (0 when DoF is off). STATIC, render_passes_011/013/046.
// Forcing both to 0 after it runs is exactly the engine's own "DoF disabled" state.
#include "postfx.h"
#include "../core/addr.h"
#include "../core/detour.h"
#include "../core/log.h"
#include <windows.h>
#include <atomic>

namespace gowvr::stereo {
namespace {

constexpr HookSite kPostFxParams{"PostFxParams", 0x140410fd0, 7,
    {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55}};
constexpr uint64_t kDofEnable = 0x141188b50, kDofCocScale = 0x141188b60;

using PFN_PostFx = void(__fastcall*)(uint8_t* rv, uint8_t flags);
PFN_PostFx o_PostFx = nullptr;
uint8_t* g_base = nullptr;
std::atomic<bool> g_noDof{false};
std::atomic<uint64_t> g_forced{0};

void __fastcall hk_PostFx(uint8_t* rv, uint8_t flags) {
    o_PostFx(rv, flags);
    if (!g_noDof.load()) return;
    float* en = (float*)((uint8_t*)GameVa(kDofEnable));
    float* coc = (float*)((uint8_t*)GameVa(kDofCocScale));
    if (*en != 0.f || *coc != 0.f) ++g_forced;
    *en = 0.f;
    *coc = 0.f;
}

}  // namespace

void NoDepthOfField(bool on) {
    static bool installed = false;
    if (!installed) {
        g_base = (uint8_t*)GetModuleHandleW(nullptr);
        installed = DetourInstall(kPostFxParams, (void*)&hk_PostFx, (void**)&o_PostFx);
        if (!installed) { Log("postfx: hook install failed; depth of field unchanged"); return; }
    }
    g_noDof = on;
    Log("postfx: depth of field %s (frames where DoF was active and forced off so far: %llu)", on ? "OFF" : "game default",
        (unsigned long long)g_forced.load());
}

}  // namespace gowvr::stereo
