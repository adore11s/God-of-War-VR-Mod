#pragma once
#include <cstdint>

namespace gowvr {

// The one build every raw RVA in this project is scoped to (ADDRESS_REGISTRY.md, "Supported builds").
constexpr uint32_t kGowTimeDateStamp = 0x628e8031;
constexpr uint32_t kGowSizeOfImage   = 0x0519f000;
constexpr uint32_t kGowEntryRva      = 0x003ec680;
constexpr const char* kGowSha256 = "a3b83b8747d44e1162f5abf951c9901a5e0aced9b9ff7df47c931849b5eaf4f7";

struct TargetInfo {
    uintptr_t base = 0;
    uint32_t timeDateStamp = 0, sizeOfImage = 0, entryRva = 0;
    bool headerMatch = false;  // fast guard, checked in DllMain before any patching
};

TargetInfo InspectTarget();
// Full-file SHA-256 of the running executable (slow: call off the loader thread). Returns lowercase hex.
bool Sha256OfExe(char out[65]);

}  // namespace gowvr
