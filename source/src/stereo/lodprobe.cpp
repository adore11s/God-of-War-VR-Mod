// D-108 LOD probe / scale (owner: "some assets' resolution sometimes doesn't improve - the low poly look").
// sub_14046f100(lodDesc, sphere, camPos, scale, &out) picks an object's LOD: metric = (|camPos - centre| - radius) *
// scale (* desc+0x5c), curved by the Model Quality exponent and compared with the LOD distance thresholds. Callers:
// the per-view LOD/visibility job (sub_14046e180: camPos / scale from the job's view entries +0x10 / +0x30) and
// sub_140470xxx (scale 1.0). "lodprobe <n>" logs what it is called with (camera positions vs the views, scale range,
// LOD indices); gowvr.ini lod_scale / "lodscale <f>" multiplies the scale (< 1 = finer LODs farther out).
#include "lodprobe.h"
#include "eyes.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/detour.h"
#include "../core/log.h"
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gowvr::stereo {
namespace {

using PFN_Lod = int(__fastcall*)(int64_t desc, const float* sphere, const float* cam, float scale, float* out);
PFN_Lod o_Lod = nullptr;
std::atomic<float> g_lodScale{-1.f};
std::atomic<int> g_probeLeft{0};
std::mutex g_mu;
struct Cam { float p[3]; float scale; uint64_t n; };
Cam g_cams[8];
int g_nCams = 0;
uint64_t g_calls = 0, g_lodHist[8] = {};
float g_minS = 1e30f, g_maxS = -1e30f;

int __fastcall hk_Lod(int64_t desc, const float* sphere, const float* cam, float scale, float* out) {
    float s = g_lodScale.load(std::memory_order_relaxed);
    if (s < 0.f) {
        s = config::GetFloat("lod_scale", 1.f);
        g_lodScale = s;
    }
    const int lod = o_Lod(desc, sphere, cam, s > 0.f ? scale * s : scale, out);
    if (g_probeLeft.load(std::memory_order_relaxed) > 0 && cam) {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_calls;
        if (lod >= 0 && lod < 8) ++g_lodHist[lod];
        g_minS = fminf(g_minS, scale);
        g_maxS = fmaxf(g_maxS, scale);
        int i = 0;
        while (i < g_nCams && (fabsf(g_cams[i].p[0] - cam[0]) > 0.05f || fabsf(g_cams[i].p[1] - cam[1]) > 0.05f ||
                               fabsf(g_cams[i].p[2] - cam[2]) > 0.05f || g_cams[i].scale != scale))
            ++i;
        if (i == g_nCams && i < 8) g_cams[g_nCams++] = {{cam[0], cam[1], cam[2]}, scale, 0};
        if (i < 8) ++g_cams[i].n;
        if (--g_probeLeft == 0) {
            Log("lodprobe: %llu calls, scale %.4f..%.4f, LOD picked 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu (lod_scale %.2f)",
                (unsigned long long)g_calls, g_minS, g_maxS, (unsigned long long)g_lodHist[0],
                (unsigned long long)g_lodHist[1], (unsigned long long)g_lodHist[2], (unsigned long long)g_lodHist[3],
                (unsigned long long)g_lodHist[4],
                (unsigned long long)(g_lodHist[5] + g_lodHist[6] + g_lodHist[7]), s);
            for (int k = 0; k < g_nCams; ++k)
                Log("lodprobe:  camera (%.3f %.3f %.3f) scale %.4f - %llu calls", g_cams[k].p[0], g_cams[k].p[1],
                    g_cams[k].p[2], g_cams[k].scale, (unsigned long long)g_cams[k].n);
        }
    }
    return lod;
}

const HookSite kLodSel = {"LodSelect", 0x14046f100, 5,  // mov [rsp+18h], rbx (5)
                          {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x63, 0x69, 0x6c}};

// D-296 (owner: "some textures are low and get good quality after a while"; a player's suggestion: "for low quality models
// it might be same-frame stereo - Ghost of Tsushima needed all these memory calls remapped: alternate-eye rendering had
// working LOD, same-frame didn't"). STATIC (decompile + GoW.exe disassembly): the per-view LOD / visibility pass
// (sub_14046ef20, its twin sub_140484280) appends every visible model part whose data is not streamed in yet as
// {owner, part} to a fixed list 0x144fc5e00 (4096 x 16 bytes) through `lock xadd` on 0x142d2f1d4 - no check for a part
// already listed, and past 4096 the entry is dropped (the count keeps counting). The model server's update
// (renModelServerContext::vfunc_1, and sub_14053a690) takes at most 4096, starts their streams (the mesh LOD data and
// the textures that come with it) and resets the count. In VR the pass runs once per eye, through frustums ~1.5 - 2x
// wider than the flat game's: the same parts are listed twice, the list fills up and the parts listed last wait for
// later frames - the low-detail models / textures that sharpen "after a while" (a single kick per frame, as in AFR,
// lists half). The list is moved to a mod-owned one stream_list_scale (4) x as big: the six instructions that address
// it (four writers, two readers - each `lea r64, [rip + disp32]`) get the new array, the four `cmp eax, 0x1000` and the
// two readers' `mov ecx, 0x1000` limits the new size. Known control: PassLuminance::vfunc_7's `lea rsi` at 0x140530c12
// also names 0x144fc5e00, as the END of the array before it (luminance shaders) - not touched. Patched at DLL load
// before any game thread exists; every site's bytes (and each lea's target) are checked first - one mismatch and
// nothing is patched. The texture requests of lights / cubemaps / effects / decals (0x14123c0d0: 11264 x 8 bytes,
// count at +0x16000, taken once a frame just before the kick at 0x140407868) are only measured (StreamReqNote).
// gowvr.ini stream_list_scale (4; 1 = the game's list)
constexpr uint64_t kStreamReqCount = 0x142d2f1d4, kStreamReqArray = 0x144fc5e00, kTexReqBuf = 0x14123c0d0;
constexpr uint32_t kStreamReqGameCap = 0x1000, kTexReqCap = 0x2c00;
struct CodeSite { uint64_t va; int kind; uint8_t op[3]; };  // kind 0: lea r64, [rip + disp32] (op = its 3 bytes);
                                                            // kind 1: op[0] imm32 (cmp eax / mov ecx, 0x1000)
const CodeSite kStreamSites[] = {
    {0x1400e5c90, 1, {0x3d, 0, 0}}, {0x1400e5c97, 0, {0x4c, 0x8d, 0x05}},     // writer sub_1400e5be0
    {0x14046ef4f, 0, {0x48, 0x8d, 0x2d}}, {0x14046f01e, 1, {0x3d, 0, 0}},     // writer sub_14046ef20 (per view)
    {0x1404842b0, 0, {0x4c, 0x8d, 0x35}}, {0x14048437e, 1, {0x3d, 0, 0}},     // writer sub_140484280 (per view)
    {0x14094b75e, 1, {0x3d, 0, 0}}, {0x14094b765, 0, {0x48, 0x8d, 0x15}},     // writer sub_14094b370
    {0x1404332f2, 1, {0xb9, 0, 0}}, {0x14043330c, 0, {0x48, 0x8d, 0x1d}},     // reader renModelServerContext::vfunc_1
    {0x14053a6ad, 1, {0xb9, 0, 0}}, {0x14053a6cc, 0, {0x48, 0x8d, 0x1d}},     // reader sub_14053a690
};
uint32_t g_streamCap = kStreamReqGameCap;  // the list's size now

// A block of `bytes` within reach of a rip-relative displacement from every site (after the image, else before it)
uint8_t* AllocNearImage(size_t bytes) {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    constexpr uintptr_t kGran = 0x10000;
    for (uintptr_t a = (end + kGran - 1) & ~(kGran - 1); a < end + 0x30000000ull; a += kGran)
        if (void* p = VirtualAlloc((void*)a, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return (uint8_t*)p;
    for (uintptr_t a = (base & ~(kGran - 1)) - bytes - kGran; a > base - 0x30000000ull && a < base; a -= kGran)
        if (void* p = VirtualAlloc((void*)(a & ~(kGran - 1)), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return (uint8_t*)p;
    return nullptr;
}

// the frame's peak demand (game thread)
uint32_t g_frameModel = 0, g_frameTex = 0, g_peakModel = 0, g_peakTex = 0;
uint64_t g_frames = 0, g_overGame = 0, g_overOurs = 0, g_texFull = 0;
ULONGLONG g_noteAt = 0;

// D-297 (owner after D-296: "you didn't fix the DLSS issue - at the end of the log I saw low quality textures"; the D-296
// line showed the model list over 4096 in 1 frame of the session - not the cause). STATIC: model LOD packs
// (Lodpack_StreamGroup 0x14042d330: a pack starts loading only when `size <= free pages x 256 KB`) and streamed texture
// mips (sub_1404dfde0) take 256 KB pages from ONE pool - the "Texture" heap that the memory setup sub_14040a610 reserves
// (VirtualAlloc MEM_RESERVE, committed as used) with config[3] MB of the table *0x14105a358 = 0x14105a338 {1368 root,
// 200 surfaces, 8 streaming, 4096 texture, ...}, clamped to the video memory - 512 MB; its free-page count is
// *0x141239018 + 0x14 (total +0x8). At Texture Quality Ultra the streamer's budget is the whole pool (sub_140442d80).
// When the pool is empty, a model's full LOD waits (the closest RESIDENT LOD is drawn: sub_14046eb90 -> sub_140484840 -
// "PS2-looking") and texture mips wait until something else is evicted - detail that arrives "after a while". The VR
// view (~133 x 106 deg against ~75 x 45 flat) keeps far more objects near enough for their full LODs and textures, and a
// VR restart / pause-menu resize starts the pool over. Now the memory setup is detoured: before it runs, config[3]
// grows in 4096 MB steps up to the card's dedicated video memory - texture_heap_reserve_mb (6144), at most
// texture_heap_max_mb (8192), only with texture_heap_min_ram_mb (24000) of RAM; the game's own clamp still applies.
// Every 2 MB the setup reserves is also taken from the game's memory allowance 0x1e94 x 2 MB, which the level data
// heaps (sub_1404b7dc0: "Could not allocate %zu bytes for '%s', got %zu"), the wad budget (sub_140677b30) and Bink
// (sub_1407b35a2, a 32-bit shift) are measured against - the three `mov eax, 0x1e94` get the same steps added, so they
// see exactly the allowance they had (a 4096 MB step also leaves the 32-bit value unchanged). Any site not as recorded:
// the game's heap. Every minute the log has the pool's size and its lowest free amount (StreamReqNote).
// gowvr.ini texture_heap_mb: 0 = automatic (as above), N = up to N MB in 4096 MB steps, 4096 = the game's
constexpr uint64_t kMemCfgPtr = 0x14105a358, kTexHeapStats = 0x141239018;
// mov eax, 0x1e94 (b8) - and the setup's own copy into 0x141237d98 (mov ecx, b9; no reader found, kept consistent)
constexpr uint64_t kAllowanceSites[] = {0x1404b7e86, 0x140677ba7, 0x1407b35a2, 0x14040ac62};
// tools/re/hookgen.py 0x14040a610: mov rax, rsp; push rdi; push r12 (6 bytes); no arguments (rcx / rdx written before
// read; the two callers pass none)
constexpr HookSite kMemInit{"MemInit", 0x14040a610, 6,
    {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xc0}};
using PFN_MemInit = void(__fastcall*)();
PFN_MemInit o_MemInit = nullptr;
uint32_t g_heapMinFree = 0xffffffffu, g_heapTotal = 0;  // pages (256 KB), the minute's lowest free count

// D-298 (owner after D-297: "still not fixed - look at the last 15 seconds of the log, where the low quality model is";
// the D-297 line: the heap never below 4 GB free - not the limit either; the last 15 s: a triptych shrine opened
// (envTriptychOpen, 1106 - 1109 s), then SteamVR ended and restarted the session). The log never said WHICH stage held the
// detail back, so every 3 s while anything waits (every 30 s otherwise) a "detail:" line now does:
//  - textures: of the streamed textures in use (0x1412390d0 + 0xcb10, 0x2c00 x 0x2c8; refs +0x23e), those wanting more
//    mip levels (min(+0x234 wanted, +0x1de count)) than are loaded (+0x1e0 - +0x1df + 1) and by how many levels; the
//    textures being loaded now (+0x7b8330, at most 16); the streamer idle flag (+0xcb05)
//  - models: the stream manager's (*0x14123bf88) pending / active / free request lists (+0x5ab38 / +0x5ab48 / +0x5ab28)
//  - the game's own load-time reduction: sub_14067fe20 (the load / fade-in sequence) sets 0x1412962fc = 2 (every texture
//    asked 2 mip levels lower, sub_140462120) with 0x142d37907 = 1 when a load starts and clears both only on that path,
//    when streaming is idle or after 25 s (0x140ef6d5c = 25000 ms); other paths (JumpToWad, ui sub_14067e930,
//    sub_14067e4f0) end a load by clearing the load flags 0x1422e36eb / e9 without it. Guard: the reduction still on
//    with neither load flag set for 3 s is cleared the game's way (both to 0) and logged. gowvr.ini load_mip_guard 0 = off
constexpr uint64_t kStreamer = 0x1412390d0, kStreamMgr = 0x14123bf88, kMipReduce = 0x1412962fc, kLoadMode = 0x142d37907,
                   kLoading1 = 0x1422e36eb, kLoading2 = 0x1422e36e9;
ULONGLONG g_detailAt = 0, g_reduceSince = 0, g_strayReduceSince = 0;
int g_reduceWas = 0;

int ListLen(const uint8_t* head) {  // a circular list {next, prev} with its head inside the manager (capped walk)
    const uint8_t* n = *(const uint8_t* const*)head;
    int k = 0;
    while (n && n != head && k < 5000) { n = *(const uint8_t* const*)n; ++k; }
    return k;
}

// D-299 (owner, D-298 build: "it's better, some are better quality, but still some parts are low quality"; "at the end
// there is the good and a little bit of bad quality textures"): the D-298 lines all session - and at the end - say
// "textures waiting 0 (0 mip levels short)", the model streams idle, 4.4 - 6 GB of the heap free: everything the game
// ASKS for is loaded. What it asks for is the limit: sub_140462120 turns each object's needed mip levels (+0x78, from the
// view it is drawn in) into requests - in VR that view spreads a ~133 deg frustum over the 1536-pixel DLSS input, about
// 1.5 - 2 levels coarser per texel than the flat game's 75 deg over 1707 pixels. Those requests minus the game's
// own global 0x1412962fc (its only reader, 0x140462184; the load sequence writes 2 / 0) - a NEGATIVE value asks for
// more. In VR, outside the game's loads (load mode 0, no load flag), it is -vr_texture_detail: 2 mip levels more with
// the D-297 heap of 8192 MB or more, 1 otherwise (each level up is capped by the texture's own size, sub_140441e30);
// the game's own 2 during a load is left alone, VR off puts back 0. gowvr.ini vr_texture_detail (-1 auto, 0 = off, 1..3)
// D-304 (owner: "remove the LOD scaler in VR settings and make level 3 (highest) the default"): auto = 3 with the D-297
// heap of 8192 MB or more (owner's logs at 3: 3.6 - 5 GB of it free), 2 with the game's 4096 MB
int g_detailBiasLogged = -99;
int VrTextureDetail() {
    const float v = config::GetFloat("vr_texture_detail", -1.f);
    if (v >= 0.f) return (int)fminf(v, 3.f);
    const uint8_t* hs = *(const uint8_t* const*)GameVa(kTexHeapStats);
    const uint32_t heapMb = hs ? *(const uint32_t*)(hs + 0x8) / 4 : 0;
    return heapMb >= 8192 ? 3 : 2;
}

// D-302 (owner: "loaded the save where the enemy was, it was finally good, but then another enemy is low quality again";
// two owner logs: the "detail:" textures in use rose to exactly 8192 and stayed there - in the second at 89.9 s, the good
// enemy's health bar at 65.9 s, the blurry one's at 102.9 s). STATIC: a texture of a loaded wad streams only once
// sub_140443390(-, texture, a, b) gives it a slot of the streamer's table (0x2c00 renStreamedTex at +0xcb10, free-slot
// stack count +0x7b8368, used list +0x7b2b10 / count +0x7b2b18); when it returns 0 both callers (the wad post-load,
// sub_140419d60 / sub_1404b3de0) clear the texture's streamable flag (+0xc bit 0): it keeps its small resident mips until
// its wad loads again. By the code that fails only with no free slot and no unreferenced slot idle 3 frames (+0x230 last
// frame, +0x236 load pending); where 8192 comes from is not found statically. Now every call is counted and each refusal
// logged with the table's state; the "detail:" line adds the slots and the texture records (0x1422a9870, 0x5000 x 0x298:
// +0x170 its slot) that are streamable but have no slot.
constexpr uint64_t kTexRecords = 0x1422a9870, kStreamFrame = 0x1422ae7d8;
constexpr uint32_t kTexRecordCount = 0x5000;
using PFN_TexRegister = uint64_t(__fastcall*)(void* a1, uint8_t* tex, char a3, char a4);
PFN_TexRegister o_TexRegister = nullptr;
std::atomic<uint64_t> g_texRegCalls{0}, g_texRegRefused{0};

uint64_t __fastcall hk_TexRegister(void* a1, uint8_t* tex, char a3, char a4) {
    const uint64_t r = o_TexRegister(a1, tex, a3, a4);
    ++g_texRegCalls;
    if ((uint8_t)r) return r;
    const uint64_t n = ++g_texRegRefused;
    if (n > 10 && n % 100) return r;
    const uint8_t* st = *(const uint8_t* const*)GameVa(kStreamer);
    if (!st) return r;
    const uint16_t freeSlots = *(const volatile uint16_t*)(st + 0x7b8368);
    const uint16_t used = *(const volatile uint16_t*)(st + 0x7b2b18);
    const uint16_t* list = *(const uint16_t* const*)(st + 0x7b2b10);
    const int frame = *(const volatile int*)GameVa(kStreamFrame);
    uint32_t inUse = 0, unref = 0, pending = 0, idlest = 0;
    for (uint32_t i = 0; list && i < used && i < 0x2c00; ++i) {
        const uint8_t* t = st + 0xcb10 + (size_t)(list[i] % 0x2c00) * 0x2c8;
        if (t[0x23e]) { ++inUse; continue; }
        ++unref;
        if (t[0x236]) ++pending;
        const uint32_t age = (uint32_t)abs(frame - *(const int*)(t + 0x230));
        if (age > idlest) idlest = age;
    }
    Log("lod: texture registration REFUSED #%llu (of %llu) - slots free %u, used %u (in use %u, unreferenced %u: load "
        "pending %u, idlest %u frames) - that texture keeps its small mips until its area loads again (D-302)",
        (unsigned long long)n, (unsigned long long)g_texRegCalls.load(), freeSlots, used, inUse, unref, pending, idlest);
    return r;
}

const HookSite kTexRegister = {"TexRegister", 0x140443390, 5,  // push rbp (2), push rdi (1), push r14 (2)
                               {0x40, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x66, 0x6e}};

void DetailNote(ULONGLONG now) {
    int reduce = *(const volatile int*)GameVa(kMipReduce);
    const uint8_t mode = *(const volatile uint8_t*)GameVa(kLoadMode);
    const bool loading = *(const volatile uint8_t*)GameVa(kLoading1) || *(const volatile uint8_t*)GameVa(kLoading2);
    if (reduce <= 0 && !mode && !loading) {  // D-299: our own (negative) value only outside the game's loads
        const int want = EyesEnabled() ? -VrTextureDetail() : 0;
        if (reduce != want) {
            *(volatile int*)GameVa(kMipReduce) = want;
            reduce = want;
        }
        if (want != g_detailBiasLogged) {
            g_detailBiasLogged = want;
            if (want) Log("lod: texture detail in VR - every texture asked %d mip level(s) finer (D-299, vr_texture_detail)", -want);
            else Log("lod: texture detail - the game's own (D-299)");
        }
    }
    if (reduce < 0) reduce = 0;  // (below: the game's own load-time reduction only)
    if (reduce != g_reduceWas) {
        if (reduce) {
            g_reduceSince = now;
            Log("lod: the game's load-time texture reduction ON (%d mip levels lower; load mode %d, loading %d) (D-298)",
                reduce, mode, loading ? 1 : 0);
        } else {
            Log("lod: the game's load-time texture reduction off after %.1f s (loading %d) (D-298)",
                g_reduceSince ? (now - g_reduceSince) / 1000.0 : 0.0, loading ? 1 : 0);
        }
        g_reduceWas = reduce;
    }
    static const bool guard = config::GetFloat("load_mip_guard", 1.f) > 0.5f;
    if (reduce && !loading) {
        if (!g_strayReduceSince) g_strayReduceSince = now;
        if (guard && now - g_strayReduceSince > 3000) {
            *(volatile int*)GameVa(kMipReduce) = 0;
            *(volatile uint8_t*)GameVa(kLoadMode) = 0;
            g_strayReduceSince = 0;
            Log("lod: the load-time texture reduction was still on 3 s after the load ended - cleared (D-298)");
        }
    } else {
        g_strayReduceSince = 0;
    }
    const uint8_t* st = *(const uint8_t* const*)GameVa(kStreamer);
    if (!st) return;
    uint32_t inUse = 0, waiting = 0, missing = 0;
    for (uint32_t i = 0; i < 0x2c00; ++i) {
        const uint8_t* t = st + 0xcb10 + (size_t)i * 0x2c8;
        if (!t[0x23e]) continue;
        ++inUse;
        const int want = t[0x234] < t[0x1de] ? t[0x234] : t[0x1de];
        const int have = (int)t[0x1e0] - (int)t[0x1df] + 1;
        if (have < want) { ++waiting; missing += (uint32_t)(want - have); }
    }
    const uint16_t inFlight = *(const volatile uint16_t*)(st + 0x7b8330);
    const uint8_t idle = st[0xcb05];
    int pending = -1, active = -1, freeReq = -1;
    if (const uint8_t* mgr = *(const uint8_t* const*)GameVa(kStreamMgr)) {
        pending = ListLen(mgr + 0x5ab38);
        active = ListLen(mgr + 0x5ab48);
        freeReq = ListLen(mgr + 0x5ab28);
    }
    const bool busy = waiting || inFlight || pending > 0 || active > 0 || reduce;
    if (now - g_detailAt < (busy ? 3000u : 30000u)) return;
    g_detailAt = now;
    const uint8_t* hs = *(const uint8_t* const*)GameVa(kTexHeapStats);
    uint32_t withSlot = 0, noSlot = 0;  // D-302: streamable texture records with / without a slot
    if (const uint8_t* rec = *(const uint8_t* const*)GameVa(kTexRecords)) {
        for (uint32_t i = 0; i < kTexRecordCount; ++i) {
            const uint8_t* r = rec + (size_t)i * 0x298;
            if (*(const uint64_t*)(r + 0x170)) ++withSlot;
            else if (r[0xc] & 1) ++noSlot;
        }
    }
    Log("detail: textures waiting %u of %u in use (%u mip levels short), loading %u, streamer idle %u | slots free %u used %u, "
        "textures with a slot %u, streamable without one %u, registrations %llu refused %llu (D-302) | model streams pending "
        "%d active %d free %d | game load reduction %d (load mode %u, loading %d) | heap free %d MB (D-298)", waiting, inUse,
        missing, inFlight, idle, *(const volatile uint16_t*)(st + 0x7b8368), *(const volatile uint16_t*)(st + 0x7b2b18),
        withSlot, noSlot, (unsigned long long)g_texRegCalls.load(), (unsigned long long)g_texRegRefused.load(), pending,
        active, freeReq, reduce, mode, loading ? 1 : 0, hs ? *(const int32_t*)(hs + 0x14) / 4 : -1);
}

uint64_t DedicatedVideoMb() {  // the largest dedicated video memory of the hardware adapters
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return 0;
    uint64_t best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best)
            best = d.DedicatedVideoMemory;
        a->Release();
    }
    f->Release();
    return best >> 20;
}

void __fastcall hk_MemInit() {
    int* cfg = *(int**)GameVa(kMemCfgPtr);
    const int game = cfg ? cfg[3] : 0;
    const float want = config::GetFloat("texture_heap_mb", 0.f);
    int mb = game;
    uint64_t vram = 0, ram = 0;
    if (cfg && game > 0) {
        int limit;
        if (want >= 1.f) {
            limit = (int)fminf(want, 32768.f);
        } else {
            vram = DedicatedVideoMb();
            MEMORYSTATUSEX ms{sizeof(ms)};
            if (GlobalMemoryStatusEx(&ms)) ram = ms.ullTotalPhys >> 20;
            const int reserve = (int)config::GetFloat("texture_heap_reserve_mb", 6144.f);
            const int maxMb = (int)config::GetFloat("texture_heap_max_mb", 8192.f);
            const int64_t fit = (int64_t)vram - reserve;
            limit = (int)(fit < (int64_t)maxMb ? fit : (int64_t)maxMb);
            if ((double)ram < config::GetFloat("texture_heap_min_ram_mb", 24000.f)) limit = game;
        }
        const int steps = limit > game ? (limit - game) / 4096 : 0;  // whole 4096 MB steps above the game's
        uint8_t* at[sizeof(kAllowanceSites) / sizeof(kAllowanceSites[0])] = {};
        bool ok = steps > 0;
        for (size_t i = 0; ok && i < sizeof(kAllowanceSites) / sizeof(kAllowanceSites[0]); ++i) {
            at[i] = (uint8_t*)GameVa(kAllowanceSites[i]);
            static const uint8_t kImm[4] = {0x94, 0x1e, 0x00, 0x00};
            ok = at[i] && (at[i][0] == 0xb8 || at[i][0] == 0xb9) && memcmp(at[i] + 1, kImm, 4) == 0;
            if (!ok) Log("lod: memory allowance site 0x%llx is not as recorded - the game's heap (D-297)",
                         (unsigned long long)kAllowanceSites[i]);
        }
        if (ok) {
            const uint32_t allowance = 0x1e94u + (uint32_t)steps * 2048u;  // 2 MB units
            for (uint8_t* p : at) {
                DWORD old = 0;
                VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old);
                memcpy(p + 1, &allowance, 4);
                VirtualProtect(p, 8, old, &old);
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            mb = game + steps * 4096;
            cfg[3] = mb;
        }
    }
    o_MemInit();
    const uint8_t* st = *(uint8_t* const*)GameVa(kTexHeapStats);
    Log("lod: model + texture streaming heap %d MB (the game's %d MB; video memory %llu MB, RAM %llu MB; set %d) - %u pages "
        "of 256 KB (D-297)", cfg ? cfg[3] : 0, game, (unsigned long long)vram, (unsigned long long)ram, mb,
        st ? *(const uint32_t*)(st + 0x8) : 0);
}

}  // namespace

// D-303 (owner, D-302 build: "enemies at the start good quality, then a new enemy showed up at the very end, low quality";
// owner log 19:48: registrations refused 0 all session, no streamable texture without a slot after 100.8 s, waiting 0,
// heap 4.7 GB free - the late enemy's textures had their slots since its area loaded, so the level ASKED for them was
// the limit). STATIC: what is asked for a drawn object's textures is its texture owners' need bytes +0x78 (five texture
// groups, a 6th byte = forced), max-merged with `lock cmpxchg` by the view passes (sub_140485590: characters through
// sub_140484f70) and turned into requests and cleared by sub_140462120 (+ our D-299 levels, capped at each texture's own
// size). D-303b (owner, first D-303 build: "again at the end a low quality new enemy"; log: not one "lod: character" line
// - that build took ro +0x30 for the owner, but it points into the model instance at +0x120, sub_140470b70 /
// sub_140484b40). The game's own path (pickups sub_14094b370): a render object's meshes (+0x1f0 count, +0x1e8 list;
// material variants through sub_1404839d0 with sub_1404bf240's context) each have a texture owner at +0x30 (+0x54 texture
// count, +0x58 texture records); sub_1404836f0(mesh, 0) max-merges its five needs to 16 (forced byte 0; 1 also flags the
// slots) and sub_1404837d0(ro, flag) does that for every mesh (game callers sub_14018a151 / sub_140847d80; the spin lock
// 0x1425297e8 of sub_1404bf240 is released before it returns). The skinned draw sub_140470b70(ro, ..) builds a
// character's palettes (fp/body.cpp hk_PaletteBuild, a2 = ro). Now in VR, once a frame per drawn character: its meshes and
// owners are checked the way the game reads them (+0x30 set, every texture a record of the pool 0x1422a9870, 0x5000 x
// 0x298; SEH-guarded), then sub_1404837d0(ro, 0): its textures are asked at full size every frame it is drawn and age out
// the game's way when not. The first 40 characters (then every 200th) are logged with their textures below full size,
// and again 10 s later; the first 5 render objects that fail the check are logged with why.
// gowvr.ini vr_character_textures (1; 0 = off)
namespace {
constexpr uint64_t kModelFullNeed = 0x1404837d0;
using PFN_ModelFullNeed = void(__fastcall*)(void* ro, uint8_t slotFlag);
// >0: a render object whose meshes' owners hold that many textures (stats: all / below full size / no slot);
// <0: why not (-1 unreadable, -2 mesh count, -3 no mesh list / model instance, -4 a texture list not of records,
// -5 no texture)
int CharacterSeh(const uint8_t* ro, const uint8_t* pool, uint32_t* stats) {
    __try {
        const int meshes = *(const int*)(ro + 0x1f0);
        if (meshes < 1 || meshes > 256) return -2;
        const uint8_t* const* list = *(const uint8_t* const* const*)(ro + 0x1e8);
        if (!list || !*(const void* const*)(ro + 0x30)) return -3;
        const uintptr_t lo = (uintptr_t)pool, hi = lo + (size_t)kTexRecordCount * 0x298;
        uint32_t n = 0, b = 0, ns = 0;
        for (int m = 0; m < meshes; ++m) {
            const uint8_t* mesh = list[m];
            if (!mesh) continue;
            const uint8_t* owner = *(const uint8_t* const*)(mesh + 0x30);
            if (!owner) continue;
            const int count = *(const int*)(owner + 0x54);
            if (count < 0 || count > 1024) return -4;
            const uint8_t* const* tex = *(const uint8_t* const* const*)(owner + 0x58);
            if (count && !tex) return -4;
            for (int t = 0; t < count; ++t) {
                const uint8_t* r = tex[t];
                if (!r) continue;
                if ((uintptr_t)r < lo || (uintptr_t)r >= hi || ((uintptr_t)r - lo) % 0x298) return -4;
                ++n;
                if (!stats) continue;
                const uint8_t* s = *(const uint8_t* const*)(r + 0x170);
                if (!s) ++ns;
                else if ((int)s[0x1e0] - (int)s[0x1df] + 1 < (int)s[0x1de]) ++b;
            }
        }
        if (!n) return -5;
        if (stats) { stats[0] = n; stats[1] = b; stats[2] = ns; }
        return (int)n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// D-303c (owner log 20:24, D-303b: "now it worked"; the same render object went from 22 textures below full size to 0):
// a 128-entry table was far too small - more skinned objects are drawn - so entries were pushed out every frame: the
// "new" count reached 1,034,000 (~5000 log lines), most objects were raised on every draw instead of once a frame, and no
// "10 s later" line came. Now 4096 entries by pointer hash; the first 40 objects are logged (and 10 s later), then a line
// a minute.
struct SeenChar { const void* ro; ULONGLONG at; int frame; bool rechecked, logged; };
constexpr size_t kSeenChars = 4096;
SeenChar g_seenChar[kSeenChars] = {};
SRWLOCK g_seenLock = SRWLOCK_INIT;
std::atomic<uint64_t> g_charNew{0}, g_charRaisedMin{0}, g_charNewMin{0};
std::atomic<int> g_charRejected{0};
std::atomic<ULONGLONG> g_charMinuteAt{0};
std::atomic<ULONGLONG> g_charCfgAt{0};
std::atomic<bool> g_charOn{true};
}  // namespace

void CharacterTextures(void* renderObject) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_charCfgAt.load(std::memory_order_relaxed) > 1000) {
        g_charCfgAt = now;
        g_charOn = config::GetFloat("vr_character_textures", 1.f) > 0.5f;
    }
    if (!renderObject || !g_charOn.load(std::memory_order_relaxed) || !EyesEnabled()) return;
    const uint8_t* pool = *(const uint8_t* const*)GameVa(kTexRecords);
    const auto fullNeed = (PFN_ModelFullNeed)GameVa(kModelFullNeed);
    if (!pool || !fullNeed) return;
    const int frame = *(const volatile int*)GameVa(kStreamFrame);
    int logKind = 0;  // 1 first seen (logged), 2 ten seconds later (logged)
    bool isNew = false;
    const size_t slot = (size_t)(((uintptr_t)renderObject >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kSeenChars - 1);
    AcquireSRWLockExclusive(&g_seenLock);
    SeenChar& c = g_seenChar[slot];
    if (c.ro != renderObject) {
        const bool logIt = g_charNew.load(std::memory_order_relaxed) < 40;
        c = {renderObject, now, frame - 1, false, logIt};
        isNew = true;
        if (logIt) logKind = 1;
    }
    const bool due = c.frame != frame;  // once a frame per character (the draw runs per view / eye)
    if (due) {
        c.frame = frame;
        if (!isNew && c.logged && !c.rechecked && now - c.at >= 10000) {
            c.rechecked = true;
            logKind = 2;
        }
    }
    ReleaseSRWLockExclusive(&g_seenLock);
    if (!due) return;
    uint32_t st[3] = {};
    const int ok = CharacterSeh((const uint8_t*)renderObject, pool, logKind ? st : nullptr);
    if (ok <= 0) {
        if (isNew && g_charRejected.fetch_add(1) < 5)
            Log("lod: character check failed (%d) for render object %p - not touched (D-303)", ok, renderObject);
        return;
    }
    fullNeed(renderObject, 0);
    ++g_charRaisedMin;
    const uint64_t k = isNew ? ++g_charNew : 0;
    if (isNew) ++g_charNewMin;
    if (logKind == 1)
        Log("lod: character #%llu %p - %u textures, %u below full size, %u not streamed - asked at full size while drawn "
            "(D-303)", (unsigned long long)k, renderObject, st[0], st[1], st[2]);
    else if (logKind == 2)
        Log("lod: character %p 10 s later - %u textures, %u below full size, %u not streamed (D-303)", renderObject, st[0],
            st[1], st[2]);
    ULONGLONG m = g_charMinuteAt.load();
    if (!m) g_charMinuteAt.compare_exchange_strong(m, now);
    else if (now - m >= 60000 && g_charMinuteAt.compare_exchange_strong(m, now))
        Log("lod: characters at full texture detail, last minute: %llu raised (once a frame each), %llu new objects (D-303)",
            (unsigned long long)g_charRaisedMin.exchange(0), (unsigned long long)g_charNewMin.exchange(0));
}

int VrTextureDetailLevel() { return VrTextureDetail(); }
bool VrTextureDetailAuto() { return config::GetFloat("vr_texture_detail", -1.f) < 0.f; }

void InstallTextureHeap() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool ok = DetourInstall(kMemInit, (void*)&hk_MemInit, (void**)&o_MemInit);
    Log("lod: streaming heap sizing %s (D-297)", ok ? "installed" : "NOT installed - the game's 4096 MB");
}

void InstallStreamLists(bool noThreads) {
    static bool done = false;
    if (done) return;
    done = true;
    const float scale = fminf(config::GetFloat("stream_list_scale", 4.f), 16.f);
    if (scale < 1.5f) { Log("lod: model streaming list - the game's (4096, stream_list_scale %.0f, D-296)", scale); return; }
    if (!noThreads) { Log("lod: model streaming list NOT enlarged - not loaded with the game (D-296)"); return; }
    uint8_t* arr = (uint8_t*)GameVa(kStreamReqArray);
    uint8_t* at[sizeof(kStreamSites) / sizeof(kStreamSites[0])] = {};
    bool ok = arr != nullptr;
    for (size_t i = 0; ok && i < sizeof(kStreamSites) / sizeof(kStreamSites[0]); ++i) {
        const CodeSite& s = kStreamSites[i];
        uint8_t* p = (uint8_t*)GameVa(s.va);
        if (!p) { ok = false; break; }
        int32_t v;
        memcpy(&v, p + (s.kind == 0 ? 3 : 1), 4);
        ok = s.kind == 0 ? memcmp(p, s.op, 3) == 0 && p + 7 + v == arr : p[0] == s.op[0] && (uint32_t)v == kStreamReqGameCap;
        if (!ok) Log("lod: model streaming list - site 0x%llx is not as recorded (D-296)", (unsigned long long)s.va);
        at[i] = p;
    }
    const uint32_t cap = (uint32_t)scale * kStreamReqGameCap;
    uint8_t* big = ok ? AllocNearImage((size_t)cap * 16) : nullptr;
    for (size_t i = 0; big && i < sizeof(kStreamSites) / sizeof(kStreamSites[0]); ++i)
        if (kStreamSites[i].kind == 0 && llabs((int64_t)(big - (at[i] + 7))) >= 0x7fff0000ll) big = nullptr;
    if (!big) {
        Log("lod: model streaming list NOT enlarged (%s) - the game's 4096 (D-296)", ok ? "no memory within reach" : "code differs");
        return;
    }
    for (size_t i = 0; i < sizeof(kStreamSites) / sizeof(kStreamSites[0]); ++i) {
        uint8_t* p = at[i];
        const int32_t v = kStreamSites[i].kind == 0 ? (int32_t)(big - (p + 7)) : (int32_t)cap;
        DWORD old = 0;
        VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p + (kStreamSites[i].kind == 0 ? 3 : 1), &v, 4);
        VirtualProtect(p, 8, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_streamCap = cap;
    Log("lod: model streaming list %p (4096 parts a frame) -> mod list %p (%u parts), 12 sites patched (D-296)", (void*)arr,
        (void*)big, cap);
}

void StreamReqNote(bool frameStart) {
    if (frameStart) {  // (seen at the kicks: the async rest of the frame can add more - a lower bound)
        ++g_frames;
        if (g_frameModel > g_peakModel) g_peakModel = g_frameModel;
        if (g_frameTex > g_peakTex) g_peakTex = g_frameTex;
        if (g_frameModel > kStreamReqGameCap) ++g_overGame;
        if (g_frameModel > g_streamCap) ++g_overOurs;
        if (g_frameTex >= kTexReqCap) ++g_texFull;
        g_frameModel = g_frameTex = 0;
        const ULONGLONG now = GetTickCount64();
        DetailNote(now);  // D-298
        if (!g_noteAt) g_noteAt = now;
        if (now - g_noteAt >= 60000 && g_frames) {
            Log("lod: streaming requests (last minute, %llu frames) - model parts peak %u a frame (the game's list 4096: over it "
                "in %llu frames; ours %u: over it in %llu) | light / effect / decal textures peak %u (full at %u: %llu "
                "frames) (D-296) | model + texture heap %u MB, free at least %u MB (D-297; 0 = models / textures waited)",
                (unsigned long long)g_frames, g_peakModel, (unsigned long long)g_overGame, g_streamCap,
                (unsigned long long)g_overOurs, g_peakTex, kTexReqCap, (unsigned long long)g_texFull, g_heapTotal / 4,
                g_heapMinFree == 0xffffffffu ? 0u : g_heapMinFree / 4);
            g_noteAt = now;
            g_frames = g_overGame = g_overOurs = g_texFull = 0;
            g_peakModel = g_peakTex = 0;
            g_heapMinFree = 0xffffffffu;
        }
    }
    if (const uint8_t* st = *(uint8_t* const*)GameVa(kTexHeapStats)) {  // D-297: the pool's pages (256 KB)
        g_heapTotal = *(const volatile uint32_t*)(st + 0x8);
        const int32_t freePages = *(const volatile int32_t*)(st + 0x14);
        const uint32_t f = freePages < 0 ? 0u : (uint32_t)freePages;
        if (f < g_heapMinFree) g_heapMinFree = f;
    }
    // the model list's count keeps counting past its size (dropped entries included): the frame's demand
    const uint32_t m = *(volatile uint32_t*)GameVa(kStreamReqCount);
    if (m > g_frameModel) g_frameModel = m;
    if (const uint8_t* buf = *(uint8_t* const*)GameVa(kTexReqBuf)) {
        const uint32_t t = *(const volatile uint32_t*)(buf + 0x16000);
        if (t > g_frameTex) g_frameTex = t;
    }
}

void InstallLodHook() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool ok = DetourInstall(kLodSel, (void*)&hk_Lod, (void**)&o_Lod);
    Log("lod: LOD selector hook %s (lod_scale %.2f)", ok ? "installed" : "NOT installed", config::GetFloat("lod_scale", 1.f));
    const bool reg = DetourInstall(kTexRegister, (void*)&hk_TexRegister, (void**)&o_TexRegister);  // D-302
    Log("lod: texture registration watch %s (D-302)", reg ? "installed" : "NOT installed");
}

void LodProbe(int calls) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_nCams = 0;
    g_calls = 0;
    for (uint64_t& h : g_lodHist) h = 0;
    g_minS = 1e30f;
    g_maxS = -1e30f;
    g_probeLeft = calls > 0 ? calls : 20000;
    Log("lodprobe: armed for %d calls", g_probeLeft.load());
}

void LodScale(float s) {
    g_lodScale = s;
    config::SetFloat("lod_scale", s);
    Log("lod: scale x%.2f (< 1 = finer LODs farther out)", s);
}

}  // namespace gowvr::stereo
