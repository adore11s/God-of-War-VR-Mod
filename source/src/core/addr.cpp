// D-187: game addresses for any build of GoW.exe - see addr.h. The approach follows the Detroit VR mod the owner pointed
// to: the known build by its identity, any other one by byte signatures, each address reported as at its recorded place /
// MOVED / missing / ambiguous, and nothing patched unless every address was found unambiguously.
#include "addr.h"
#include "log.h"
#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gowvr {
namespace {

struct Sig {
    uint64_t va;
    int kind;  // 0 code: match + dispOff / 1 referenced: match + insnEnd + int32 at match + dispOff + delta / 2 section
    int dispOff, insnEnd;
    int64_t delta;
    const char* pat;
};
const Sig kSigs[] = {
#include "addrsigs.inc"
};
constexpr size_t kN = sizeof(kSigs) / sizeof(kSigs[0]);

struct Parsed {
    std::vector<uint8_t> b, m;  // bytes and mask (1 = must match)
    int anchor = -1;            // first index of two consecutive fixed bytes
};

bool g_known = false, g_usable = false;
char g_buildText[192] = "not checked yet";  // the outcome in one line (VR summary in the log)
uintptr_t g_base = 0;
uintptr_t g_resolved[kN] = {};
Parsed g_parsed[kN];

Parsed Parse(const char* s) {
    Parsed p;
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') { p.b.push_back(0); p.m.push_back(0); s += 2; continue; }
        char h[3] = {s[0], s[1], 0};
        p.b.push_back((uint8_t)strtoul(h, nullptr, 16));
        p.m.push_back(1);
        s += 2;
    }
    for (size_t i = 0; i + 1 < p.b.size(); ++i)
        if (p.m[i] && p.m[i + 1]) { p.anchor = (int)i; break; }
    return p;
}

bool MatchAt(const Parsed& p, const uint8_t* at) {
    for (size_t i = 0; i < p.b.size(); ++i)
        if (p.m[i] && at[i] != p.b[i]) return false;
    return true;
}

uintptr_t TargetOf(const Sig& s, const uint8_t* match) {
    if (s.kind == 0) return (uintptr_t)match + s.dispOff;
    int32_t d;
    memcpy(&d, match + s.dispOff, 4);
    return (uintptr_t)match + s.insnEnd + d + s.delta;
}

const IMAGE_SECTION_HEADER* Section(const char* name) {
    auto* dos = (const IMAGE_DOS_HEADER*)g_base;
    auto* nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp((const char*)sec[i].Name, name, 8) == 0) return &sec[i];
    return nullptr;
}

void Scan() {
    const IMAGE_SECTION_HEADER* text = Section(".text");
    if (!text) { Log("addr: no .text section"); return; }
    const uint8_t* lo = (const uint8_t*)g_base + text->VirtualAddress;
    const size_t len = text->Misc.VirtualSize;
    // buckets by the anchor pair
    std::vector<std::vector<int>> bucket(65536);
    for (size_t i = 0; i < kN; ++i) {
        g_parsed[i] = Parse(kSigs[i].pat);
        if (kSigs[i].kind != 2 && g_parsed[i].anchor >= 0) {
            const Parsed& p = g_parsed[i];
            bucket[(p.b[p.anchor] << 8) | p.b[p.anchor + 1]].push_back((int)i);
        }
    }
    std::vector<int> hits(kN, 0);
    std::vector<uintptr_t> first(kN, 0);
    std::vector<bool> agree(kN, true);
    for (size_t pos = 0; pos + 1 < len; ++pos) {
        const auto& cand = bucket[(lo[pos] << 8) | lo[pos + 1]];
        for (int i : cand) {
            const Parsed& p = g_parsed[i];
            if (pos < (size_t)p.anchor) continue;
            const uint8_t* at = lo + pos - p.anchor;
            if (at + p.b.size() > lo + len || !MatchAt(p, at)) continue;
            const uintptr_t t = TargetOf(kSigs[i], at);
            if (hits[i] == 0) first[i] = t;
            else if (t != first[i]) agree[i] = false;  // two places that lead to different addresses
            ++hits[i];
        }
    }
    int same = 0, moved = 0, missing = 0, ambiguous = 0;
    for (size_t i = 0; i < kN; ++i) {
        const Sig& s = kSigs[i];
        const uintptr_t recorded = g_base + (uintptr_t)(s.va - 0x140000000ull);
        if (s.kind == 2) {
            char name[9] = {};
            memcpy(name, s.pat, strlen(s.pat) < 8 ? strlen(s.pat) : 8);
            const IMAGE_SECTION_HEADER* sec = Section(name);
            g_resolved[i] = sec ? g_base + sec->VirtualAddress : 0;
        } else if (hits[i] > 0 && agree[i]) {
            g_resolved[i] = first[i];
        }
        if (!g_resolved[i]) {
            if (hits[i] > 1) {
                ++ambiguous;
                Log("addr:   0x%llx ambiguous - its signature leads to %d places", (unsigned long long)s.va, hits[i]);
            } else {
                ++missing;
                Log("addr:   0x%llx missing - no match for its signature", (unsigned long long)s.va);
            }
        } else if (g_resolved[i] == recorded) {
            ++same;
        } else {
            ++moved;
            Log("addr:   0x%llx MOVED -> 0x%llx", (unsigned long long)s.va,
                (unsigned long long)(g_resolved[i] - g_base + 0x140000000ull));
        }
    }
    g_usable = missing == 0 && ambiguous == 0;
    Log("addr: %zu addresses - %d at their recorded place, %d moved, %d missing, %d ambiguous -> %s", kN, same, moved,
        missing, ambiguous, g_usable ? "all found, the mod runs" : "NOT all found, the mod stays off (flat game untouched)");
    sprintf_s(g_buildText, "%s: %zu addresses - %d at their recorded place, %d moved, %d missing, %d ambiguous",
              g_known ? "recorded Steam build, signature scan forced" : "UNVERIFIED build, found by signature", kN, same,
              moved, missing, ambiguous);
}

