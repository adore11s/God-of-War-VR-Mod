// M0 / RE-04: observe device creation, swap chains and Present cadence. Read-only: every hook calls the original
// with unchanged arguments and only logs.
#include "d3d_hooks.h"
#include "../core/log.h"
#include "../core/patch.h"
#include "../diag/shot.h"
#include "../diag/cbprobe.h"
#include "../diag/crashlog.h"
#include "../input/vpad.h"
#include "../stereo/eyes.h"
#include "rendersize.h"
#include "../stereo/sharpui.h"
#include "../stereo/dlsseyes.h"
#include "../stereo/taaeyes.h"
#include "../stereo/mutate.h"
#include "../vr/xr.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace gowvr::d3d {
bool g_xrMono = false;  // diagnostic: submit the backbuffer to both eyes even when an eye pair exists
namespace {

using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                               IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);

PFN_D3D11_CREATE_DEVICE o_D3D11CreateDevice = nullptr;
PFN_CreateDXGIFactory1 o_CreateDXGIFactory1 = nullptr;
PFN_CreateSwapChain o_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;
PFN_ResizeBuffers o_ResizeBuffers = nullptr;
PFN_GetProcAddress o_GetProcAddress = nullptr;

std::atomic<uint64_t> g_presents{0};
std::atomic<int> g_packetsLeft{0};
std::atomic<int> g_gpaLogged{0};
DWORD g_presentThread = 0;
double g_windowStart = 0, g_last = 0, g_min = 1e9, g_max = 0;
uint64_t g_windowFrames = 0;

void LogModulesOnce() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
    const char* interesting[] = {"d3d11", "d3d12", "dxgi", "vulkan", "opengl", "nvngx", "amd_ags", "openxr",
                                 "openvr", "renderdoc", "gameoverlay", "rtss", "reshade", "d3d9", "nvapi"};
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH)) continue;
        char lower[MAX_PATH];
        strcpy_s(lower, name);
        _strlwr_s(lower);
        for (auto key : interesting)
            if (strstr(lower, key)) { Log("module: %p %s", (void*)mods[i], name); break; }
    }
}

// RE-04 packet: what is bound on the immediate context at the moment the frame is presented.
void LogFramePacket(IDXGISwapChain* sc, const char* via, UINT sync, UINT flags) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    char rtDesc[128] = "none", dsDesc[128] = "none";
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        dev->GetImmediateContext(&ctx);
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->RSGetViewports(&nvp, &vp);
        auto describe = [](ID3D11View* v, char* out, size_t n) {
            if (!v) return;
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(&t))) {
                D3D11_TEXTURE2D_DESC td;
                t->GetDesc(&td);
                sprintf_s(out, n, "%p %ux%u fmt=%d", (void*)r, td.Width, td.Height, (int)td.Format);
                t->Release();
            }
            if (r) r->Release();
        };
        describe(rtv, rtDesc, sizeof(rtDesc));
        describe(dsv, dsDesc, sizeof(dsDesc));
    }
    Log("frameCandidate=%s swapchain=%p device=%p callIndex=%llu sync=%u flags=0x%x bb=%ux%u fmt=%d "
        "color=[%s] depth=[%s] viewport=%.0f,%.0f,%.0fx%.0f nvp=%u",
        via, (void*)sc, (void*)dev, (unsigned long long)g_presents.load(), sync, flags, d.BufferDesc.Width,
        d.BufferDesc.Height, (int)d.BufferDesc.Format, rtDesc, dsDesc, vp.TopLeftX, vp.TopLeftY, vp.Width,
        vp.Height, nvp);
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
}

