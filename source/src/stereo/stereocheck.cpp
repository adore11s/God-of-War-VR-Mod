// D-336 stereo check: see stereocheck.h. Reads only (the game, its files, Windows) - nothing here changes the picture. The
// one per-frame cost is the GPU queue probe at Present (one event query issued, finished ones polled without a flush).
#include "stereocheck.h"
#include "dlsseyes.h"
#include "eyes.h"
#include "../core/addr.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/parts.h"
#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace gowvr::stereo {
namespace {

// ---- GPU queue probe (render thread) ----
constexpr int kQ = 8;
ID3D11Device* g_qDev = nullptr;
ID3D11DeviceContext* g_qCtx = nullptr;
ID3D11Query* g_q[kQ] = {};
uint64_t g_qIssued = 0, g_qDone = 0;
std::atomic<int> g_gpuQueued{-1};
std::atomic<uint64_t> g_gpuHist[4];  // frames still queued at Present: 0, 1, 2, 3+
// the logging's own cost (owner: "see if the new logging lags the game"): QPC ticks spent in the probe, and calls
std::atomic<uint64_t> g_probeTicks{0}, g_probeCalls{0};
double TicksToUs(uint64_t t) {
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
    return (double)t * 1e6 / f;
}
struct CostTimer {
    std::atomic<uint64_t>* sum;
    LARGE_INTEGER t0;
    explicit CostTimer(std::atomic<uint64_t>* s) : sum(s) { QueryPerformanceCounter(&t0); }
    ~CostTimer() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        *sum += (uint64_t)(t1.QuadPart - t0.QuadPart);
    }
};

void QueryRelease() {
    for (auto& q : g_q)
        if (q) { q->Release(); q = nullptr; }
    if (g_qCtx) { g_qCtx->Release(); g_qCtx = nullptr; }
    if (g_qDev) { g_qDev->Release(); g_qDev = nullptr; }
    g_qIssued = g_qDone = 0;
}

// ---- files ----
std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (wchar_t* s = wcsrchr(p, L'\\')) *s = 0;
    return p;
}

