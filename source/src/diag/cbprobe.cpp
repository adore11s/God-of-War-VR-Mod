#include "cbprobe.h"
#include "../core/addr.h"
#include "../core/log.h"
#include "../core/patch.h"
#include "../core/detour.h"
#include <windows.h>
#include <d3d11_1.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <mutex>

#pragma intrinsic(_ReturnAddress)

namespace gowvr::diag {
namespace {

using PFN_Update = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                              const void*, UINT, UINT);
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
PFN_Update o_Update = nullptr;
PFN_Map o_Map = nullptr;
PFN_Unmap o_Unmap = nullptr;

std::atomic<int> g_framesLeft{0};
uintptr_t g_base = 0;

struct Row { uintptr_t caller; const char* api; UINT bytes; int matchFloat; uint64_t count; };
std::mutex g_mu;
Row g_rows[64];
int g_nrows = 0;

// Outstanding maps (the engine keeps several buffers mapped at once). Render thread only.
struct Pending { ID3D11Resource* res; void* ptr; UINT bytes; uintptr_t caller; };
Pending g_pending[64];
int g_npending = 0;

float g_cam[3];

bool CurrentCam() {
    auto viewA = *(uint8_t**)((uint8_t*)GameVa(0x142d4e5d8ull));
    if (!viewA) return false;
    memcpy(g_cam, viewA + 0x180, 12);
    return true;
}

int Match(const void* data, UINT bytes) {
    if (!data || bytes < 64) return -1;
    const float* f = (const float*)data;
    UINT n = bytes / 4;
    __try {
        for (UINT k = 0; k + 8 < n; ++k)
            if (fabsf(f[k] - g_cam[0]) < 0.5f && fabsf(f[k + 4] - g_cam[1]) < 0.5f && fabsf(f[k + 8] - g_cam[2]) < 0.5f)
                return (int)k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

void Record(uintptr_t caller, const char* api, UINT bytes, int k) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < g_nrows; ++i)
        if (g_rows[i].caller == caller && g_rows[i].matchFloat == k) { ++g_rows[i].count; return; }
    if (g_nrows < 64) g_rows[g_nrows++] = {caller, api, bytes, k, 1};
}

UINT BufferBytes(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC d;
    ((ID3D11Buffer*)r)->GetDesc(&d);
    return d.ByteWidth;
}

// Unconditional census of update callers (instrument check: did the hooks see the traffic at all?).
std::atomic<uint64_t> g_updCalls{0}, g_upd1Calls{0};
struct CensusRow { uintptr_t caller; UINT minBytes, maxBytes; uint64_t count; };
CensusRow g_census[32];
int g_ncensus = 0;

void Census(uintptr_t caller, UINT bytes) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < g_ncensus; ++i)
        if (g_census[i].caller == caller) {
            CensusRow& r = g_census[i];
            ++r.count;
            if (bytes < r.minBytes) r.minBytes = bytes;
            if (bytes > r.maxBytes) r.maxBytes = bytes;
            return;
        }
    if (g_ncensus < 32) g_census[g_ncensus++] = {caller, bytes, bytes, 1};
}

void STDMETHODCALLTYPE hk_Update(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                 const void* src, UINT rp, UINT dp) {
    if (g_framesLeft > 0) {
        ++g_updCalls;
        Census((uintptr_t)_ReturnAddress(), box ? box->right - box->left : 0);
    }
    if (g_framesLeft > 0 && r) {
        UINT bytes = box ? box->right - box->left : BufferBytes(r);
        int k = bytes ? Match(src, bytes) : -1;
        if (k >= 0) Record((uintptr_t)_ReturnAddress(), "UpdateSubresource", bytes, k);
    }
    o_Update(c, r, sub, box, src, rp, dp);
}

using PFN_Update1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                               const void*, UINT, UINT, UINT);
PFN_Update1 o_Update1 = nullptr;

void STDMETHODCALLTYPE hk_Update1(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                  const void* src, UINT rp, UINT dp, UINT flags) {
    if (g_framesLeft > 0) {
        ++g_upd1Calls;
        Census((uintptr_t)_ReturnAddress(), box ? box->right - box->left : 0);
    }
    if (g_framesLeft > 0 && r) {
        UINT bytes = box ? box->right - box->left : BufferBytes(r);
        int k = bytes ? Match(src, bytes) : -1;
        if (k >= 0) Record((uintptr_t)_ReturnAddress(), "UpdateSubresource1", bytes, k);
    }
    o_Update1(c, r, sub, box, src, rp, dp, flags);
}

HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT fl,
                                 D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = o_Map(c, r, sub, type, fl, m);
    if (g_framesLeft > 0 && SUCCEEDED(hr) && m && g_npending < 64 &&
        (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        UINT bytes = BufferBytes(r);
        if (bytes) g_pending[g_npending++] = {r, m->pData, bytes, (uintptr_t)_ReturnAddress()};
    }
    return hr;
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    for (int i = 0; i < g_npending; ++i) {
        if (g_pending[i].res != r) continue;
        Pending p = g_pending[i];
        g_pending[i] = g_pending[--g_npending];
        if (g_framesLeft > 0) {
            int k = Match(p.ptr, p.bytes > (4u << 20) ? (4u << 20) : p.bytes);
            if (k >= 0) Record(p.caller, "Map/Unmap", p.bytes, k);
        }
        break;
    }
    o_Unmap(c, r, sub);
}

