// R1 (native re-entry) work: the frame-graph kick `sub_140426da0` (FRAME_MAP.md).
// Stage 1 — observe: snapshot the render-frame globals before/after the kick to learn which job counters, graphs and
// counts the kick itself creates (those must be drained/released between eyes; the rest must not be touched).
#include "r1.h"
#include "../core/addr.h"
#include "../core/detour.h"
#include "../core/log.h"
#include "../core/config.h"
#include "eyes.h"
#include "headcam.h"
#include "mutate.h"
#include "../fp/body.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace gowvr::stereo {
namespace {

// tools/re/hookgen.py 0x140426da0: push r15; sub rsp,0x40 (6 bytes stolen)
constexpr HookSite kKick{"FrameGraphKick", 0x140426da0, 6,
    {0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x80, 0x3d, 0x49, 0x6c, 0x7b, 0x02, 0x00, 0x8b, 0x0d, 0x65}};

struct Global { const char* name; uint64_t va; int bytes; };
const Global kGlobals[] = {
    {"cnt7320", 0x1412d7320, 8}, {"cnt7318", 0x1412d7318, 8}, {"cnt7368", 0x1412d7368, 8},
    {"cnt7350", 0x1412d7350, 8}, {"cnt7340", 0x1412d7340, 8}, {"cnt7348", 0x1412d7348, 8},
    {"cnt7360", 0x1412d7360, 8}, {"cnt7328", 0x1412d7328, 8}, {"cnt7358", 0x1412d7358, 8},
    {"cnt7370", 0x1412d7370, 8}, {"adea8", 0x1422adea8, 8}, {"ade98", 0x1422ade98, 8},
    {"ae210", 0x1422ae210, 8}, {"faa8", 0x14123faa8, 8}, {"exec990", 0x142298990, 8},
    {"exec988", 0x142298988, 8}, {"a4d8", 0x14502a4d8, 8}, {"g7f98", 0x144f97f98, 8},
    {"g8858", 0x144fa8858, 8}, {"g8838", 0x144fa8838, 8}, {"g5138", 0x144fc5138, 8},
    {"items", 0x142298980, 4}, {"chunks", 0x1422a93c0, 4}, {"passes", 0x142d24d04, 4},
    {"adef0", 0x1422adef0, 4}, {"ade90", 0x1422ade90, 4}, {"ref9a0", 0x1422989a0, 8},
    {"frameIdx", 0x142d19024, 4},
};
constexpr int kN = sizeof(kGlobals) / sizeof(kGlobals[0]);

using PFN_Kick = void(__fastcall*)();
PFN_Kick o_Kick = nullptr;

// Particle frame job ("Particle Update And Emit / Allocate Buffers / Simulate and Render"), called synchronously by the
// kick launcher (0x1404785fb). It advances triple-buffered GPU upload rings fenced by executed frames, so it must run
// once per frame: in the kick whose recording is executed (kick 2). Skipped in kick 1 with mask 0x8000.
constexpr HookSite kParticleFrame{"ParticleFrame", 0x1405888e0, 5,
    {0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54}};
using PFN_Particle = void(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
PFN_Particle o_Particle = nullptr;
std::atomic<bool> g_skipParticles{false};
std::atomic<uint64_t> g_particleSkips{0};

template <class T> T& G(uint64_t va);
template <class F> F Fn(uint64_t va);
extern std::atomic<int> g_kickPhase;
// D-221 (owner: six crashes 2026-10-15 in one cutscene - in VR only: the same cutscene played flat (F9) did not crash;
// the dead objects were the game's own PassParticles / PassParticlesSync / PassClear objects, overwritten with float
// data): the particle frame job, on EVERY call, rotates three deferred-free lists 0x144fe80c0 -> c8 -> d0 and frees the
// d0 nodes back to the effects pool (fx_051.c). A node retired during a call is freed three calls later - three FRAMES
// in the flat game, so the render thread has drawn its last use. In VR the job runs in both kicks (F-009 / F-013: it
// cannot be skipped in either), so a node was freed ~1.5 frames later - before the render thread finished the previous
// frame's second eye (F-013's sync waits only before kick 2) - and reused while still drawn: effects data written over
// freed / reused memory. When a cutscene retires many effects at once that corrupted the heap. Now the rotation happens
// once per frame: kick 2's call frees nothing (d0 held back) and afterwards the lists are put back, kick 2's retired
// nodes merged into c0 - the game's own three-frame delay. Taken under the lists' own spinlock 0x1427a2c48 (dword,
// cmpxchg 0 -> 1, as fx_046 / fx_059). The upload-ring counters still advance per call (F-013's sync covers those).
// gowvr.ini particle_free_once 0 = off
std::atomic<uint64_t> g_freeHeld{0};
void ListsLock(volatile long* l) { while (_InterlockedCompareExchange(l, 1, 0) != 0) YieldProcessor(); }
void ListsUnlock(volatile long* l) { _InterlockedCompareExchange(l, 0, 1); }

void __fastcall hk_Particle(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (g_skipParticles.load()) {
        ++g_particleSkips;
        return;
    }
    // D-223: off by default - the cutscene crash was the record array (D-223), not this (kept for testing)
    static const bool once = config::GetFloat("particle_free_once", 0.f) > 0.5f;
    if (!once || g_kickPhase.load() != 3) {
        o_Particle(a, b, c, d);
        return;
    }
    uint64_t* c0 = &G<uint64_t>(0x144fe80c0);
    uint64_t* c8 = &G<uint64_t>(0x144fe80c8);
    uint64_t* d0 = &G<uint64_t>(0x144fe80d0);
    volatile long* lock = (volatile long*)&G<int32_t>(0x1427a2c48);
    ListsLock(lock);
    const uint64_t D = *d0, C8 = *c8, C0 = *c0;
    *d0 = 0;  // the job's rotation frees d0: nothing to free in this call
    ListsUnlock(lock);
    o_Particle(a, b, c, d);
    ListsLock(lock);
    // the job did d0 = C8, c8 = C0, c0 = 0, then retired nodes N onto c0 -> back to (D, C8, N + C0)
    uint64_t* n = (uint64_t*)*c0;
    if (n) {
        uint64_t* tail = n;
        int guard = 0;
        while (tail[0] && ++guard < 1000000) tail = (uint64_t*)tail[0];
        tail[0] = C0;
        if (C0) ((uint64_t*)C0)[1] = (uint64_t)tail;
        *c0 = (uint64_t)n;
    } else {
        *c0 = C0;
    }
    *c8 = C8;
    *d0 = D;
    ListsUnlock(lock);
    const uint64_t k = ++g_freeHeld;
    if (k <= 3 || k % 20000 == 0)
        Log("r1: particle frees held to once per frame (%llu kick-2 calls, D-221)", (unsigned long long)k);
}
// D-223 (owner: the VR cutscene crash - hardware watchpoints D-222 caught the writer: a memset in sub_14046f300 at
// GoW+0x46f3f5 ran over PassParticles / PassParticlesSync / PassClear): the per-frame render-object record array
// (0x1412d7388, 0x180-byte records, 0xb40000 bytes = 30720 records, allocated once at renderer init right before those
// pass objects, never freed) is filled by `count 0x1412d7380 += n; memset(base + old*0x180, 0, n*0x180)` with NO capacity
// check, and the count is reset once per FRAME (sub_140469990, called at the top of the frame step sub_140673600). In VR
// both eyes' kicks append within one frame - twice the records; an object-heavy cutscene went past 30720 and the memset
// wrote over the objects allocated behind the array. Resetting the count between the eyes would let eye 1 overwrite eye
// 0's records before they are drawn (F-011), so the array is made big enough instead: right after the frame's reset
// (no records in use for the new frame) the base points at a mod-owned array of 3x the size; the game's own array is
// left as it is (last frame's readers stay valid); if the renderer is re-created the swap repeats at the next frame.
// The frame's peak record count is logged (the old limit is 30720). gowvr.ini render_records_scale (3; 1 = off)
constexpr uint64_t kRecCount = 0x1412d7380, kRecBase = 0x1412d7388;
constexpr size_t kRecSize = 0x180, kRecGameBytes = 0xb40000;
uint8_t* g_recBig = nullptr;
size_t g_recBigBytes = 0;
uint32_t g_recPeak = 0;
uint64_t g_recFrames = 0;
using PFN_FrameReset = void(__fastcall*)();
void __fastcall hk_FrameReset() {
    const uint32_t used = G<uint32_t>(kRecCount);  // last frame's total, read before the reset
    if (used > g_recPeak) g_recPeak = used;
    Fn<PFN_FrameReset>(0x140469990)();
    static const float scale = config::GetFloat("render_records_scale", 3.f);
    if (scale > 1.f) {
        if (!g_recBig) {
            g_recBigBytes = (size_t)(kRecGameBytes * scale) & ~(size_t)0xfff;
            g_recBig = (uint8_t*)VirtualAlloc(nullptr, g_recBigBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        uint8_t*& base = G<uint8_t*>(kRecBase);
        if (g_recBig && base && base != g_recBig) {
            Log("r1: render-object record array %p (%zu records) -> mod array %p (%zu records) (D-223)", (void*)base,
                kRecGameBytes / kRecSize, (void*)g_recBig, g_recBigBytes / kRecSize);
            base = g_recBig;
        }
    }
    if (++g_recFrames % 5400 == 0 || (used > kRecGameBytes / kRecSize && used == g_recPeak)) {
        Log("r1: render-object records - peak %u per frame (the game's array holds %zu, ours %zu) (D-223)", g_recPeak,
            kRecGameBytes / kRecSize, g_recBigBytes / kRecSize);
        if (g_recFrames % 5400 == 0) g_recPeak = 0;
    }
}
const HookSite kFrameResetCall{"FrameReset_call", 0x140673604, 5,
    {0xe8, 0x87, 0x63, 0xdf, 0xff, 0x80, 0x3d, 0xd9, 0x00, 0xc7, 0x01, 0x00, 0x75, 0x0e, 0x80, 0x3d}};

// D-225 (owner: "the fire flames at Brok's shop produce light flares that get stretched all over my VR screen"): the
// lens flares are a flat-screen effect - PassLensFlare::vfunc_3 (0x14050e5d0) draws sprites placed along the line from
// the light's projected screen spot through the screen centre, sized for the flat picture; in each eye's ~130 deg view
// they come out stretched across the image (VR mods turn screen-space flares off). The whole draw sits under
// `if (flare count != 0)`, so skipping the call is exactly "no flares this frame". Skipped only while VR stereo runs (the
// flat game keeps them). gowvr.ini vr_lens_flares 1 = keep them in VR
extern std::atomic<bool> g_vr;  // (defined below)
const HookSite kLensFlareDraw{"LensFlareDraw", 0x14050e5d0, 5,  // push rbp; push rsi; push r13 (5)
    {0x40, 0x55, 0x56, 0x41, 0x55, 0x48, 0x8d, 0xac, 0x24, 0xc0, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec}};
using PFN_FlareDraw = void(__fastcall*)(void*, void*, void*, void*);
PFN_FlareDraw o_FlareDraw = nullptr;
void __fastcall hk_FlareDraw(void* self, void* a, void* b, void* c) {
    static const bool keep = config::GetFloat("vr_lens_flares", 0.f) > 0.5f;
    if (g_vr.load() && !keep) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) == 0) Log("r1: lens flares off in VR (screen-space effect, D-225)");
        return;
    }
    o_FlareDraw(self, a, b, c);
}

uint8_t* g_base = nullptr;
std::atomic<int> g_observe{0};

uint64_t Read(const Global& g) {
    uint64_t v = 0;
    memcpy(&v, (uint8_t*)GameVa(g.va), g.bytes);
    return v;
}

// ---- Stage 2: zero-delta double kick (RE-09 three-capture gate, capture B) ---------------------------------------
// Engine functions used to drain/reset exactly the state the kick created (observed above), in the frame tail's order.
using PFN_WaitRelease = void(__fastcall*)(uint64_t counter, char release);  // sub_140b35180
using PFN_GraphRelease = void(__fastcall*)(uint64_t graph);                 // sub_14063cde0
using PFN_WaitCounter = void(__fastcall*)(uint64_t counter);                // sub_140b35270
using PFN_Void = void(__fastcall*)();
using PFN_PassCall = void(__fastcall*)(void* pass, void* record);

std::atomic<int> g_double{0};
std::atomic<unsigned> g_mask{0x1ff};
std::atomic<uint64_t> g_doubled{0};

template <class T> T& G(uint64_t va) { return *(T*)((uint8_t*)GameVa(va)); }
template <class F> F Fn(uint64_t va) { return (F)((uint8_t*)GameVa(va)); }

void WaitZero(uint64_t va) {
    uint64_t& c = G<uint64_t>(va);
    if (c) {
        Fn<PFN_WaitRelease>(0x140b35180)(c, 1);
        c = 0;
    }
}

bool g_fogFix = true;  // F-012 policy switch (fogfix on|off)

void PassLoop(int vslot, bool resetFilter) {
    int n = G<int>(0x142d24d04);
    uint8_t* recs = &G<uint8_t>(0x142590b00);
    uint8_t* table = G<uint8_t*>(0x142d4e298);
    for (int i = 0; i < n; ++i) {
        uint8_t* rec = recs + i * 0x88;
        if (resetFilter) {
            uint8_t* owner = *(uint8_t**)(rec + 8);
            if (owner && owner[0x3a4]) continue;
        }
        int idx = *(int*)(rec + 0x7c);
        void* pass = *(void**)(table + (int64_t)idx * 0xb0 + 0xa8);
        // PassVolumetricFog::vfunc_1 flips its history ping-pong: history weights +0x4c8/+0x4cc, valid flag +0x4d0,
        // parity +0x4d1 (and the 0x1412390a8/b0 pointers, restored by the bank). Between the eyes the rest of vfunc_1
        // runs as in the stable build, but the flip is undone, so history alternates exactly once per frame (F-012).
        // Skipping the whole call instead hung the GPU in gameplay (F-013).
        const bool fog = g_fogFix && vslot == 1 && *(uint64_t*)pass == (uint64_t)GameVa(0x140df8b28ull);  // D-187
        uint8_t fogState[12];
        if (fog) memcpy(fogState, (uint8_t*)pass + 0x4c8, sizeof(fogState));
        (*(PFN_PassCall**)pass)[vslot](pass, rec);
        if (fog) memcpy((uint8_t*)pass + 0x4c8, fogState, sizeof(fogState));
    }
}

float g_eyeOff[2] = {0.f, 0.f}, g_eyeYawDeg[2] = {0.f, 0.f};
bool g_eyesOn = false;
std::atomic<bool> g_vr{false};  // (declared above for D-225)
uint64_t g_syncTimeouts = 0;
double g_syncMsSum = 0, g_syncMsMax = 0;
float g_wobble = 0.f;  // diagnostic: both eyes get the same per-frame sideways camera motion (amplitude, metres)  // VR mode: continuous double kick, head-tracked eyes
float g_savedDt = 0.f;  // real render delta-time while kick 2 runs with 0 (mask 0x4000)

void BetweenEyes(unsigned m) {
    if (m & 0x800) {  // render-begin counters (created by sub_14046c8e0 before the kick, waited in the frame tail)
        WaitZero(0x1412d7320);
        WaitZero(0x1412d7318);
        WaitZero(0x1412d7368);
        WaitZero(0x1412d7350);
    }
    if (m & 0x001) {  // tail slice 1: kick-created job counters and graphs
        WaitZero(0x1412d7340);
        WaitZero(0x1412d7348);
        static const uint64_t kGraphs[] = {0x144f97f98ull, 0x144fa8858ull, 0x144fa8838ull, 0x144fc5138ull};
        // 0x10000 (effects job once per frame): the four graphs come from the particle/effects job (fx sub_1405888e0);
        // kick 2 skips that job and reuses kick 1's results, so the graphs stay alive until the frame tail releases them.
        if (!(m & 0x10000))
        for (uint64_t g : kGraphs) {
            // null check: the engine tail calls sub_14063cde0 unconditionally because every kick creates all four
            // graphs, but with the particle job skipped in kick 1 (mask 0x8000) one is never created (F-008).
            if (G<uint64_t>(g)) Fn<PFN_GraphRelease>(0x14063cde0)(G<uint64_t>(g));
            G<uint64_t>(g) = 0;
        }
        // skipped on purpose: memcpy DAT_142d02650 -> DAT_142d02e60 (previous-frame state, once per simulation frame)
        WaitZero(0x1422ae210);
        WaitZero(0x14123faa8);
    }
    if (m & 0x002) PassLoop(1, false);  // per-pass end-of-frame (vfunc_1)
    if (m & 0x004) {                    // execution-job reference and wait
        int64_t prev = _InterlockedExchangeAdd64((volatile int64_t*)&G<int64_t>(0x1422989a0), -1);
        if (prev == 1) Fn<PFN_WaitCounter>(0x140b35270)(G<uint64_t>(0x142298990));
    }
    if (m & 0x008) {  // render reset block entry calls
        Fn<PFN_Void>(0x14044ec50)();
        Fn<PFN_Void>(0x140469b40)();
    }
    if (m & 0x010) {
        WaitZero(0x1412d7360);
        WaitZero(0x1412d7328);
        WaitZero(0x1412d7358);
        WaitZero(0x1412d7370);
    }
    if (m & 0x020) Fn<PFN_WaitRelease>(0x140b35180)(G<uint64_t>(0x142298990), 1);  // exec990 (kick-created)
    if (m & 0x040) G<int>(0x142298980) = 0;                                          // work-item count
    if (m & 0x080) PassLoop(5, true);                                                // per-pass reset (vfunc_5)
    if (m & 0x100) {
        G<int>(0x1422adef0) = 0;
        G<int>(0x1422ade90) = 0;
    }
    // Engine's own complete render-frame reset (sub_140478980: sub_140469b40, drains 7360/7328/7358/7370, clears the
    // batch/object lists, releases exec990/exec988, zeroes items, per-pass vfunc_5). Replaces 0x008..0x100.
    if (m & 0x200) Fn<PFN_Void>(0x140478980)();
    // Keep kick 1's recording: the reset above waited for its recording jobs (exec988); the chunk contexts still point
    // at kick 1's packets until kick 2 re-records them (eyes.cpp submits the copies before kick 2's).
    if (EyesEnabled()) EyesSteal();
    if (m & 0x400) {  // light-server per-frame accumulators (reset only in renLightServer::vfunc_2, before the kick)
        G<uint32_t>(0x1412d7314) = 0;
        G<uint64_t>(0x142454748) = 0;
    }
    // Render delta-time 0 for everything the second render-begin + kick launch (restored after kick 2 drains).
    if (m & 0x4000) {
        g_savedDt = G<float>(0x144fadce0);
        G<float>(0x144fadce0) = 0.f;
    }
    // Engine's own render-frame begin (sub_14046c8e0: per-object render-prep batches 7320/7318/7368/7350 and the
    // batch executor via sub_14050e140) so the second kick's registered batches get executed.
    if (m & 0x1000) Fn<PFN_Void>(0x14046c8e0)();
}

// Snapshot of the whole job-counter block 0x1412d7300..0x1412d7380 plus exec/ref globals, for double-kick forensics.
void Snap(const char* tag) {
    char line[1200];
    int len = sprintf_s(line, "r1snap %s:", tag);
    for (uint64_t va = 0x1412d7300; va < 0x1412d7380; va += 8)
        len += sprintf_s(line + len, sizeof(line) - len, " %llx=%llx", (unsigned long long)(va & 0xff),
                         (unsigned long long)G<uint64_t>(va));
    len += sprintf_s(line + len, sizeof(line) - len, " exec990=%llx exec988=%llx ref9a0=%lld items=%d",
                     (unsigned long long)G<uint64_t>(0x142298990), (unsigned long long)G<uint64_t>(0x142298988),
                     (long long)G<int64_t>(0x1422989a0), G<int>(0x142298980));
    Log("%s", line);
}

std::atomic<int> g_snap{0};

// ---- Per-eye state bank (gate B policy "snapshot/restore"): ranges from gowvr_logs/r1bank.txt (tools/re/r1bankgen.py)
struct BankRange { uint64_t va; uint32_t len; uint32_t off; };
BankRange g_bank[4096];
int g_nbank = 0;
uint8_t g_bankData[1 << 20];

bool BankLoad() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    *strrchr(path, '\\') = 0;
    strcat_s(path, "\\gowvr_logs\\r1bank.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) { Log("r1bank: cannot open %s", path); return false; }
    g_nbank = 0;
    uint32_t total = 0;
    unsigned long long va, len;
    while (g_nbank < 4096 && fscanf_s(f, "%llx %llx", &va, &len) == 2) {
        if (va < 0x14105a000 || va + len > 0x14105a000 + 0x4064bd8 || total + len > sizeof(g_bankData)) continue;
        if (!GameVa(va) || !GameVa(va + len - 1)) continue;  // D-187: not found on this build
        g_bank[g_nbank++] = {va, (uint32_t)len, total};
        total += (uint32_t)len;
    }
    fclose(f);
    Log("r1bank: loaded %d ranges, %u bytes", g_nbank, total);
    return true;
}

// Verified per-eye bank (F-011, 2026-09-27): only explicit temporal state. A diff-derived bank also rewinds per-frame
// GPU allocation counters (0x144fdc600..) so kick 2 overwrites eye 0's data before it executes.
void BankDefault() {
    static const BankRange kDefault[] = {
        {0x1411960d0, 8, 0},   // reflection parity
        {0x1412390a8, 16, 0},  // volumetric fog history pointer
        {0x14123b760, 8, 0},   // render-time accumulator
        {0x1412d7598, 8, 0},   // 16-step jitter / sequence index
        {0x142454748, 8, 0},   // light-server accumulator (restoring it fixes eye-1 emissive/particle lighting)
    };
    g_nbank = 0;
    uint32_t total = 0;
    for (const auto& r : kDefault) {
        g_bank[g_nbank++] = {r.va, r.len, total};
        total += r.len;
    }
    Log("r1bank: built-in whitelist, %d ranges, %u bytes", g_nbank, total);
}

void BankCapture() {
    for (int i = 0; i < g_nbank; ++i) memcpy(g_bankData + g_bank[i].off, &G<uint8_t>(g_bank[i].va), g_bank[i].len);
}

void BankRestore() {
    for (int i = 0; i < g_nbank; ++i) {
        // D-117 (owner crash 2026-10-05 19:24, d3d11.dll+0x193fe4 on the first stereo frame after an area load): the fog
        // history pointers 0x1412390a8 / b0 point INTO the PassVolumetricFog object (this + 0x348 / 0x3a8 and + 0x1c8 /
        // 0x228, render_passes_019 vfunc_1 - the ping-pong moves each by 0x60 only). A load resets them to a default
        // (render_passes_021) or rebuilds the pass: restoring the pre-kick values then gave eye 1 pointers into a freed /
        // other object. Restored only while eye 0 left them on the same object (each within 0x60), else the game's kept
        if (g_bank[i].va == 0x1412390a8 && g_bank[i].len == 16) {
            const int64_t* now = &G<int64_t>(0x1412390a8);
            const int64_t* was = (const int64_t*)(g_bankData + g_bank[i].off);
            if (llabs(now[0] - was[0]) > 0x60 || llabs(now[1] - was[1]) > 0x60) {
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1) < 20)
                    Log("r1bank: fog history pointers changed in eye 0 (%llx %llx -> %llx %llx) - kept (pass rebuilt / reset)",
                        (unsigned long long)was[0], (unsigned long long)was[1], (unsigned long long)now[0],
                        (unsigned long long)now[1]);
                continue;
            }
        }
        memcpy(&G<uint8_t>(g_bank[i].va), g_bankData + g_bank[i].off, g_bank[i].len);
    }
}

