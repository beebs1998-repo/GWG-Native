// Virtual arcade joystick for GigaWing Generations.
#pragma once

#include <windows.h>

namespace arcade {

// Resolves the real DirectInput8Create from dinput8.dll. Must run before
// Hooked_DirectInput8Create is called.
bool Install();

}  // namespace arcade

// Replacement for DirectInput8Create, installed into game.exe's IAT.
// Note the parameter order: the instance handle comes first, the CLSID fourth.
HRESULT STDMETHODCALLTYPE Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dataType,
                                                    REFGUID guid, void** out,
                                                    LPUNKNOWN reserved);