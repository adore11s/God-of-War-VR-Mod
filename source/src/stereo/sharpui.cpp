// D-104 sharp HUD (owner: "yeah I want the sharp HUD"; D-101: the UI shrunk by its 2D view to 0.35 is drawn ~3x smaller
// into the 3840x2130 image with no mip filtering - pixelated - and the reticle cannot be sized on its own).
//
// How:
// - Game thread (UiScaleView -> SharpUiMarkView): the 2D UI views C / D are NOT scaled; their camera x (view +0x180, 0
//   for the UI camera) is set to kMarker. That value reaches the view's viewData constant buffer (floats [16..31]
//   camera->world, position x at float 19 / 28), so the frame's own data says "draw this UI for the composite".
// - Render thread, the game's command executor (sub_1409ab650 - every game draw / bind / constant upload of the frame)
//   at its D3D11 call sites. (Not the context's function table: LIVE 2026-10-04 d3d11 keeps it inside the context object
//   and keeps rewriting it - its own fast-path variants into the draw / UpdateSubresource slots every frame, everything
//   on SwapDeviceContextState - so table hooks there vanished: probe 0 draws seen.)
//   UpdateSubresource: a constant buffer written with an orthographic projection (float 47 = 1) and the marker at float
//     19 or 28 is a sharp-UI viewData buffer (LIVE: float 19; rewritten with anything else: no longer one).
//   VSSetConstantBuffers / OMSetRenderTargets(+UAV) / OMSetBlendState / ClearState: what the GAME binds is recorded.
//   Draw*: right before each draw - a marked viewData buffer bound to the VS and the game's render target a full-
//     resolution one (LIVE: the UI goes into an offscreen 3840x2130 R10G10B10A2 image, copied to the backbuffer by the
//     frame's last draw) -> our transparent HUD target is bound instead (same size, the game's depth view kept) with a
//     blend state that also accumulates coverage in alpha; anything else -> the game's own bindings again. The
//     executor's redundant-state cache therefore always matches what D3D has bound when we let go.
//   The mod's own drawing (xr.cpp blit, our composite, eye copies) calls d3d11 directly: never seen here.
// - The paste (per eye kick): right before the draw that copies the UI's image to the backbuffer (fallback: before the
//   eye image is copied - eyes.cpp CaptureEye - and at Present, onto the backbuffer): mips generated, the HUD pasted in
//   premultiplied over - the whole UI scaled about the screen centre (UiScaleCurrent: ui_scale, ui_scale_fp in F1 / F3),
//   and while aiming (VR left trigger) the reticle region (ui_reticle_x / _y, radius ui_reticle_r) at ui_reticle_scale
//   at its true screen spot. Trilinear (ui_mip_bias): no pixel crawl. LIVE 2026-10-04 harness eye shots: both eyes.
// - Safety: the marked viewData not seen while views are marked (300 frames), or an executor site not hooked -> sharp
//   HUD off, the scaled UI as before.
#include "sharpui.h"
#include "mutate.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/detour.h"
#include "../diag/shot.h"
#include "../vr/xr.h"
#include "../fp/firstperson.h"
#include <windows.h>
#include <d3d11_1.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace gowvr::stereo {
namespace {

constexpr float kMarker = 0.0009765625f;  // 1/1024, exact in float: the UI camera's x (always 0 otherwise)

using PFN_Update = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                             UINT, UINT);
using PFN_VSSetCB = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using PFN_OMSetRT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*);
using PFN_OMSetRTUAV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                 ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                                 const UINT*);
using PFN_OMSetBlend = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInst = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInst = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_SetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DeviceChild*, ID3D11ClassInstance* const*, UINT);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_CopyRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                 ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

// the function d3d11 has in the context's table slot right now (calls the real thing; nothing of ours is in the table)
template <class F>
F Cur(ID3D11DeviceContext* c, int i) { return (F)(*(void***)c)[i]; }

std::atomic<bool> g_enabled{true}, g_broken{false}, g_installed{false};
bool g_sitesOk = false;  // every executor site the redirect needs is hooked (InstallSites)
std::atomic<uint64_t> g_markedFrames{0}, g_substFrames{0}, g_substitutions{0};
std::atomic<uint64_t> g_markSeen{0}, g_bbDrawsMarked{0};  // marked viewData uploads; backbuffer draws after one
std::atomic<float> g_retScale{-2.f};  // D-154: -2 = not read yet, <= 0 = the HUD's scale
std::atomic<ULONGLONG> g_retObjAt{0};  // D-149: last time the aiming icon's UI objects were placed (body.cpp)

// ---- render-thread state (all hooks run on the render thread) ------------------------------------------------------
ID3D11Device* g_dev = nullptr;
struct GameState {
    ID3D11RenderTargetView* rtv[8] = {};
    UINT nRtv = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {1, 1, 1, 1};
    UINT mask = 0xffffffff;
    ID3D11Buffer* vscb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
} g_gs, g_gsSaved;
bool g_subst = false, g_drew = false, g_needClear = true;
// constant buffers currently holding a marked UI viewData. D-109: a fixed array, never a growing vector - crash
// 2026-10-05 00:02 in Reconcile read g_uiBuffers' begin / end from two different reallocation states (end < begin).
// D-110 (code audit M6, the "two rare cases"): every slot is one atomic pointer and the table is written from ANY
// thread - the upload site a7582 also runs on job worker threads, whose uploads D-108 ignored: a UI buffer rewritten
// there with scene data stayed marked (scene draws with it would have gone into the HUD target), and a UI viewData
// uploaded there was never marked (that UI drawn unscaled). A full table no longer refuses new UI buffers (stale marks
// filled it -> HUD unscaled): the least recently marked slot makes room.
constexpr int kMaxUiBufs = 32;
std::atomic<ID3D11Buffer*> g_uiBuf[kMaxUiBufs] = {};
std::atomic<uint64_t> g_uiBufTick[kMaxUiBufs] = {};  // g_uiMarkTick when the slot was last marked (LRU)
std::atomic<uint64_t> g_uiMarkTick{0};
std::atomic<int> g_uiBufN{0};                        // slots [0, n) may be in use (only grows; slots are reused from 0)
ID3D11Texture2D* g_bbTex = nullptr;      // the swapchain's buffer 0 (raw pointer, compared only)
ID3D11RenderTargetView* g_bbCheckRtv = nullptr;
bool g_bbCheckResult = false;
// HUD target
ID3D11Texture2D* g_uiTex = nullptr;
ID3D11RenderTargetView* g_uiRtv = nullptr;
ID3D11ShaderResourceView* g_uiSrv = nullptr;
UINT g_uiW = 0, g_uiH = 0;
// converted blend states (game state -> HUD-target state), originals AddRef'd so their addresses are never reused
struct BlendConv { ID3D11BlendState* orig; ID3D11BlendState* conv; bool forceFactor; };
constexpr int kMaxBlends = 128;  // D-109: fixed (see g_uiBuf)
BlendConv g_blends[kMaxBlends] = {};
std::atomic<int> g_blendN{0};
BlendConv g_nullConv{nullptr, nullptr, true};
// composite
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11BlendState* g_overBlend = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3DDeviceContextState* g_ownState = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11RenderTargetView* g_bbRtv = nullptr;
ID3D11Texture2D* g_bbRtvFor = nullptr;
bool g_compositeReady = false, g_compositeFailed = false;

bool IsBackbuffer(ID3D11RenderTargetView* rtv) {
    if (!rtv || !g_bbTex) return false;
    if (rtv == g_bbCheckRtv) return g_bbCheckResult;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    const bool is = r == g_bbTex;
    if (r) r->Release();
    g_bbCheckRtv = rtv;  // (a raw cache key only)
    g_bbCheckResult = is;
    return is;
}