// Engine call sites of UpdateSubresource in the render-thread command interpreter (tools/re guard bytes).
constexpr HookSite kUpdSites[3] = {
    {"UpdSite_a7582", 0x1409a7582, 7, {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xff, 0xc3, 0x41, 0x3b, 0x5e, 0x20, 0x72, 0xaf, 0x48}},
    {"UpdSite_ab707", 0x1409ab707, 7, {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xe9, 0xf0, 0x0b, 0x00, 0x00, 0x4d, 0x8b, 0x47, 0x10}},
    {"UpdSite_abf76", 0x1409abf76, 7, {0x41, 0xff, 0x92, 0x80, 0x01, 0x00, 0x00, 0xe9, 0x81, 0x03, 0x00, 0x00, 0x41, 0x8b, 0x47, 0x30}},
};
struct SiteStats { std::atomic<uint64_t> calls{0}, matches{0}; std::atomic<int> firstFloat{-1}; std::atomic<UINT> bytes{0}; };
SiteStats g_site[3];
std::atomic<uintptr_t> g_lastSrc[3];

template <int N>
void STDMETHODCALLTYPE hk_Site(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                               const void* src, UINT rp, UINT dp) {
    if (g_framesLeft > 0) {
        ++g_site[N].calls;
        UINT bytes = r ? (box ? box->right - box->left : BufferBytes(r)) : 0;
        int k = bytes ? Match(src, bytes) : -1;
        if (k >= 0) {
            ++g_site[N].matches;
            g_site[N].firstFloat = k;
            g_site[N].bytes = bytes;
            if (bytes == 688) g_lastSrc[N] = (uintptr_t)src;  // ConstBuf__viewData size
        }
    }
    auto fn = (PFN_Update)(*(void***)c)[48];  // whatever d3d11 currently has in the slot
    fn(c, r, sub, box, src, rp, dp);
}

}  // namespace

void CbProbeInstallSites() {
    bool a = DetourCallSite(kUpdSites[0], (void*)&hk_Site<0>);
    bool b = DetourCallSite(kUpdSites[1], (void*)&hk_Site<1>);
    bool c = DetourCallSite(kUpdSites[2], (void*)&hk_Site<2>);
    Log("cbprobe: call sites a7582=%d ab707=%d abf76=%d", a, b, c);
}

void CbProbeSiteReport() {
    for (int i = 0; i < 3; ++i)
        Log("cbprobe: site %s calls=%llu camMatches=%llu lastMatchFloat=%d bytes=%u lastSrc=%p", kUpdSites[i].name,
            (unsigned long long)g_site[i].calls.load(), (unsigned long long)g_site[i].matches.load(),
            g_site[i].firstFloat.load(), g_site[i].bytes.load(), (void*)g_lastSrc[i].load());
    for (auto& s : g_site) { s.calls = 0; s.matches = 0; }
}

void CbProbeInstall(ID3D11DeviceContext* ctx) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    bool a = PatchVtable(ctx, 48, (void*)&hk_Update, (void**)&o_Update);
    bool b = PatchVtable(ctx, 14, (void*)&hk_Map, (void**)&o_Map);
    bool c = PatchVtable(ctx, 15, (void*)&hk_Unmap, (void**)&o_Unmap);
    bool d = false;
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) {
        d = PatchVtable(c1, 116, (void*)&hk_Update1, (void**)&o_Update1);
        c1->Release();
    }
    Log("cbprobe: context %p vtable %p hooks Update=%d Map=%d Unmap=%d Update1=%d (hk_Update=%p hk_Map=%p)",
        (void*)ctx, *(void**)ctx, a, b, c, d, (void*)&hk_Update, (void*)&hk_Map);
}

void CbProbeArm(int frames) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_nrows = 0;
    g_framesLeft = frames;
    Log("cbprobe: armed for %d frames", frames);
}

void CbProbeOnPresent() {
    if (g_framesLeft > 0) {
        CurrentCam();
        --g_framesLeft;
    }
}

void CbProbeReport() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < g_nrows; ++i) {
        Row& r = g_rows[i];
        bool inExe = r.caller >= g_base && r.caller < g_base + 0x519f000;
        Log("cbprobe: caller=%s0x%llx api=%s bytes=%u camPosAtFloat=%d count=%llu", inExe ? "GoW+" : "",
            (unsigned long long)(inExe ? r.caller - g_base + 0x140000000ull : r.caller), r.api, r.bytes, r.matchFloat,
            (unsigned long long)r.count);
    }
    Log("cbprobe: report rows=%d updCalls=%llu upd1Calls=%llu", g_nrows, (unsigned long long)g_updCalls.load(),
        (unsigned long long)g_upd1Calls.load());
    for (int i = 0; i < g_ncensus; ++i) {
        CensusRow& r = g_census[i];
        bool inExe = r.caller >= g_base && r.caller < g_base + 0x519f000;
        Log("cbprobe: census caller=%s0x%llx count=%llu boxBytes=%u..%u", inExe ? "GoW+" : "",
            (unsigned long long)(inExe ? r.caller - g_base + 0x140000000ull : r.caller), (unsigned long long)r.count,
            r.minBytes, r.maxBytes);
    }
    g_ncensus = 0;
    g_updCalls = 0;
    g_upd1Calls = 0;
}

}  // namespace gowvr::diag
