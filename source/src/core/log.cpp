#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>

namespace gowvr {
namespace {
CRITICAL_SECTION g_lock;
FILE* g_file = nullptr;
LARGE_INTEGER g_freq, g_start;
// D-112: the last lines, for the crash report (written under g_lock; read by the crash handler)
constexpr int kRing = 80;
char g_ring[kRing][320];
unsigned g_ringN = 0;
}  // namespace

double NowMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_start.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

void LogOpen() {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);

    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    wcscat_s(dir, L"\\gowvr_logs");
    CreateDirectoryW(dir, nullptr);

    wchar_t cur[MAX_PATH], prev[MAX_PATH];
    swprintf_s(cur, L"%s\\gowvr.log", dir);
    swprintf_s(prev, L"%s\\gowvr.prev.log", dir);
    MoveFileExW(cur, prev, MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(cur, L"w", _SH_DENYWR);
}

void LogClose() {
    EnterCriticalSection(&g_lock);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    LeaveCriticalSection(&g_lock);
}

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    if (g_file) {
        fprintf(g_file, "[%10.3f][t%5lu] %s\n", NowMs(), GetCurrentThreadId(), buf);
        fflush(g_file);
    }
    snprintf(g_ring[g_ringN % kRing], sizeof(g_ring[0]), "[%10.3f][t%5lu] %s", NowMs(), GetCurrentThreadId(), buf);
    ++g_ringN;
    LeaveCriticalSection(&g_lock);
}

int LogRecent(void (*sink)(const char* line, void* ctx), void* ctx) {
    // a crashing thread may hold the lock (a fault inside Log): read without it then
    const bool locked = TryEnterCriticalSection(&g_lock) != 0;
    const unsigned n = g_ringN, first = n > (unsigned)kRing ? n - kRing : 0;
    for (unsigned i = first; i < n; ++i) sink(g_ring[i % kRing], ctx);
    if (locked) LeaveCriticalSection(&g_lock);
    return (int)(n - first);
}

}  // namespace gowvr
