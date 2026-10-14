// Presentation: windowed swap chain, borderless frame, centring, cursor.
#pragma once

namespace present {

// Replacement for Direct3DCreate8, installed into game.exe's IAT.
//
// Declared as void* so that this header does not have to pull in d3d8.h: the
// return type is not part of the decorated name, so the definition in
// present.cpp (which does include it) links against this.
void* __stdcall Hooked_Direct3DCreate8(unsigned int version);

// Starts the window tidying thread, driven by the launcher's [Presentation]
// settings.
void Start();

}  // namespace present