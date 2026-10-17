#include "target.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstdio>

namespace gowvr {

TargetInfo InspectTarget() {
    TargetInfo t;
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    t.base = (uintptr_t)base;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    t.timeDateStamp = nt->FileHeader.TimeDateStamp;
    t.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    t.entryRva = nt->OptionalHeader.AddressOfEntryPoint;
    t.headerMatch = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                    t.timeDateStamp == kGowTimeDateStamp && t.sizeOfImage == kGowSizeOfImage &&
                    t.entryRva == kGowEntryRva;
    return t;
}

bool Sha256OfExe(char out[65]) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    uint8_t digest[32];
    ok = ok && BCryptFinishHash(h, digest, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (ok)
        for (int i = 0; i < 32; ++i) sprintf_s(out + i * 2, 3, "%02x", digest[i]);
    return ok;
}

}  // namespace gowvr
