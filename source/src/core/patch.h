#pragma once
#include <windows.h>
#include <cstdint>

namespace gowvr {

// Replaces module's import slot dll!func with hook. Returns false (and patches nothing) if not imported.
bool PatchImport(HMODULE module, const char* dll, const char* func, void* hook, void** original);
// D-216: every import slot of `module` for a function of any DLL whose name starts with `dllPrefix` (case-insensitive),
// matched by ordinal OR by name (0 / null = not by that), in the normal AND the delay-load import tables. The first
// real function found is stored in *original (a delay-load slot resolves it itself). Returns how many slots it patched.
int PatchImportAny(HMODULE module, const char* dllPrefix, uint16_t ordinal, const char* name, void* hook, void** original);
// D-287: the same slots, where another module wrote its own hook over `hook` later (seen with Valve Index controllers:
// the VR controllers did nothing in the Xbox pad mode): each is set back to `hook`. *foreign gets the pointer found
// there (the first one), for the caller to chain to. Pointers into `module` itself (a delay-load helper) are left.
// Returns how many slots were taken back.
int ReclaimImportAny(HMODULE module, const char* dllPrefix, uint16_t ordinal, const char* name, void* hook, void** foreign);
// Replaces vtable[index] with hook unless it already is hook. Stores the previous entry in *original once.
bool PatchVtable(void* object, int index, void* hook, void** original);

}  // namespace gowvr