// ---- .data diff instrument (side-effect census for gate B) ----------------------------------------------------------
constexpr uint64_t kDataVa = 0x14105a000;
constexpr size_t kDataSize = 0x4064bd8;
std::atomic<int> g_diffMode{0};  // 1 = diff across kick1+between (double path), 2 = control: across one normal frame
uint8_t* g_diffA = nullptr;
uint8_t* g_diffB = nullptr;
uint8_t* g_preKick = nullptr;
int g_ctlStage = 0;

void DataSnap(uint8_t*& buf) {
    if (!buf) buf = (uint8_t*)VirtualAlloc(nullptr, kDataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf) memcpy(buf, (uint8_t*)GameVa(kDataVa), kDataSize);
}

void DataDiffWrite(const char* file) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    *strrchr(path, '\\') = 0;
    strcat_s(path, "\\gowvr_logs\\");
    strcat_s(path, file);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    size_t ranges = 0, bytes = 0;
    for (size_t off = 0; off < kDataSize;) {
        if (g_diffA[off] == g_diffB[off]) { ++off; continue; }
        size_t start = off;
        while (off < kDataSize && (g_diffA[off] != g_diffB[off] || (off + 8 < kDataSize &&
               memcmp(g_diffA + off, g_diffB + off, 8) != 0)))
            ++off;
        size_t len = off - start;
        fprintf(f, "%llx %zx", (unsigned long long)(kDataVa + start), len);
        size_t show = len < 16 ? len : 16;
        fprintf(f, " before=");
        for (size_t i = 0; i < show; ++i) fprintf(f, "%02x", g_diffA[start + i]);
        fprintf(f, " after=");
        for (size_t i = 0; i < show; ++i) fprintf(f, "%02x", g_diffB[start + i]);
        fprintf(f, "\n");
        ++ranges;
        bytes += len;
    }
    fclose(f);
    Log("r1diff: %s ranges=%zu bytes=%zu", file, ranges, bytes);
}

