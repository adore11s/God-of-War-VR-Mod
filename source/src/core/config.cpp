#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gowvr::config {
namespace {

std::mutex g_mu;
bool g_loaded = false;
std::vector<std::string> g_lines;                  // the file as last read / written (order and comments kept)
std::unordered_map<std::string, float> g_values;   // first "key=" line of each key, as GetFloat always read it
FILETIME g_stamp{};                                 // last-write time of the file we hold
ULONGLONG g_lastCheck = 0;

std::wstring Path() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    *wcsrchr(p, L'\\') = 0;
    return std::wstring(p) + L"\\gowvr.ini";
}

bool Stamp(FILETIME* ft) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(Path().c_str(), GetFileExInfoStandard, &d)) { *ft = FILETIME{}; return false; }
    *ft = d.ftLastWriteTime;
    return true;
}

std::vector<std::string> ReadLines() {
    std::vector<std::string> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, Path().c_str(), L"rb") || !f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        std::string s = buf;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        out.push_back(s);
    }
    fclose(f);
    return out;
}

void Parse() {
    g_values.clear();
    for (const auto& l : g_lines) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = l.substr(0, eq);
        if (!g_values.count(key)) g_values[key] = (float)atof(l.c_str() + eq + 1);
    }
}

void Load() {
    Stamp(&g_stamp);
    g_lines = ReadLines();
    Parse();
    g_loaded = true;
}

void EnsureLoaded() {
    if (!g_loaded) { Load(); return; }
    const ULONGLONG now = GetTickCount64();  // a hand edit while the game runs: re-read when the file changed
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    FILETIME ft;
    Stamp(&ft);
    if (CompareFileTime(&ft, &g_stamp) != 0) {
        Load();
        Log("config: gowvr.ini changed on disk - reloaded");
    }
}

// The whole file to gowvr.ini.tmp, then moved over gowvr.ini (one rename: the old or the new file, never half of one).
bool Write() {
    const std::wstring path = Path(), tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) { Log("config: cannot write gowvr.ini.tmp"); return false; }
    bool ok = true;
    for (const auto& l : g_lines) ok &= fprintf(f, "%s\r\n", l.c_str()) >= 0;
    ok &= fflush(f) == 0;
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log("config: replacing gowvr.ini failed (%lu)", GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    Stamp(&g_stamp);
    return true;
}

void SetLocked(const char* key, float value) {
    char nv[128];
    sprintf_s(nv, "%s=%g", key, value);
    const size_t kl = strlen(key);
    bool replaced = false;
    for (auto& l : g_lines)
        if (l.size() > kl && l.compare(0, kl, key) == 0 && l[kl] == '=') { l = nv; replaced = true; }
    if (!replaced) g_lines.push_back(nv);
    g_values[key] = value;
}

}  // namespace

float GetFloat(const char* key, float def) {
    std::lock_guard<std::mutex> lk(g_mu);
    EnsureLoaded();
    const auto it = g_values.find(key);
    return it == g_values.end() ? def : it->second;
}

void SetFloats(const std::vector<std::pair<const char*, float>>& values) {
    if (values.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_loaded) Load();
    else {  // keep a hand edit made since the last read
        FILETIME ft;
        Stamp(&ft);
        if (CompareFileTime(&ft, &g_stamp) != 0) Load();
    }
    for (const auto& kv : values) SetLocked(kv.first, kv.second);
    if (Write()) {
        if (values.size() == 1) Log("config: %s=%g", values[0].first, values[0].second);
        else Log("config: %zu values saved (%s=%g ...)", values.size(), values[0].first, values[0].second);
    }
}

void SetFloat(const char* key, float value) { SetFloats({{key, value}}); }

bool CreateIfMissing(const char* text) {
    if (!text || !*text || GetFileAttributesW(Path().c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, Path().c_str(), L"wb") || !f) { Log("config: could not create gowvr.ini"); return false; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    std::lock_guard<std::mutex> lk(g_mu);
    g_loaded = false;  // read from the new file at the next GetFloat
    Log("config: gowvr.ini created with the default VR settings (first start)");
    return true;
}

}  // namespace gowvr::config