std::string ReadText(const std::wstring& path, size_t max) {
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return text;
    std::vector<char> buf(max);
    text.assign(buf.data(), fread(buf.data(), 1, max, f));
    fclose(f);
    return text;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// the game's settings.ini ("Key=Value   ; Values: ..." lines): every key=value, in order
std::vector<std::pair<std::string, std::string>> GameSettings(bool* readable) {
    std::vector<std::pair<std::string, std::string>> kv;
    const std::string text = ReadText(GameDir() + L"\\settings.ini", 65536);
    *readable = !text.empty();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        const size_t sc = line.find(';');
        if (sc != std::string::npos) line = line.substr(0, sc);
        line = Trim(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '[' || eq == std::string::npos || eq == 0) continue;
        kv.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return kv;
}

std::string Setting(const std::vector<std::pair<std::string, std::string>>& kv, const char* key) {
    for (const auto& p : kv)
        if (_stricmp(p.first.c_str(), key) == 0) return p.second;
    return "?";
}

// a long text over several log lines (the log line holds 2 KB), cut at a separator
void LogLong(const char* head, const std::string& text, char sep) {
    constexpr size_t kMax = 1500;
    size_t pos = 0;
    int part = 1;
    if (text.empty()) { Log("%s (none)", head); return; }
    while (pos < text.size()) {
        size_t len = text.size() - pos;
        if (len > kMax) {
            const size_t cut = text.rfind(sep, pos + kMax);
            len = cut != std::string::npos && cut > pos ? cut - pos + 1 : kMax;
        }
        Log("%s%s %s", head, part > 1 || len < text.size() - pos ? (" (" + std::to_string(part) + ")").c_str() : "",
            text.substr(pos, len).c_str());
        pos += len;
        ++part;
    }
}

// ---- stereo-relevant gowvr.ini keys and their built-in defaults ----
struct KeyDef { const char* key; float def; };
const KeyDef kStereoKeys[] = {
    {"afr_rendering", 1.f}, {"mono_rendering", 0.f}, {"dlss_per_eye", 1.f}, {"dlss_fixes_auto", 1.f},
    {"dlss_vr_phases", 0.f}, {"dlss_vr_sharpness", 0.f}, {"dlss_preset", 0.f}, {"eye_history_fix", 1.f},
    {"skin_per_frame", 1.f}, {"jitter_per_frame", 1.f}, {"taa_per_eye", 0.f}, {"sync_wait_ms", 1500.f},
    {"vr_lens_flares", 0.f}, {"particle_free_once", 0.f}, {"render_records_scale", 3.f}, {"mv_health", 1.f},
    {"parallel_eyes", 1.f}, {"crop_to_eye_fov", 2.f}, {"xr_dither", 1.f}, {"afr_reproject", 2.f},
    {"stream_list_scale", 4.f}, {"lod_scale", 1.f}, {"vr_texture_detail", -1.f}, {"vr_character_textures", 1.f},
    {"load_mip_guard", 1.f}, {"hold_bad_poses", 1.f}, {"signature_scan", 0.f}, {"verified_build_only", 0.f},
    {"render_size_auto", 1.f}, {"xr_loader", 1.f}, {"dev_plugin", 0.f}, {"stereo_gpu_probe", 1.f},
};

std::string ChangedStereoKeys() {
    std::string out;
    char b[96];
    for (const auto& k : kStereoKeys) {
        if (!config::HasKey(k.key)) continue;
        const float v = config::GetFloat(k.key, k.def);
        if (v == k.def) continue;
        sprintf_s(b, "%s%s=%g (default %g)", out.empty() ? "" : ", ", k.key, v, k.def);
        out += b;
    }
    return out;
}

// ---- this PC ----
std::string CpuText() {
    wchar_t name[128] = {};
    DWORD sz = sizeof(name);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, name, &sz);
    char out[256];
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    RTL_OSVERSIONINFOW os{sizeof(os)};
    using PFN_RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto f = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&os);
    sprintf_s(out, "CPU %ls (%lu logical cores), RAM %llu MB, Windows %lu.%lu build %lu", name[0] ? name : L"?",
              GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), (unsigned long long)(ms.ullTotalPhys >> 20), os.dwMajorVersion,
              os.dwMinorVersion, os.dwBuildNumber);
    return out;
}

// other DLLs in the game (not Windows' own, not this mod): overlays and injectors that hook rendering are flagged
std::string OtherDlls(std::string* hookers) {
    HMODULE mods[1024];
    DWORD need = 0;
    std::string out;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return "?";
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const size_t winLen = wcslen(win);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&OtherDlls, &self);
    const HMODULE exe = GetModuleHandleW(nullptr);
    static const char* kHook[] = {"reshade", "dxgi.dll", "d3d11.dll", "specialk", "sk64", "rtss", "gameoverlayrenderer",
                                  "discord", "graphics-hook", "overlay", "medal", "nvspcap", "dinput8.dll", "winmm.dll",
                                  "fraps", "bandicam", "action_x64", "afterburner", "xsplit", "lossless", "uevr"};
    const size_t n = need / sizeof(HMODULE);
    for (size_t i = 0; i < n && i < 1024; ++i) {
        if (mods[i] == self || mods[i] == exe) continue;
        wchar_t p[MAX_PATH] = {};
        if (!GetModuleFileNameW(mods[i], p, MAX_PATH)) continue;
        if (winLen && _wcsnicmp(p, win, winLen) == 0) continue;  // Windows' own (System32, WinSxS, the driver store)
        const wchar_t* base = wcsrchr(p, L'\\');
        base = base ? base + 1 : p;
        char b[MAX_PATH];
        sprintf_s(b, "%ls", base);
        for (char* c = b; *c; ++c) *c = (char)tolower((unsigned char)*c);
        if (!out.empty()) out += ", ";
        out += b;
        for (const char* k : kHook)
            if (strstr(b, k)) {
                if (!hookers->empty()) *hookers += ", ";
                *hookers += b;
                break;
            }
    }
    return out;
}

