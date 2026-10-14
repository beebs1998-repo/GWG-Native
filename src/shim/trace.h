// Diagnostic: samples the instruction pointer of the game's main thread.
#pragma once

#include <windows.h>

namespace trace {

// Starts the sampler if GWG_NATIVE_TRACEPC=1. Safe to call more than once.
void Start();

}  // namespace trace