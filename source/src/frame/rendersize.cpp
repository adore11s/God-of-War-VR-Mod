// Square VR picture (rendersize.h). STATIC (decompile, win_platform): in Windowed and Borderless mode the game renders
// at its window's client size. The message pump sub_14098fbe0 compares GetClientRect(game window) with the client size
// it keeps (0x14504c398 / 0x14504c39c) every frame and flags a resize when they differ (also after WM_SIZE, 0x14504c334);
// the per-frame display update (win_platform_014) then renders and resizes the swapchain to that size. So while VR runs
// GoW.exe's GetClientRect reports N x N for the game window: the game itself renders and resizes to N x N, and DXGI
// stretches the swapchain onto the (small) window on Present. Only GoW.exe's import slots are patched - DXGI, the
// runtime and the mod see the real window.
// The settings save (win_platform_009, on exit and on an options change) takes WindowLeft / Top / Width / Height from
// GetWindowPlacement(game window) rcNormalPosition: while the window is small that reports the window as it was, so
// settings.ini keeps the player's own window.
// The render aspect (win_platform_014, after the pump): the game renders its client height x the aspect of the
// AspectRatio setting - the .rdata table 0x140d60e38 {Auto 0, 16:10 1.6, 16:9 1.778, 21:9 2.333} indexed by the live
// setting 0x141227c20 - and "Auto" (0) never goes below 16:10, so an N x N client was rendered N x 0.625 N,
// letterboxed. While square every entry is 1.0: the game renders the whole N x N. The settings save writes the
// setting's NAME by its index, so settings.ini keeps the player's AspectRatio; the original values come back with VR off.
#include "rendersize.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/patch.h"
#include "../stereo/mutate.h"
#include <windows.h>
#include <dxgi.h>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace gowvr::frame {
namespace {

using PFN_GetClientRect = BOOL(WINAPI*)(HWND, LPRECT);
using PFN_GetWindowPlacement = BOOL(WINAPI*)(HWND, WINDOWPLACEMENT*);
PFN_GetClientRect o_GetClientRect = nullptr;
PFN_GetWindowPlacement o_GetWindowPlacement = nullptr;

std::atomic<HWND> g_wnd{nullptr};     // the game window (the swapchain's output window)
// the client size the game is told (w << 32 | h), 0 = its real one: N x N for the stereo view; while the flat 2D screen
// shows (menus, loading, fades, videos - owner: "the 2D cutscenes are rendering square"), the window's own size from
// before it was made small, so those frames are the game's normal 16:9 (or whatever its window was) picture
std::atomic<uint64_t> g_spoof{0};
bool g_square = false;                // the stereo view renders N x N now
int g_ownW = 0, g_ownH = 0;           // the window's client size before it was made small
std::atomic<bool> g_shrunk{false};    // the window is the small square now
std::atomic<uint64_t> g_spoofs{0};    // GetClientRect answers N x N (the game asked)
SRWLOCK g_savedLock = SRWLOCK_INIT;
WINDOWPLACEMENT g_saved{};            // the window before it was made small (reported to the game while small)
int g_windowPx = 720;                 // desktop_window (0 = leave the window alone)

constexpr uint64_t kAspectTableVa = 0x140d60e38;  // float[4] render aspects of the AspectRatio setting (.rdata)
constexpr float kAspects[4] = {0.f, 1.6f, 1.7777778f, 2.3333333f};
bool g_aspectSquare = false;
bool g_squareFailed = false;  // the aspect table could not be set: no square picture this run

// The render aspect table: 1.0 (square) or the game's own values. Only over the values the game shipped with.
bool SetSquareAspect(bool square) {
    if (square == g_aspectSquare) return true;
    float* t = (float*)GameVa(kAspectTableVa);
    if (!t) return false;
    for (int i = 0; i < 4; ++i) {
        const float want = square ? kAspects[i] : 1.f;  // what the table must hold now
        if (fabsf(t[i] - want) > 1e-4f) {
            static bool logged = false;
            if (!logged) Log("rendersize: aspect table %.4f %.4f %.4f %.4f is not the game's - left alone", t[0], t[1],
                             t[2], t[3]);
            logged = true;
            if (square) g_squareFailed = true;
            return false;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(t, 4 * sizeof(float), PAGE_READWRITE, &old)) { if (square) g_squareFailed = true; return false; }
    for (int i = 0; i < 4; ++i) t[i] = square ? 1.f : kAspects[i];
    VirtualProtect(t, 4 * sizeof(float), old, &old);
    g_aspectSquare = square;
    Log("rendersize: render aspect %s", square ? "1.0 (square, every AspectRatio setting)" : "the game's own again");
    return true;
}

BOOL WINAPI hk_GetClientRect(HWND w, LPRECT r) {
    const BOOL ok = o_GetClientRect(w, r);
    const uint64_t s = g_spoof.load(std::memory_order_relaxed);
    if (ok && s && r && w && w == g_wnd.load(std::memory_order_relaxed)) {
        r->left = r->top = 0;
        r->right = (LONG)(s >> 32);
        r->bottom = (LONG)(s & 0xffffffffu);
        g_spoofs.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

BOOL WINAPI hk_GetWindowPlacement(HWND w, WINDOWPLACEMENT* p) {
    const BOOL ok = o_GetWindowPlacement(w, p);
    if (ok && p && w && w == g_wnd.load(std::memory_order_relaxed) && g_shrunk.load()) {
        AcquireSRWLockShared(&g_savedLock);
        p->rcNormalPosition = g_saved.rcNormalPosition;
        p->showCmd = g_saved.showCmd;
        ReleaseSRWLockShared(&g_savedLock);
    }
    return ok;
}

// Owner 2026-10-18: the picture is the size the runtime asks for (SteamVR: its resolution setting, VDXR: Virtual
// Desktop's VR Graphics Quality). Quest testers on VDXR drew 2048 into 2688x2880 eye images - stretched up a third,
// blurry on a Quest's sharper screen. 0 = no session yet.
std::atomic<int> g_eyePx{0};
int AutoSize() {
    const int v = g_eyePx.load();
    if (v <= 0 || config::GetFloat("render_size_auto", 1.f) < 0.5f) return 0;
    const int n = (v + 7) & ~7;  // the larger side of the eye image, rounded up to a multiple of 8
    return n < 512 ? 512 : n > 8192 ? 8192 : n;
}

// render_size: N (512..8192, a multiple of 8), 0 / below 512 = the game's own size; with a session the automatic size
// instead (render_size_auto=1, the default)
int ConfiguredSize() {
    if (const int a = AutoSize()) return a;
    const int v = (int)config::GetFloat("render_size", 2048.f);
    return v >= 512 && v <= 8192 ? (v & ~7) : 0;
}

bool Windowed(HWND w) { return (GetWindowLongPtrW(w, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; }

// The window -> a desktop_window x desktop_window client area in the middle of its monitor. SWP_ASYNCWINDOWPOS: the
// window belongs to the game's main thread, and this runs on the render thread (a synchronous move could wait on a
// thread that waits on this one).
void Shrink(HWND w) {
    RECT c{};
    o_GetClientRect(w, &c);
    if (c.right - c.left == g_windowPx && c.bottom - c.top == g_windowPx) return;
    if (!g_shrunk.load()) {
        WINDOWPLACEMENT p{sizeof(p)};
        if (!o_GetWindowPlacement(w, &p)) return;
        AcquireSRWLockExclusive(&g_savedLock);
        g_saved = p;
        ReleaseSRWLockExclusive(&g_savedLock);
        g_ownW = c.right - c.left;
        g_ownH = c.bottom - c.top;
        g_shrunk = true;
        if (p.showCmd == SW_SHOWMAXIMIZED) ShowWindowAsync(w, SW_RESTORE);
    }
    RECT r{0, 0, g_windowPx, g_windowPx};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    SetWindowPos(w, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    static int logs = 0;
    if (logs++ < 5)
        Log("rendersize: desktop window %dx%d (was %ldx%ld; the headset still gets the full square)", g_windowPx,
            g_windowPx, c.right - c.left, c.bottom - c.top);
}

void Restore(HWND w) {
    g_shrunk = false;
    AcquireSRWLockShared(&g_savedLock);
    const WINDOWPLACEMENT p = g_saved;
    ReleaseSRWLockShared(&g_savedLock);
    const RECT& r = p.rcNormalPosition;
    SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (p.showCmd == SW_SHOWMAXIMIZED) ShowWindowAsync(w, SW_MAXIMIZE);
    Log("rendersize: desktop window back to %ldx%ld", r.right - r.left, r.bottom - r.top);
}

}  // namespace

void InstallRenderSize() {
    const int dw = (int)config::GetFloat("desktop_window", 720.f);
    g_windowPx = dw >= 240 && dw <= 4096 ? dw : 0;
    HMODULE exe = GetModuleHandleW(nullptr);
    const int a = PatchImportAny(exe, "user32", 0, "GetClientRect", (void*)&hk_GetClientRect, (void**)&o_GetClientRect);
    const int b = PatchImportAny(exe, "user32", 0, "GetWindowPlacement", (void*)&hk_GetWindowPlacement,
                                 (void**)&o_GetWindowPlacement);
    if (!a || !b || !o_GetClientRect || !o_GetWindowPlacement) {
        Log("rendersize: GoW.exe imports not found (GetClientRect %d, GetWindowPlacement %d) - the game keeps its own "
            "size in VR", a, b);
        o_GetClientRect = nullptr;  // (RenderSizeTick stays off; a patched slot still calls the real function)
        return;
    }
    const int n = ConfiguredSize();
    if (n)
        Log("rendersize: installed - %dx%d in VR (gowvr.ini render_size), desktop window %d", n, n, g_windowPx);
    else
        Log("rendersize: installed - off (gowvr.ini render_size), the game's own size in VR");
}

void RenderSizeTick(IDXGISwapChain* sc, bool vrRunning) {
    if (!o_GetClientRect || !sc) return;
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;
    HWND w = d.OutputWindow;
    g_wnd = w;
    const int cfg = vrRunning ? ConfiguredSize() : 0;
    const bool flat = cfg && stereo::Screen2DFrame();  // the flat 2D screen shows this frame
    bool square = cfg && !flat;
    if (square != g_aspectSquare && !SetSquareAspect(square)) square = false;  // no square aspect: no square client
    // the game's pump sees the new client size at its next frame and renders / resizes to it
    uint64_t spoof = 0;
    if (square) spoof = (uint64_t)cfg << 32 | (uint32_t)cfg;
    else if (flat && g_shrunk.load() && g_ownW > 0 && g_ownH > 0) spoof = (uint64_t)g_ownW << 32 | (uint32_t)g_ownH;
    if (spoof != g_spoof.load()) {
        g_spoof = spoof;
        if (square) Log("rendersize: square %dx%d for the stereo view (the game renders and resizes to it)", cfg, cfg);
        else if (flat) Log("rendersize: flat 2D screen at the window's own size %dx%d", g_ownW, g_ownH);
        else Log("rendersize: the game's own size again (VR %s)", vrRunning ? "on, render_size off" : "off");
    }
    g_square = square;
    static ULONGLONG check = 0;
    const ULONGLONG now = GetTickCount64();
    if (cfg && g_windowPx && Windowed(w)) {
        // once a second: the game sets its window again when its resolution or display mode changes
        if (now - check >= 1000) { check = now; Shrink(w); }
    } else if (g_shrunk.load()) {
        Restore(w);
    }
}

bool RenderSizeExpected(bool flatFrame, uint32_t* w, uint32_t* h) {
    if (!o_GetClientRect || !g_wnd.load()) return false;
    const int cfg = ConfiguredSize();
    if (!cfg) return false;
    if (!flatFrame) {
        if (g_squareFailed) return false;
        *w = *h = (uint32_t)cfg;
        return true;
    }
    if (!g_shrunk.load() || g_ownW <= 0 || g_ownH <= 0) return false;  // borderless: the game's own size, unknown here
    *w = (uint32_t)g_ownW;
    *h = (uint32_t)g_ownH;
    return true;
}

void SetRuntimeEyeSize(int px) {
    if (px == g_eyePx.exchange(px)) return;
    if (px > 0) {
        const int a = AutoSize();
        if (a) Log("rendersize: the runtime asks for %dpx eye images - the picture is %dx%d (render_size_auto=1)", px, a, a);
        else Log("rendersize: the runtime asks for %dpx eye images - the picture stays render_size (render_size_auto=0)", px);
    }
}

int AutoRenderSize() { return AutoSize(); }
bool RuntimeEyeSizeKnown() { return g_eyePx.load() > 0; }

void RenderSizeDiagText(char* out, size_t n) {
    RECT c{};
    HWND w = g_wnd.load();
    if (o_GetClientRect && w) o_GetClientRect(w, &c);
    const uint64_t s = g_spoof.load();
    sprintf_s(out, n, "square picture %s (render_size %d%s), the game told %ux%u, desktop window %s %ldx%ld "
              "(desktop_window %d, own %dx%d), client size asks answered %llu",
              g_square ? "ON" : "off", ConfiguredSize(), AutoSize() ? ", auto" : "", (unsigned)(s >> 32),
              (unsigned)(s & 0xffffffffu),
              g_shrunk.load() ? "small" : "the game's", c.right - c.left, c.bottom - c.top, g_windowPx, g_ownW, g_ownH,
              (unsigned long long)g_spoofs.load());
}

}  // namespace gowvr::frame
