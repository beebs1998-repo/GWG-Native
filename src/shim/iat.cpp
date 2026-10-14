// Import Address Table patching.
//
// Everything we need to intercept in game.exe is a *static* import, so we do
// not need a detour/inline-hook engine: rewriting the target's own IAT slots is
// both simpler and safer (no trampolines, no code patching).
//
// Note this only rewrites the *main module's* IAT. Our shim's own imports are
// untouched, so the shim can freely call the real CreateFileA/ReadFile/etc.
//
// Two subtleties worth keeping:
//   - sizeof(IMAGE_THUNK_DATA32) is 8 because the struct holds two unions, but
//     each entry in an on-disk PE32 import table is only 4 bytes. The loops
//     below step explicitly by 4.
//   - the IAT lives in .rdata, which the loader leaves read-only after it
//     resolves imports. The destination page has to be made writable first.

#include "iat.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

namespace {

constexpr size_t kThunkStride = 4;  // bytes per PE32 import table entry

// Resolves `fnName` inside `dllName` as loaded in this process. Only needed
// when the import has no OriginalFirstThunk, in which case names are no longer
// recoverable from the IAT and we must match on the resolved address.
void* GetRealProcAddress(const char* dllName, const char* fnName) {
    HMODULE mod = GetModuleHandleA(dllName);
    if (!mod) return NULL;
    return (void*)GetProcAddress(mod, fnName);
}

DWORD ReadThunk(const BYTE* table, size_t index) {
    DWORD value = 0;
    memcpy(&value, table + index * kThunkStride, sizeof(value));
    return value;
}

}  // namespace

bool PatchIAT(const char* dllName, const char* fnName, void* newAddr) {
    HMODULE base = GetModuleHandleA(NULL);
    if (!base) return false;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return false;

    void* realProc = GetRealProcAddress(dllName, fnName);

    IMAGE_IMPORT_DESCRIPTOR* desc =
        (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + dir->VirtualAddress);

    for (; desc->Name; ++desc) {
        const char* name = (const char*)((BYTE*)base + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;
        if (!desc->FirstThunk) continue;

        const BYTE* thunk = (const BYTE*)base + desc->FirstThunk;
        // After the loader has run, FirstThunk holds resolved addresses, so
        // names are only recoverable from OriginalFirstThunk. Without it we can
        // only match by comparing the resolved address against GetProcAddress.
        const BYTE* orig = desc->OriginalFirstThunk
                               ? (const BYTE*)base + desc->OriginalFirstThunk
                               : NULL;

        for (size_t i = 0; ReadThunk(thunk, i) != 0; ++i) {
            bool match = false;

            if (orig) {
                DWORD lookup = ReadThunk(orig, i);
                if (!(lookup & IMAGE_ORDINAL_FLAG32)) {
                    const IMAGE_IMPORT_BY_NAME* ibn =
                        (const IMAGE_IMPORT_BY_NAME*)((BYTE*)base + lookup);
                    if (strcmp((const char*)ibn->Name, fnName) == 0) match = true;
                }
            } else if (realProc) {
                if ((void*)(ULONG_PTR)ReadThunk(thunk, i) == realProc) match = true;
            }

            if (!match) continue;

            BYTE* dst = (BYTE*)base + desc->FirstThunk + i * kThunkStride;

            DWORD oldProtect = 0;
            if (!VirtualProtect(dst, sizeof(ULONG_PTR), PAGE_READWRITE,
                                &oldProtect)) {
                log::Write("iat: VirtualProtect failed for %s!%s (%lu)", dllName,
                           fnName, GetLastError());
                return false;
            }

            DWORD replacement = (DWORD)(ULONG_PTR)newAddr;
            memcpy(dst, &replacement, sizeof(replacement));

            DWORD ignored = 0;
            VirtualProtect(dst, sizeof(ULONG_PTR), oldProtect, &ignored);

            log::Write("iat: patched %s!%s at entry %u -> %p", dllName, fnName,
                       (unsigned)i, newAddr);
            return true;
        }
    }
    return false;
}