// A full-resolution target (the backbuffer's width x height). LIVE probe 2026-10-04: the game draws its UI into an
// offscreen 3840x2130 R10G10B10A2 image (the upscaled scene), then copies that to the backbuffer with one last draw.
struct RtInfo { ID3D11RenderTargetView* rtv; bool full; };
RtInfo g_rtCache[16];
int g_rtCacheN = 0;
bool IsFullRes(ID3D11RenderTargetView* rtv) {
    if (!rtv || !g_bbTex) return false;
    for (int i = 0; i < g_rtCacheN; ++i) if (g_rtCache[i].rtv == rtv) return g_rtCache[i].full;
    bool full = false;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    if (r) {
        D3D11_RESOURCE_DIMENSION dim;
        r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td, bd;
            ((ID3D11Texture2D*)r)->GetDesc(&td);
            g_bbTex->GetDesc(&bd);
            full = td.Width == bd.Width && td.Height == bd.Height && td.SampleDesc.Count == 1;
        }
        r->Release();
    }
    // D-124 (owner crashes 19:24 / 19:30 / 19:5x / 20:0x, all in the HUD paste on the first stereo frame after an area
    // load): once full (new areas = new render targets) the slot was (int)(ptr >> 4) % 16 - NEGATIVE when bit 35 of the
    // pointer is set, so it wrote 16 bytes BEFORE the array: over g_rs and g_cb (rtv pointer + the bool's padding ->
    // g_cb = junk / 0 / "TANGENT"-text memory). Unsigned modulo now
    g_rtCache[g_rtCacheN < 16 ? g_rtCacheN++ : (int)(((uintptr_t)rtv >> 4) % 16u)] = {rtv, full};  // (raw cache keys only)
    return full;
}

int UiBufCount() {
    const int n = g_uiBufN.load(std::memory_order_acquire);
    return n < 0 ? 0 : n > kMaxUiBufs ? kMaxUiBufs : n;
}
bool IsUiBuffer(ID3D11Buffer* b) {
    if (!b) return false;
    const int n = UiBufCount();
    for (int i = 0; i < n; ++i) if (g_uiBuf[i].load(std::memory_order_acquire) == b) return true;
    return false;
}
// a constant buffer was rewritten: it holds a marked UI viewData now (ui) or not. Any thread (D-110).
void UiBufSet(ID3D11Buffer* b, bool ui) {
    if (!b) return;
    const int n = UiBufCount();
    if (!ui) {  // rewritten with something else: no longer a UI buffer (every slot holding it)
        for (int i = 0; i < n; ++i) {
            ID3D11Buffer* e = b;
            g_uiBuf[i].compare_exchange_strong(e, nullptr, std::memory_order_acq_rel);
        }
        return;
    }
    const uint64_t tick = ++g_uiMarkTick;
    for (int i = 0; i < n; ++i)
        if (g_uiBuf[i].load(std::memory_order_acquire) == b) { g_uiBufTick[i] = tick; return; }
    for (int pass = 0; pass < 2; ++pass) {
        int slot = -1;
        if (pass == 0) {  // a free slot, lowest first (keeps the scanned range short)
            for (int i = 0; i < kMaxUiBufs && slot < 0; ++i)
                if (!g_uiBuf[i].load(std::memory_order_acquire)) slot = i;
        } else {  // full: the least recently marked slot
            uint64_t oldest = ~0ull;
            for (int i = 0; i < kMaxUiBufs; ++i)
                if (const uint64_t t = g_uiBufTick[i].load(); t < oldest) { oldest = t; slot = i; }
        }
        if (slot < 0) continue;
        ID3D11Buffer* e = g_uiBuf[slot].load(std::memory_order_acquire);
        if (pass == 0 && e) continue;  // taken meanwhile: look again
        if (!g_uiBuf[slot].compare_exchange_strong(e, b, std::memory_order_acq_rel)) continue;
        g_uiBufTick[slot] = tick;
        int h = g_uiBufN.load();
        while (h < slot + 1 && !g_uiBufN.compare_exchange_weak(h, slot + 1)) {}
        ++g_markSeen;
        return;
    }
}

const BlendConv& Convert(ID3D11BlendState* bs) {
    if (!bs) {
        if (!g_nullConv.conv) {
            D3D11_BLEND_DESC d{};
            auto& t = d.RenderTarget[0];
            t.BlendEnable = TRUE;
            t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_ZERO; t.BlendOp = D3D11_BLEND_OP_ADD;
            t.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR; t.DestBlendAlpha = D3D11_BLEND_ZERO; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            g_dev->CreateBlendState(&d, &g_nullConv.conv);
        }
        return g_nullConv;
    }
    int nb = g_blendN.load(std::memory_order_relaxed);
    if (nb > kMaxBlends) nb = kMaxBlends;
    for (int i = 0; i < nb; ++i) if (g_blends[i].orig == bs) return g_blends[i];
    if (nb >= kMaxBlends) {  // full (never seen): the game's own state, unconverted
        static BlendConv passthrough{nullptr, nullptr, false};
        return passthrough;
    }
    D3D11_BLEND_DESC d{};
    bs->GetDesc(&d);
    bool force = false;
    const int n = d.IndependentBlendEnable ? 8 : 1;
    for (int i = 0; i < n; ++i) {
        auto& t = d.RenderTarget[i];
        if (!t.BlendEnable) {  // opaque UI: covers what is under it -> alpha 1 (through the blend factor)
            t.BlendEnable = TRUE;
            t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_ZERO; t.BlendOp = D3D11_BLEND_OP_ADD;
            t.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR; t.DestBlendAlpha = D3D11_BLEND_ZERO; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (i == 0) force = true;
        } else {
            // additive (dest ONE): coverage unchanged -> the composite adds it; otherwise "over": alpha accumulates
            const bool additive = t.DestBlend == D3D11_BLEND_ONE;
            t.SrcBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
            t.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
            t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        if (t.RenderTargetWriteMask & 7) t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    d.AlphaToCoverageEnable = FALSE;
    BlendConv c{bs, nullptr, force};
    if (FAILED(g_dev->CreateBlendState(&d, &c.conv))) c.conv = nullptr;
    bs->AddRef();
    g_blends[nb] = c;
    g_blendN = nb + 1;
    return g_blends[nb];
}

void ApplyConvertedBlend(ID3D11DeviceContext* ctx) {
    const BlendConv& c = Convert(g_gs.blend);
    static const FLOAT one[4] = {1, 1, 1, 1};
    Cur<PFN_OMSetBlend>(ctx, 35)(ctx, c.conv ? c.conv : g_gs.blend, c.forceFactor ? one : g_gs.factor, g_gs.mask);
}

void RestoreGame(ID3D11DeviceContext* ctx) {
    Cur<PFN_OMSetRT>(ctx, 33)(ctx, g_gs.nRtv, g_gs.rtv, g_gs.dsv);
    Cur<PFN_OMSetBlend>(ctx, 35)(ctx, g_gs.blend, g_gs.factor, g_gs.mask);
}

bool EnsureUiTarget() {
    if (!g_bbTex) return false;
    D3D11_TEXTURE2D_DESC bd;
    g_bbTex->GetDesc(&bd);
    if (g_uiTex && g_uiW == bd.Width && g_uiH == bd.Height) return true;
    if (g_uiSrv) { g_uiSrv->Release(); g_uiSrv = nullptr; }
    if (g_uiRtv) { g_uiRtv->Release(); g_uiRtv = nullptr; }
    if (g_uiTex) { g_uiTex->Release(); g_uiTex = nullptr; }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = bd.Width;
    d.Height = bd.Height;
    d.MipLevels = 0;  // full chain
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiTex))) { Log("sharpui: HUD target creation failed"); return false; }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = d.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipSlice = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = d.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = (UINT)-1;
    if (FAILED(g_dev->CreateRenderTargetView(g_uiTex, &rd, &g_uiRtv)) ||
        FAILED(g_dev->CreateShaderResourceView(g_uiTex, &sd, &g_uiSrv))) {
        Log("sharpui: HUD target views failed");
        return false;
    }
    g_uiW = bd.Width;
    g_uiH = bd.Height;
    Log("sharpui: HUD target %ux%u RGBA16F with mips", g_uiW, g_uiH);
    return true;
}

// ---- probe ("sharpui probe [frames]"): where the marked UI view data is uploaded and what the frame's last draws are ----
// Besides the camera x (kMarker), SharpUiMarkView puts k * 2^-100 into the w column ([0][3], 0 for these views) of six
// matrices of the UI view - a vertex's x adds x * ~1e-30 to w: nothing on screen. An exact copy names the field; a CPU
// product (world x view-projection per UI element) leaves a tiny value in the same range.
constexpr int kTinyFields = 6;
const uint32_t kTinyOff[kTinyFields] = {0x10, 0x50, 0x90, 0xd0, 0x110, 0x150};
const char* const kTinyName[kTinyFields] = {"+0x10 world->view", "+0x50 view*proj", "+0x90 view*proj*viewport",
                                            "+0xd0 proj", "+0x110 proj*viewport", "+0x150 camera->world"};
