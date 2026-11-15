// D-329: the desktop window's picture while VR runs (desktopview.h).
#include "desktopview.h"
#include "rendersize.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../stereo/sharpui.h"
#include "../stereo/eyes.h"
#include "../stereo/mutate.h"
#include "../vr/xr.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <cmath>
#include <cstdint>

namespace gowvr::frame {
namespace {

ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3DDeviceContextState* g_state = nullptr;  // our own pipeline state: the game's is parked untouched meanwhile
bool g_ready = false, g_failed = false;
ID3D11Texture2D* g_copy = nullptr;          // the backbuffer's picture, read while it is drawn back smaller
ID3D11ShaderResourceView* g_copySrv = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc{};
ID3D11Texture2D* g_rtvFor = nullptr;        // (raw pointer, compared only)
ID3D11RenderTargetView* g_rtv = nullptr;
uint64_t g_fits = 0;

const char kShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target { return src.SampleLevel(smp, i.uv, 0); }
)";

bool Create(ID3D11Device* dev) {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;
    g_dev = dev;
    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
    using PFN_Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob**, ID3DBlob**);
    auto compile = c ? (PFN_Compile)GetProcAddress(c, "D3DCompile") : nullptr;
    if (!compile) { Log("desktopview: d3dcompiler_47 unavailable - the desktop picture is stretched as before"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "desktopview", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, sizeof(kShader) - 1, "desktopview", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("desktopview: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (vsb) vsb->Release();
        if (err) err->Release();
        return false;
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    vsb->Release();
    psb->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    ID3D11Device1* dev1 = nullptr;
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen;
    ok = ok && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1)) &&
         SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &g_state));
    if (dev1) dev1->Release();
    if (!ok) { Log("desktopview: resources failed - the desktop picture is stretched as before"); return false; }
    g_failed = false;
    g_ready = true;
    return true;
}

void ReleaseCopy() {
    if (g_copySrv) { g_copySrv->Release(); g_copySrv = nullptr; }
    if (g_copy) { g_copy->Release(); g_copy = nullptr; }
    g_copyDesc = {};
}

bool EnsureCopy(const D3D11_TEXTURE2D_DESC& bd) {
    if (g_copy && g_copyDesc.Width == bd.Width && g_copyDesc.Height == bd.Height && g_copyDesc.Format == bd.Format)
        return true;
    ReleaseCopy();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = bd.Width;
    d.Height = bd.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = bd.Format;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_copy)) ||
        FAILED(g_dev->CreateShaderResourceView(g_copy, nullptr, &g_copySrv))) {
        static bool logged = false;
        if (!logged) Log("desktopview: picture copy %ux%u fmt=%d failed - stretched as before", d.Width, d.Height, (int)d.Format);
        logged = true;
        ReleaseCopy();
        return false;
    }
    g_copyDesc = d;
    return true;
}

// The backbuffer's picture redrawn so that DXGI's stretch onto a cw x ch client area gives it back its own shape:
// - crop (D-329c, owner: "16:9 with crop, no black bars"): the square 3D view - its middle band of the window's shape,
//   drawn over the whole backbuffer (a viewport larger than the target; the rest is clipped). The headset keeps the
//   whole picture; the HUD sits in the middle and stays inside the band (ui_scale 0.35 / 0.42 < 9/16).
// - otherwise (flat screens: menus, videos, loading - their edges hold text): drawn smaller, black around it
void Fit(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, int cw, int ch, bool crop) {
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || !d.Width || !d.Height || d.ArraySize != 1) return;
    const double pa = (double)d.Width / (double)d.Height, wa = (double)cw / (double)ch;
    if (fabs(pa / wa - 1.0) < 0.01) return;  // (within 1 %: the stretch cannot be seen)
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    const bool made = dev && Create(dev);
    if (dev) dev->Release();
    if (!made || !EnsureCopy(d)) return;
    if (g_rtvFor != bb) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        if (FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv))) g_rtv = nullptr;
        g_rtvFor = bb;
    }
    if (!g_rtv) return;
    float vx = 0.f, vy = 0.f, vw = (float)d.Width, vh = (float)d.Height;
    if (crop) {
        if (wa > pa) vh = floorf((float)d.Height * (float)(wa / pa) + 0.5f);  // wider window: the top and bottom cut
        else vw = floorf((float)d.Width * (float)(pa / wa) + 0.5f);           // taller: the sides cut
        vx = floorf(((float)d.Width - vw) * 0.5f);
        vy = floorf(((float)d.Height - vh) * 0.5f);
        if (vw > 32767.f || vh > 32767.f) return;  // (D3D11 viewport range; never at 16:9 below N = 18000)
    } else if (wa > pa) {  // the window is wider than the picture: bars left and right
        vw = floorf((float)d.Width * (float)(pa / wa) + 0.5f);
        vx = floorf(((float)d.Width - vw) * 0.5f);
    } else {        // taller: bars above and below
        vh = floorf((float)d.Height * (float)(wa / pa) + 0.5f);
        vy = floorf(((float)d.Height - vh) * 0.5f);
    }
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) return;
    ctx->CopyResource(g_copy, bb);
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_state, &prev);
    static const FLOAT black[4] = {0, 0, 0, 1};
    c1->ClearRenderTargetView(g_rtv, black);
    D3D11_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
    c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c1->IASetInputLayout(nullptr);
    c1->VSSetShader(g_vs, nullptr, 0);
    c1->PSSetShader(g_ps, nullptr, 0);
    c1->PSSetShaderResources(0, 1, &g_copySrv);
    c1->PSSetSamplers(0, 1, &g_samp);
    c1->RSSetState(g_rs);
    c1->RSSetViewports(1, &vp);
    c1->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c1->OMSetRenderTargets(1, &g_rtv, nullptr);
    c1->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = nullptr;
    c1->PSSetShaderResources(0, 1, &noSrv);
    ID3D11RenderTargetView* noRtv = nullptr;
    c1->OMSetRenderTargets(1, &noRtv, nullptr);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
    static bool lastCrop = !crop;
    if (++g_fits <= 2 || g_fits % 20000 == 0 || crop != lastCrop)
        Log("desktopview: %ux%u picture %s the %dx%d desktop window (shape kept) #%llu", d.Width, d.Height,
            crop ? "cropped to fill" : "fitted with bars into", cw, ch, (unsigned long long)g_fits);
    lastCrop = crop;
}

