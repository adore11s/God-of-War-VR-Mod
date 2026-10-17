// gowvr entry point. Loaded as a version.dll proxy from the game directory.
// First log line is the RE-03 identity packet; a session without it is invalid.
#include <windows.h>
#include <ctime>
#include "core/log.h"
#include "core/target.h"
#include "core/detour.h"
#include "core/addr.h"
#include "frame/d3d_hooks.h"
#include "diag/cmd.h"
#include "diag/crashlog.h"
#include "stereo/lodprobe.h"
#include "input/vpad.h"

#ifndef GOWVR_VERSION
#define GOWVR_VERSION "dev"
#endif

namespace proxy { bool LoadReal(); }

namespace {
HMODULE g_self = nullptr;
bool g_armed = false;

DWORD WINAPI HashThread(void*) {
    char hex[65] = {};
    bool ok = gowvr::Sha256OfExe(hex);
    bool match = ok && strcmp(hex, gowvr::kGowSha256) == 0;
    gowvr::Log("identity: exe sha256=%s match=%d", ok ? hex : "<error>", match);
    if (!match) gowvr::Log("identity: not the recorded Steam build - the addresses come from the signature scan (addr: lines)");
    return 0;
}

void OnAttach(bool staticLoad) {
    gowvr::LogOpen();
    wchar_t self[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameW(g_self, self, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // link time of THIS dll (PE header): __DATE__/__TIME__ only changed when this file was recompiled
    char linked[32] = "?";
    {
        const auto* dos = (const IMAGE_DOS_HEADER*)g_self;
        const auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)g_self + dos->e_lfanew);
        __time64_t ts = nt->FileHeader.TimeDateStamp;
        tm lt{};
        if (_localtime64_s(&lt, &ts) == 0) strftime(linked, sizeof(linked), "%Y-%m-%d %H:%M:%S", &lt);
    }
    gowvr::Log("gowvr %s linked %s | dll=%ls | exe=%ls | pid=%lu | arch=x64", GOWVR_VERSION, linked, self, exe,
               GetCurrentProcessId());

    if (!proxy::LoadReal()) gowvr::Log("proxy: FAILED to load the real version.dll; version APIs will crash");

    gowvr::TargetInfo t = gowvr::InspectTarget();
    gowvr::Log("identity: base=%p timestamp=0x%08x sizeOfImage=0x%08x entry=0x%08x headerMatch=%d", (void*)t.base,
               t.timeDateStamp, t.sizeOfImage, t.entryRva, t.headerMatch);
    if (HANDLE h = CreateThread(nullptr, 0, HashThread, nullptr, 0, nullptr)) CloseHandle(h);

    // D-187: the recorded Steam build as it is; any other build (Epic, an update, an unverified copy) only when every
    // address the mod uses was found again by its signature - otherwise nothing is patched
    gowvr::AddrInit(t.headerMatch);
    if (!gowvr::AddrUsable()) {
        gowvr::Log("guard: %s -> fail closed, no hooks installed (flat game untouched)",
                   t.headerMatch ? "addresses not found" : "unknown GoW.exe build and not every address found by signature");
        return;
    }
    gowvr::diag::InstallCrashLog();  // D-099
    // D-110 (code audit M5): we are inside DllMain - GoW.exe's static import of version.dll, before its entry point, so
    // no game thread exists yet: the detours below patch without suspending threads (suspending loader threads under
    // the loader lock and then allocating could hang the launch)
    gowvr::DetourEarlyInstall(staticLoad);  // (a dynamic load - some injector - keeps the thread suspension)
    gowvr::diag::InstallCrashGuards();  // D-105
    gowvr::stereo::InstallLodHook();    // D-108
    gowvr::DetourEarlyInstall(false);
    gowvr::d3d::Install();
    gowvr::input::InstallVirtualPad();
    gowvr::diag::StartCommandChannel();
    g_armed = true;
}
}  // namespace

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OnAttach(reserved != nullptr);  // lpReserved non-null: loaded as a static import
    } else if (reason == DLL_PROCESS_DETACH) {
        gowvr::Log("detach: processTerminating=%d armed=%d presents=%llu", reserved != nullptr, g_armed,
                   (unsigned long long)gowvr::d3d::PresentCount());
        gowvr::LogClose();
    }
    return TRUE;
}
