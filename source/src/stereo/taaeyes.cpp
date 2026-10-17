// D-182 (owner: "some objects and characters have offsets in stereo: the left eye shows a shadow copy of them on their
// left, the right eye on their right; and they shimmer a bit, left eye to the left, right eye to the right").
// settings.ini: DLSSQuality=Off, FSRQuality=Off - the game's own TAA runs (D-108's per-eye DLSS does not apply). TAA
// blends this frame with its HISTORY - last frame's TAA output. The R1 double kick renders eye 0 then eye 1 every frame
// through the same pass, so each eye's history was the OTHER eye's image (half an IPD away): a faint copy of the scene
// shifted outward, visible on characters / objects against a contrasting background (the environment's similar colours
// hide it in TAA's neighbourhood clamp), plus shimmer. (vrframework guides 01 §6 / 08 §6.2: "double-buffer the temporal
// history per eye".)
// STATIC (decompile + disassembly):
//  - PassCombinedPostEffects (vftable 0x140def118) loads 12 compute variants of posteffectscombined_temporal.hlsl into
//    pass +0x110 .. +0x168 (vfunc_7); sub_140418780 dispatches one (output render target 0x54, inputs through the
//    shader's resource table);
//  - a shader handle h: the compute-state flush sub_14099fc90 emits record 0x10 with *( *(h + 8) + 0xb8 ) = the
//    ID3D11ComputeShader; the executor sub_1409ab650 sets it (CSSetShader, record +0x10) and dispatches at 0x1409abe96
//    (`call [rax+0x148]` = Dispatch, 6 bytes, then `jmp 0x1409ac303`);
//  - the pass objects: the pass records (count 0x142d24d04, 0x88 B each at 0x142590b00, +0x7c table index) -> table
//    0x142d4e298 + index * 0xb0 + 0xa8 (r1.cpp PassLoop).
// Now the Dispatch call site is detoured. On the render thread, while the eyes render (EyesRenderEye 0 / 1), a dispatch
// with one of the 12 TAA shaders bound:
//  - before it: every bound compute input that is a texture some TAA dispatch WROTE (its output; ping-pong or not) gets
//    this eye's own last TAA output copied in (inputs unbound around the copy);
//  - after it: its output is copied to this eye's private image.
// So each eye's TAA only ever sees its own past. Eye 0's frame is fully executed before eye 1's streams, so replacing
// the shared history image in between loses nothing. gowvr.ini taa_per_eye 0 = off. Log: the shaders found, then every
// 1800 TAA dispatches the inputs restored / outputs saved (and when no input matched a TAA output - then the history is
// not one of them and nothing is changed).
#include "taaeyes.h"
#include "../core/addr.h"
#include "eyes.h"
#include "../core/config.h"
#include "../core/detour.h"
#include "../core/log.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstring>

