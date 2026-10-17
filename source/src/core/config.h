#pragma once
#include <utility>
#include <vector>

namespace gowvr::config {

// gowvr.ini next to GoW.exe: one "key=value" per line (other lines - comments - are kept as they are).
// D-110 (code audit M1): read once into memory; GetFloat is a lookup (it used to open and parse the whole file on every
// call - every frame from the game thread). A hand edit of the file while the game runs is picked up within a second.
float GetFloat(const char* key, float def);
// D-110 (code audit M8): written to gowvr.ini.tmp and moved over gowvr.ini - a crash mid-write no longer empties it.
void SetFloat(const char* key, float value);
// Several keys in ONE write (the learned grips: 13 values).
void SetFloats(const std::vector<std::pair<const char*, float>>& values);

}  // namespace gowvr::config