void OnPresent(IDXGISwapChain* sc, const char* via, UINT sync, UINT flags) {
    uint64_t n = ++g_presents;
    double now = NowMs();
    DWORD tid = GetCurrentThreadId();
    if (n == 1) {
        LogModulesOnce();
        g_windowStart = now;
    } else {
        double dt = now - g_last;
        if (dt < g_min) g_min = dt;
        if (dt > g_max) g_max = dt;
    }
    if (g_presentThread != tid) {
        Log("present: thread changed %lu -> %lu at call %llu", g_presentThread, tid, (unsigned long long)n);
        g_presentThread = tid;
    }
    g_last = now;
    ++g_windowFrames;
    if (n <= 3 || n % 1800 == 0 || g_packetsLeft.load() > 0) {
        if (g_packetsLeft > 0) --g_packetsLeft;
        LogFramePacket(sc, via, sync, flags);
    }
    if (now - g_windowStart >= 5000.0) {
        Log("cadence: presents=%llu window=%.0fms frames=%llu avg=%.2fms min=%.2fms max=%.2fms",
            (unsigned long long)n, now - g_windowStart, (unsigned long long)g_windowFrames,
            (now - g_windowStart) / double(g_windowFrames), g_min, g_max);
        g_windowStart = now;
        g_windowFrames = 0;
        g_min = 1e9;
        g_max = 0;
    }
}

// Eye pair -> OpenXR (render thread). While the XR session runs, xrWaitFrame paces the game: desktop vsync off.
void VrOnPresent(IDXGISwapChain* sc, UINT& sync) {
    ID3D11Texture2D *e0 = nullptr, *e1 = nullptr;
    vr::EyeViews v;
    bool pair = !g_xrMono && stereo::EyesPair(&e0, &e1, &v);
    vr::XrOnPresent(sc, pair ? e0 : nullptr, pair ? e1 : nullptr, pair && v.valid ? &v : nullptr);
    if (vr::XrRunning()) sync = 0;
    frame::RenderSizeTick(sc, vr::XrRunning());  // square VR picture while VR runs
}

// D-104: the frame's last HUD (eye 1, or the only one with the eye capture off) pasted in before it is copied / shown
void SharpUiOnPresent(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    stereo::SharpUiPresentTick();
    stereo::SharpUiFinish(ctx, bb);
    ctx->Release();
    dev->Release();
    bb->Release();
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    OnPresent(sc, "IDXGISwapChain::Present", sync, flags);
    SharpUiOnPresent(sc);
    stereo::EyesOnPresent(sc);
    stereo::LoadWatchTick();
    VrOnPresent(sc, sync);
    diag::ShotOnPresent(sc);
    diag::CbProbeOnPresent();
    return o_Present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    OnPresent(sc, "IDXGISwapChain1::Present1", sync, flags);
    SharpUiOnPresent(sc);
    stereo::EyesOnPresent(sc);
    stereo::LoadWatchTick();
    VrOnPresent(sc, sync);
    diag::ShotOnPresent(sc);
    return o_Present1(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    stereo::SharpUiOnResize();  // D-104: no view of the old backbuffer may survive the resize
    HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, fl);
    Log("ResizeBuffers(sc=%p count=%u %ux%u fmt=%d flags=0x%x) -> 0x%08lx", (void*)sc, count, w, h, (int)fmt, fl, hr);
    return hr;
}

void HookSwapChain(IDXGISwapChain* sc) {
    bool a = PatchVtable(sc, 8, (void*)&hk_Present, (void**)&o_Present);
    bool b = PatchVtable(sc, 13, (void*)&hk_ResizeBuffers, (void**)&o_ResizeBuffers);
    bool c = false;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(&sc1))) {
        c = PatchVtable(sc1, 22, (void*)&hk_Present1, (void**)&o_Present1);
        sc1->Release();
    }
    Log("swapchain %p hooked: Present=%d ResizeBuffers=%d Present1=%d", (void*)sc, a, b, c);
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                             IDXGISwapChain** out) {
    HRESULT hr = o_CreateSwapChain(f, dev, d, out);
    Log("CreateSwapChain(dev=%p %ux%u fmt=%d buffers=%u effect=%d windowed=%d flags=0x%x hwnd=%p) -> 0x%08lx sc=%p",
        (void*)dev, d->BufferDesc.Width, d->BufferDesc.Height, (int)d->BufferDesc.Format, d->BufferCount,
        (int)d->SwapEffect, d->Windowed, d->Flags, (void*)d->OutputWindow, hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        HookSwapChain(*out);
        input::SetGameWindow(d->OutputWindow);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                                    IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, d, fs, o, out);
    Log("CreateSwapChainForHwnd(dev=%p %ux%u fmt=%d buffers=%u effect=%d flags=0x%x hwnd=%p) -> 0x%08lx sc=%p",
        (void*)dev, d->Width, d->Height, (int)d->Format, d->BufferCount, (int)d->SwapEffect, d->Flags,
        (void*)hwnd, hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        HookSwapChain(*out);
        input::SetGameWindow(hwnd);
    }
    return hr;
}