int Find(uint64_t va) {
    size_t lo = 0, hi = kN;  // kSigs is sorted by va (the generator writes it sorted)
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kSigs[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return lo < kN && kSigs[lo].va == va ? (int)lo : -1;
}

}  // namespace

void AddrInit(bool knownBuild) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    g_known = knownBuild;
    const bool force = config::GetFloat("signature_scan", 0.f) > 0.5f;
    if (knownBuild && !force) {
        g_usable = true;
        Log("addr: GAME BUILD VERIFIED (the recorded Steam build) - %zu recorded addresses used as they are", kN);
        sprintf_s(g_buildText, "VERIFIED (the recorded Steam build of 2022-05-25)");
        return;
    }
    if (!knownBuild && config::GetFloat("verified_build_only", 0.f) > 0.5f) {
        g_usable = false;
        Log("addr: UNVERIFIED build and gowvr.ini verified_build_only=1 - the mod stays off");
        sprintf_s(g_buildText, "UNVERIFIED build, verified_build_only=1 - mod off");
        return;
    }
    const ULONGLONG t0 = GetTickCount64();
    Log("addr: %s - finding the %zu addresses by their signatures", knownBuild ? "signature scan forced (signature_scan=1)"
                                                                            : "UNVERIFIED build", kN);
    Scan();
    if (knownBuild) g_known = false;  // (forced scan: use what the scan found, like another build would)
    Log("addr: signature scan took %llu ms", (unsigned long long)(GetTickCount64() - t0));
}

// tools/addrtest: the scan over another mapped copy of GoW.exe; returns how many addresses differ from the recorded ones
// (-1 = not all found)
int AddrSelfTest(uintptr_t imageBase) {
    g_base = imageBase;
    g_known = false;
    Scan();
    if (!g_usable) return -1;
    int bad = 0;
    for (size_t i = 0; i < kN; ++i)
        if (g_resolved[i] != g_base + (uintptr_t)(kSigs[i].va - 0x140000000ull)) ++bad;
    return bad;
}

bool AddrUsable() { return g_usable; }
bool AddrKnownBuild() { return g_known; }
const char* AddrBuildText() { return g_buildText; }

uintptr_t GameVa(uint64_t staticVa) {
    if (g_known) return g_base + (uintptr_t)(staticVa - 0x140000000ull);
    const int i = Find(staticVa);
    if (i >= 0) return g_resolved[i];
    // an address computed from a recorded one (a table + index * stride, a struct field): relative to the nearest
    // recorded address at or below it, within 64 KB
    size_t lo = 0, hi = kN;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kSigs[mid].va <= staticVa) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && g_resolved[lo - 1] && staticVa - kSigs[lo - 1].va < 0x10000)
        return g_resolved[lo - 1] + (uintptr_t)(staticVa - kSigs[lo - 1].va);
    static LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 20)
        Log("addr: 0x%llx is not in the signature table - not usable on this build", (unsigned long long)staticVa);
    return 0;
}

bool AddrGuardMayDiffer(uint64_t staticVa, int i) {
    if (g_known) return false;
    const int k = Find(staticVa);
    if (k < 0 || kSigs[k].kind != 0) return false;
    const Parsed& p = g_parsed[k];
    const size_t at = (size_t)kSigs[k].dispOff + (size_t)i;
    return at < p.m.size() && !p.m[at];  // a wildcarded (moving) byte; beyond the pattern it must match
}

}  // namespace gowvr
