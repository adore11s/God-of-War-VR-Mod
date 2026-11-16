#pragma once
#include <cstddef>

namespace gowvr {

// D-336 (owner: "people see laser lights / god rays out of flames in Native stereo that I never see, even PS VR2 players -
// make detailed logging so their log says what didn't load"): every hook / patch / module part the mod sets up reports its
// outcome here. The detours report themselves (core/detour.cpp); other parts call PartNote where they know. The VR summary
// and the once-a-minute VR check list what did NOT load, by name and reason, in one place.
// The latest outcome of a name counts (a part retried later and then loaded is fine). Not for DllMain-suspended windows:
// it allocates (call it after the threads are resumed).
void PartNote(const char* name, bool ok, const char* why = nullptr);
// "N parts loaded, M NOT loaded: name (why); ..." - returns M
int PartsText(char* out, size_t n);
// how many parts reported, and how many of them failed
void PartsCount(int* loaded, int* failed);

}  // namespace gowvr
