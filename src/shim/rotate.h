// Rotation: show the game's portrait picture upright in a landscape window.
//
// The game draws its 480x640 portrait picture turned on its side into a 640x480
// back buffer, so what reaches the screen is already sideways. The game window
// is captured, the picture is turned the other way so it reads upright, and it
// is scaled uniformly into a borderless window covering the desktop, centred
// with black bars down the sides.
//
// Capture is by BitBlt of the game window, not by reading the back buffer, and
// both reasons matter. The back buffer cannot be had: the game never asks for it
// (it renders implicitly, so there is no GetBackBuffer call to hook), and this
// Direct3D 8 runtime's method order cannot be read off the headers.
//
// The window cannot simply be blitted while completely covered either. Once a
// Direct3D window is entirely hidden the driver stops presenting into it and a
// BitBlt of it comes back blank. That is why `sliver` exists: the game window is
// parked against the bottom of the screen and this window stops short of the
// bottom edge by that many pixels, leaving a strip of the game window uncovered
// and therefore still being presented into. It cannot be zero. PrintWindow would
// remove the need for it and does return the right picture, but it deadlocks the
// process - see the comment at the capture site.
//
// The output half is plain Win32, so nothing here depends on the Direct3D 8
// method order at all - the whole difficulty the device proxy in present.cpp
// exists to work around.
#pragma once

#include <windows.h>

namespace rotate {

// Direction the captured picture is turned. The game draws its portrait art
// rotated one way, so this is a correction rather than a choice of axis.
enum Direction {
    kCounterClockwise = 0,  // default: reads upright
    kClockwise = 1,
};

struct Options {
    bool enabled = false;
    int fps = 60;
    Direction direction = kCounterClockwise;
    bool smooth = false;  // smooth scaling instead of preserving hard pixels
    int sliver = 2;       // pixels of game window left uncovered, see rotate.cpp
};

// Creates the presentation window and starts the compositing thread. Safe to
// call once; does nothing unless enabled. Returns the window created, or null.
HWND Start(HWND gameWnd, const Options& options);

// Capture is done by the caller, on its own thread, for the frame it has just
// finished drawing. Doing it here rather than from the compositing thread is
// deliberate: reaching into another thread's Direct3D window with GDI while that
// thread is activating or presenting is what hangs the process, and the caller
// is the thread that owns the window and knows the frame is complete.
//
// BeginCapture returns a DC to blit the game window into, or null if the
// previous frame has not been collected yet. EndCapture must be called if and
// only if BeginCapture returned a DC.
HDC BeginCapture(int width, int height);
void EndCapture();

// Puts the presentation window back above the game window if it has drifted
// below it. Call from the thread that created it; see rotate.cpp.
void KeepAboveGame();

}  // namespace rotate