std::atomic<uint64_t> g_kicks{0};  // D-083 loadwatch
std::atomic<int> g_kickPhase{0};   // D-099 crash log: where the main thread is in the frame's kicks
struct KickPhaseReset { ~KickPhaseReset() { g_kickPhase = 0; } };
void __fastcall hk_Kick() {
    ++g_kicks;
    const KickPhaseReset phaseReset;
    g_kickPhase = 4;
    if (g_diffMode.load() == 2) {  // control: .data across one normal frame (pre-kick N vs pre-kick N+1)
        if (g_ctlStage == 0) {
            DataSnap(g_preKick);
            g_ctlStage = 1;
        } else {
            DataSnap(g_diffB);
            std::swap(g_diffA, g_preKick);
            DataDiffWrite("r1diff_frame.txt");
            std::swap(g_diffA, g_preKick);
            g_ctlStage = 0;
            g_diffMode = 0;
        }
    }
    // D-083: a 2D screen frame (VR, pause menu) is one plain kick with the game's camera (xr.cpp shows it as a screen);
    // the per-eye histories are stale afterwards
    const bool flat2D = g_vr.load() && Screen2DFrame();
    static bool wasFlat2D = false;
    if (wasFlat2D && !flat2D) ResetEyeHistory();
    wasFlat2D = flat2D;
    if (flat2D) {
        fp::BodyHideDrawBegin();
        o_Kick();
        fp::BodyHideDrawEnd();
        return;
    }
    if (g_double.load() > 0) {
        bool snap = g_snap.load() > 0;
        if (snap) Snap("before-k1");
        if (g_diffMode.load() == 1) DataSnap(g_preKick);
        const bool bank = (g_mask.load() & 0x2000) && g_nbank > 0;
        if (bank) BankCapture();
        bool headOn = false;
        if (g_vr.load()) {
            vr::EyeViews used;
            headOn = HeadcamArm(&used);
            EyesSetFrameViews(headOn ? used : vr::EyeViews{});
            if (headOn) { EyeFrameBegin(); SetEyeIndex(0); }
        }
        float wob = g_wobble != 0.f ? g_wobble * sinf((float)g_doubled.load() * 0.15f) : 0.f;
        if (g_eyesOn && !headOn) { SetEyeIndex(0); SetEyeCamera(true, g_eyeOff[0] + wob, g_eyeYawDeg[0]); }
        if (g_mask.load() & 0x8000) g_skipParticles = true;
        fp::BodyHideDrawBegin();   // F7 body / neck shrunk for the drawing only
        fp::WaitHeroFrame();       // D-042: eye 0 must not draw the previous / a half-finished hero pass
        fp::BodyHideEnsure();      // D-082: a pass (forced above, or faulted) may have put the neck back
        fp::EyeProbe(0);
        g_kickPhase = 1;
        o_Kick();                  // eye 0
        g_kickPhase = 2;
        fp::EyeProbe(1);
        g_skipParticles = false;
        if (snap) Snap("after-k1");
        bool diff = g_diffMode.load() == 1;
        if (diff) DataSnap(g_diffA);  // note: taken after kick 1 returns; see pre-kick snapshot below
        BetweenEyes(g_mask.load());
        if (snap) Snap("after-between");
        if (diff) {
            DataSnap(g_diffB);
            DataDiffWrite("r1diff_between.txt");  // what the between-eyes step changed
            std::swap(g_diffA, g_preKick);         // pre-kick-1 snapshot vs pre-kick-2
            DataDiffWrite("r1diff_k1_between.txt");
            std::swap(g_diffA, g_preKick);
            g_diffMode = 0;
        }
        if (bank) BankRestore();   // kick 2 sees the per-frame render state kick 1 saw
        // Time-driven render work (particle update/emit, render-time accumulator, animated shader time) must advance once
        // per simulation frame: kick 2 runs with render delta-time 0 until its asynchronous work has finished.
        // (dt was set to 0 inside BetweenEyes, after kick 1 drained and before render-begin; see g_savedDt)
        const bool freezeDt = (g_mask.load() & 0x4000) != 0;
        if (headOn) SetEyeIndex(1);
        else if (g_eyesOn) { SetEyeIndex(1); SetEyeCamera(true, g_eyeOff[1] + wob, g_eyeYawDeg[1]); }
        // Sync point (0x20000): kick 2 rewrites the effects job's 3-slot CPU blocks (0x1422b2c00 + slot*0x55c0) that the
        // previous frame's eye-0 streams still upload from when the render thread executes them (PassParticles ->
        // sub_140588220 records copies from CPU memory). Wait until the render thread has reached Present for every
        // submitted frame (completed 0x142d50678 == submitted 0x142d50670; completed is incremented before Present).
        if (g_mask.load() & 0x20000) {
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            int spins = 0;
            while (G<uint64_t>(0x142d50670) != G<uint64_t>(0x142d50678)) {
                QueryPerformanceCounter(&t1);
                if ((t1.QuadPart - t0.QuadPart) * 1000 > f.QuadPart * 200) { ++g_syncTimeouts; break; }
                if (++spins < 64) YieldProcessor(); else Sleep(0);
            }
            QueryPerformanceCounter(&t1);
            double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            g_syncMsSum += ms;
            if (ms > g_syncMsMax) g_syncMsMax = ms;
        }
        // Effects job once per frame (0x10000): its per-frame upload rings have 3 slots and must advance once per frame;
        // advancing twice reused a slot ~1.5 frames later while the GPU could still draw eye 0 from it (F-013).
        if (g_mask.load() & 0x10000) g_skipParticles = true;
        fp::BodyHideEnsure();
        fp::EyeProbe(2);
        g_kickPhase = 3;
        o_Kick();                  // eye 1
        g_kickPhase = 0;
        fp::EyeProbe(3);
        fp::BodyHideDrawEnd();
        g_skipParticles = false;
        if (headOn) SetEyePoses(false, nullptr, nullptr, false);
        else if (g_eyesOn) SetEyeCamera(false, 0.f, 0.f);
        // Waiting for kick 2's asynchronous work before the frame tail self-deadlocks (the execution job's dependencies
        // are only released by the tail itself — F-006), so dt is restored as soon as the synchronous kick returns.
        if (freezeDt) G<float>(0x144fadce0) = g_savedDt;
        if (snap) { Snap("after-k2"); --g_snap; }
        uint64_t k = ++g_doubled;
        if (k <= 5 || k % 1000 == 0) {
            Log("r1: double kick #%llu items=%d chunks=%u passes=%d frameIdx=%d sync avg=%.2fms max=%.2fms timeouts=%llu",
                (unsigned long long)k, G<int>(0x142298980), G<unsigned>(0x1422a93c0), G<int>(0x142d24d04),
                G<int>(0x142d19024), g_syncMsSum / (double)k, g_syncMsMax, (unsigned long long)g_syncTimeouts);
            g_syncMsMax = 0;
        }
        if (--g_double == 0)
            Log("r1: double-kick window over (%llu frames, particle jobs skipped in kick 1: %llu)",
                (unsigned long long)g_doubled.load(), (unsigned long long)g_particleSkips.load());
        return;
    }
    if (g_observe.load() <= 0) {
        fp::BodyHideDrawBegin();
        o_Kick();
        fp::BodyHideDrawEnd();
        return;
    }
    uint64_t before[kN], after[kN];
    for (int i = 0; i < kN; ++i) before[i] = Read(kGlobals[i]);
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    o_Kick();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < kN; ++i) after[i] = Read(kGlobals[i]);
    char line[1600];
    int len = sprintf_s(line, "r1observe: kick %.3f ms tid=%lu |", double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
                        GetCurrentThreadId());
    for (int i = 0; i < kN && len < 1500; ++i)
        len += sprintf_s(line + len, sizeof(line) - len, " %s=%llx%s%llx", kGlobals[i].name,
                         (unsigned long long)before[i], before[i] == after[i] ? "=" : "->", (unsigned long long)after[i]);
    Log("%s", line);
    --g_observe;
}

}  // namespace
uint64_t R1Kicks() { return g_kicks.load(); }  // D-083 loadwatch
int R1KickPhase() { return g_kickPhase.load(); }

