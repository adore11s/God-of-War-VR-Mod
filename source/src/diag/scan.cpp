// In-process memory search for copies of a frozen camera (diagnostic, read-only).
#include "scan.h"
#include "../core/log.h"
#include <windows.h>
#include <cstring>

namespace gowvr::diag {
namespace {
// Returns the first offset >= start where needle occurs, or -1. SEH-guarded: the region may be freed concurrently.
long long FindIn(const uint8_t* b, size_t n, size_t start, const void* needle, size_t nn) {
    __try {
        for (size_t off = start; off + nn <= n; off += 4)
            if (memcmp(b + off, needle, nn) == 0) return (long long)off;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}
}  // namespace

int ScanForCamera(const unsigned char* camWorld64) {
    if (!camWorld64) { Log("scan: no frozen camera (run 'freeze on' first)"); return 0; }
    float c[16];
    memcpy(c, camWorld64, 64);
    // Needles: engine layout row 0 (X basis, 0) and transposed row 0 as stored in viewData (C00, C10, C20, Tx),
    // plus the position triple alone.
    float engineRow0[4] = {c[0], c[1], c[2], c[3]};
    float cbRow0[4] = {c[0], c[4], c[8], c[12]};
    float pos[3] = {c[12], c[13], c[14]};
    struct Needle { const char* name; const void* p; size_t n; } needles[] = {
        {"engineRow0", engineRow0, 16}, {"cbRow0(transposed)", cbRow0, 16}, {"position", pos, 12}};
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    int hits = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi);
         p = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)))
            continue;
        const uint8_t* b = (const uint8_t*)mbi.BaseAddress;
        size_t n = mbi.RegionSize;
        for (auto& nd : needles) {
            for (long long off = FindIn(b, n, 0, nd.p, nd.n); off >= 0; off = FindIn(b, n, (size_t)off + 4, nd.p, nd.n)) {
                uintptr_t a = (uintptr_t)(b + off);
                if (++hits <= 200)
                    Log("scan: %s at %p region=%p type=0x%lx%s", nd.name, (void*)a, mbi.BaseAddress, mbi.Type,
                        (a >= exe && a < exe + 0x519f000) ? " (in GoW.exe image)" : "");
            }
        }
    }
    Log("scan: done, hits=%d", hits);
    return hits;
}

}  // namespace gowvr::diag
