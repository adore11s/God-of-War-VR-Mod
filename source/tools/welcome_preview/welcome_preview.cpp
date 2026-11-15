// D-328: welcome_preview.exe - every welcome window drawn by the mod's own code (src/vr/welcome_draw.cpp, the same
// built-in Montserrat as RCDATA 3101..3103) and saved as PNGs, to check a new or changed window before the headset.
//   welcome_preview.exe [output folder]   (default: docs\visuals next to this build's source, else the current folder)
// Writes welcome_<n>_<title>.png (each window over black, as it floats in the headset; the bubble also pointed at) and
// welcome_all.png (all windows in one sheet). Nothing of the game is needed.
#include "../../src/vr/welcome_draw.h"
#include <wincodec.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace gowvr {
void Log(const char* fmt, ...) {  // the drawing code's log lines go to the console
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}
}  // namespace gowvr

using namespace gowvr::vr::welcome;

static const wchar_t* kNames[kWelPages] = {L"welcome", L"kofi_supporter", L"first_person", L"vr_controls"};

// a premultiplied 0xAARRGGBB window over a solid background colour -> 0xFFRRGGBB
static void Over(const uint32_t* src, int w, int h, uint32_t bg, uint32_t* dst, int dstW, int ox, int oy) {
    const int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t c = src[y * w + x];
            const int a = c >> 24;
            const int r = ((c >> 16) & 255) + br * (255 - a) / 255, g = ((c >> 8) & 255) + bgc * (255 - a) / 255,
                      b = (c & 255) + bb * (255 - a) / 255;
            dst[(oy + y) * dstW + ox + x] = 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 |
                                            (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
        }
}

static bool SavePng(IWICImagingFactory* wic, const std::wstring& path, const uint32_t* px, int w, int h) {
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
         SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)px)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring out = argc > 1 ? argv[1] : L"";
    if (out.empty()) {
#ifdef GOWVR_VISUALS_DIR
        out = GOWVR_VISUALS_DIR;
#else
        out = L".";
#endif
    }
    CreateDirectoryW(out.c_str(), nullptr);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { std::printf("COM failed\n"); return 1; }
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        std::printf("WIC failed\n");
        return 1;
    }
    if (!MakeD2D()) { std::printf("Direct2D / DirectWrite not available\n"); return 1; }
    const uint32_t black = 0x000000u, sheetBg = 0x101010u;
    const int gap = 40, cols = 2, rows = (kWelPages + cols - 1) / cols;
    const int sw = cols * kWW + (cols + 1) * gap, sh = rows * kWH + (rows + 1) * gap;
    std::vector<uint32_t> sheet((size_t)sw * sh, 0xff000000u | sheetBg);
    std::vector<uint32_t> one((size_t)kWW * kWH);
    int fails = 0;
    for (int p = 0; p < kWelPages; ++p) {
        for (int hover = 0; hover < 2; ++hover) {
            WelcomeDraw(p, hover != 0);
            Over(g_wOut, kWW, kWH, black, one.data(), kWW, 0, 0);
            wchar_t name[160];
            swprintf_s(name, L"%s\\welcome_%d_%s%s.png", out.c_str(), p + 1, kNames[p], hover ? L"_pointed" : L"");
            if (SavePng(wic, name, one.data(), kWW, kWH)) std::wprintf(L"wrote %s\n", name);
            else { std::wprintf(L"FAILED %s\n", name); ++fails; }
            if (!hover)
                Over(g_wOut, kWW, kWH, sheetBg, sheet.data(), sw, gap + (p % cols) * (kWW + gap), gap + (p / cols) * (kWH + gap));
        }
    }
    const std::wstring all = out + L"\\welcome_all.png";
    if (SavePng(wic, all, sheet.data(), sw, sh)) std::wprintf(L"wrote %s\n", all.c_str());
    else { std::wprintf(L"FAILED %s\n", all.c_str()); ++fails; }
    wic->Release();
    CoUninitialize();
    return fails ? 1 : 0;
}
