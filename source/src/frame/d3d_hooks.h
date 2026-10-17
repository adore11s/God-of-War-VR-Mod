#pragma once
#include <cstdint>

namespace gowvr::d3d {

// Patches GoW.exe's own imports (D3D11CreateDevice, CreateDXGIFactory1, GetProcAddress). Call only after the
// target guard passed.
void Install();
uint64_t PresentCount();
// Log the RE-04 frame packet for the next n Presents.
void LogPackets(int n);

extern bool g_xrMono;  // diagnostic `xrmono on`

}  // namespace gowvr::d3d
