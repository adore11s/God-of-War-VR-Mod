// D-062 G1 research (owner: attacks must not move the player forward): where does the game keep Kratos' position?
// Writing the object matrix (obj+0x60) does not hold (D-014: rebuilt every frame from the game's own state), so the
// authoritative copy is searched live:
//   posscan        - every float triplet within 2 cm of the game root position in the hero object (0x1000), its
//                    creature (obj+0x220, 0x6000) and the blocks either points to (one level, 0x800 each)
//   postrack       - for 2 s (do a lunge meanwhile): which candidates follow the moving root (live copies)
//   poshold <ms> <i> - writes the position of the moment into candidate i every frame for <ms>, then logs how far
//                    the game root travelled (0 = that copy holds Kratos in place). "all" / "live" were removed after
//                    they crashed the game (writes into unrelated game data).
#include "posdiag.h"
#include "body.h"
#include "../core/log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gowvr::fp {
namespace {

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const uint8_t*)p + n <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}

struct Cand { uint8_t* base; size_t off; char tag[64]; bool dbl = false; int follow = 0, frames = 0; };
void CandRead(const Cand& c, float out[3]) {
    if (c.dbl) { const double* d = (const double*)(c.base + c.off); for (int i = 0; i < 3; ++i) out[i] = (float)d[i]; }
    else memcpy(out, c.base + c.off, 12);
}
std::vector<Cand> g_cands;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<int> g_trackFrames{0}, g_holdMs{0};
ULONGLONG g_holdUntil = 0;
int g_holdSel = -1;  // -1 all, -2 live, >= 0 one
float g_holdPos[3], g_trackStart[3];
bool g_holdLogged = true;

void ScanPos(uint8_t* p, size_t len, const char* tag, const float* pos) {
    if (!p || (uintptr_t)p < 0x10000) return;
    for (size_t o = 0; o + 12 <= len; o += 4) {
        if ((o & 0xfff) == 0 && !Readable(p + o, 16)) return;
        if (!Readable(p + o, 12)) return;
        const float* f = (const float*)(p + o);
        // horizontal match (a capsule / physics body sits higher than the feet); double copies checked too
        const double* dd = (const double*)(p + o);
        const bool dbl = (o & 7) == 0 && o + 24 <= len && Readable(p + o, 24) && fabs(dd[0] - pos[0]) < 0.02 &&
                         fabs(dd[2] - pos[2]) < 0.02 && fabs(dd[1] - pos[1]) < 3.0;
        if (dbl || (fabsf(f[0] - pos[0]) < 0.02f && fabsf(f[2] - pos[2]) < 0.02f && fabsf(f[1] - pos[1]) < 3.f)) {
            if (g_cands.size() >= 96) return;
            Cand c{p, o, {}};
            c.dbl = dbl;
            sprintf_s(c.tag, "%s+0x%zx%s y%+.2f", tag, o, dbl ? " (double)" : "", dbl ? dd[1] - pos[1] : f[1] - pos[1]);
            g_cands.push_back(c);
        }
    }
}

}  // namespace

void PosScan() {
    uint8_t* obj = HeroObject();
    float pos[3], yaw;
    if (!obj || !GameRoot(pos, &yaw)) { Log("posscan: no hero"); return; }
    AcquireSRWLockExclusive(&g_lock);
    g_cands.clear();
    ScanPos(obj, 0x1000, "obj", pos);
    uint8_t* cr = Readable(obj + 0x220, 8) ? *(uint8_t**)(obj + 0x220) : nullptr;
    if (cr) ScanPos(cr, 0x6000, "cr", pos);
    // one level from the object (0x1000) and the creature (0x6000), two levels from the object's first 0x400
    for (int pass = 0; pass < 2; ++pass) {
        uint8_t* b = pass ? cr : obj;
        if (!b) continue;
        const size_t span = pass ? 0x6000 : 0x1000;
        for (size_t o = 0; o < span; o += 8) {
            if (!Readable(b + o, 8)) break;
            uint8_t* q = *(uint8_t**)(b + o);
            if ((uintptr_t)q < 0x10000 || ((uintptr_t)q & 7) || q == obj || q == cr || !Readable(q, 8)) continue;
            char tag[32];
            sprintf_s(tag, "%s+0x%zx->", pass ? "cr" : "obj", o);
            ScanPos(q, 0x1000, tag, pos);
            if (!pass && o < 0x400) {
                for (size_t o2 = 0; o2 < 0x200; o2 += 8) {
                    if (!Readable(q + o2, 8)) break;
                    uint8_t* q2 = *(uint8_t**)(q + o2);
                    if ((uintptr_t)q2 < 0x10000 || ((uintptr_t)q2 & 7) || q2 == obj || q2 == cr || !Readable(q2, 8)) continue;
                    char tag2[40];
                    sprintf_s(tag2, "obj+0x%zx->+0x%zx->", o, o2);
                    ScanPos(q2, 0x800, tag2, pos);
                }
            }
        }
    }
    Log("posscan: game root (%.3f %.3f %.3f), %zu candidates", pos[0], pos[1], pos[2], g_cands.size());
    for (size_t i = 0; i < g_cands.size(); ++i) Log("posscan: [%zu] %s (%p)", i, g_cands[i].tag, g_cands[i].base + g_cands[i].off);
    ReleaseSRWLockExclusive(&g_lock);
}