float TinyMarker(int k) { return ldexpf((float)(k + 1), -100); }
std::atomic<int> g_probeArm{0}, g_probeFrames{0};
int g_probeFinds = 0, g_probeTiny = 0;
std::vector<ID3D11Resource*> g_probeMarked;  // resources an upload put a marker into (probe frames, raw)
struct Run {
    ID3D11RenderTargetView* rtv;
    UINT w, h;
    int fmt;
    bool bb;
    int draws, markedVs, markedPs;
    void *vs, *ps;
    ID3D11Buffer* vscb[4];
};
Run g_runs[64];
int g_runN = 0;  // runs this frame (ring index n % 64): a new run when the target or a shader changes
struct MarkedRt { ID3D11RenderTargetView* rtv; UINT w, h; int fmt; bool bb; uint64_t draws; };
MarkedRt g_markedRts[8];
int g_nMarkedRts = 0;
uint64_t g_probeDraws = 0, g_probeBbDraws = 0, g_probeMarkedDraws = 0, g_probeUpd = 0, g_probeSubDraws = 0;
void *g_curVs = nullptr, *g_curPs = nullptr;
ID3D11Buffer* g_pscb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};

bool Probing() { return g_probeFrames.load(std::memory_order_relaxed) > 0; }

uint64_t Rva(uintptr_t a) {
    static const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    return a >= base && a < base + 0x6000000 ? 0x140000000ull + (a - base) : (uint64_t)a;
}

