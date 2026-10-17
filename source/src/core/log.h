#pragma once
#include <cstdint>

namespace gowvr {

// Opens <gameDir>\gowvr_logs\gowvr.log, rotating the previous run to gowvr.prev.log.
void LogOpen();
void LogClose();
// printf-style, one line, timestamped with ms since LogOpen and the calling thread id. Flushed per line.
void Log(const char* fmt, ...);
double NowMs();
// D-112: the last (up to 80) log lines, oldest first, handed to `sink` - for the crash report. Returns how many.
int LogRecent(void (*sink)(const char* line, void* ctx), void* ctx);

}  // namespace gowvr
