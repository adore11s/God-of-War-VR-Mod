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
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../diag/shot.h"
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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

// dlss-vr: NVSDK_NGX_Parameter (nvsdk_ngx_defs.h), declared exactly as the SDK declares it so this compiler lays out the
// same vtable as the NGX runtime's (both MSVC). The game hands one to CreateFeature and EvaluateFeature.
struct NgxParams {
    virtual void Set(const char* name, unsigned long long v) = 0;
    virtual void Set(const char* name, float v) = 0;
    virtual void Set(const char* name, double v) = 0;
    virtual void Set(const char* name, unsigned int v) = 0;
    virtual void Set(const char* name, int v) = 0;
    virtual void Set(const char* name, ID3D11Resource* v) = 0;
    virtual void Set(const char* name, struct ID3D12Resource* v) = 0;
    virtual void Set(const char* name, void* v) = 0;
    virtual NgxResult Get(const char* name, unsigned long long* v) const = 0;
    virtual NgxResult Get(const char* name, float* v) const = 0;
    virtual NgxResult Get(const char* name, double* v) const = 0;
    virtual NgxResult Get(const char* name, unsigned int* v) const = 0;
    virtual NgxResult Get(const char* name, int* v) const = 0;
    virtual NgxResult Get(const char* name, ID3D11Resource** v) const = 0;
    virtual NgxResult Get(const char* name, struct ID3D12Resource** v) const = 0;
    virtual NgxResult Get(const char* name, void** v) const = 0;
    virtual void Reset() = 0;
};
float GetF(const NgxParams* p, const char* n, float def = -999.f) { float v = def; return p->Get(n, &v) == kNgxOk ? v : def; }
int GetI(const NgxParams* p, const char* n, int def = -999) { int v = def; return p->Get(n, &v) == kNgxOk ? v : def; }
unsigned GetU(const NgxParams* p, const char* n, unsigned def = 0xffffffffu) {
    unsigned v = def;
    return p->Get(n, &v) == kNgxOk ? v : def;
}

// dlss-vr LIVE 2026-10-18 (dlsslog): the game's jitter is the 8-step Halton(2,3) sequence minus 0.5 and steps once per
// RENDER - in VR eye 0 got phases 1 3 5 7 and eye 1 got 0 2 4 6 (4 phases per eye, never the same in both eyes).
int JitterPhase(float x, float y) {
    static const float kX[8] = {0.f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f};
    static const float kY[8] = {0.f, 1.f / 3.f, 2.f / 3.f, 1.f / 9.f, 4.f / 9.f, 7.f / 9.f, 2.f / 9.f, 5.f / 9.f};
    for (int i = 0; i < 8; ++i)
        if (fabsf(x - (kX[i] - 0.5f)) < 2e-3f && fabsf(y - (kY[i] - 0.5f)) < 2e-3f) return i;
    return -1;
}

