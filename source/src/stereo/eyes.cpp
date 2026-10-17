// Execute both kicks' recordings (R1 eye submission).
//
// Facts this relies on (STATIC, FRAME_MAP.md "Eye submission"):
// - A chunk context is 0x850 bytes at 0x1422989c0 + (frameIdx + chunk*2)*0x850; the render thread executes a submitted
//   context as sub_1409ab650(ctx+0x50), which only walks the packet list whose head is at ctx+0x98.
// - Packets live in the per-frame allocator DAT_142d4b278[DAT_142d18ff4], reset only when that allocator is reused two
//   frames later (main frame start, after waiting for the render thread) — so kick 1's packets stay valid after kick 2
//   re-records the same contexts.
// So a byte copy of kick 1's contexts, taken after kick 1's recording jobs drained (engine reset waits exec988), is a
// self-contained eye-0 recording. It is pushed into the render ring right before kick 2's chunk 0 at end-of-frame
// submission; the render thread then runs: main ctx, eye 0 chunks, eye 1 chunks, Present.
#include "eyes.h"
#include "../core/addr.h"
#include "sharpui.h"
#include "../core/detour.h"
#include "../core/log.h"
#include "../diag/shot.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gowvr::stereo {
namespace {

// tools/re/hookgen.py: both are `call sub_1409a09c0` (5 bytes, rcx = context)
constexpr HookSite kSubmitMain{"ChunkSubmitMain", 0x140410178, 5,
    {0xe8, 0x43, 0x08, 0x59, 0x00, 0xff, 0xc3, 0x3b, 0x1d, 0x3b, 0x92, 0xe9, 0x01, 0x72, 0xd9, 0x8b}};
constexpr HookSite kSubmitAlt{"ChunkSubmitAlt", 0x1404baa08, 5,
    {0xe8, 0xb3, 0x5f, 0x4e, 0x00, 0xff, 0xc3, 0x3b, 0x1d, 0xab, 0xe9, 0xde, 0x01, 0x72, 0xd9, 0x48}};
// Command-stream executor sub_1409ab650 (render thread): mov r11,rsp; push r12
constexpr HookSite kExec{"CmdExec", 0x1409ab650, 5,
    {0x4c, 0x8b, 0xdc, 0x41, 0x54, 0x41, 0x57, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x8b}};

constexpr uint64_t kCtxBase = 0x1422989c0, kChunkCount = 0x1422a93c0, kFrameIdx = 0x142d19024;
constexpr uint64_t kSubmitSerial = 0x142d50670;  // incremented at the start of sub_140410030
constexpr size_t kCtxSize = 0x850;
constexpr int kSlots = 4, kMaxChunks = 16;

uint8_t* g_base = nullptr;
template <class T> T& G(uint64_t va) { return *(T*)((uint8_t*)GameVa(va)); }
using PFN_Submit = void(__fastcall*)(uint8_t* ctx);
using PFN_Exec = void(__fastcall*)(uint8_t* stream);
PFN_Exec o_Exec = nullptr;

alignas(16) uint8_t g_copies[kSlots][kMaxChunks][kCtxSize];
int g_slot = 0;
std::atomic<int> g_pendingN{0};  // stolen, not yet submitted
uint8_t* g_pendingBase = nullptr;
uint32_t g_pendingSerial = 0;
// ctx+0x50 of the last eye-0 copy submitted, per slot (the render thread may still be a frame behind the submitter)
std::atomic<uint8_t*> g_eye0LastStream[kSlots];
int g_pendingSlot = 0;
vr::EyeViews g_frameViews;          // game thread: views for the frame being kicked
vr::EyeViews g_slotViews[kSlots];   // views travelling with each stolen slot
std::atomic<int> g_execSlot{-1};    // render thread: slot whose eye 0 was captured this frame
bool g_pairThisFrame = false;
vr::EyeViews g_pairViews;

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_stolen{0}, g_submitted{0}, g_dropped{0}, g_eye0Caps{0}, g_eye1Caps{0}, g_missing{0};

// Render-thread capture state.
IDXGISwapChain* g_sc = nullptr;
ID3D11Texture2D* g_eyeTex[2] = {nullptr, nullptr};
D3D11_TEXTURE2D_DESC g_eyeDesc{};
bool g_eye0ThisFrame = false;
// D-108: which eye's command streams the render thread is executing (eye 0's run first, then eye 1's, then Present):
// 0 from Present until eye 0's last stream has executed, then 1; -1 with the eyes off
std::atomic<int> g_renderEye{-1};

// Eye screenshot hand-off (render thread -> command thread).
std::mutex g_shotMu;
std::atomic<bool> g_shotReq{false}, g_shotReady{false};
std::vector<uint8_t> g_shotPx[2];
UINT g_shotW = 0, g_shotH = 0, g_shotPitch[2] = {0, 0};
DXGI_FORMAT g_shotFmt = DXGI_FORMAT_UNKNOWN;

void Submit(uint8_t* ctx) { ((PFN_Submit)((uint8_t*)GameVa(0x1409a09c0ull)))(ctx); }

// Owner: "a one-frame flash of the startup intro when I get into 3D": eye 0 is copied mid-frame, eye 1 at Present - a
// resize in between made both textures again at eye 1, so eye 0 went out as a new, never written texture (whatever
// that video memory held: the intro from VR's start, same size). A pair goes out only when both eyes were copied into
// the textures that exist now.
uint64_t g_eyeGen = 0, g_capGen[2] = {~0ull, ~0ull};

bool EnsureEyeTex(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& bd) {
    if (g_eyeTex[0] && g_eyeDesc.Width == bd.Width && g_eyeDesc.Height == bd.Height && g_eyeDesc.Format == bd.Format)
        return true;
    for (auto& t : g_eyeTex) if (t) { t->Release(); t = nullptr; }
    D3D11_TEXTURE2D_DESC d = bd;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.SampleDesc = {1, 0};
    d.MipLevels = 1;
    d.ArraySize = 1;
    for (auto& t : g_eyeTex)
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) { Log("eyes: eye texture creation failed"); return false; }
    g_eyeDesc = d;
    ++g_eyeGen;
    Log("eyes: eye textures %ux%u fmt=%d", d.Width, d.Height, (int)d.Format);
    return true;
}

