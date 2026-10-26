#include "patch.h"
#include "log.h"
#include <cstdlib>
#include <cstring>

namespace gowvr {
namespace {
bool WritePointer(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
}  // namespace

bool PatchImport(HMODULE module, const char* dll, const char* func, void* hook, void** original) {
    auto base = (uint8_t*)module;
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    // "#<n>" selects an import by ordinal (GoW imports XInput by ordinal).
    const bool byOrdinal = func[0] == '#';
    const uint64_t ordinal = byOrdinal ? strtoull(func + 1, nullptr, 10) : 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                if (!byOrdinal || IMAGE_ORDINAL64(names->u1.Ordinal) != ordinal) continue;
            } else {
                if (byOrdinal) continue;
                auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                if (strcmp((const char*)byName->Name, func) != 0) continue;
            }
            void** slot = (void**)&slots->u1.Function;
            if (*slot == hook) return true;
            *original = *slot;
            return WritePointer(slot, hook);
        }
    }
    return false;
}

namespace {
// every import slot of `module` for dllPrefix* ! (ordinal or name), normal and delay-load tables: f(dll, slot, delay)
template <class F> void ForImportSlots(HMODULE module, const char* dllPrefix, uint16_t ordinal, const char* name, F&& f) {
    auto base = (uint8_t*)module;
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    const size_t plen = strlen(dllPrefix);
    // one thunk table: names (ordinal or by-name) + the slots they fill
    auto table = [&](const char* dll, IMAGE_THUNK_DATA64* names, IMAGE_THUNK_DATA64* slots, bool delay) {
        for (; names->u1.AddressOfData; ++names, ++slots) {
            bool match;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                match = ordinal && IMAGE_ORDINAL64(names->u1.Ordinal) == ordinal;
            } else {
                auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                match = name && strcmp((const char*)byName->Name, name) == 0;
            }
            if (match) f(dll, (void**)&slots->u1.Function, delay);
        }
    };
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress)
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
            const char* dll = (const char*)(base + imp->Name);
            if (_strnicmp(dll, dllPrefix, plen) != 0) continue;
            table(dll, (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk)),
                  (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk), false);
        }
    auto& ddir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (ddir.VirtualAddress)
        for (auto d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + ddir.VirtualAddress); d->DllNameRVA; ++d) {
            const char* dll = (const char*)(base + d->DllNameRVA);
            if (_strnicmp(dll, dllPrefix, plen) != 0 || !d->ImportNameTableRVA || !d->ImportAddressTableRVA) continue;
            table(dll, (IMAGE_THUNK_DATA64*)(base + d->ImportNameTableRVA), (IMAGE_THUNK_DATA64*)(base + d->ImportAddressTableRVA),
                  true);
        }
}
}  // namespace

int PatchImportAny(HMODULE module, const char* dllPrefix, uint16_t ordinal, const char* name, void* hook, void** original) {
    int patched = 0;
    ForImportSlots(module, dllPrefix, ordinal, name, [&](const char* dll, void** slot, bool delay) {
        if (*slot == hook) { ++patched; return; }
        void* real = *slot;
        if (delay) {  // the slot holds the delay-load helper until the first call: resolve the function now
            HMODULE m = LoadLibraryA(dll);
            real = m ? (void*)GetProcAddress(m, name ? name : MAKEINTRESOURCEA(ordinal)) : nullptr;
            if (!real && m && ordinal) real = (void*)GetProcAddress(m, MAKEINTRESOURCEA(ordinal));
            if (!real) return;
        }
        if (!*original) *original = real;
        if (WritePointer(slot, hook)) ++patched;
    });
    return patched;
}

int ReclaimImportAny(HMODULE module, const char* dllPrefix, uint16_t ordinal, const char* name, void* hook, void** foreign) {
    int taken = 0;
    auto base = (uint8_t*)module;
    const size_t size = ((IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    ForImportSlots(module, dllPrefix, ordinal, name, [&](const char*, void** slot, bool) {
        void* p = *slot;
        if (!p || p == hook || ((uint8_t*)p >= base && (uint8_t*)p < base + size)) return;
        if (!*foreign) *foreign = p;
        if (WritePointer(slot, hook)) ++taken;
    });
    return taken;
}

bool PatchVtable(void* object, int index, void* hook, void** original) {
    void** vtable = *(void***)object;
    if (vtable[index] == hook) return true;
    if (!*original) *original = vtable[index];
    else if (vtable[index] != *original) {
        // A second concrete class (or another hooker) owns this slot: record it, do not chain blindly.
        Log("patch: vtable %p[%d] = %p differs from saved original %p; not patched", vtable, index,
            vtable[index], *original);
        return false;
    }
    return WritePointer(&vtable[index], hook);
}

}  // namespace gowvr