// SteamVR's own settings that change what reaches the eyes (motion smoothing = frames the runtime makes up itself)
std::string SteamVrSettings(bool* smoothingOn) {
    *smoothingOn = false;
    wchar_t steam[MAX_PATH] = {};
    DWORD sz = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) != ERROR_SUCCESS)
        return "Steam not found";
    const std::string text = ReadText(std::wstring(steam) + L"\\config\\steamvr.vrsettings", 1 << 20);
    if (text.empty()) return "steamvr.vrsettings not found";
    std::string out;
    for (const char* key : {"motionSmoothing", "motionSmoothingOverride", "supersampleScale", "supersampleManualOverride",
                            "preferredRefreshRate", "allowSupersampleFiltering", "enableHomeApp"}) {
        const std::string k = std::string("\"") + key + "\"";
        size_t pos = 0;
        int found = 0;
        std::string vals;
        while ((pos = text.find(k, pos)) != std::string::npos && found < 4) {
            pos += k.size();
            const size_t colon = text.find(':', pos);
            if (colon == std::string::npos) break;
            size_t e = text.find_first_of(",\r\n}", colon + 1);
            const std::string v = Trim(text.substr(colon + 1, e == std::string::npos ? std::string::npos : e - colon - 1));
            if (!strcmp(key, "motionSmoothing") && v == "true") *smoothingOn = true;
            vals += (found ? "/" : "") + v;
            ++found;
        }
        if (found) out += (out.empty() ? "" : ", ") + std::string(key) + "=" + vals;
    }
    // SteamVR's default for motion smoothing is ON when the key is absent
    if (text.find("\"motionSmoothing\"") == std::string::npos) {
        *smoothingOn = true;
        out += (out.empty() ? "" : ", ") + std::string("motionSmoothing not set (SteamVR default: on)");
    }
    return out.empty() ? "no frame-related keys" : out;
}

}  // namespace

