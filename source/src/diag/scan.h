#pragma once

namespace gowvr::diag {

// Logs every readable address holding the frozen camera (engine layout row, transposed CB row, position).
int ScanForCamera(const unsigned char* camWorld64);

}  // namespace gowvr::diag