// D-329d (owner: "now two views left and right are fighting over at all times which one should show"; playbook 08.2):
// with AFR every Present carries one eye in turn, so the desktop window alternated between the eyes - a shift of the
// stereo disparity, invisible at the old 720 square (the picture shrunk to 0.35), plain in the 1280 x 720 crop (0.63).
// The left eye's pictures are kept (with their desktop HUD) and copied over each right-eye picture after its headset
// copy: the desktop shows the left eye steadily, each picture twice. Native stereo needs nothing (its Present always
// carries eye 1); mono has one picture. Forgotten outside the stereo view so no old left picture ever comes back.
ID3D11Texture2D* g_keep = nullptr;
D3D11_TEXTURE2D_DESC g_keepDesc{};
bool g_keepValid = false;
uint64_t g_kept = 0, g_replaced = 0;

void OneEye(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd) {
    static const bool on = config::GetFloat("desktop_one_eye", 1.f) > 0.5f;
    const bool afr = on && vr::XrRunning() && stereo::EyesAfrOn() && !stereo::EyesMonoOn() && !stereo::Screen2DFrame() &&
                     bd.SampleDesc.Count == 1 && bd.ArraySize == 1;
    if (!afr) { g_keepValid = false; return; }
    const bool same = g_keep && g_keepDesc.Width == bd.Width && g_keepDesc.Height == bd.Height &&
                      g_keepDesc.Format == bd.Format;
    const int eye = stereo::EyesPresentEye();
    if (eye == 0) {
        if (!same) {
            if (g_keep) { g_keep->Release(); g_keep = nullptr; }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = bd.Width;
            d.Height = bd.Height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = bd.Format;
            d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ID3D11Device* dev = nullptr;
            bb->GetDevice(&dev);
            const bool ok = dev && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_keep));
            if (dev) dev->Release();
            if (!ok) {
                g_keep = nullptr;
                g_keepValid = false;
                static bool logged = false;
                if (!logged) Log("desktopview: kept-eye image %ux%u failed - the desktop shows both AFR eyes", d.Width, d.Height);
                logged = true;
                return;
            }
            g_keepDesc = d;
        }
        ctx->CopyResource(g_keep, bb);
        g_keepValid = true;
        ++g_kept;
    } else if (g_keepValid && same) {  // the right eye (or a frame whose eye is unknown): the last left picture
        ctx->CopyResource(bb, g_keep);
        ++g_replaced;
    } else {
        return;
    }
    const uint64_t n = g_kept + g_replaced;
    if (n <= 2 || n % 20000 == 0)
        Log("desktopview: AFR - the desktop shows the left eye (%llu kept, %llu right pictures replaced)",
            (unsigned long long)g_kept, (unsigned long long)g_replaced);
}

}  // namespace

void DesktopViewOnPresent(IDXGISwapChain* sc) {
    if (!sc) return;
    static const bool fitOn = config::GetFloat("desktop_fit", 1.f) > 0.5f;
    int cw = 0, ch = 0;
    const bool owned = fitOn && DesktopWindowClient(&cw, &ch);
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) dev->GetImmediateContext(&ctx);
    if (ctx) {
        stereo::SharpUiDesktopHud(ctx, bb);  // (does nothing unless the HUD went only to the headset's layer)
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        OneEye(ctx, bb, bd);  // D-329d: AFR - the left eye steadily (after its HUD, before the fit)
        // D-329c: crop only the square stereo view (gowvr.ini desktop_crop 0 = bars there too); a square picture on a
        // flat-screen frame (the game's switch, 08.3) is shown whole
        static const bool cropOn = config::GetFloat("desktop_crop", 1.f) > 0.5f;
        const bool crop = cropOn && SquareViewNow() && bd.Width == bd.Height;
        if (owned) Fit(ctx, bb, cw, ch, crop);
        ctx->Release();
    }
    if (dev) dev->Release();
    bb->Release();
}

void DesktopViewOnResize() {
    g_keepValid = false;  // D-329d: (a new size gets a new kept image at the next left picture)
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_rtvFor = nullptr;
}

}  // namespace gowvr::frame