// SEH-guarded region scan (a region can be freed by the game while it is read); returns hits written to out
static int ScanRegionSeh(const float* f, size_t n, const float* pos, size_t* out, int cap) {
    int k = 0;
    __try {
        for (size_t i = 0; i + 3 <= n && k < cap; ++i)
            if (fabsf(f[i] - pos[0]) < 0.01f && fabsf(f[i + 2] - pos[2]) < 0.01f && fabsf(f[i + 1] - pos[1]) < 3.f) out[k++] = i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
}

// "posscanall": every committed private read-write region (the physics world included) - stalls the game for seconds
void PosScanAll() {
    float pos[3], yaw;
    if (!GameRoot(pos, &yaw)) { Log("posscan: no hero"); return; }
    const ULONGLONG t0 = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    g_cands.clear();
    size_t scanned = 0;
    uint8_t* p = (uint8_t*)0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(p, &mbi, sizeof(mbi)) && g_cands.size() < 400) {
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xff;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY) && size <= (8ull << 30)) {
            scanned += size;
            const float* f = (const float*)base;
            static size_t hits[400];
            const int k = ScanRegionSeh(f, size / 4, pos, hits, 400 - (int)g_cands.size());
            for (int j = 0; j < k; ++j) {
                Cand c{base, hits[j] * 4, {}};
                sprintf_s(c.tag, "%p y%+.2f", base + hits[j] * 4, f[hits[j] + 1] - pos[1]);
                g_cands.push_back(c);
            }
        }
        p = base + size;
        if ((uintptr_t)p >= 0x7fff00000000ull) break;
    }
    Log("posscanall: %zu candidates in %.0f MB (%llu ms)", g_cands.size(), scanned / 1048576.0,
        (unsigned long long)(GetTickCount64() - t0));
    ReleaseSRWLockExclusive(&g_lock);
}

// "posfind <i>": pointers into the struct holding candidate i (value within 0x3000 below it) in the hero object /
// creature and the blocks they point to - a stable path to the real position copy
void PosFind(int i) {
    AcquireSRWLockShared(&g_lock);
    if (i < 0 || i >= (int)g_cands.size()) { ReleaseSRWLockShared(&g_lock); Log("posfind: no candidate %d", i); return; }
    const uintptr_t target = (uintptr_t)(g_cands[i].base + g_cands[i].off);
    ReleaseSRWLockShared(&g_lock);
    uint8_t* obj = HeroObject();
    if (!obj) return;
    uint8_t* cr = Readable(obj + 0x220, 8) ? *(uint8_t**)(obj + 0x220) : nullptr;
    int hits = 0;
    auto check = [&](uint8_t* blk, size_t len, const char* tag) {
        for (size_t o = 0; o + 8 <= len && hits < 40; o += 8) {
            if ((o & 0xfff) == 0 && !Readable(blk + o, 8)) return;
            const uintptr_t v = *(uintptr_t*)(blk + o);
            if (v <= target && target - v < 0x3000) {
                Log("posfind: %s+0x%zx = %p -> target at +0x%llx", tag, o, (void*)v, (unsigned long long)(target - v));
                ++hits;
            }
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        uint8_t* b = pass ? cr : obj;
        if (!b) continue;
        const size_t span = pass ? 0x6000 : 0x1000;
        check(b, span, pass ? "cr" : "obj");
        for (size_t o = 0; o < span; o += 8) {
            if (!Readable(b + o, 8)) break;
            uint8_t* q = *(uint8_t**)(b + o);
            if ((uintptr_t)q < 0x10000 || ((uintptr_t)q & 7) || !Readable(q, 0x800)) continue;
            char tag[32];
            sprintf_s(tag, "%s+0x%zx->", pass ? "cr" : "obj", o);
            check(q, 0x800, tag);
        }
    }
    Log("posfind: candidate %d at %p, %d pointers found", i, (void*)target, hits);
}

void PosTrack() {
    float yaw;
    if (!GameRoot(g_trackStart, &yaw)) return;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& c : g_cands) c.follow = c.frames = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_trackFrames = 120;
    Log("postrack: 120 frames - make Kratos move now");
}