// first float holding the camera marker or a tiny (marker-range) value; -1 none
int ScanMarkers(const void* data, UINT bytes, UINT from, float* found) {
    const float* f = (const float*)data;
    UINT n = bytes / 4;
    if (n > (1u << 20)) n = 1u << 20;
    __try {
        for (UINT i = from; i < n; ++i) {
            const float v = f[i], a = fabsf(v);
            if (v == kMarker || (a > 5.0e-34f && a < 8.0e-27f)) { *found = v; return (int)i; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

bool ReadFloats(const void* src, float* out, int n) {
    __try {
        memcpy(out, src, (size_t)n * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// exact markers seen in uploads (probe frames): resource + byte offset - a later copy out of that range carries them on
struct MarkerAt { ID3D11Resource* res; UINT byteOff; };
MarkerAt g_markerAt[256];
int g_nMarkerAt = 0;

void ProbeMark(ID3D11Resource* r) {
    for (ID3D11Resource* m : g_probeMarked) if (m == r) return;
    if (g_probeMarked.size() < 256) g_probeMarked.push_back(r);
}

void ProbeUploadOne(const char* api, uintptr_t caller, ID3D11Resource* r, const void* data, UINT bytes, UINT off, int i,
                    float v);

void ProbeUpload(const char* api, uintptr_t caller, ID3D11Resource* r, const void* data, UINT bytes, UINT off) {
    if (!data || bytes < 4) return;
    UINT from = 0;
    for (int hits = 0; hits < 64; ++hits) {
        float v = 0.f;
        const int i = ScanMarkers(data, bytes, from, &v);
        if (i < 0) return;
        from = (UINT)i + 1;
        ProbeUploadOne(api, caller, r, data, bytes, off, i, v);
    }
}

void ProbeUploadOne(const char* api, uintptr_t caller, ID3D11Resource* r, const void* data, UINT bytes, UINT off, int i,
                    float v) {
    int field = -1;
    for (int k = 0; k < kTinyFields; ++k) if (v == TinyMarker(k)) field = k;
    const bool exact = v == kMarker || field >= 0;
    if (exact) {
        ProbeMark(r);
        if (g_nMarkerAt < 256) g_markerAt[g_nMarkerAt++] = {r, off + (UINT)i * 4};
    }
    int& budget = exact ? g_probeFinds : g_probeTiny;
    if (budget >= (exact ? 24 : 6)) return;
    ++budget;
    const int n = (int)(bytes / 4);
    int b0 = i & ~15;
    if (b0 + 16 > n) b0 = n > 16 ? n - 16 : 0;
    const int cnt = n - b0 < 16 ? n - b0 : 16;
    float m[16] = {};
    ReadFloats((const float*)data + b0, m, cnt);
    Log("sharpui probe: %s caller 0x%llx res %p (%u bytes at +%u) float %d = %g [%s] | from float %d: %.4g %.4g %.4g %.4g / "
        "%.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g",
        api, (unsigned long long)Rva(caller), (void*)r, bytes, off, i, v,
        v == kMarker ? "camera x" : field >= 0 ? kTinyName[field] : "tiny (product?)", b0, m[0], m[1], m[2], m[3], m[4],
        m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

// a copy out of a range holding an exact marker: the destination carries it (probe frames)
void ProbeCopy(uintptr_t caller, ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box) {
    static int logged = 0;
    for (int k = 0; k < g_nMarkerAt; ++k) {
        const MarkerAt& m = g_markerAt[k];
        if (m.res != src || (box && (m.byteOff < box->left || m.byteOff >= box->right))) continue;
        ProbeMark(dst);
        if (logged < 24) {
            ++logged;
            UINT dstBytes = 0;
            D3D11_RESOURCE_DIMENSION dim;
            dst->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd;
                ((ID3D11Buffer*)dst)->GetDesc(&bd);
                dstBytes = bd.ByteWidth;
            }
            Log("sharpui probe: copy caller 0x%llx %p [%u..%u) -> %p (%u bytes) at +%u: marker at src +%u = dst float %d",
                (unsigned long long)Rva(caller), (void*)src, box ? box->left : 0, box ? box->right : 0, (void*)dst,
                dstBytes, dstX, m.byteOff, (int)((dstX + m.byteOff - (box ? box->left : 0)) / 4));
        }
        return;
    }
}

bool ProbeIsMarked(ID3D11Buffer* b) {
    if (!b) return false;
    for (ID3D11Resource* m : g_probeMarked) if (m == b) return true;
    return false;
}

void ProbeDraw() {
    ++g_probeDraws;
    ID3D11RenderTargetView* rtv = g_gs.nRtv ? g_gs.rtv[0] : nullptr;
    bool mv = false, mp = false;
    for (ID3D11Buffer* b : g_gs.vscb) if (ProbeIsMarked(b)) { mv = true; break; }
    for (ID3D11Buffer* b : g_pscb) if (ProbeIsMarked(b)) { mp = true; break; }
    Run* cur = g_runN ? &g_runs[(g_runN - 1) % 64] : nullptr;
    if (!cur || cur->rtv != rtv || cur->vs != g_curVs || cur->ps != g_curPs) {
        cur = &g_runs[g_runN % 64];
        ++g_runN;
        *cur = {};
        cur->rtv = rtv;
        cur->vs = g_curVs;
        cur->ps = g_curPs;
        for (int i = 0; i < 4; ++i) cur->vscb[i] = g_gs.vscb[i];
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            if (res) {
                D3D11_RESOURCE_DIMENSION d;
                res->GetType(&d);
                if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC td;
                    ((ID3D11Texture2D*)res)->GetDesc(&td);
                    cur->w = td.Width;
                    cur->h = td.Height;
                    cur->fmt = (int)td.Format;
                }
                cur->bb = res == g_bbTex;
                res->Release();
            }
        }
    }
    ++cur->draws;
    if (cur->bb) ++g_probeBbDraws;
    if (mv) ++cur->markedVs;
    if (mp) ++cur->markedPs;
    if (mv || mp) {
        ++g_probeMarkedDraws;
        int i = 0;
        while (i < g_nMarkedRts && g_markedRts[i].rtv != rtv) ++i;
        if (i == g_nMarkedRts && i < 8) g_markedRts[g_nMarkedRts++] = {rtv, cur->w, cur->h, cur->fmt, cur->bb, 0};
        if (i < 8) ++g_markedRts[i].draws;
    }
}

void ProbeDump() {
    Log("sharpui probe: done - draws %llu (backbuffer %llu), draws with a marked buffer bound %llu; UpdateSubresource "
        "uploads %llu; marked resources %zu; markSeen %llu, UI redirects %llu, draws into the HUD target %llu",
        (unsigned long long)g_probeDraws, (unsigned long long)g_probeBbDraws, (unsigned long long)g_probeMarkedDraws,
        (unsigned long long)g_probeUpd, g_probeMarked.size(), (unsigned long long)g_markSeen.load(),
        (unsigned long long)g_substitutions.load(), (unsigned long long)g_probeSubDraws);
    for (int i = 0; i < g_nMarkedRts; ++i)
        Log("sharpui probe: marked draws onto rtv %p %ux%u fmt %d%s: %llu", (void*)g_markedRts[i].rtv, g_markedRts[i].w,
            g_markedRts[i].h, g_markedRts[i].fmt, g_markedRts[i].bb ? " (BACKBUFFER)" : "",
            (unsigned long long)g_markedRts[i].draws);
    const int first = g_runN > 64 ? g_runN - 64 : 0;
    Log("sharpui probe: last frame's final %d of %d draw runs (target / shaders changes):", g_runN - first, g_runN);
    for (int n = first; n < g_runN; ++n) {
        const Run& r = g_runs[n % 64];
        Log("sharpui probe:  run %d rtv %p %ux%u fmt %d%s draws %d markedVS %d markedPS %d vs %p ps %p vscb %p %p %p %p", n,
            (void*)r.rtv, r.w, r.h, r.fmt, r.bb ? " BB" : "", r.draws, r.markedVs, r.markedPs, r.vs, r.ps,
            (void*)r.vscb[0], (void*)r.vscb[1], (void*)r.vscb[2], (void*)r.vscb[3]);
    }
}

// ---- D-145 UI draw census (owner: "the head-following UI takes the enemy bars into it - they should be in the normal 3D
// world"). The enemy bars are UI models like the rest of the HUD (2D views C / D), placed by the game at the projection
// of the enemy; nothing in the code tells their draws from the screen-fixed HUD's, so they are pasted scaled about the
// centre with the rest (at ui_scale_fp they sit at half the angle - they follow the head). Read-only evidence: while
// gameplay first person shows UI, every 15 s one frame's redirected UI draws are logged (draw arguments, shaders, each
// VS constant buffer's last uploaded floats) with the head's yaw / pitch; a world-anchored element is the one whose
// placement moves against the head. 24 per session; command "sharpui census" arms one at once.
std::atomic<int> g_censusArm{0};
bool g_census = false;  // render thread: capture uploads (frame 1), log the UI draws (frame 2)
int g_censusFrames = 0, g_censusNo = 0, g_censusDraws = 0;
struct CbSnap { ID3D11Buffer* b; UINT bytes; float f[16]; };
constexpr int kCbSnaps = 1024;
CbSnap g_cbSnap[kCbSnaps] = {};
void CensusUpload(ID3D11Buffer* b, const void* src, UINT bytes) {
    size_t h = ((uintptr_t)b >> 4) % kCbSnaps;
    for (int k = 0; k < 16; ++k, h = (h + 1) % kCbSnaps) {
        CbSnap& c = g_cbSnap[h];
        if (c.b && c.b != b) continue;
        c.b = b;
        c.bytes = bytes;
        memset(c.f, 0, sizeof(c.f));
        memcpy(c.f, src, bytes < sizeof(c.f) ? bytes : sizeof(c.f));
        return;
    }
}
const CbSnap* CensusFind(ID3D11Buffer* b) {
    size_t h = ((uintptr_t)b >> 4) % kCbSnaps;
    for (int k = 0; k < 16; ++k, h = (h + 1) % kCbSnaps) {
        if (g_cbSnap[h].b == b) return &g_cbSnap[h];
        if (!g_cbSnap[h].b) return nullptr;
    }
    return nullptr;
}
void CensusDraw(const char* kind, UINT a, UINT b, UINT c, INT d) {
    if (!g_census || g_censusFrames != 1 || !g_subst || g_censusDraws >= 200) return;
    ++g_censusDraws;
    char line[1400];
    int n = sprintf_s(line, "sharpui census %d: #%d %s(%u %u %u %d) vs %p ps %p", g_censusNo, g_censusDraws, kind, a, b, c,
                      d, g_curVs, g_curPs);
    for (int i = 0; i < 4 && n > 0 && n < (int)sizeof(line) - 240; ++i) {
        ID3D11Buffer* cb = g_gs.vscb[i];
        if (!cb) continue;
        int m = 0;
        if (IsUiBuffer(cb)) {
            m = sprintf_s(line + n, sizeof(line) - n, " | b%d UI view", i);
        } else if (const CbSnap* s = CensusFind(cb)) {
            m = sprintf_s(line + n, sizeof(line) - n, " | b%d %p %uB:", i, (void*)cb, s->bytes);
            for (int f = 0; f < 16 && m > 0 && n + m < (int)sizeof(line) - 24; ++f) {
                const int k = sprintf_s(line + n + m, sizeof(line) - n - m, " %.4g", s->f[f]);
                if (k <= 0) break;
                m += k;
            }
        } else {
            m = sprintf_s(line + n, sizeof(line) - n, " | b%d %p (no upload seen)", i, (void*)cb);
        }
        if (m <= 0) break;
        n += m;
    }
    Log("%s", line);
}
void CensusPresent() {
    static ULONGLONG lastCensus = 0;
    static uint64_t lastSubst = 0;
    const uint64_t subst = g_substitutions.load();
    const uint64_t perFrame = subst - lastSubst;
    lastSubst = subst;
    if (g_census) {
        if (--g_censusFrames <= 0) {
            g_census = false;
            Log("sharpui census %d: done (%d redirected UI draws logged)", g_censusNo, g_censusDraws);
        } else {
            vr::EyeViews ev = vr::XrLatestViews();
            float yaw = 0.f, pitch = 0.f;
            if (ev.valid) {
                const float* q = ev.pose[0].q;
                const float fx = -2.f * (q[0] * q[2] + q[3] * q[1]), fy = -2.f * (q[1] * q[2] - q[3] * q[0]),
                            fz = -(1.f - 2.f * (q[0] * q[0] + q[1] * q[1]));
                yaw = atan2f(fx, -fz) * 57.2958f;
                pitch = asinf(fmaxf(-1.f, fminf(1.f, fy))) * 57.2958f;
            }
            Log("sharpui census %d: head yaw %.1f pitch %.1f deg (headset space), UI scale %.2f, %llu redirects last frame",
                g_censusNo, yaw, pitch, UiScaleCurrent(), (unsigned long long)perFrame);
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const bool asked = g_censusArm.exchange(0) != 0;
    if (asked || (fp::FpMode() == 1 && perFrame >= 6 && now - lastCensus >= 15000 && g_censusNo < 24)) {
        lastCensus = now;
        ++g_censusNo;
        g_census = true;
        g_censusFrames = 2;
        g_censusDraws = 0;
        memset(g_cbSnap, 0, sizeof(g_cbSnap));
    }
}

bool OnRenderThread();  // D-108 (below)
ID3D11RenderTargetView* g_uiOrigRtv = nullptr;  // the game's target of the redirected UI draws (raw, compared / bound)
void PasteHud(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const char* where);

// Right before a game draw: our HUD target for a marked UI draw onto a full-resolution target, the game's bindings
// otherwise; and the frame's final draw onto the backbuffer (the copy of the UI's image) gets the HUD pasted into that
// image first - so the HUD goes through the game's own last pass like its UI did.
void Reconcile(ID3D11DeviceContext* ctx) {
    if (!OnRenderThread()) return;  // D-108
    if (Probing()) ProbeDraw();
    bool uiCb = false;
    if (UiBufCount() > 0)
        for (ID3D11Buffer* b : g_gs.vscb) if (b && IsUiBuffer(b)) { uiCb = true; break; }
    const bool full = g_gs.nRtv >= 1 && IsFullRes(g_gs.rtv[0]);
    if (full && g_markSeen.load()) ++g_bbDrawsMarked;
    const bool want = uiCb && full && EnsureUiTarget();
    if (!want && g_drew && g_uiOrigRtv && g_gs.nRtv >= 1 && g_gs.rtv[0] != g_uiOrigRtv && IsBackbuffer(g_gs.rtv[0])) {
        if (g_subst) {
            RestoreGame(ctx);
            g_subst = false;
        }
        PasteHud(ctx, g_uiOrigRtv, "into the UI image before its copy to the backbuffer");
    }
    if (want && !g_subst) {
        g_uiOrigRtv = g_gs.rtv[0];
        if (g_needClear) {
            static const FLOAT clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(g_uiRtv, clear);
            g_needClear = false;
        }
        ID3D11RenderTargetView* r = g_uiRtv;
        Cur<PFN_OMSetRT>(ctx, 33)(ctx, 1, &r, g_gs.dsv);
        ApplyConvertedBlend(ctx);
        g_subst = true;
        g_drew = true;
        ++g_substitutions;
    } else if (!want && g_subst) {
        RestoreGame(ctx);
        g_subst = false;
    }
    if (g_subst && Probing()) ++g_probeSubDraws;
}

// ---- hooks: the executor's call sites ---------------------------------------------------------------------------------
// Each forwards to the function d3d11 has in the context's table at that moment (Cur), then / before doing its part.
// D-108: only on the render thread (the one that presents and runs the frame's command streams). The upload site
// 0x1409a7582 (sub_1409a73e0, batched constant uploads) is also run by the job system's worker threads (d3d11's own
// lock serializes the D3D calls, not our bookkeeping): crash 2026-10-04 23:35 in site_Update walking g_uiBuffers while
// another thread reallocated it. Calls from any other thread are only forwarded.
std::atomic<DWORD> g_renderTid{0};  // set at Present (SharpUiPresentTick)
bool OnRenderThread() { return GetCurrentThreadId() == g_renderTid.load(std::memory_order_relaxed); }

void STDMETHODCALLTYPE site_Update(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
                                   const void* src, UINT rp, UINT dp) {
    Cur<PFN_Update>(ctx, 48)(ctx, dst, sub, box, src, rp, dp);
    if (!dst || !src) return;
    // D-110: the UI-buffer table is updated from every thread (atomic slots); the probe stays render-thread only
    const bool renderThread = OnRenderThread();
    D3D11_RESOURCE_DIMENSION dim;
    dst->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    ID3D11Buffer* b = (ID3D11Buffer*)dst;
    D3D11_BUFFER_DESC bd;
    b->GetDesc(&bd);
    if (renderThread && Probing()) {
        ++g_probeUpd;
        ProbeUpload("UpdateSubresource", (uintptr_t)_ReturnAddress(), dst, src, box ? box->right - box->left : bd.ByteWidth,
                    box ? box->left : 0);
    }
    if (box || sub) return;
    if (renderThread && g_census && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) CensusUpload(b, src, bd.ByteWidth);  // D-145
    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || bd.ByteWidth < 48 * 4) return;
    const float* f = (const float*)src;
    // the camera position column (x at float 19) or row (float 28) - either storage; orthographic: P33 (float 47) = 1
    const bool ui = f[47] == 1.f && (f[19] == kMarker || f[28] == kMarker);
    UiBufSet(b, ui);
}

void STDMETHODCALLTYPE site_VSSetCB(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11Buffer* const* bufs) {
    Cur<PFN_VSSetCB>(ctx, 7)(ctx, start, n, bufs);
    if (!OnRenderThread()) return;
    for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
        g_gs.vscb[start + i] = bufs ? bufs[i] : nullptr;
}

void STDMETHODCALLTYPE site_PSSetCB(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11Buffer* const* bufs) {
    Cur<PFN_VSSetCB>(ctx, 16)(ctx, start, n, bufs);
    if (!OnRenderThread()) return;
    for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
        g_pscb[start + i] = bufs ? bufs[i] : nullptr;
}

void STDMETHODCALLTYPE site_VSSetShader(ID3D11DeviceContext* c, ID3D11DeviceChild* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (OnRenderThread()) g_curVs = s;
    Cur<PFN_SetShader>(c, 11)(c, s, ci, n);
}

void STDMETHODCALLTYPE site_PSSetShader(ID3D11DeviceContext* c, ID3D11DeviceChild* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (OnRenderThread()) g_curPs = s;
    Cur<PFN_SetShader>(c, 9)(c, s, ci, n);
}

void RecordRTs(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (n > 8) n = 8;
    for (UINT i = 0; i < 8; ++i) g_gs.rtv[i] = i < n && rtvs ? rtvs[i] : nullptr;  // raw: the game keeps them alive
    g_gs.nRtv = n;
    g_gs.dsv = dsv;
}

void STDMETHODCALLTYPE site_OMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                    ID3D11DepthStencilView* dsv) {
    if (!OnRenderThread()) { Cur<PFN_OMSetRT>(ctx, 33)(ctx, n, rtvs, dsv); return; }
    RecordRTs(n, rtvs, dsv);
    Cur<PFN_OMSetRT>(ctx, 33)(ctx, n, rtvs, dsv);
    if (g_subst) {  // the game's targets are bound now; its blend state too
        Cur<PFN_OMSetBlend>(ctx, 35)(ctx, g_gs.blend, g_gs.factor, g_gs.mask);
        g_subst = false;
    }
}

void STDMETHODCALLTYPE site_OMSetRTUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                       ID3D11DepthStencilView* dsv, UINT us, UINT nu,
                                       ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (!OnRenderThread()) { Cur<PFN_OMSetRTUAV>(ctx, 34)(ctx, n, rtvs, dsv, us, nu, uavs, counts); return; }
    if (g_subst) {  // the game's targets back first, then let the call change (or keep) them
        RestoreGame(ctx);
        g_subst = false;
    }
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) RecordRTs(n, rtvs, dsv);
    Cur<PFN_OMSetRTUAV>(ctx, 34)(ctx, n, rtvs, dsv, us, nu, uavs, counts);
}

void STDMETHODCALLTYPE site_OMSetBlend(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, const FLOAT factor[4], UINT mask) {
    if (!OnRenderThread()) { Cur<PFN_OMSetBlend>(ctx, 35)(ctx, bs, factor, mask); return; }
    g_gs.blend = bs;
    if (factor) memcpy(g_gs.factor, factor, sizeof(g_gs.factor));
    else { g_gs.factor[0] = g_gs.factor[1] = g_gs.factor[2] = g_gs.factor[3] = 1.f; }
    g_gs.mask = mask;
    if (g_subst) ApplyConvertedBlend(ctx);
    else Cur<PFN_OMSetBlend>(ctx, 35)(ctx, bs, factor, mask);
}

void STDMETHODCALLTYPE site_ClearState(ID3D11DeviceContext* ctx) {
    Cur<PFN_ClearState>(ctx, 110)(ctx);
    if (!OnRenderThread()) return;
    g_gs = GameState{};
    for (ID3D11Buffer*& b : g_pscb) b = nullptr;
    g_curVs = g_curPs = nullptr;
    g_subst = false;
}

void STDMETHODCALLTYPE site_DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("DrawIndexed", a, 0, b, d);
    Cur<PFN_DrawIndexed>(c, 12)(c, a, b, d);
}
void STDMETHODCALLTYPE site_Draw(ID3D11DeviceContext* c, UINT a, UINT b) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("Draw", a, 0, b, 0);
    Cur<PFN_Draw>(c, 13)(c, a, b);
}
void STDMETHODCALLTYPE site_DrawIndexedInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("DrawIndexedInstanced", a, b, d, e);
    Cur<PFN_DrawIndexedInst>(c, 20)(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE site_DrawInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("DrawInstanced", a, b, d, (INT)e);
    Cur<PFN_DrawInst>(c, 21)(c, a, b, d, e);
}
void STDMETHODCALLTYPE site_DrawIndexedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("DrawIndexedInstancedIndirect", 0, 0, o, 0);
    Cur<PFN_DrawIndirect>(c, 39)(c, b, o);
}
void STDMETHODCALLTYPE site_DrawInstIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    Reconcile(c);
    if (g_census && OnRenderThread()) CensusDraw("DrawInstancedIndirect", 0, 0, o, 0);
    Cur<PFN_DrawIndirect>(c, 40)(c, b, o);
}

