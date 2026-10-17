#pragma once
#include <cstdint>

struct IDXGISwapChain;

namespace gowvr::diag {

// Request a backbuffer screenshot; written as gowvr_logs/shots/<name>.png at 1/`div` resolution.
void ShotRequest(const char* name, int div);
// Called from the Present hook (render thread) before the original Present.
void ShotOnPresent(IDXGISwapChain* sc);
// Called from the command thread: encodes a completed capture, if any. Returns true when a file was written.
bool ShotFlush(int timeoutMs);
// Encode a mapped 32-bit texture (R10G10B10A2 / RGBA8 / BGRA8) to gowvr_logs/shots/<name>.png at 1/div scale.
bool WritePng(const char* name, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t pitch, int format, int div);

}  // namespace gowvr::diag