bool R1Install() {
    static bool done = false;
    if (done) return true;
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    bool a = DetourInstall(kKick, (void*)&hk_Kick, (void**)&o_Kick);
    bool b = DetourInstall(kParticleFrame, (void*)&hk_Particle, (void**)&o_Particle);
    const bool rr = DetourCallSite(kFrameResetCall, (void*)&hk_FrameReset);  // D-223
    const bool lf = DetourInstall(kLensFlareDraw, (void*)&hk_FlareDraw, (void**)&o_FlareDraw);  // D-225
    Log("r1: lens flare VR skip (D-225) %s", lf ? "installed" : "NOT installed");
    Log("r1: render-object record array guard (D-223) %s", rr ? "installed" : "NOT installed");
    done = a && b;
    return done;
}

void R1Observe(int frames) {
    g_observe = frames;
    Log("r1: observing %d kicks", frames);
}

bool R1BankLoad() { return BankLoad(); }

void R1Mask(unsigned mask) {
    g_mask = mask;
    Log("r1: mask=0x%x", mask);
}

void R1FogFix(bool on) {
    g_fogFix = on;
    Log("r1: fog history fix %s", on ? "on" : "off");
}

void R1Wobble(float amp) {
    g_wobble = amp;
    Log("r1: wobble %.3f", amp);
}