void STDMETHODCALLTYPE site_CopyRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
                                       ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) {
    Cur<PFN_CopyRegion>(c, 46)(c, dst, dstSub, x, y, z, src, srcSub, box);
    if (Probing() && dst && src && OnRenderThread()) ProbeCopy((uintptr_t)_ReturnAddress(), dst, x, src, box);
}
void STDMETHODCALLTYPE site_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    Cur<PFN_CopyResource>(c, 47)(c, dst, src);
    if (Probing() && dst && src && OnRenderThread()) ProbeCopy((uintptr_t)_ReturnAddress(), dst, 0, src, nullptr);
}

// The call sites (GoW.exe a3b83b87; scratch execcalls / sitecheck listings 2026-10-04: no branch lands inside a patch):
// 6- / 7-byte `call [reg+slot*8]` -> DetourCallSite; 3-byte ones -> DetourCaseCall on their switch case (setup ... call
// ... jmp 0x1409ac303), whose first whole instructions (`stolen`) become the jmp to the stub.
const HookSite kUpdSiteA = {"sharpui_Update_a7582", 0x1409a7582, 7,
                            {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xff, 0xc3, 0x41, 0x3b, 0x5e, 0x20, 0x72, 0xaf, 0x48}};
const HookSite kUpdSiteB = {"sharpui_Update_ab707", 0x1409ab707, 7,
                            {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xe9, 0xf0, 0x0b, 0x00, 0x00, 0x4d, 0x8b, 0x47, 0x10}};
