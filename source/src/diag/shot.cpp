// Backbuffer screenshots for driving/inspecting the game without touching the user's desktop.
// Render thread: CopyResource to a staging texture, Map, copy rows to a CPU buffer.
// Command thread: convert (R10G10B10A2 / RGBA8 / BGRA8), box-downscale and encode PNG through WIC.
#include "shot.h"
#include "../core/log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace gowvr::diag {
namespace {
std::mutex g_mu;
std::atomic<bool> g_pending{false};
std::atomic<bool> g_ready{false};
std::string g_name;
int g_div = 4;
std::vector<uint8_t> g_pixels;
UINT g_w = 0, g_h = 0, g_pitch = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;

void ToRgb8(uint32_t px, DXGI_FORMAT f, uint8_t* out) {
    switch (f) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            out[0] = (uint8_t)(((px >> 0) & 1023) >> 2);
            out[1] = (uint8_t)(((px >> 10) & 1023) >> 2);
            out[2] = (uint8_t)(((px >> 20) & 1023) >> 2);
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            out[0] = (uint8_t)(px >> 16);
            out[1] = (uint8_t)(px >> 8);
            out[2] = (uint8_t)px;
            break;
        default:  // R8G8B8A8
            out[0] = (uint8_t)px;
            out[1] = (uint8_t)(px >> 8);
            out[2] = (uint8_t)(px >> 16);
            break;
    }
}
}  // namespace

void ShotRequest(const char* name, int div) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_name = name;
    g_div = div < 1 ? 1 : div;
    g_ready = false;
    g_pending = true;
}

void ShotOnPresent(IDXGISwapChain* sc) {
    if (!g_pending.exchange(false)) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_w = d.Width;
            g_h = d.Height;
            g_pitch = m.RowPitch;
            g_fmt = d.Format;
            g_pixels.assign((uint8_t*)m.pData, (uint8_t*)m.pData + (size_t)m.RowPitch * d.Height);
            ctx->Unmap(st, 0);
            g_ready = true;
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
    bb->Release();
}

bool ShotFlush(int timeoutMs) {
    for (int t = 0; !g_ready && t < timeoutMs; t += 10) Sleep(10);
    if (!g_ready.exchange(false)) {
        Log("shot: no frame captured (game not presenting?)");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    return WritePng(g_name.c_str(), g_pixels.data(), g_w, g_h, g_pitch, (int)g_fmt, g_div);
}

bool WritePng(const char* name, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t pitch, int format, int div) {
    const DXGI_FORMAT fmt = (DXGI_FORMAT)format;
    if (div < 1) div = 1;
    UINT ow = w / div, oh = h / div;
    std::vector<uint8_t> rgb((size_t)ow * oh * 3);
    for (UINT y = 0; y < oh; ++y)
        for (UINT x = 0; x < ow; ++x) {
            uint32_t acc[3] = {0, 0, 0};
            for (int dy = 0; dy < div; ++dy)
                for (int dx = 0; dx < div; ++dx) {
                    uint32_t px = *(const uint32_t*)&pixels[(size_t)(y * div + dy) * pitch + (size_t)(x * div + dx) * 4];
                    uint8_t c[3];
                    ToRgb8(px, fmt, c);
                    acc[0] += c[0];
                    acc[1] += c[1];
                    acc[2] += c[2];
                }
            uint8_t* o = &rgb[((size_t)y * ow + x) * 3];
            int n = div * div;
            // WIC 24bppBGR
            o[0] = (uint8_t)(acc[2] / n);
            o[1] = (uint8_t)(acc[1] / n);
            o[2] = (uint8_t)(acc[0] / n);
        }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    *wcsrchr(path, L'\\') = 0;
    wcscat_s(path, L"\\gowvr_logs\\shots");
    CreateDirectoryW(path, nullptr);
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"%s\\%hs.png", path, name);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(file, GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(ow, oh));
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
         SUCCEEDED(fr->WritePixels(oh, ow * 3, (UINT)rgb.size(), rgb.data())) && SUCCEEDED(fr->Commit()) &&
         SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (s) s->Release();
    if (f) f->Release();
    Log("shot: %ls %ux%u (src %ux%u fmt=%d) %s", file, ow, oh, w, h, format, ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace gowvr::diag
