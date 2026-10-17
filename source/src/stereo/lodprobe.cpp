// D-108 LOD probe / scale (owner: "some assets' resolution sometimes doesn't improve - the low poly look").
// sub_14046f100(lodDesc, sphere, camPos, scale, &out) picks an object's LOD: metric = (|camPos - centre| - radius) *
// scale (* desc+0x5c), curved by the Model Quality exponent and compared with the LOD distance thresholds. Callers:
// the per-view LOD/visibility job (sub_14046e180: camPos / scale from the job's view entries +0x10 / +0x30) and
// sub_140470xxx (scale 1.0). "lodprobe <n>" logs what it is called with (camera positions vs the views, scale range,
// LOD indices); gowvr.ini lod_scale / "lodscale <f>" multiplies the scale (< 1 = finer LODs farther out).
#include "lodprobe.h"
#include "../core/config.h"
#include "../core/detour.h"
#include "../core/log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace gowvr::stereo {
namespace {

using PFN_Lod = int(__fastcall*)(int64_t desc, const float* sphere, const float* cam, float scale, float* out);
PFN_Lod o_Lod = nullptr;
std::atomic<float> g_lodScale{-1.f};
std::atomic<int> g_probeLeft{0};
std::mutex g_mu;
struct Cam { float p[3]; float scale; uint64_t n; };
Cam g_cams[8];
int g_nCams = 0;
uint64_t g_calls = 0, g_lodHist[8] = {};
float g_minS = 1e30f, g_maxS = -1e30f;

int __fastcall hk_Lod(int64_t desc, const float* sphere, const float* cam, float scale, float* out) {
    float s = g_lodScale.load(std::memory_order_relaxed);
    if (s < 0.f) {
        s = config::GetFloat("lod_scale", 1.f);
        g_lodScale = s;
    }
    const int lod = o_Lod(desc, sphere, cam, s > 0.f ? scale * s : scale, out);
    if (g_probeLeft.load(std::memory_order_relaxed) > 0 && cam) {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_calls;
        if (lod >= 0 && lod < 8) ++g_lodHist[lod];
        g_minS = fminf(g_minS, scale);
        g_maxS = fmaxf(g_maxS, scale);
        int i = 0;
        while (i < g_nCams && (fabsf(g_cams[i].p[0] - cam[0]) > 0.05f || fabsf(g_cams[i].p[1] - cam[1]) > 0.05f ||
                               fabsf(g_cams[i].p[2] - cam[2]) > 0.05f || g_cams[i].scale != scale))
            ++i;
        if (i == g_nCams && i < 8) g_cams[g_nCams++] = {{cam[0], cam[1], cam[2]}, scale, 0};
        if (i < 8) ++g_cams[i].n;
        if (--g_probeLeft == 0) {
            Log("lodprobe: %llu calls, scale %.4f..%.4f, LOD picked 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu (lod_scale %.2f)",
                (unsigned long long)g_calls, g_minS, g_maxS, (unsigned long long)g_lodHist[0],
                (unsigned long long)g_lodHist[1], (unsigned long long)g_lodHist[2], (unsigned long long)g_lodHist[3],
                (unsigned long long)g_lodHist[4],
                (unsigned long long)(g_lodHist[5] + g_lodHist[6] + g_lodHist[7]), s);
            for (int k = 0; k < g_nCams; ++k)
                Log("lodprobe:  camera (%.3f %.3f %.3f) scale %.4f - %llu calls", g_cams[k].p[0], g_cams[k].p[1],
                    g_cams[k].p[2], g_cams[k].scale, (unsigned long long)g_cams[k].n);
        }
    }
    return lod;
}

const HookSite kLodSel = {"LodSelect", 0x14046f100, 5,  // mov [rsp+18h], rbx (5)
                          {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x63, 0x69, 0x6c}};

}  // namespace

void InstallLodHook() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool ok = DetourInstall(kLodSel, (void*)&hk_Lod, (void**)&o_Lod);
    Log("lod: LOD selector hook %s (lod_scale %.2f)", ok ? "installed" : "NOT installed", config::GetFloat("lod_scale", 1.f));
}

void LodProbe(int calls) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_nCams = 0;
    g_calls = 0;
    for (uint64_t& h : g_lodHist) h = 0;
    g_minS = 1e30f;
    g_maxS = -1e30f;
    g_probeLeft = calls > 0 ? calls : 20000;
    Log("lodprobe: armed for %d calls", g_probeLeft.load());
}

void LodScale(float s) {
    g_lodScale = s;
    config::SetFloat("lod_scale", s);
    Log("lod: scale x%.2f (< 1 = finer LODs farther out)", s);
}

}  // namespace gowvr::stereo