void PosHold(int ms, int sel) {
    // CRASH 2026-09-30 21:56 (GoW.exe+0x9f1312, Lua VM): "poshold all" wrote the position into every scanned copy,
    // including Lua data - only ONE named candidate may be written now
    if (sel < 0) { Log("poshold: refused - name one candidate index (all / live wrote into game data and crashed)"); return; }
    float yaw;
    if (!GameRoot(g_holdPos, &yaw)) return;
    g_holdSel = sel;
    g_holdUntil = GetTickCount64() + (ULONGLONG)ms;
    g_holdLogged = false;
    Log("poshold: %d ms, candidates %s%d, at (%.3f %.3f %.3f)", ms, sel == -1 ? "all" : sel == -2 ? "live" : "#", sel,
        g_holdPos[0], g_holdPos[1], g_holdPos[2]);
}

void PosDiagTick() {
    float pos[3], yaw;
    const bool have = GameRoot(pos, &yaw);
    if (int f = g_trackFrames.load(); f > 0 && have) {
        g_trackFrames = f - 1;
        const float mx = pos[0] - g_trackStart[0], mz = pos[2] - g_trackStart[2];
        const bool moved = mx * mx + mz * mz > 0.3f * 0.3f;
        AcquireSRWLockExclusive(&g_lock);
        for (auto& c : g_cands) {
            if (!Readable(c.base + c.off, c.dbl ? 24 : 12)) continue;
            float v[3];
            CandRead(c, v);
            if (moved) {
                ++c.frames;
                if (fabsf(v[0] - pos[0]) < 0.05f && fabsf(v[2] - pos[2]) < 0.05f) ++c.follow;
            }
        }
        if (f == 1) {
            int live = 0;
            for (size_t i = 0; i < g_cands.size(); ++i)
                if (g_cands[i].frames && g_cands[i].follow * 2 > g_cands[i].frames) {  // live copies only
                    Log("postrack: [%zu] %s follows the moving root in %d of %d frames", i, g_cands[i].tag, g_cands[i].follow,
                        g_cands[i].frames);
                    ++live;
                }
            Log("postrack: done (%d live copies; root moved %.2f m)", live, sqrtf(mx * mx + mz * mz));
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (g_holdUntil) {
        const ULONGLONG now = GetTickCount64();
        if (now < g_holdUntil) {
            AcquireSRWLockShared(&g_lock);
            for (size_t i = 0; i < g_cands.size(); ++i) {
                const Cand& c = g_cands[i];
                if (g_holdSel >= 0 && (int)i != g_holdSel) continue;
                if (g_holdSel == -2 && !(c.frames && c.follow * 2 > c.frames)) continue;
                if (IsBadWritePtr(c.base + c.off, c.dbl ? 24 : 12)) continue;
                if (c.dbl) {  // horizontal only (gravity / floors stay the game's)
                    double* v = (double*)(c.base + c.off);
                    v[0] = g_holdPos[0]; v[2] = g_holdPos[2];
                } else {
                    float* v = (float*)(c.base + c.off);
                    v[0] = g_holdPos[0]; v[2] = g_holdPos[2];
                }
            }
            ReleaseSRWLockShared(&g_lock);
        } else if (!g_holdLogged && have) {
            g_holdLogged = true;
            g_holdUntil = 0;
            const float mx = pos[0] - g_holdPos[0], mz = pos[2] - g_holdPos[2];
            Log("poshold: done - game root travelled %.2f m while held", sqrtf(mx * mx + mz * mz));
        }
    }
}

}  // namespace gowvr::fp
