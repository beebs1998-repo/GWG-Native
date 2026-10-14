// Diagnostic: measure which IDirect3DDevice8 vtable slots the game calls.
#pragma once

namespace devprobe {

// Wraps the game's device in counting stubs, so the log records which vtable
// slots it actually uses. Returns the device unchanged unless
// GWG_NATIVE_DEVPROBE=1, so this is inert by default.
void* Wrap(void* device);

// True once the results have been written to the log.
bool Done();

}  // namespace devprobe