// "dlssfind": which 32-bit value in GoW.exe's writable sections steps with the jitter phase (the counter to bank)
struct FindCand { uint32_t* p; uint32_t last; };
std::vector<FindCand> g_cands;
std::atomic<int> g_findSteps{0};
int g_findPhase = -1;
std::vector<std::pair<uint32_t*, std::vector<uint32_t>>> g_snap;  // step 0: the writable sections' values
void FindStep(int phase) {
    if (phase < 0) return;
    if (g_findPhase < 0) {  // first step: a snapshot of every writable section
        g_snap.clear();
        HMODULE exe = GetModuleHandleW(nullptr);
        auto* dos = (IMAGE_DOS_HEADER*)exe;
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        size_t n = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            if (!(s->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uint32_t* b = (uint32_t*)((uint8_t*)exe + s->VirtualAddress);
            g_snap.push_back({b, std::vector<uint32_t>(b, b + s->Misc.VirtualSize / 4)});
            n += s->Misc.VirtualSize / 4;
        }
        g_findPhase = phase;
        Log("dlssfind: phase %d, snapshot of %zu values", phase, n);
        return;
    }
    const uint32_t step = (uint32_t)((phase - g_findPhase) & 7);
    if (!g_snap.empty()) {  // second step: every value that moved by exactly the phase step since the snapshot
        g_cands.clear();
        for (auto& sec : g_snap)
            for (size_t i = 0; i < sec.second.size(); ++i)
                if (sec.first[i] - sec.second[i] == step && step) g_cands.push_back({sec.first + i, sec.first[i]});
        g_snap.clear();
        g_findPhase = phase;
        Log("dlssfind: phase %d (step %u), %zu candidates", phase, step, g_cands.size());
        return;
    }
    size_t k = 0;
    for (const FindCand& c : g_cands) {
        const uint32_t v = *c.p;
        if (v - c.last == step && step) g_cands[k++] = {c.p, v};
    }
    g_cands.resize(k);
    g_findPhase = phase;
    Log("dlssfind: phase %d, %zu candidates left", phase, g_cands.size());
    if (g_findSteps.load() == 1 || g_cands.size() <= 12)
        for (size_t i = 0; i < g_cands.size() && i < 40; ++i)
            Log("dlssfind:   %p (va 0x%llx) = %u", g_cands[i].p,
                (unsigned long long)((uint8_t*)g_cands[i].p - (uint8_t*)GetModuleHandleW(nullptr) + 0x140000000ull),
                g_cands[i].last);
}

// "dlssfindj": where in GoW.exe's writable sections this evaluation's jitter pair (pixels) sits - counted per address
// over N evaluations; the addresses that hold the jitter every time lead to the code that computes it
std::vector<std::pair<uint32_t, int>> g_jitHits;  // (section offset from the image base, hits)
std::atomic<int> g_jitSteps{0};
int g_jitTotal = 0;
void FindJitter(float jx, float jy) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    ++g_jitTotal;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const float* b = (const float*)((uint8_t*)exe + s->VirtualAddress);
        const size_t n = s->Misc.VirtualSize / 4;
        for (size_t k = 0; k + 1 < n; ++k) {
            if (b[k] != jx || b[k + 1] != jy) continue;
            const uint32_t rva = s->VirtualAddress + (uint32_t)(k * 4);
            bool found = false;
            for (auto& h : g_jitHits)
                if (h.first == rva) { ++h.second; found = true; break; }
            if (!found && g_jitHits.size() < 4096) g_jitHits.push_back({rva, 1});
        }
    }
}
void FindJitterReport() {
    std::sort(g_jitHits.begin(), g_jitHits.end(), [](auto& a, auto& b) { return a.second > b.second; });
    Log("dlssfindj: %d evaluations, %zu addresses held the jitter pair at least once", g_jitTotal, g_jitHits.size());
    for (size_t i = 0; i < g_jitHits.size() && i < 30; ++i)
        Log("dlssfindj:   va 0x%llx  %d / %d", 0x140000000ull + g_jitHits[i].first, g_jitHits[i].second, g_jitTotal);
    g_jitHits.clear();
    g_jitTotal = 0;
}

// dlss-vr: the game's jitter phase count while DLSS runs (0x143e60c04 = 1): 0x143e60c18 = 8 (LIVE), read by the
// jittered-projection builder sub_1404d2130 (index = counter % count into its 64-entry Halton table). NVIDIA's DLSS
// guide asks for 8 x (output / render)^2 phases (Quality 1.5x -> 18); in VR it is that, from the sizes DLSS was created
// with (gowvr.ini dlss_vr_phases: 0 = that, N = N phases, -1 = the game's 8). The game's value comes back with VR off.
constexpr uint64_t kDlssOnVa = 0x143e60c04;
constexpr uint64_t kDlssPhasesVa = 0x143e60c18;
std::atomic<float> g_dlssScale{0.f};  // OutWidth / Width at the last DLSS create
int g_origPhases = -1, g_setPhases = -1;