// Copy the current backbuffer into eye texture `eye` (render thread).
bool CaptureEye(IDXGISwapChain* sc, int eye) {
    if (!sc) return false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    bool ok = bd.SampleDesc.Count == 1 && EnsureEyeTex(dev, bd);
    SharpUiFinish(ctx, bb);  // D-104: this eye's HUD pasted in before the copy
    if (ok) {
        ctx->CopyResource(g_eyeTex[eye], bb);
        g_capGen[eye & 1] = g_eyeGen;
    }
    ctx->Release();
    dev->Release();
    bb->Release();
    return ok;
}

void ReadbackEyes(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    D3D11_TEXTURE2D_DESC sd = g_eyeDesc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    std::lock_guard<std::mutex> lk(g_shotMu);
    for (int e = 0; e < 2; ++e) {
        ID3D11Texture2D* st = nullptr;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) return;
        ctx->CopyResource(st, g_eyeTex[e]);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            g_shotPx[e].assign((uint8_t*)m.pData, (uint8_t*)m.pData + (size_t)m.RowPitch * sd.Height);
            g_shotPitch[e] = m.RowPitch;
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    g_shotW = sd.Width;
    g_shotH = sd.Height;
    g_shotFmt = sd.Format;
    g_shotReady = true;
}

void __fastcall hk_Submit(uint8_t* ctx) {
    const int n = g_pendingN.load();
    if (n > 0) {
        uint8_t* chunk0 = &G<uint8_t>(kCtxBase) + (size_t)G<int>(kFrameIdx) * kCtxSize;
        const uint32_t serial = G<uint32_t>(kSubmitSerial);
        if (ctx == chunk0) {
            // main path increments the serial before submitting; the alternative path does not
            if (serial == g_pendingSerial + 1 || serial == g_pendingSerial) {
                for (int i = 0; i < n; ++i) Submit(g_pendingBase + (size_t)i * kCtxSize);
                g_eye0LastStream[g_pendingSlot] = g_pendingBase + (size_t)(n - 1) * kCtxSize + 0x50;
                ++g_submitted;
            } else {
                ++g_dropped;  // stolen in a frame that never submitted: its packets may already be recycled
            }
            g_pendingN = 0;
        }
    }
    Submit(ctx);
}

void __fastcall hk_Exec(uint8_t* stream) {
    o_Exec(stream);
    for (auto& slot : g_eye0LastStream) {
        uint8_t* last = slot.load();
        if (!last || stream != last) continue;
        slot = nullptr;
        g_renderEye = 1;
        if (CaptureEye(g_sc, 0)) {
            g_execSlot = (int)(&slot - g_eye0LastStream);
            g_eye0ThisFrame = true;
            ++g_eye0Caps;
        }
        break;
    }
}

}  // namespace

bool EyesInstall() {
    static bool done = false;
    if (done) return true;
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    bool a = DetourCallSite(kSubmitMain, (void*)&hk_Submit);
    bool b = DetourCallSite(kSubmitAlt, (void*)&hk_Submit);
    bool c = DetourInstall(kExec, (void*)&hk_Exec, (void**)&o_Exec);
    done = a && b && c;
    Log("eyes: install submitMain=%d submitAlt=%d exec=%d", a, b, c);
    return done;
}