const HookSite kUpdSiteC = {"sharpui_Update_abf76", 0x1409abf76, 7,
                            {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xe9, 0x81, 0x03, 0x00, 0x00, 0x41, 0x8b, 0x47, 0x30}};
const HookSite kDrawIdxInstSite = {"sharpui_DrawIndexedInstanced", 0x1409abe05, 7,
                                   {0x41, 0xff, 0x92, 0xa0, 0x00, 0x00, 0x00, 0xe9, 0xf2, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24}};
const HookSite kDrawInstSite = {"sharpui_DrawInstanced", 0x1409abe77, 7,
                                {0x41, 0xff, 0x92, 0xa8, 0x00, 0x00, 0x00, 0xe9, 0x80, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24}};
const HookSite kDrawIdxIndSite = {"sharpui_DrawIndexedInstancedIndirect", 0x1409abe20, 6,
                                  {0xff, 0x90, 0x38, 0x01, 0x00, 0x00, 0xe9, 0xd8, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
const HookSite kDrawInstIndSite = {"sharpui_DrawInstancedIndirect", 0x1409abe3a, 6,
                                   {0xff, 0x90, 0x40, 0x01, 0x00, 0x00, 0xe9, 0xbe, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
const HookSite kPSSetCBSite = {"sharpui_PSSetConstantBuffers", 0x1409ab77b, 6,
                               {0xff, 0x90, 0x80, 0x00, 0x00, 0x00, 0xe9, 0x7d, 0x0b, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x4c}};
const HookSite kOMSetRTUAVSite = {"sharpui_OMSetRenderTargetsAndUAVs", 0x1409aba55, 6,
                                  {0xff, 0x90, 0x10, 0x01, 0x00, 0x00, 0xe9, 0xa3, 0x08, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x45}};
const HookSite kOMSetBlendSite = {"sharpui_OMSetBlendState", 0x1409abc7f, 6,
                                  {0xff, 0x90, 0x18, 0x01, 0x00, 0x00, 0xe9, 0x79, 0x06, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
const HookSite kOMSetRTSiteA = {"sharpui_OMSetRenderTargets_abd72", 0x1409abd72, 6,
                                {0xff, 0x90, 0x08, 0x01, 0x00, 0x00, 0xe9, 0x86, 0x05, 0x00, 0x00, 0x80, 0x3d, 0xb4, 0x91, 0x9d}};
const HookSite kOMSetRTSiteB = {"sharpui_OMSetRenderTargets_abda6", 0x1409abda6, 6,
                                {0xff, 0x90, 0x08, 0x01, 0x00, 0x00, 0xe9, 0x52, 0x05, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
const HookSite kClearStateSite = {"sharpui_ClearState", 0x1409ac295, 6,
                                  {0xff, 0x90, 0x70, 0x03, 0x00, 0x00, 0xeb, 0x66, 0x49, 0x8b, 0x04, 0x24, 0x49, 0x8b, 0xcc, 0xff}};
const HookSite kCopyRegionSite = {"sharpui_CopySubresourceRegion", 0x1409abf39, 7,
                                  {0x41, 0xff, 0x92, 0x70, 0x01, 0x00, 0x00, 0xe9, 0xbe, 0x03, 0x00, 0x00, 0x41, 0x8b, 0x47, 0x44}};
const HookSite kCopyResourceSite = {"sharpui_CopyResource", 0x1409abeca, 6,
                                    {0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0xe9, 0x2e, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
// cases: mov rax,[r12] (4) + mov rcx,r12 (3) = 7 bytes replaced
const HookSite kDrawIdxCase = {"sharpui_DrawIndexed", 0x1409abdc7, 7,
                               {0x49, 0x8b, 0x04, 0x24, 0x49, 0x8b, 0xcc, 0x45, 0x8b, 0x4f, 0x18, 0x45, 0x8b, 0x47, 0x14, 0x41}};
const uint8_t kDrawIdxCall[3] = {0xff, 0x50, 0x60};  // call [rax+0x60] at +0x13
const HookSite kDrawCase = {"sharpui_Draw", 0x1409abe45, 7,
                            {0x49, 0x8b, 0x04, 0x24, 0x49, 0x8b, 0xcc, 0x45, 0x8b, 0x47, 0x14, 0x41, 0x8b, 0x57, 0x10, 0xff}};
const uint8_t kDrawCall[3] = {0xff, 0x50, 0x68};  // call [rax+0x68] at +0xf
// mov rax,[r12] (4) + lea r9,[rsp+0x58] (5) = 9 bytes replaced (the rest of the setup: mov r8d,ecx; mov rcx,r12)
const HookSite kVSSetCBCase = {"sharpui_VSSetConstantBuffers", 0x1409ab752, 9,
                               {0x49, 0x8b, 0x04, 0x24, 0x4c, 0x8d, 0x4c, 0x24, 0x58, 0x44, 0x8b, 0xc1, 0x49, 0x8b, 0xcc, 0xff}};
const uint8_t kVSSetCBCall[3] = {0xff, 0x50, 0x38};  // call [rax+0x38] at +0xf
// mov rax,[r12] (4) + xor r9d,r9d (3) = 7 bytes replaced
const HookSite kVSSetShaderCase = {"sharpui_VSSetShader", 0x1409abd0a, 7,
                                   {0x49, 0x8b, 0x04, 0x24, 0x45, 0x33, 0xc9, 0x49, 0x8b, 0x57, 0x10, 0x45, 0x33, 0xc0, 0x49, 0x8b}};
const uint8_t kVSSetShaderCall[3] = {0xff, 0x50, 0x58};  // call [rax+0x58] at +0x11
const HookSite kPSSetShaderCase = {"sharpui_PSSetShader", 0x1409abd23, 7,
                                   {0x49, 0x8b, 0x04, 0x24, 0x45, 0x33, 0xc9, 0x49, 0x8b, 0x57, 0x10, 0x45, 0x33, 0xc0, 0x49, 0x8b}};
const uint8_t kPSSetShaderCall[3] = {0xff, 0x50, 0x48};  // call [rax+0x48] at +0x11

// Installed at device creation: the render thread executes no command stream yet (no thread inside a patched case).
// Returns false when one the feature needs is missing (the rest - probe only - may fail alone).
bool InstallSites() {
    int need = 0, got = 0;
    auto req = [&](bool ok) { ++need; if (ok) ++got; };
    req(DetourCallSite(kUpdSiteB, (void*)&site_Update));  // the viewData uploads (LIVE: the UI's at ab707)
    DetourCallSite(kUpdSiteA, (void*)&site_Update);
    DetourCallSite(kUpdSiteC, (void*)&site_Update);
    req(DetourCallSite(kDrawIdxInstSite, (void*)&site_DrawIndexedInst));
    req(DetourCallSite(kDrawInstSite, (void*)&site_DrawInst));
    req(DetourCallSite(kDrawIdxIndSite, (void*)&site_DrawIndexedIndirect));
    req(DetourCallSite(kDrawInstIndSite, (void*)&site_DrawInstIndirect));
    req(DetourCaseCall(kDrawIdxCase, 0x13, kDrawIdxCall, 3, (void*)&site_DrawIndexed));
    req(DetourCaseCall(kDrawCase, 0xf, kDrawCall, 3, (void*)&site_Draw));
    req(DetourCaseCall(kVSSetCBCase, 0xf, kVSSetCBCall, 3, (void*)&site_VSSetCB));
    req(DetourCallSite(kOMSetRTSiteA, (void*)&site_OMSetRT));
    req(DetourCallSite(kOMSetRTSiteB, (void*)&site_OMSetRT));
    req(DetourCallSite(kOMSetRTUAVSite, (void*)&site_OMSetRTUAV));
    req(DetourCallSite(kOMSetBlendSite, (void*)&site_OMSetBlend));
    req(DetourCallSite(kClearStateSite, (void*)&site_ClearState));
    DetourCallSite(kPSSetCBSite, (void*)&site_PSSetCB);
    DetourCaseCall(kVSSetShaderCase, 0x11, kVSSetShaderCall, 3, (void*)&site_VSSetShader);
    DetourCaseCall(kPSSetShaderCase, 0x11, kPSSetShaderCall, 3, (void*)&site_PSSetShader);
    DetourCallSite(kCopyRegionSite, (void*)&site_CopyRegion);
    DetourCallSite(kCopyResourceSite, (void*)&site_CopyResource);
    if (got != need) Log("sharpui: %d of %d required executor sites hooked", got, need);
    return got == need;
}

// ---- composite -------------------------------------------------------------------------------------------------------
const char kShader[] = R"(
Texture2D ui : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) {
    float s;       // the whole UI's scale about the screen centre
    float sRet;    // the reticle region's scale
    float retOn;   // 1: reticle region active (aiming)
    float rSrc;    // reticle region radius (UI height units)
    float2 retS;   // the game's reticle centre (UI uv)
    float2 retD;   // where it is drawn (eye image uv): the shot's direction
    float aspect;  // width / height
    float3 pad;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target {
    float2 a = 0.5 + (i.uv - 0.5) / s;
    float4 m = ui.Sample(smp, a);
    if (any(a < 0) || any(a > 1)) m = 0;
    float2 b = retS + (i.uv - retD) / sRet;
    float4 r = ui.Sample(smp, b);
    if (retOn > 0.5) {
        float2 da = (a - retS) * float2(aspect, 1);
        if (dot(da, da) < rSrc * rSrc) m = 0;
        float2 db = (b - retS) * float2(aspect, 1);
        if (dot(db, db) >= rSrc * rSrc || any(b < 0) || any(b > 1)) r = 0;
        m = r + m * (1 - saturate(r.a));
    }
    return float4(max(m.rgb, 0), saturate(m.a));
}
)";

bool CreateComposite() {
    if (g_compositeReady || g_compositeFailed) return g_compositeReady;
    g_compositeFailed = true;
    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
    using PFN_Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob**, ID3DBlob**);
    auto compile = c ? (PFN_Compile)GetProcAddress(c, "D3DCompile") : nullptr;
    if (!compile) { Log("sharpui: d3dcompiler_47 unavailable"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "sharpui", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, sizeof(kShader) - 1, "sharpui", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("sharpui: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    bool ok = SUCCEEDED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    vsb->Release();
    psb->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.MipLODBias = config::GetFloat("ui_mip_bias", 0.f);  // < 0 crisper (some shimmer back), > 0 softer
    D3D11_BLEND_DESC bd{};
    auto& t = bd.RenderTarget[0];
    t.BlendEnable = TRUE;
    t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D11_BLEND_OP_ADD;
    t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 48;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_samp)) && SUCCEEDED(g_dev->CreateBlendState(&bd, &g_overBlend)) &&
         SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_rs)) && SUCCEEDED(g_dev->CreateBuffer(&cd, nullptr, &g_cb));
    ID3D11Device1* dev1 = nullptr;
    D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel(), chosen;
    ok = ok && SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1)) &&
         SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen,
                                                  &g_ownState));
    if (dev1) dev1->Release();
    if (!ok) { Log("sharpui: composite resources failed"); return false; }
    g_compositeFailed = false;
    g_compositeReady = true;
    return true;
}

// The HUD pasted onto `target` (full resolution, same size as the HUD target)
void Composite(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target) {
    if (!CreateComposite() || !g_uiSrv || !target) return;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) return;
    // constants
    const float uiScale = UiScaleCurrent();
    // D-154 (owner: "make the aiming icon small again, don't make it go bigger while holding L2"): by default the reticle
    // region is pasted at the HUD's own scale (it still moves to the aim point, D-107); ui_reticle_scale > 0 = that size
    if (g_retScale.load() < -1.5f) g_retScale = config::GetFloat("ui_reticle_scale", -1.f);
    const float retScale = g_retScale.load() > 0.f ? g_retScale.load() : uiScale;
    // D-107: while head-aiming the game's reticle (its UI spot) is drawn where the shot goes - the head's forward
    // (mutate.cpp AlignReticleToHead turns the game's aim view to match); otherwise it scales with the rest of the UI
    float su = 0.5f, sv = 0.5f, sr = 0.f, du = 0.5f, dv = 0.5f;
    const bool aiming = AimReticleDisplay(&su, &sv, &sr, &du, &dv);
    // D-149: the aiming icon's own objects are placed (fp body.cpp ReticlePlace) - no region paste, which took everything
    // inside its circle along (enemy bars); the region paste remains the fallback when they were not seen
    const bool objRet = GetTickCount64() - g_retObjAt.load() < 300;
    struct { float s, sRet, retOn, rSrc, retSx, retSy, retDx, retDy, aspect, pad0, pad1, pad2; } k{
        fmaxf(uiScale, 0.05f), fmaxf(retScale, 0.05f), aiming && !objRet ? 1.f : 0.f, sr, su, sv, du, dv,
        (float)g_uiW / (float)(g_uiH ? g_uiH : 1), 0.f, 0.f, 0.f};
    // (D-118 / D-122 / D-123 changed this function after crashes here - reverted in D-124: the cause was g_rtCache
    // writing over g_cb / g_rs, not the context-state swap or the Map)
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_ownState, &prev);  // the game's whole pipeline state parked in `prev`
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c1->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &k, sizeof(k));
        c1->Unmap(g_cb, 0);
    }
    c1->GenerateMips(g_uiSrv);
    D3D11_VIEWPORT vp{0, 0, (float)g_uiW, (float)g_uiH, 0, 1};
    static const FLOAT one[4] = {1, 1, 1, 1};
    c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c1->IASetInputLayout(nullptr);
    c1->VSSetShader(g_vs, nullptr, 0);
    c1->PSSetShader(g_ps, nullptr, 0);
    c1->PSSetShaderResources(0, 1, &g_uiSrv);
    c1->PSSetSamplers(0, 1, &g_samp);
    c1->PSSetConstantBuffers(0, 1, &g_cb);
    c1->RSSetState(g_rs);
    c1->RSSetViewports(1, &vp);
    c1->OMSetBlendState(g_overBlend, one, 0xffffffff);
    c1->OMSetRenderTargets(1, &target, nullptr);
    c1->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = nullptr;
    c1->PSSetShaderResources(0, 1, &noSrv);
    ID3D11RenderTargetView* noRtv = nullptr;
    c1->OMSetRenderTargets(1, &noRtv, nullptr);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
}