void HookFactory(IUnknown* unk) {
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(unk->QueryInterface(&f))) {
        PatchVtable(f, 10, (void*)&hk_CreateSwapChain, (void**)&o_CreateSwapChain);
        f->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(unk->QueryInterface(&f2))) {
        PatchVtable(f2, 15, (void*)&hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd);
        f2->Release();
    }
}

HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void** out) {
    HRESULT hr = o_CreateDXGIFactory1(riid, out);
    Log("CreateDXGIFactory1 -> 0x%08lx factory=%p", hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) HookFactory((IUnknown*)*out);
    return hr;
}

HRESULT WINAPI hk_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                    const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                    ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    HRESULT hr = o_D3D11CreateDevice(adapter, type, sw, flags, levels, nlevels, sdk, dev, got, ctx);
    char desc[128] = "default";
    if (adapter) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(adapter->GetDesc(&ad)))
            sprintf_s(desc, "%ls luid=%08lx:%08lx", ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);
    }
    Log("D3D11CreateDevice(adapter=[%s] type=%d flags=0x%x nlevels=%u) -> 0x%08lx device=%p level=0x%x ctx=%p",
        desc, (int)type, flags, nlevels, hr, dev ? (void*)*dev : nullptr, got ? (unsigned)*got : 0u,
        ctx ? (void*)*ctx : nullptr);
    if (SUCCEEDED(hr) && ctx && *ctx) {
        diag::CbProbeInstall(*ctx);
        stereo::SharpUiInstall(*ctx);  // D-104 (after cbprobe: chains its UpdateSubresource hook)
        stereo::TaaEyesInstall();      // D-182: per-eye TAA history
        if (dev && *dev) diag::CrashNoteDevice(*dev);  // D-112: GPU / driver for crash reports
    }
    return hr;
}

// Dynamic lookups bypass the IAT; route the graphics entry points through the same hooks and log the rest.
FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = o_GetProcAddress(m, name);
    if (!p || IS_INTRESOURCE(name)) return p;
    if (!strcmp(name, "D3D11CreateDevice") && !o_D3D11CreateDevice) {
        o_D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)p;
        Log("GetProcAddress(D3D11CreateDevice) redirected");
        return (FARPROC)&hk_D3D11CreateDevice;
    }
    if (!strcmp(name, "D3D11CreateDevice") && (void*)p == (void*)o_D3D11CreateDevice) return (FARPROC)&hk_D3D11CreateDevice;
    if (!strcmp(name, "CreateDXGIFactory1") && (!o_CreateDXGIFactory1 || (void*)p == (void*)o_CreateDXGIFactory1)) {
        o_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)p;
        return (FARPROC)&hk_CreateDXGIFactory1;
    }
    if (FARPROC w = stereo::DlssEyesWrap(name, p)) {  // D-108 per-eye DLSS
        Log("GetProcAddress(%s) -> per-eye DLSS wrapper", name);
        return w;
    }
    if (g_gpaLogged.fetch_add(1) < 400) {
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(m, mod, MAX_PATH);
        const char* base = strrchr(mod, '\\');
        Log("GetProcAddress(%s!%s)", base ? base + 1 : mod, name);
    }
    return p;
}

}  // namespace

void Install() {
    HMODULE exe = GetModuleHandleW(nullptr);
    bool a = PatchImport(exe, "d3d11.dll", "D3D11CreateDevice", (void*)&hk_D3D11CreateDevice, (void**)&o_D3D11CreateDevice);
    bool b = PatchImport(exe, "dxgi.dll", "CreateDXGIFactory1", (void*)&hk_CreateDXGIFactory1, (void**)&o_CreateDXGIFactory1);
    bool c = PatchImport(exe, "KERNEL32.dll", "GetProcAddress", (void*)&hk_GetProcAddress, (void**)&o_GetProcAddress);
    Log("d3d: IAT D3D11CreateDevice=%d CreateDXGIFactory1=%d GetProcAddress=%d", a, b, c);
}

uint64_t PresentCount() { return g_presents.load(); }
void LogPackets(int n) { g_packetsLeft = n; }

}  // namespace gowvr::d3d
