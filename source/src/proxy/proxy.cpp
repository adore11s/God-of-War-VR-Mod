// Fills the export-stub pointers from the real System32 version.dll.
#include <windows.h>
#include "../core/log.h"

#define REAL(name) extern "C" void* g_real_##name = nullptr;
REAL(GetFileVersionInfoA) REAL(GetFileVersionInfoByHandle) REAL(GetFileVersionInfoExA)
REAL(GetFileVersionInfoExW) REAL(GetFileVersionInfoSizeA) REAL(GetFileVersionInfoSizeExA)
REAL(GetFileVersionInfoSizeExW) REAL(GetFileVersionInfoSizeW) REAL(GetFileVersionInfoW)
REAL(VerFindFileA) REAL(VerFindFileW) REAL(VerInstallFileA) REAL(VerInstallFileW)
REAL(VerLanguageNameA) REAL(VerLanguageNameW) REAL(VerQueryValueA) REAL(VerQueryValueW)
#undef REAL

namespace proxy {

bool LoadReal() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return false;
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return false;
    int missing = 0;
#define RESOLVE(name)                                                     \
    g_real_##name = (void*)GetProcAddress(real, #name);                   \
    if (!g_real_##name) { ++missing; gowvr::Log("proxy: missing export %s", #name); }
    RESOLVE(GetFileVersionInfoA) RESOLVE(GetFileVersionInfoByHandle) RESOLVE(GetFileVersionInfoExA)
    RESOLVE(GetFileVersionInfoExW) RESOLVE(GetFileVersionInfoSizeA) RESOLVE(GetFileVersionInfoSizeExA)
    RESOLVE(GetFileVersionInfoSizeExW) RESOLVE(GetFileVersionInfoSizeW) RESOLVE(GetFileVersionInfoW)
    RESOLVE(VerFindFileA) RESOLVE(VerFindFileW) RESOLVE(VerInstallFileA) RESOLVE(VerInstallFileW)
    RESOLVE(VerLanguageNameA) RESOLVE(VerLanguageNameW) RESOLVE(VerQueryValueA) RESOLVE(VerQueryValueW)
#undef RESOLVE
    gowvr::Log("proxy: real version.dll=%ls missing_exports=%d", path, missing);
    // GetFileVersionInfoByHandle is absent on some Windows builds; the others are required.
    return missing <= 1;
}

}  // namespace proxy