// "sharpui hudshot": the HUD target (before the paste) read back - coverage logged, written as shots/hud_target.png
std::atomic<bool> g_hudShotReq{false};
float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.f : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
void HudShot(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target) {
    D3D11_TEXTURE2D_DESC d;
    g_uiTex->GetDesc(&d);
    d.MipLevels = 1;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, g_uiTex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::vector<uint8_t> rgba((size_t)d.Width * d.Height * 4);
        uint64_t covered = 0, colored = 0;
        float maxA = 0.f, maxC = 0.f;
        for (UINT y = 0; y < d.Height; ++y) {
            const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < d.Width; ++x) {
                const float r = HalfToFloat(row[x * 4]), g = HalfToFloat(row[x * 4 + 1]), b = HalfToFloat(row[x * 4 + 2]),
                            a = HalfToFloat(row[x * 4 + 3]);
                const float c = fmaxf(r, fmaxf(g, b));
                if (a > 0.004f) ++covered;
                if (c > 0.004f) ++colored;
                maxA = fmaxf(maxA, a);
                maxC = fmaxf(maxC, c);
                uint8_t* o = &rgba[((size_t)y * d.Width + x) * 4];
                // premultiplied colour over a dark grey (uncovered = grey): shows colour and coverage
                const float bgk = 0.18f * (1.f - fminf(fmaxf(a, 0.f), 1.f));
                o[0] = (uint8_t)(fminf(fmaxf(r + bgk, 0.f), 1.f) * 255.f);
                o[1] = (uint8_t)(fminf(fmaxf(g + bgk, 0.f), 1.f) * 255.f);
                o[2] = (uint8_t)(fminf(fmaxf(b + bgk, 0.f), 1.f) * 255.f);
                o[3] = 255;
            }
        }
        ctx->Unmap(st, 0);
        ID3D11Resource* tr = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        if (target) target->GetResource(&tr);
        if (tr) { ((ID3D11Texture2D*)tr)->GetDesc(&td); tr->Release(); }
        Log("sharpui hudshot: HUD target %ux%u - covered px %llu, coloured px %llu, max alpha %.3f, max colour %.3f; paste "
            "target %ux%u fmt %d; UI dsv %p",
            d.Width, d.Height, (unsigned long long)covered, (unsigned long long)colored, maxA, maxC, td.Width, td.Height,
            (int)td.Format, (void*)g_gs.dsv);
        diag::WritePng("hud_target", rgba.data(), d.Width, d.Height, d.Width * 4, (int)DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    }
    st->Release();
}

