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
std::atomic<int> g_ownW{0}, g_ownH{0};  // the window's client size before it was made small (D-329: set on the main thread)
std::atomic<bool> g_shrunk{false};    // the window is the small square now
std::atomic<uint64_t> g_spoofs{0};    // GetClientRect answers N x N (the game asked)
SRWLOCK g_savedLock = SRWLOCK_INIT;
WINDOWPLACEMENT g_saved{};            // the window before it was made small (reported to the game while small)
int g_windowPx = 720;                 // desktop_window: the window's client HEIGHT (0 = leave the window alone)
// D-329b (owner 2026-11-15: "make the default size HD"): the default window is 16:9 - 720 -> 1280 x 720
int DefaultW() { return (g_windowPx * 16 + 4) / 9; }

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

// ---- D-329: the desktop window can be resized by the player while VR runs ------------------------------------------
// Owner 2026-11-15: "make it so the window can be resized by the user; nothing may break (the UI, the 2D screens)".
// The game never sees the window's size in VR (its GetClientRect is answered N x N, or its own size on flat screens), so
// any window size is safe for the game; the picture is fitted into it with its shape kept (desktopview.cpp). The
// window belongs to the game's main thread: every change to it (sizing border, size, giving it back) is made THERE, in
// a window procedure put in front of the game's, on a message posted by the render thread (never a synchronous call
// into another thread's window from the render thread). The player's size is kept in gowvr.ini (desktop_window_w /
// _h) and used at the next VR start; the game's settings.ini still gets the player's own window (GetWindowPlacement).
constexpr int kMinClientPx = 160;
constexpr LONG_PTR kSizingStyle = WS_THICKFRAME | WS_MAXIMIZEBOX;
UINT g_msg = 0;                             // RegisterWindowMessageW: "update the window" (posted, render thread)
WNDPROC g_prevProc = nullptr;
std::atomic<HWND> g_procWnd{nullptr};       // the window with our procedure in front
std::atomic<bool> g_wantOwn{false};         // render thread: the window should be the mod's (VR, Windowed)
std::atomic<bool> g_posted{false};          // an update message is on its way
std::atomic<bool> g_sizing{false};          // the player drags the window's edge / title bar right now
std::atomic<int> g_userW{-1}, g_userH{-1};  // the player's client size (-1 = not read yet, 0 = desktop_window square)
std::atomic<uint64_t> g_userResizes{0};
LONG_PTR g_addedStyle = 0;                  // (main thread) the style bits added, taken away again when VR ends
bool g_placed = false;                      // (main thread) the window was put in the middle of its monitor this time

void ReadUserSize() {
    if (g_userW.load() >= 0) return;
    const int w = (int)config::GetFloat("desktop_window_w", 0.f), h = (int)config::GetFloat("desktop_window_h", 0.f);
    const bool ok = w >= kMinClientPx && h >= kMinClientPx && w <= 16384 && h <= 16384;
    g_userH = ok ? h : 0;
    g_userW = ok ? w : 0;
}

// D-329c (owner: "the window always 16:9"): every client size the mod sets or keeps is 16:9, from its height
int W169(int h) { return (h * 16 + 4) / 9; }
int H169(int w) { return (w * 9 + 8) / 16; }
// the window's frame (outer size minus client size) for its current style
void FrameSize(HWND w, int* fw, int* fh) {
    RECT f{0, 0, 0, 0};
    AdjustWindowRectEx(&f, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE));
    *fw = f.right - f.left;
    *fh = f.bottom - f.top;
}
// the largest 16:9 client that fits the monitor's work area (false: no monitor)
bool MaxClient169(HWND w, int* cw, int* ch, MONITORINFO* out = nullptr) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    int fw = 0, fh = 0;
    FrameSize(w, &fw, &fh);
    int mw = (mi.rcWork.right - mi.rcWork.left) - fw, mh = (mi.rcWork.bottom - mi.rcWork.top) - fh;
    if (mw < kMinClientPx || mh < kMinClientPx) return false;
    if (W169(mh) > mw) mh = H169(mw);
    *cw = W169(mh);
    *ch = mh;
    if (out) *out = mi;
    return true;
}