void R1Vr(bool on) {
    if (on) {
        BankDefault();
        g_mask = 0x27e07;  // + sync point before kick 2 (F-013)
        ResetEyeHistory();
        g_snap = 0;
        SetHeadCamera(true);
        g_vr = true;
        g_double = 0x7fffffff;
    } else {
        g_vr = false;
        SetHeadCamera(false);
        g_double = 0;
        ResetEyeHistory();
    }
    Log("r1: VR mode %s", on ? "on (continuous double kick, head-tracked eyes, whitelist bank)" : "off");
}

void R1Eyes(bool on, float off0, float off1, float yaw0, float yaw1) {
    g_eyeOff[0] = off0; g_eyeOff[1] = off1; g_eyeYawDeg[0] = yaw0; g_eyeYawDeg[1] = yaw1;
    g_eyesOn = on;
    ResetEyeHistory();
    Log("r1: eyes %s offX=(%.4f, %.4f) yaw=(%.2f, %.2f)", on ? "on" : "off", off0, off1, yaw0, yaw1);
}

void R1Diff(int mode) {
    g_ctlStage = 0;
    g_diffMode = mode;
    Log("r1diff: armed mode %d (%s)", mode, mode == 1 ? "double-kick" : "control frame");
}

void R1Double(int frames, unsigned mask) {
    g_mask = mask;
    g_doubled = 0;
    g_snap = 2;
    g_double = frames;
    Log("r1: zero-delta double kick for %d frames, between-eyes mask=0x%x", frames, mask);
}

}  // namespace gowvr::stereo