void PasteHud(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const char* where) {
    if (g_hudShotReq.exchange(false)) HudShot(ctx, target);
    Composite(ctx, target);
    g_drew = false;
    g_needClear = true;  // the next UI draw (the other eye's) starts on a clear HUD target
    const uint64_t n = ++g_substFrames;
    if (n <= 3 || n % 3000 == 0)
        Log("sharpui: HUD composited %s #%llu (scale %.2f, reticle %.2f, %llu UI redirects so far)", where,
            (unsigned long long)n, UiScaleCurrent(), g_retScale.load(), (unsigned long long)g_substitutions.load());
}

}  // namespace

void SharpUiInstall(ID3D11DeviceContext* ctx) {
    if (g_installed.exchange(true) || !ctx) return;
    g_enabled = config::GetFloat("sharp_ui", 1.f) > 0.5f;
    ctx->GetDevice(&g_dev);  // (kept for the process lifetime)
    g_sitesOk = InstallSites();
    if (!g_sitesOk) g_enabled = false;
    Log("sharpui: executor hooks %s, sharp HUD %s", g_sitesOk ? "installed" : "INCOMPLETE",
        g_enabled.load() ? "on" : "off");
}

bool SharpUiMarkView(uint8_t* view) {
    if (!g_enabled.load() || g_broken.load() || !g_installed.load()) return false;
    // the UI camera: camera->world translation (view +0x180) on the +z axis - LIVE 2026-10-04 (0, 0, 30); CAMERA_FINDINGS
    // saw (0, 0, 10) - its x (0) carries the marker
    float* pos = (float*)(view + 0x180);
    if (fabsf(pos[0]) > 0.01f || fabsf(pos[1]) > 0.01f || pos[2] < 1.f) return false;
    pos[0] = kMarker;
    if (g_probeArm.load() || Probing())
        for (int k = 0; k < kTinyFields; ++k) {  // probe markers (w column, 0 in these views)
            float* w = (float*)(view + kTinyOff[k]) + 3;
            if (*w == 0.f) *w = TinyMarker(k);
        }
    // safety (600 view updates = ~300 frames): the marked viewData never uploaded through the hooked sites -> the scaled
    // UI as before. ("Nothing redirected" is no failure: the game hides its HUD while idle - LIVE 1 UI draw per frame.)
    const uint64_t marked = ++g_markedFrames;
    if (marked > 600 && g_probeArm.load() == 0 && !Probing() && g_markSeen.load() == 0) {
        g_broken = true;
        Log("sharpui: sharp HUD not working (marked uploads %llu, backbuffer draws %llu, redirected %llu) - scaled UI",
            (unsigned long long)g_markSeen.load(), (unsigned long long)g_bbDrawsMarked.load(),
            (unsigned long long)g_substitutions.load());
        pos[0] = 0.f;
        for (int k = 0; k < kTinyFields; ++k) {
            float* w = (float*)(view + kTinyOff[k]) + 3;
            if (*w == TinyMarker(k)) *w = 0.f;
        }
        return false;
    }
    return true;
}

void SharpUiFinish(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb) {
    if (!g_installed.load() || !ctx) return;
    if (bb && bb != g_bbTex) {
        g_bbTex = bb;
        g_bbCheckRtv = nullptr;
    }
    if (g_subst) {
        RestoreGame(ctx);
        g_subst = false;
    }
    if (g_drew && g_bbTex) {  // no copy to the backbuffer after the UI (UI drawn into the backbuffer itself): paste there
        if (g_bbRtvFor != g_bbTex) {
            if (g_bbRtv) { g_bbRtv->Release(); g_bbRtv = nullptr; }
            if (FAILED(g_dev->CreateRenderTargetView(g_bbTex, nullptr, &g_bbRtv))) g_bbRtv = nullptr;
            g_bbRtvFor = g_bbTex;
        }
        PasteHud(ctx, g_bbRtv, "onto the backbuffer");
    }
    g_drew = false;
    g_needClear = true;
}

void SharpUiOnResize() {
    if (g_bbRtv) { g_bbRtv->Release(); g_bbRtv = nullptr; }
    g_bbRtvFor = nullptr;
    g_bbTex = nullptr;
    g_bbCheckRtv = nullptr;
    g_rtCacheN = 0;
    g_uiOrigRtv = nullptr;
    g_subst = false;
    g_drew = false;
    g_needClear = true;
}

bool SharpUiActive() { return g_installed.load() && g_sitesOk && g_enabled.load() && !g_broken.load(); }

void SharpUiEnable(bool on) {
    if (on && !g_sitesOk) { Log("sharpui: executor hooks incomplete - sharp HUD stays off"); return; }
    g_enabled = on;
    g_broken = false;
    g_markedFrames = 0;
    config::SetFloat("sharp_ui", on ? 1.f : 0.f);
    Log("sharpui: sharp HUD %s", on ? "on" : "off");
}

void SharpUiProbe(int frames) {
    g_broken = false;
    g_markedFrames = 0;
    g_probeArm = frames > 0 ? frames : 2;
    Log("sharpui probe: armed for %d frames (sharp HUD %s)", g_probeArm.load(), g_enabled.load() ? "on" : "off");
}

void SharpUiPresentTick() {
    if (!g_installed.load()) return;
    if (g_renderTid.load() != GetCurrentThreadId()) {  // D-108: the render thread (Present = the executor's thread)
        Log("sharpui: render thread %lu", GetCurrentThreadId());
        g_renderTid = GetCurrentThreadId();
    }
    if (const int f = g_probeArm.exchange(0)) {  // render thread: reset, then count
        g_probeMarked.clear();
        g_probeFinds = g_probeTiny = 0;
        g_nMarkedRts = 0;
        g_probeDraws = g_probeBbDraws = g_probeMarkedDraws = g_probeUpd = g_probeSubDraws = 0;
        g_nMarkerAt = 0;
        g_runN = 0;
        g_probeFrames = f;
        return;
    }
    if (g_sitesOk && g_enabled.load() && !g_broken.load()) CensusPresent();  // D-145
    if (!Probing()) return;
    if (--g_probeFrames == 0) ProbeDump();
    else g_runN = 0;
}

void SharpUiReticleObjectsMoved() { g_retObjAt = GetTickCount64(); }
float SharpUiReticleScaleNow() {
    if (g_retScale.load() < -1.5f) g_retScale = config::GetFloat("ui_reticle_scale", -1.f);
    return g_retScale.load() > 0.f ? g_retScale.load() : UiScaleCurrent();
}

void SharpUiCensus() {
    g_censusArm = 1;
    Log("sharpui census: armed (next frame's UI draws)");
}

void SharpUiHudShot() {
    g_hudShotReq = true;
    Log("sharpui hudshot: requested (next HUD paste)");
}

void SharpUiReticleScale(float s) {
    g_retScale = s;
    config::SetFloat("ui_reticle_scale", s);
    Log("sharpui: reticle scale %.2f", s);
}

}  // namespace gowvr::stereo