// the client size the window should have now: 16:9 at the player's height, else at desktop_window (HD); never above
// the monitor
void TargetClient(HWND w, int* cw, int* ch) {
    ReadUserSize();
    int th = g_userH.load();
    if (th <= 0 || g_userW.load() <= 0) th = g_windowPx;
    int tw = W169(th);
    int mw = 0, mh = 0;
    if (MaxClient169(w, &mw, &mh) && th > mh) { tw = mw; th = mh; }
    *cw = tw;
    *ch = th;
}

// main thread: the window becomes (or stays) the mod's small, resizable window
void OwnWindowNow(HWND w) {
    if (!g_shrunk.load()) {
        WINDOWPLACEMENT p{sizeof(p)};
        if (!o_GetWindowPlacement(w, &p)) return;
        RECT c{};
        o_GetClientRect(w, &c);
        AcquireSRWLockExclusive(&g_savedLock);
        g_saved = p;
        ReleaseSRWLockExclusive(&g_savedLock);
        g_ownW = c.right - c.left;
        g_ownH = c.bottom - c.top;
        g_placed = false;
        g_shrunk = true;  // (after g_own*: the render thread reads them once this is set)
        if (IsZoomed(w)) ShowWindow(w, SW_RESTORE);
    }
    const LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
    if ((st & kSizingStyle) != kSizingStyle) {  // (again if the game set its style anew)
        g_addedStyle |= kSizingStyle & ~st;
        SetWindowLongPtrW(w, GWL_STYLE, st | kSizingStyle);
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (g_sizing.load() || IsZoomed(w) || IsIconic(w)) return;  // the player's own doing: left alone
    int tw = 0, th = 0;
    TargetClient(w, &tw, &th);
    RECT c{};
    o_GetClientRect(w, &c);
    const int cw = c.right - c.left, ch = c.bottom - c.top;
    if (abs(cw - tw) <= 1 && abs(ch - th) <= 1) { g_placed = true; return; }  // (WM_SIZING's rounding)
    RECT r{0, 0, tw, th};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    if (!g_placed) {  // the first time: in the middle of its monitor
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
        SetWindowPos(w, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        g_placed = true;
    } else {  // later (the game set its window again): the size only, where the player put it
        SetWindowPos(w, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    static int logs = 0;
    if (logs++ < 5)
        Log("rendersize: desktop window %dx%d, resizable (was %dx%d; the headset still gets the full picture)", tw, th,
            cw, ch);
}

// main thread: the player's window back as it was before VR (position, size, maximized), the sizing border taken away
void ReleaseWindowNow(HWND w) {
    if (!g_shrunk.load()) return;
    if (g_addedStyle) {
        SetWindowLongPtrW(w, GWL_STYLE, GetWindowLongPtrW(w, GWL_STYLE) & ~g_addedStyle);
        g_addedStyle = 0;
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    AcquireSRWLockShared(&g_savedLock);
    WINDOWPLACEMENT p = g_saved;
    ReleaseSRWLockShared(&g_savedLock);
    p.length = sizeof(p);
    p.flags = 0;
    p.showCmd = p.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
    SetWindowPlacement(w, &p);
    g_sizing = false;
    g_shrunk = false;
    const RECT& r = p.rcNormalPosition;
    Log("rendersize: desktop window back to %ldx%ld%s", r.right - r.left, r.bottom - r.top,
        p.showCmd == SW_SHOWMAXIMIZED ? " (maximized)" : "");
}

// main thread: the player let go of the window's edge - that size is theirs (kept in gowvr.ini for the next VR start)
void RememberUserSize(HWND w) {
    if (!g_shrunk.load() || IsZoomed(w) || IsIconic(w)) return;
    RECT c{};
    o_GetClientRect(w, &c);
    const int cw = c.right - c.left, ch = c.bottom - c.top;
    if (cw < kMinClientPx || ch < kMinClientPx) return;
    int tw = 0, th = 0;
    TargetClient(w, &tw, &th);
    if (abs(ch - th) <= 1) return;  // only moved (the height is what the player chose; the width is 16:9 of it)
    // (a window snapped to half the screen is not 16:9: its height is kept, the size check makes the width 16:9)
    const int uh = ch, uw = W169(ch);
    g_userW = uw;
    g_userH = uh;
    ++g_userResizes;
    config::SetFloats({{"desktop_window_w", (float)uw}, {"desktop_window_h", (float)uh}});
    Log("rendersize: the player resized the desktop window to %dx%d (16:9; kept: gowvr.ini desktop_window_w / _h)", uw,
        uh);
}

// main thread, WM_SIZING (the player drags an edge or a corner): the dragged rectangle kept 16:9 - the edge dragged
// decides, a corner by whichever side moved further; the opposite edge / corner stays where it is
void Sizing169(HWND w, WPARAM edge, RECT* r) {
    int fw = 0, fh = 0;
    FrameSize(w, &fw, &fh);
    int cw = (r->right - r->left) - fw, ch = (r->bottom - r->top) - fh;
    const bool byWidth = edge == WMSZ_LEFT || edge == WMSZ_RIGHT ||
                         ((edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMLEFT ||
                           edge == WMSZ_BOTTOMRIGHT) && cw * 9 > ch * 16);
    if (byWidth) ch = H169(cw);
    else cw = W169(ch);
    if (ch < kMinClientPx) { ch = kMinClientPx; cw = W169(ch); }
    int mw = 0, mh = 0;
    if (MaxClient169(w, &mw, &mh) && ch > mh) { cw = mw; ch = mh; }
    const int ww = cw + fw, wh = ch + fh;
    const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    if (left) r->left = r->right - ww;
    else r->right = r->left + ww;
    if (top) r->top = r->bottom - wh;
    else r->bottom = r->top + wh;
}

LRESULT CALLBACK DesktopWindowProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_msg && m == g_msg) {
        g_posted = false;
        if (g_wantOwn.load()) OwnWindowNow(h);
        else ReleaseWindowNow(h);
        return 0;
    }
    switch (m) {
    case WM_ENTERSIZEMOVE:
        if (g_shrunk.load()) g_sizing = true;
        break;
    case WM_EXITSIZEMOVE:
        if (g_sizing.exchange(false)) RememberUserSize(h);
        break;
    case WM_SIZING: {
        const LRESULT r = CallWindowProcW(g_prevProc, h, m, wp, lp);
        if (!g_shrunk.load() || !lp) return r;
        Sizing169(h, wp, (RECT*)lp);  // D-329c
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        const LRESULT r = CallWindowProcW(g_prevProc, h, m, wp, lp);
        if (g_shrunk.load() && lp) {
            auto* mm = (MINMAXINFO*)lp;
            int fw = 0, fh = 0;
            FrameSize(h, &fw, &fh);
            mm->ptMinTrackSize.x = W169(kMinClientPx) + fw;
            mm->ptMinTrackSize.y = kMinClientPx + fh;
            // D-329c: maximized = the largest 16:9 window on its monitor, centred in the work area. ptMaxPosition is
            // relative to the monitor's own corner (Windows moves it onto the monitor the window maximizes on)
            MONITORINFO mi{sizeof(mi)};
            int cw = 0, ch = 0;
            if (MaxClient169(h, &cw, &ch, &mi)) {
                const int ww = cw + fw, wh = ch + fh;
                const int ax = (mi.rcWork.right - mi.rcWork.left - ww) / 2, ay = (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
                mm->ptMaxSize.x = ww;
                mm->ptMaxSize.y = wh;
                mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left + ax;
                mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top + ay;
                if (mm->ptMaxTrackSize.x < ww) mm->ptMaxTrackSize.x = ww;
                if (mm->ptMaxTrackSize.y < wh) mm->ptMaxTrackSize.y = wh;
            }
        }
        return r;
    }
    default:
        break;
    }
    return CallWindowProcW(g_prevProc, h, m, wp, lp);
}

// render thread: our procedure in front of the game window's (once; another window falls back to the old way)
bool HookWindow(HWND w) {
    if (g_procWnd.load() == w) return true;
    if (g_procWnd.load() && IsWindow(g_procWnd.load())) return false;
    if (!g_msg) g_msg = RegisterWindowMessageW(L"gowvr_desktop_window");
    if (!g_msg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return false;
    g_prevProc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)&DesktopWindowProc);
    if (!g_prevProc) { Log("rendersize: window procedure not set - the desktop window is not resizable"); return false; }
    g_procWnd = w;
    Log("rendersize: desktop window procedure set (resizable while VR runs, D-329)");
    return true;
}

// The window -> a desktop_window x desktop_window client area in the middle of its monitor. SWP_ASYNCWINDOWPOS: the
// window belongs to the game's main thread, and this runs on the render thread (a synchronous move could wait on a
// thread that waits on this one). D-329: only the fallback now (our window procedure could not be set).
void Shrink(HWND w) {
    RECT c{};
    o_GetClientRect(w, &c);
    if (c.right - c.left == DefaultW() && c.bottom - c.top == g_windowPx) return;
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
    RECT r{0, 0, DefaultW(), g_windowPx};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    SetWindowPos(w, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    static int logs = 0;
    if (logs++ < 5)
        Log("rendersize: desktop window %dx%d (was %ldx%ld; the headset still gets the full square)", DefaultW(),
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
    else if (flat && g_shrunk.load() && g_ownW > 0 && g_ownH > 0) spoof = (uint64_t)g_ownW.load() << 32 | (uint32_t)g_ownH.load();
    if (spoof != g_spoof.load()) {
        g_spoof = spoof;
        if (square) Log("rendersize: square %dx%d for the stereo view (the game renders and resizes to it)", cfg, cfg);
        else if (flat) Log("rendersize: flat 2D screen at the window's own size %dx%d", g_ownW.load(), g_ownH.load());
        else Log("rendersize: the game's own size again (VR %s)", vrRunning ? "on, render_size off" : "off");
    }
    g_square = square;
    static ULONGLONG check = 0;
    const ULONGLONG now = GetTickCount64();
    const bool own = cfg && g_windowPx && Windowed(w);
    if (HookWindow(w)) {  // D-329: every change made on the window's own thread
        g_wantOwn = own;
        // once a second while owned (the game sets its window again when its resolution or display mode changes), at
        // once when VR starts or ends
        if (own != g_shrunk.load() || (own && now - check >= 1000)) {
            check = now;
            if (!g_posted.exchange(true) && !PostMessageW(w, g_msg, 0, 0)) g_posted = false;
        }
        return;
    }
    if (own) {
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
    *w = (uint32_t)g_ownW.load();
    *h = (uint32_t)g_ownH.load();
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

bool SquareViewNow() { return g_square; }

bool DesktopWindowClient(int* w, int* h) {
    const HWND wnd = g_wnd.load();
    if (!o_GetClientRect || !wnd || !g_shrunk.load() || IsIconic(wnd)) return false;
    RECT c{};
    if (!o_GetClientRect(wnd, &c)) return false;
    *w = c.right - c.left;
    *h = c.bottom - c.top;
    return *w > 0 && *h > 0;
}

void RenderSizeDiagText(char* out, size_t n) {
    RECT c{};
    HWND w = g_wnd.load();
    if (o_GetClientRect && w) o_GetClientRect(w, &c);
    const uint64_t s = g_spoof.load();
    sprintf_s(out, n, "square picture %s (render_size %d%s), the game told %ux%u, desktop window %s %ldx%ld "
              "(desktop_window %d, own %dx%d), client size asks answered %llu",
              g_square ? "ON" : "off", ConfiguredSize(), AutoSize() ? ", auto" : "", (unsigned)(s >> 32),
              (unsigned)(s & 0xffffffffu),
              g_shrunk.load() ? "small" : "the game's", c.right - c.left, c.bottom - c.top, g_windowPx, g_ownW.load(), g_ownH.load(),
              (unsigned long long)g_spoofs.load());
}

}  // namespace gowvr::frame
