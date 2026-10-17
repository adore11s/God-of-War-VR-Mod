// D-187: maps GoW.exe as an image (no code of it runs: DONT_RESOLVE_DLL_REFERENCES) and runs the mod's signature scan
// (src/core/addr.cpp) over it - every address must be found exactly at its recorded place.
//   addrtest.exe "C:\...\God of War 2018\GoW.exe"
#include "../../src/core/addr.h"
#include "../../src/core/log.h"
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: addrtest <GoW.exe>\n"); return 2; }
    gowvr::LogOpen();
    HMODULE m = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { wprintf(L"cannot map %s (%lu)\n", argv[1], GetLastError()); return 3; }
    const ULONGLONG t0 = GetTickCount64();
    const int bad = gowvr::AddrSelfTest((uintptr_t)m);
    wprintf(L"scan %llu ms: %s\n", GetTickCount64() - t0,
            bad < 0 ? L"NOT all addresses found" : bad == 0 ? L"all addresses found at their recorded places" : L"some differ");
    if (bad > 0) wprintf(L"%d addresses differ\n", bad);
    gowvr::LogClose();
    return bad == 0 ? 0 : 1;
}
