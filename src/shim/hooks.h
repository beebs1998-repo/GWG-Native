// Hook installation for the injected shim.
#pragma once

#include <windows.h>

namespace hooks {

// Redirects the intercepted APIs in game.exe's IAT. Returns true if every
// entry point was found.
bool Install();

// Restores the original entry points (used on process detach / clean exit).
void Uninstall();


}  // namespace hooks