void StereoCheckOnPresent(IDXGISwapChain* sc) {
    static const bool on = config::GetFloat("stereo_gpu_probe", 1.f) > 0.5f;
    if (!on || !sc) return;
    const CostTimer cost(&g_probeTicks);
    ++g_probeCalls;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    if (dev != g_qDev) {
        QueryRelease();
        g_qDev = dev;  // (the reference GetDevice gave is kept)
        dev->GetImmediateContext(&g_qCtx);
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        int made = 0;
        for (auto& q : g_q)
            if (SUCCEEDED(dev->CreateQuery(&qd, &q))) ++made; else q = nullptr;
        Log("stereo check: GPU queue probe on device %p (%d event queries)", (void*)dev, made);
        PartNote("GpuQueueProbe (D-336 log only)", made > 0, "no event queries");
    } else {
        dev->Release();
    }
    if (!g_qCtx) return;
    while (g_qDone < g_qIssued) {
        ID3D11Query* q = g_q[g_qDone % kQ];
        BOOL r = FALSE;
        if (q && g_qCtx->GetData(q, &r, sizeof(r), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        ++g_qDone;
    }
    const uint64_t queued = g_qIssued - g_qDone;
    g_gpuQueued = (int)(queued > 99 ? 99 : queued);
    ++g_gpuHist[queued > 3 ? 3 : queued];
    if (queued < (uint64_t)kQ) {
        if (ID3D11Query* q = g_q[g_qIssued % kQ]) g_qCtx->End(q);
        ++g_qIssued;
    }
}

int StereoGpuQueued() { return g_gpuQueued.load(std::memory_order_relaxed); }

void StereoCheckLog(bool stereo, bool first) {
    LARGE_INTEGER c0;
    QueryPerformanceCounter(&c0);
    if (first) {
        Log("stereo check: mod gowvr %s | game GoW.exe %s", GOWVR_VERSION, AddrBuildText());
        Log("stereo check: %s", CpuText().c_str());
        LogLong("stereo check: gowvr.ini as read -", config::KeysText(), ' ');
    }
    // what did not load
    char parts[1800];
    const int failed = PartsText(parts, sizeof(parts));
    Log("stereo check: parts - %s", parts);
    // the game's graphics settings, all of them
    bool readable = false;
    const auto gs = GameSettings(&readable);
    std::string all;
    for (const auto& p : gs) {
        if (_stricmp(p.first.c_str(), "WindowLeft") == 0 || _stricmp(p.first.c_str(), "WindowTop") == 0) continue;
        all += (all.empty() ? "" : " ") + p.first + "=" + p.second;
    }
    LogLong("stereo check: game settings.ini -", readable ? all : std::string("(not readable)"), ' ');
    // the upscaler and what each eye's temporal history is
    const uint8_t* dlssFlag = (const uint8_t*)GameVa(0x143e60c04);  // (dlsseyes.cpp kDlssOnVa)
    const bool dlss = dlssFlag && *dlssFlag == 1;
    const std::string fsr = Setting(gs, "FSRQuality");
    const bool fsrOn = !dlss && fsr != "?" && _stricmp(fsr.c_str(), "Off") != 0;
    char dl[384], ta[256];
    DlssStatsText(dl, sizeof(dl));
    TaaEyesStatsText(ta, sizeof(ta));
    const char* path = dlss ? "DLSS - each eye its own DLSS history, previous camera and skinned previous pose, one jitter "
                              "phase for both (the owner-tested path)"
                     : fsrOn ? "FSR - ONE FSR history for both eyes (no per-eye FSR), each eye's previous camera = the "
                               "other eye's (the pre-DLSS logic, D-242)"
                             : "game TAA - each eye's previous camera = the other eye's (the pre-DLSS logic, D-242)";
    Log("stereo check: upscaler %s (settings.ini DLSSQuality=%s FSRQuality=%s RenderScale=%s): %s | %s | %s",
        dlss ? "DLSS" : fsrOn ? "FSR" : "off (TAA)", Setting(gs, "DLSSQuality").c_str(), fsr.c_str(),
        Setting(gs, "RenderScale").c_str(), path, dl, ta);
    // rendering method now
    const bool mono = config::GetFloat("mono_rendering", 0.f) > 0.5f;
    const bool afr = !mono && config::GetFloat("afr_rendering", 1.f) > 0.5f;
    const char* method = mono ? "Mono" : afr ? "AFR" : "Native stereo";
    Log("stereo check: rendering method %s (%s)", method, stereo ? "stereo game view" : "flat screen now");
    // stereo keys changed from the built-in defaults
    const std::string changed = ChangedStereoKeys();
    Log("stereo check: stereo gowvr.ini keys changed from the defaults: %s", changed.empty() ? "none" : changed.c_str());
    // other DLLs, SteamVR's frame settings
    std::string hookers;
    const std::string dlls = OtherDlls(&hookers);
    LogLong("stereo check: other DLLs in the game -", dlls, ',');
    bool smoothing = false;
    const std::string svr = SteamVrSettings(&smoothing);
    Log("stereo check: SteamVR settings: %s", svr.c_str());
    // the render passes of the frame by class name (which effects run with this player's settings and scene)
    if (stereo) {
        static char passes[6000];
        const int count = R1PassNames(passes, sizeof(passes));
        LogLong(("stereo check: render passes (" + std::to_string(count) + ") -").c_str(), passes, ',');
    }
    // the verdict: what differs from the owner's tested setup (verified Steam build, every part loaded, DLSS, Native
    // stereo, no other renderer hooks)
    std::string diff;
    auto add = [&](const std::string& s) { diff += (diff.empty() ? "" : " | ") + s; };
    if (!AddrKnownBuild()) add("game build NOT the recorded Steam build (addresses found by signature)");
    if (failed) add(std::to_string(failed) + " part(s) NOT loaded (see 'parts')");
    if (!dlss) add(std::string("upscaler ") + (fsrOn ? "FSR" : "TAA") + " instead of DLSS");
    if (!changed.empty()) add("stereo keys changed in gowvr.ini");
    if (!hookers.empty()) add("other DLLs that may hook rendering: " + hookers);
    if (smoothing) add("SteamVR motion smoothing on (only matters on the SteamVR runtime)");
    if (mono || afr) add(std::string("rendering ") + method + " (the rays were reported in Native stereo)");
    LARGE_INTEGER c1;
    QueryPerformanceCounter(&c1);
    Log("stereo check: VERDICT - %s", diff.empty() ? "same as the owner-tested setup" : ("differs: " + diff).c_str());
    Log("stereo check: (this block took %.2f ms on the render thread, once per VR summary)",
        TicksToUs((uint64_t)(c1.QuadPart - c0.QuadPart)) / 1000.0);
}

void StereoHealthLog(int number, uint64_t headsetFrames, double displayMs) {
    LARGE_INTEGER c0;
    QueryPerformanceCounter(&c0);
    static uint64_t lastProbeT = 0, lastProbeN = 0;
    const uint64_t pt = g_probeTicks.load(), pn = g_probeCalls.load();
    const double probeUs = pn > lastProbeN ? TicksToUs(pt - lastProbeT) / (double)(pn - lastProbeN) : 0.0;
    lastProbeT = pt;
    lastProbeN = pn;
    static ULONGLONG lastT = 0;
    static uint64_t lastF = 0, lastGpu[4] = {};
    const ULONGLONG now = GetTickCount64();
    const double secs = lastT ? (now - lastT) / 1000.0 : 0.0;
    const double fps = secs > 0.0 ? (double)(headsetFrames - lastF) / secs : 0.0;
    lastT = now;
    lastF = headsetFrames;
    uint64_t g[4], gn = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t v = g_gpuHist[i].load();
        g[i] = v - lastGpu[i];
        lastGpu[i] = v;
        gn += g[i];
    }
    auto pct = [&](uint64_t a) { return gn ? 100.0 * (double)a / (double)gn : 0.0; };
    static char r1[2000];
    R1HealthText(r1, sizeof(r1));
    Log("stereo health %d: %s", number, r1);
    char dl[384], ta[256];
    DlssStatsText(dl, sizeof(dl));
    TaaEyesStatsText(ta, sizeof(ta));
    int loaded = 0, failed = 0;
    PartsCount(&loaded, &failed);
    // the runtime's frame period grows when it drops the app to a fraction of the panel's rate (it then makes up the other
    // frames by reprojection): the fastest period seen this session stands for the panel's own rate
    static double fastestMs = 0.0;
    if (displayMs > 0.0 && (fastestMs <= 0.0 || displayMs < fastestMs)) fastestMs = displayMs;
    const double hz = displayMs > 0.0 ? 1000.0 / displayMs : 0.0, panelHz = fastestMs > 0.0 ? 1000.0 / fastestMs : 0.0;
    char rate[256];
    if (secs <= 0.0) sprintf_s(rate, "headset frames: first check (the rate from the next one on)");
    else
        sprintf_s(rate, "headset frames %.1f per second, runtime pacing %.0f Hz (fastest seen %.0f Hz)%s", fps, hz, panelHz,
                  hz > 0.0 && panelHz > 1.4 * hz ? " - the runtime runs the game at a FRACTION of the headset rate and makes "
                                                   "up the rest (reprojection / motion smoothing)"
                  : hz > 0.0 && fps < 0.92 * hz ? " - BELOW the runtime's pacing (frames made up by reprojection)"
                                                : " - keeps up");
    Log("stereo health %d: %s | GPU frames queued at the game's Present: 0 %.0f%%, 1 %.0f%%, 2 %.0f%%, 3+ %.0f%% | %s | %s | "
        "parts %d loaded, %d NOT loaded",
        number, rate, pct(g[0]), pct(g[1]), pct(g[2]), pct(g[3]), dl, ta, loaded, failed);
    LARGE_INTEGER c1;
    QueryPerformanceCounter(&c1);
    Log("stereo health %d: logging cost - GPU probe %.1f us per frame, these lines %.2f ms (once per check)", number,
        probeUs, TicksToUs((uint64_t)(c1.QuadPart - c0.QuadPart)) / 1000.0);
}

}  // namespace gowvr::stereo
