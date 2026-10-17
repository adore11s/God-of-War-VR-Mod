// D-108 per-eye DLSS (owner: "research how to fix the DLSS smear around characters in VR").
// Research (UEVR ghosting fix and others): temporal upscalers ghost in VR when an eye's history, previous-camera motion
// vectors and jitter do not belong to that same eye. Here the R1 double kick renders eye 0 and eye 1 every frame and
// both run the game's DLSS pass on ONE NGX feature, whose history lives inside NGX: eye 0 is reconstructed from eye 1's
// last output and vice versa - with the eyes' ~6.5 cm parallax, near objects (characters) smear. (The game's own
// temporal state - previous view matrices for the motion vectors - is already per eye: r1.cpp bank / mutate.cpp.)
// Fix: the game reaches NGX through GetProcAddress on _nvngx.dll (LIVE log), so its CreateFeature gets a twin: for
// DLSS Super Resolution (feature 1) a second instance is created with the same parameters, and EvaluateFeature uses it
// while the render thread executes eye 1's command streams (eyes.cpp EyesRenderEye). Release / Shutdown drop the twin.
#include "dlsseyes.h"
#include "eyes.h"
#include "../core/config.h"
#include "../core/log.h"
#include <d3d11.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace gowvr::stereo {
namespace {

using NgxResult = int;  // NVSDK_NGX_Result: 0x1 success, 0xBAD0xxxx failures
constexpr NgxResult kNgxOk = 1;
constexpr int kFeatureSuperSampling = 1;  // NVSDK_NGX_Feature_SuperSampling
using PFN_Create = NgxResult(__cdecl*)(ID3D11DeviceContext*, int, void*, void**);
using PFN_Eval = NgxResult(__cdecl*)(ID3D11DeviceContext*, const void*, const void*, void*);
using PFN_Release = NgxResult(__cdecl*)(void*);
using PFN_Shutdown = NgxResult(__cdecl*)();
using PFN_Shutdown1 = NgxResult(__cdecl*)(ID3D11Device*);
PFN_Create o_Create = nullptr;
PFN_Eval o_Eval = nullptr;
PFN_Release o_Release = nullptr;
PFN_Shutdown o_Shutdown = nullptr;
PFN_Shutdown1 o_Shutdown1 = nullptr;

std::atomic<int> g_on{-1};
bool On() {
    if (g_on.load() < 0) g_on = config::GetFloat("dlss_per_eye", 1.f) > 0.5f ? 1 : 0;
    return g_on.load() > 0;
}

struct Twin { const void* game; void* eye1; };
std::mutex g_mu;
std::vector<Twin> g_twins;
std::atomic<uint64_t> g_evals[2];

NgxResult __cdecl hk_Create(ID3D11DeviceContext* ctx, int feature, void* params, void** out) {
    const NgxResult r = o_Create(ctx, feature, params, out);
    if (r == kNgxOk && out && *out && feature == kFeatureSuperSampling && On()) {
        void* twin = nullptr;
        const NgxResult r2 = o_Create(ctx, feature, params, &twin);
        if (r2 == kNgxOk && twin) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_twins.push_back({*out, twin});
            Log("dlss: per-eye instance created (game %p, eye 1 %p)", *out, twin);
        } else {
            Log("dlss: second instance FAILED (0x%08x) - both eyes share the game's", (unsigned)r2);
        }
    }
    return r;
}

NgxResult __cdecl hk_Eval(ID3D11DeviceContext* ctx, const void* handle, const void* params, void* cb) {
    const void* h = handle;
    const int eye = EyesRenderEye();
    if (eye == 1 && On()) {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const Twin& t : g_twins)
            if (t.game == handle) { h = t.eye1; break; }
    }
    const uint64_t n = ++g_evals[h != handle ? 1 : 0];
    if (n == 1 || n == 600)
        Log("dlss: evaluate #%llu on the %s instance (render eye %d)", (unsigned long long)n,
            h != handle ? "eye-1" : "game's", eye);
    return o_Eval(ctx, h, params, cb);
}

NgxResult __cdecl hk_Release(void* handle) {
    void* twin = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (size_t i = 0; i < g_twins.size(); ++i)
            if (g_twins[i].game == handle) {
                twin = g_twins[i].eye1;
                g_twins.erase(g_twins.begin() + (ptrdiff_t)i);
                break;
            }
    }
    if (twin) {
        o_Release(twin);
        Log("dlss: per-eye instance released with the game's %p", handle);
    }
    return o_Release(handle);
}

void DropTwins() {  // NGX shutdown frees every feature itself
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_twins.empty()) Log("dlss: NGX shutdown - %zu per-eye instance(s) dropped", g_twins.size());
    g_twins.clear();
}
NgxResult __cdecl hk_Shutdown() { DropTwins(); return o_Shutdown(); }
NgxResult __cdecl hk_Shutdown1(ID3D11Device* dev) { DropTwins(); return o_Shutdown1(dev); }

}  // namespace

FARPROC DlssEyesWrap(const char* name, FARPROC p) {
    if (!p || !name || strncmp(name, "NVSDK_NGX_D3D11_", 16) != 0) return nullptr;
    const char* f = name + 16;
    if (!strcmp(f, "CreateFeature")) { o_Create = (PFN_Create)p; return (FARPROC)&hk_Create; }
    if (!strcmp(f, "EvaluateFeature")) { o_Eval = (PFN_Eval)p; return (FARPROC)&hk_Eval; }
    if (!strcmp(f, "ReleaseFeature")) { o_Release = (PFN_Release)p; return (FARPROC)&hk_Release; }
    if (!strcmp(f, "Shutdown")) { o_Shutdown = (PFN_Shutdown)p; return (FARPROC)&hk_Shutdown; }
    if (!strcmp(f, "Shutdown1")) { o_Shutdown1 = (PFN_Shutdown1)p; return (FARPROC)&hk_Shutdown1; }
    return nullptr;
}

void DlssEyesEnable(bool on) {
    g_on = on ? 1 : 0;
    config::SetFloat("dlss_per_eye", on ? 1.f : 0.f);
    Log("dlss: per-eye instances %s (applies to DLSS created after this; restart the game for the current one)",
        on ? "on" : "off");
}

}  // namespace gowvr::stereo