// "dlssmv": the motion vectors DLSS gets, per eye - statistics in render pixels (value x MV.Scale) and a PNG
// (gowvr_logs/shots/dlssmv_eyeE.png: grey = 0, red / green = +x / +y, 16 per pixel). With a still head and camera they
// must be ~0 outside animated objects: if not, DLSS rejects its history every frame (blocky shimmering edges).
std::atomic<int> g_mvCaps[3] = {};  // eye 0, eye 1, flat
std::atomic<uint64_t> g_mvFrame{0};  // (= g_frame, for the capture log)
float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.f : e == 31 ? 65504.f : ldexpf(1.f + m / 1024.f, (int)e - 15);
    return s ? -v : v;
}
void CaptureMv(ID3D11DeviceContext* ctx, const NgxParams* p, int eye) {
    ID3D11Resource* r = nullptr;
    if (p->Get("MotionVectors", &r) != kNgxOk || !r) { Log("dlssmv: no MotionVectors resource"); return; }
    ID3D11Texture2D* t = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = sd.ArraySize = 1;
    sd.SampleDesc = {1, 0};
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const float sx = GetF(p, "MV.Scale.X", 1.f), sy = GetF(p, "MV.Scale.Y", 1.f);
            unsigned w = GetU(p, "DLSS.Render.Subrect.Dimensions.Width", d.Width);
            unsigned h = GetU(p, "DLSS.Render.Subrect.Dimensions.Height", d.Height);
            if (w > d.Width || w == 0) w = d.Width;
            if (h > d.Height || h == 0) h = d.Height;
            std::vector<uint32_t> png((size_t)w * h);
            double sumX = 0, sumY = 0, absX = 0, absY = 0;
            float maxL = 0.f;
            size_t over = 0, over05 = 0, over15 = 0, n = 0;
            for (unsigned y = 0; y < h; ++y) {
                const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (unsigned x = 0; x < w; ++x) {
                    float vx = 0.f, vy = 0.f;
                    if (d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                        const uint16_t* q = (const uint16_t*)row + x * (d.Format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4);
                        vx = Half(q[0]);
                        vy = Half(q[1]);
                    } else if (d.Format == DXGI_FORMAT_R32G32_FLOAT || d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                        const float* q = (const float*)row + x * (d.Format == DXGI_FORMAT_R32G32_FLOAT ? 2 : 4);
                        vx = q[0];
                        vy = q[1];
                    }
                    const float px = vx * sx, py = vy * sy;
                    sumX += px; sumY += py; absX += fabsf(px); absY += fabsf(py);
                    const float l = sqrtf(px * px + py * py);
                    if (l > maxL) maxL = l;
                    if (l > 0.5f) ++over;
                    if (l > 0.05f) ++over05;
                    if (l > 0.15f) ++over15;
                    ++n;
                    auto c8 = [](float v) { int i = (int)lroundf(128.f + 16.f * v); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
                    png[(size_t)y * w + x] = c8(px) | c8(py) << 8 | 128u << 16 | 0xff000000u;
                }
            }
            ctx->Unmap(st, 0);
            Log("dlssmv: eye %d %ux%u fmt %d (texture %ux%u) scale %.1f %.1f | mean px %.3f %.3f | mean |px| %.3f %.3f | "
                "max %.2f | %.1f %% over 0.5 px, %.1f %% over 0.15, %.1f %% over 0.05 (frame %llu)", eye, w, h,
                (int)d.Format, d.Width, d.Height, sx, sy, n ? sumX / n : 0.0, n ? sumY / n : 0.0, n ? absX / n : 0.0,
                n ? absY / n : 0.0, maxL, n ? 100.0 * over / n : 0.0, n ? 100.0 * over15 / n : 0.0,
                n ? 100.0 * over05 / n : 0.0, (unsigned long long)g_mvFrame.load());
            char name[32];
            sprintf_s(name, "dlssmv_eye%d", eye);
            diag::WritePng(name, (const uint8_t*)png.data(), w, h, w * 4, (int)DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        }
        st->Release();
    }
    if (dev) dev->Release();
    t->Release();
}

std::atomic<int> g_logEvals{0};    // dlsslog N
std::atomic<int> g_logBuilds{0};   // RenderView builds logged alongside
std::atomic<uint64_t> g_resets[3];  // evaluations with Reset set: eye 0, eye 1, flat
std::atomic<uint64_t> g_frame{0};   // eye-0 / flat evaluations (a frame counter for the log)

struct Twin { const void* game; void* eye1; };
std::mutex g_mu;
std::vector<Twin> g_twins;
std::atomic<uint64_t> g_evals[2];

