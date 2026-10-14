// Import Address Table patching helpers.
#pragma once

#include <windows.h>

// Redirects `fnName` imported from `dllName` by the *main executable* to
// `newAddr`. Returns true if the slot was found and patched.
bool PatchIAT(const char* dllName, const char* fnName, void* newAddr);