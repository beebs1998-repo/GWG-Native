// Records which thread is the game's main thread.
//
// The PC sampler (trace.cpp) needs this: a thread's Win32 start address points
// into kernel32's initialisation thunk, so threads cannot be told apart by
// enumeration, and the game does not create its window through a call we hook.
//
// The hardware bring-up (opening COM2) and DirectInput setup both run on the
// main thread during start-up, so whichever is seen first identifies it. The
// first caller wins.
#pragma once

#include <windows.h>

inline DWORD& GameMainThreadId() {
    static DWORD id = 0;
    return id;
}

// Records the calling thread unless it is the shim's own worker, which would
// make the sampler chase itself.
inline void NotePossibleMainThread() {
    DWORD id = GameMainThreadId();
    if (id) return;
    GameMainThreadId() = GetCurrentThreadId();
}