void EyesSteal() {
    if (!g_on.load() || !g_base) return;
    if (g_pendingN.load() > 0) ++g_dropped;  // previous steal never reached submission
    unsigned n = G<unsigned>(kChunkCount);
    if (n == 0 || n > (unsigned)kMaxChunks) {
        g_pendingN = 0;
        return;
    }
    g_slot = (g_slot + 1) % kSlots;
    const int fi = G<int>(kFrameIdx);
    for (unsigned i = 0; i < n; ++i)
        memcpy(g_copies[g_slot][i], &G<uint8_t>(kCtxBase) + ((size_t)fi + (size_t)i * 2) * kCtxSize, kCtxSize);
    g_pendingBase = g_copies[g_slot][0];
    g_pendingSlot = g_slot;
    g_slotViews[g_slot] = g_frameViews;
    g_pendingSerial = G<uint32_t>(kSubmitSerial);
    g_pendingN = (int)n;
    uint64_t k = ++g_stolen;
    if (k <= 3 || k % 300 == 0)
        Log("eyes: steal #%llu chunks=%u frameIdx=%d submitted=%llu dropped=%llu eye0caps=%llu eye1caps=%llu missing=%llu",
            (unsigned long long)k, n, fi, (unsigned long long)g_submitted.load(), (unsigned long long)g_dropped.load(),
            (unsigned long long)g_eye0Caps.load(), (unsigned long long)g_eye1Caps.load(),
            (unsigned long long)g_missing.load());
}

void EyesOnPresent(IDXGISwapChain* sc) {
    g_sc = sc;
    g_pairThisFrame = false;
    struct NextFrameEye0 { ~NextFrameEye0() { g_renderEye = g_on.load() ? 0 : -1; } } nextEye;  // D-108
    if (!g_on.load()) {
        g_eye0ThisFrame = false;
        return;
    }
    if (!g_eye0ThisFrame) {
        ++g_missing;
        return;
    }
    g_eye0ThisFrame = false;
    if (!CaptureEye(sc, 1)) return;
    ++g_eye1Caps;
    int slot = g_execSlot.exchange(-1);
    g_pairViews = slot >= 0 ? g_slotViews[slot] : vr::EyeViews{};
    g_pairThisFrame = true;
    if (g_shotReq.exchange(false)) {
        ID3D11Device* dev = nullptr;
        g_eyeTex[0]->GetDevice(&dev);
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        ReadbackEyes(dev, ctx);
        ctx->Release();
        dev->Release();
    }
}

void EyesEnable(bool on) {
    if (on && !EyesInstall()) {
        Log("eyes: not enabled (install failed)");
        return;
    }
    g_pendingN = 0;
    for (auto& slot : g_eye0LastStream) slot = nullptr;
    g_on = on;
    Log("eyes: %s (stolen=%llu submitted=%llu dropped=%llu eye0caps=%llu eye1caps=%llu missing=%llu)",
        on ? "on" : "off", (unsigned long long)g_stolen.load(), (unsigned long long)g_submitted.load(),
        (unsigned long long)g_dropped.load(), (unsigned long long)g_eye0Caps.load(),
        (unsigned long long)g_eye1Caps.load(), (unsigned long long)g_missing.load());
}

bool EyesEnabled() { return g_on.load(); }
int EyesRenderEye() { return g_renderEye.load(std::memory_order_relaxed); }

void EyesSetFrameViews(const vr::EyeViews& v) { g_frameViews = v; }

bool EyesPair(ID3D11Texture2D** eye0, ID3D11Texture2D** eye1, vr::EyeViews* views) {
    if (!g_pairThisFrame || !g_eyeTex[0] || !g_eyeTex[1]) return false;
    if (g_capGen[0] != g_eyeGen || g_capGen[1] != g_eyeGen) {  // an eye copied into textures made again since
        static int logs = 0;
        if (logs++ < 10) Log("eyes: pair skipped - the eye textures were made again between the two eyes' copies");
        return false;
    }
    *eye0 = g_eyeTex[0];
    *eye1 = g_eyeTex[1];
    *views = g_pairViews;
    return true;
}

void EyesShot(const char* name, int div) {
    g_shotReady = false;
    g_shotReq = true;
    for (int t = 0; !g_shotReady && t < 3000; t += 10) Sleep(10);
    if (!g_shotReady.exchange(false)) {
        g_shotReq = false;
        Log("eyeshot: no eye pair captured (eyes on? double kick running?)");
        return;
    }
    std::lock_guard<std::mutex> lk(g_shotMu);
    for (int e = 0; e < 2; ++e) {
        std::string n = std::string(name) + (e ? "_eye1" : "_eye0");
        diag::WritePng(n.c_str(), g_shotPx[e].data(), g_shotW, g_shotH, g_shotPitch[e], (int)g_shotFmt, div);
    }
}

}  // namespace gowvr::stereo