namespace gowvr::stereo {
namespace {

const HookSite kDispatchSite = {"taaeyes_Dispatch", 0x1409abe96, 6,
                                {0xff, 0x90, 0x48, 0x01, 0x00, 0x00, 0xe9, 0x62, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x04, 0x24, 0x49}};
constexpr uint64_t kPassVtbl = 0x140def118;   // PassCombinedPostEffects::vftable
constexpr uint64_t kPassCount = 0x142d24d04, kPassRecs = 0x142590b00, kPassTable = 0x142d4e298;

using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);

std::atomic<bool> g_on{true};
uint8_t* g_base = nullptr;
void* g_taaCs[12] = {};
int g_taaN = 0;
ULONGLONG g_nextScan = 0;

constexpr int kSlots = 16;  // TAA output (UAV) slots followed - the executor binds compute outputs from slot 8 (CSSetUnorderedAccessViews(8, ..) at 0x1409aba79 / 0x1409abb6b)
struct Out { ID3D11Resource* res; int slot; };
Out g_outs[8] = {};  // textures TAA dispatches wrote and from which slot (identity only, no reference held)
int g_nOuts = 0;
ID3D11Texture2D* g_priv[2][kSlots] = {};  // each eye's last TAA output per slot
D3D11_TEXTURE2D_DESC g_privDesc[2][kSlots] = {};
uint64_t g_dispatches = 0, g_restored = 0, g_saved = 0, g_noMatch = 0;
// Owner: "a black frame of the intro cutscene in 3D space when I get into 3D after the startup intros": while the flat
// 2D screen shows only eye 0 renders, so eye 1's history stayed the dark intro from VR's start (2304 x 2304) - and
// back in 3D at that size it was restored into eye 1's TAA. A history is only used when it is the last frame's (saved
// within 200 ms), and the switch from the flat screen to stereo drops them all (TaaEyesReset).
ULONGLONG g_privAt[2][kSlots] = {};
std::atomic<bool> g_resetReq{false};
uint64_t g_stale = 0;

template <class T> T Rd(uint64_t va) { return *(T*)((uint8_t*)GameVa(va)); }

// the 12 TAA compute shaders out of the PassCombinedPostEffects object (game memory, guarded)
int ScanShaders(void** out) {
    int n = 0;
    __try {
        const int count = Rd<int>(kPassCount);
        uint8_t* table = Rd<uint8_t*>(kPassTable);
        if (!table || count <= 0 || count > 4096) return 0;
        const uint64_t vt = (uint64_t)((uint8_t*)GameVa(kPassVtbl));
        for (int i = 0; i < count && !n; ++i) {
            uint8_t* rec = (uint8_t*)GameVa(kPassRecs) + (size_t)i * 0x88;
            const int idx = *(int*)(rec + 0x7c);
            if (idx < 0 || idx > 4096) continue;
            uint8_t* pass = *(uint8_t**)(table + (int64_t)idx * 0xb0 + 0xa8);
            if (!pass || *(uint64_t*)pass != vt) continue;
            for (int k = 0; k < 12; ++k) {
                uint8_t* h = *(uint8_t**)(pass + 0x110 + k * 8);
                if (!h) continue;
                uint8_t* p = *(uint8_t**)(h + 8);
                if (!p) continue;
                void* cs = *(void**)(p + 0xb8);
                if (cs) out[n++] = cs;
            }
            if (!n) return -1;  // the pass, shaders not loaded yet
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

bool IsTaa(ID3D11DeviceContext* c) {
    ID3D11ComputeShader* cs = nullptr;
    c->CSGetShader(&cs, nullptr, nullptr);
    if (!cs) return false;
    cs->Release();  // (identity only)
    for (int i = 0; i < g_taaN; ++i)
        if (g_taaCs[i] == cs) return true;
    return false;
}

int OutSlot(ID3D11Resource* r) {  // the slot a TAA dispatch wrote r from, -1 = never written by TAA
    for (int i = 0; i < g_nOuts; ++i)
        if (g_outs[i].res == r) return g_outs[i].slot;
    return -1;
}

void NoteOut(ID3D11Resource* r, int slot) {
    for (int i = 0; i < g_nOuts; ++i)
        if (g_outs[i].res == r) { g_outs[i].slot = slot; return; }
    if (g_nOuts < 8) { g_outs[g_nOuts++] = {r, slot}; return; }
    memmove(g_outs, g_outs + 1, sizeof(g_outs[0]) * 7);
    g_outs[7] = {r, slot};
}

bool SameDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize && a.SampleDesc.Count == b.SampleDesc.Count;
}

ID3D11Texture2D* AsTex(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* d) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    auto* t = (ID3D11Texture2D*)r;
    t->GetDesc(d);
    return t;
}

void STDMETHODCALLTYPE site_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    auto orig = (PFN_Dispatch)(*(void***)c)[41];
    const int eye = EyesRenderEye();
    if (!g_on.load(std::memory_order_relaxed) || (eye != 0 && eye != 1)) { orig(c, x, y, z); return; }
    if (g_taaN <= 0) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextScan) {
            g_nextScan = now + 2000;
            g_taaN = ScanShaders(g_taaCs);
            if (g_taaN > 0) Log("taaeyes: %d TAA compute shaders found - each eye keeps its own TAA history", g_taaN);
        }
        if (g_taaN <= 0) { orig(c, x, y, z); return; }
    }
    if (!IsTaa(c)) { orig(c, x, y, z); return; }
    ++g_dispatches;
    const ULONGLONG now = GetTickCount64();
    if (g_resetReq.exchange(false)) {
        memset(g_privAt, 0, sizeof(g_privAt));
        Log("taaeyes: per-eye histories dropped (the flat screen handed back to stereo)");
    }
    // before: this eye's own history into every bound input that is a TAA output
    ID3D11ShaderResourceView* srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    c->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv);
    bool matched = false, copied = false;
    for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
        if (!srv[i]) continue;
        ID3D11Resource* r = nullptr;
        srv[i]->GetResource(&r);
        const int os = r ? OutSlot(r) : -1;
        if (os >= 0) {
            matched = true;
            D3D11_TEXTURE2D_DESC d;
            const bool fresh = now - g_privAt[eye][os] < 200;
            if (!fresh && g_priv[eye][os] && ++g_stale <= 10)
                Log("taaeyes: eye %d history (slot %d) is old - not used", eye, os);
            if (fresh && g_priv[eye][os] && AsTex(r, &d) && SameDesc(d, g_privDesc[eye][os])) {
                if (!copied) {  // the inputs unbound around the copy (no read / write hazard), then bound again
                    ID3D11ShaderResourceView* none[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
                    c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, none);
                }
                c->CopyResource(r, g_priv[eye][os]);
                copied = true;
                ++g_restored;
            }
        }
        if (r) r->Release();
    }
    if (copied) c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv);
    if (!matched) ++g_noMatch;
    for (auto*& v : srv)
        if (v) { v->Release(); v = nullptr; }

    orig(c, x, y, z);

    // after: its output = this eye's history for next frame
    ID3D11UnorderedAccessView* uavs[kSlots] = {};
    c->CSGetUnorderedAccessViews(0, kSlots, uavs);
    for (int s = 0; s < kSlots; ++s) {
        ID3D11UnorderedAccessView* uav = uavs[s];
        if (!uav) continue;
        ID3D11Resource* r = nullptr;
        uav->GetResource(&r);
        D3D11_TEXTURE2D_DESC d;
        if (r && AsTex(r, &d)) {
            NoteOut(r, s);
            ID3D11Texture2D*& priv = g_priv[eye][s];
            if (!priv || !SameDesc(d, g_privDesc[eye][s])) {
                if (priv) { priv->Release(); priv = nullptr; }
                ID3D11Device* dev = nullptr;
                c->GetDevice(&dev);
                D3D11_TEXTURE2D_DESC pd = d;
                pd.Usage = D3D11_USAGE_DEFAULT;
                pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                pd.CPUAccessFlags = 0;
                pd.MiscFlags = 0;
                if (dev && SUCCEEDED(dev->CreateTexture2D(&pd, nullptr, &priv))) {
                    g_privDesc[eye][s] = d;
                    Log("taaeyes: eye %d history (output slot %d) %ux%u fmt %d", eye, s, d.Width, d.Height, (int)d.Format);
                } else {
                    priv = nullptr;
                }
                if (dev) dev->Release();
            }
            if (priv) {
                c->CopyResource(priv, r);
                g_privAt[eye][s] = now;
                ++g_saved;
            }
        }
        if (r) r->Release();
        uav->Release();
    }
    if (g_dispatches % 1800 == 0)
        Log("taaeyes: %llu TAA dispatches - history restored per eye %llu, saved %llu, no TAA-output input %llu",
            (unsigned long long)g_dispatches, (unsigned long long)g_restored, (unsigned long long)g_saved,
            (unsigned long long)g_noMatch);
}

}  // namespace

void TaaEyesInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    TaaEyesEnable(config::GetFloat("taa_per_eye", 0.f) > 0.5f);  // D-189: off by default (owner: "everything moves like shifting")
    if (!DetourCallSite(kDispatchSite, (void*)&site_Dispatch)) Log("taaeyes: Dispatch site not hooked - TAA history shared");
}

void TaaEyesReset() { g_resetReq = true; }

void TaaEyesEnable(bool on) {
    g_on = on;
    Log("taaeyes: per-eye TAA history %s", on ? "on" : "off");
}

}  // namespace gowvr::stereo