NgxResult __cdecl hk_Create(ID3D11DeviceContext* ctx, int feature, void* params, void** out) {
    if (feature == kFeatureSuperSampling && params) {
        NgxParams* p = (NgxParams*)params;
        // dlss-vr: what the game creates DLSS with. Flags: 1 HDR, 2 MVLowRes, 4 MVJittered, 8 DepthInverted,
        // 0x20 DoSharpening, 0x40 AutoExposure
        Log("dlss: create %ux%u -> %ux%u quality %d flags 0x%x (HDR %d, low-res MVs %d, jittered MVs %d, depth inverted %d, "
            "sharpening %d, auto exposure %d)", GetU(p, "Width"), GetU(p, "Height"), GetU(p, "OutWidth"),
            GetU(p, "OutHeight"), GetI(p, "PerfQualityValue"), GetI(p, "DLSS.Feature.Create.Flags"),
            GetI(p, "DLSS.Feature.Create.Flags") & 1, (GetI(p, "DLSS.Feature.Create.Flags") >> 1) & 1,
            (GetI(p, "DLSS.Feature.Create.Flags") >> 2) & 1, (GetI(p, "DLSS.Feature.Create.Flags") >> 3) & 1,
            (GetI(p, "DLSS.Feature.Create.Flags") >> 5) & 1, (GetI(p, "DLSS.Feature.Create.Flags") >> 6) & 1);
        if (GetU(p, "Width") > 0 && GetU(p, "Width") != 0xffffffffu)
            g_dlssScale = (float)GetU(p, "OutWidth") / (float)GetU(p, "Width");
        // gowvr.ini dlss_preset (0 = the DLL's own): the render preset hint for every quality mode (1 = A ... 11 = K);
        // honoured only by DLSS 3.1 and newer DLLs (the game ships 2.3.4)
        const int preset = (int)config::GetFloat("dlss_preset", 0.f);
        if (preset > 0) {
            for (const char* k : {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
                                  "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
                                  "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"})
                p->Set(k, (unsigned int)preset);
            Log("dlss: render preset hint %d set (dlss_preset)", preset);
        }
    }
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
    if (params) {
        NgxParams* p = (NgxParams*)const_cast<void*>(params);
        if (eye <= 0) g_mvFrame = ++g_frame;
        if (g_findSteps.load() > 0) {
            FindStep(JitterPhase(GetF(p, "Jitter.Offset.X"), GetF(p, "Jitter.Offset.Y")));
            if (g_findSteps.fetch_sub(1) == 1) { g_cands.clear(); g_snap.clear(); g_findPhase = -1; }
        }
        if (g_mvCaps[eye < 0 ? 2 : eye].load() > 0 && g_mvCaps[eye < 0 ? 2 : eye].fetch_sub(1) == 1)
            CaptureMv(ctx, p, eye);
        if (g_jitSteps.load() > 0) {
            const float jx = GetF(p, "Jitter.Offset.X"), jy = GetF(p, "Jitter.Offset.Y");
            if (jx != 0.f || jy != 0.f) FindJitter(jx, jy);
            if (g_jitSteps.fetch_sub(1) == 1) FindJitterReport();
        }
        const int reset = GetI(p, "Reset", 0);
        if (reset) ++g_resets[eye < 0 ? 2 : eye];
        const float gameSharp = GetF(p, "Sharpness");
        // dlss-vr: DLSS 2.x sharpening (settings.ini DLSSSharpness, 100 = the maximum) makes every thin edge shimmer in a
        // headset; in VR the sharpness is gowvr.ini dlss_vr_sharpness (-1..1, default 0 = none; below -1 = the game's)
        static float logged = -999.f;
        const float vrSharp = config::GetFloat("dlss_vr_sharpness", 0.f);
        if (eye >= 0 && vrSharp >= -1.f && vrSharp <= 1.f && gameSharp > -999.f) {
            p->Set("Sharpness", vrSharp);
            if (logged != gameSharp) {
                logged = gameSharp;
                Log("dlss: VR sharpness %.2f instead of the game's %.2f (dlss_vr_sharpness)", vrSharp, gameSharp);
            }
        }
        if (g_logEvals.load() > 0) {
            g_logEvals.fetch_sub(1);
            Log("dlsslog: f%llu eye %d %s jitter %.4f %.4f mvscale %.1f %.1f reset %d sharp %.2f->%.2f subrect %ux%u "
                "pre-exposure %.3f exposure scale %.3f", (unsigned long long)g_frame.load(), eye,
                h != handle ? "twin" : "game", GetF(p, "Jitter.Offset.X"), GetF(p, "Jitter.Offset.Y"),
                GetF(p, "MV.Scale.X"), GetF(p, "MV.Scale.Y"), reset, gameSharp, GetF(p, "Sharpness"),
                GetU(p, "DLSS.Render.Subrect.Dimensions.Width"), GetU(p, "DLSS.Render.Subrect.Dimensions.Height"),
                GetF(p, "DLSS.Pre.Exposure"), GetF(p, "DLSS.Exposure.Scale"));
            if (g_logEvals.load() == 0)
                Log("dlsslog: done - evaluations with Reset so far: eye 0 %llu, eye 1 %llu, flat %llu",
                    (unsigned long long)g_resets[0].load(), (unsigned long long)g_resets[1].load(),
                    (unsigned long long)g_resets[2].load());
        }
    }
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

void DlssVrFrame(bool stereo) {
    uint8_t* on = (uint8_t*)GameVa(kDlssOnVa);
    int* phases = (int*)GameVa(kDlssPhasesVa);
    if (!on || !phases) return;
    const int cfg = (int)config::GetFloat("dlss_vr_phases", 0.f);
    if (stereo && *on == 1 && cfg >= 0) {
        int want = cfg;
        if (want == 0) {
            const float s = g_dlssScale.load();
            want = s > 0.f ? (int)lroundf(8.f * s * s) : 16;
        }
        want = want < 8 ? 8 : want > 64 ? 64 : want;
        if (*phases != want) {
            if (g_origPhases < 0) g_origPhases = *phases;
            Log("dlss: VR jitter phases %d (the game's %d; DLSS scale %.2f, dlss_vr_phases)", want, *phases,
                g_dlssScale.load());
            *phases = want;
        }
        g_setPhases = want;
    } else if (g_origPhases >= 0) {
        if (*phases == g_setPhases) *phases = g_origPhases;
        Log("dlss: jitter phases back to the game's %d", g_origPhases);
        g_origPhases = g_setPhases = -1;
    }
}

void DlssLog(int evals) {
    g_logBuilds = evals;
    g_logEvals = evals;
    Log("dlsslog: next %d evaluations (and RenderView builds)", evals);
}

void DlssFind(int evals) {
    if (evals == 0) {  // "dlssfind 0": capture the motion vectors (the 3rd evaluation of each eye from now)
        for (auto& c : g_mvCaps) c = 3;
        Log("dlssmv: capturing the motion vectors of each eye");
        return;
    }
    if (evals < 0) {
        g_jitSteps = -evals;
        Log("dlssfindj: the next %d evaluations", -evals);
        return;
    }
    g_findSteps = evals;
    Log("dlssfind: the next %d evaluations", evals);
}

void DlssNoteProjection(int eye, const float* proj) {
    if (g_logBuilds.load() <= 0 || !proj) return;
    g_logBuilds.fetch_sub(1);
    // a jittered projection carries the offset in its off-centre terms (row-vector: [8] [9]; column-vector: [2] [6])
    Log("dlsslog: f%llu build eye %d proj off-centre [8] %.6f [9] %.6f [2] %.6f [6] %.6f (P00 %.4f P11 %.4f)",
        (unsigned long long)g_frame.load(), eye, proj[8], proj[9], proj[2], proj[6], proj[0], proj[5]);
    // every other matrix in the RenderView (0x590 bytes) with the same P00 / P11: a jittered copy would show non-zero
    // off-centre terms there
    char line[900];
    int len = sprintf_s(line, "dlsslog:   other projections:");
    for (int i = 4; i + 16 <= 0x590 / 4 && len < 820; i += 4)
        if (proj[i] == proj[0] && proj[i + 5] == proj[5])
            len += sprintf_s(line + len, sizeof(line) - len, " +0x%x [8] %.6f [9] %.6f [2] %.6f [6] %.6f |", i * 4,
                             proj[i + 8], proj[i + 9], proj[i + 2], proj[i + 6]);
    Log("%s", line);
}

void DlssEyesEnable(bool on) {
    g_on = on ? 1 : 0;
    config::SetFloat("dlss_per_eye", on ? 1.f : 0.f);
    Log("dlss: per-eye instances %s (applies to DLSS created after this; restart the game for the current one)",
        on ? "on" : "off");
}

}  // namespace gowvr::stereo
