#include "rotate.h"

#include <windows.h>
#include <string.h>

#include "log.h"

namespace rotate {
namespace {

const wchar_t kClassName[] = L"GWGRotateOverlay";

// A 32bpp top-down DIB we address directly. Turning the picture does a full
// frame pixel shuffle, which through GetPixel/SetPixel would be far too slow.
struct Bitmap {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    DWORD* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool Create(HDC reference, int w, int h) {
        if (w <= 0 || h <= 0) return false;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;  // negative: top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        bitmap = ::CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits,
                                    nullptr, 0);
        if (!bitmap || !bits) return false;

        dc = ::CreateCompatibleDC(reference);
        if (!dc) {
            ::DeleteObject(bitmap);
            bitmap = nullptr;
            return false;
        }

        old = ::SelectObject(dc, bitmap);
        pixels = reinterpret_cast<DWORD*>(bits);
        width = w;
        height = h;
        return true;
    }

    void Destroy() {
        if (dc) {
            if (old) ::SelectObject(dc, old);
            ::DeleteDC(dc);
        }
        if (bitmap) ::DeleteObject(bitmap);
        dc = nullptr;
        bitmap = nullptr;
        old = nullptr;
        pixels = nullptr;
        width = height = 0;
    }
};

HWND g_game = nullptr;
HWND g_overlay = nullptr;
Options g_options;
volatile LONG g_running = 1;

// The frame the game hands over, and the flag saying one is waiting. The lock is
// held across the blit by BeginCapture and released by EndCapture, which is what
// stops the compositing thread reading a half-drawn frame.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
bool g_hasFrame = false;
Bitmap g_capture;

// Set when the window is invalidated - something covered it, or it was moved -
// so the next frame repaints all of it rather than only the picture.
volatile LONG g_repaint = 0;

void EnsureLock() {
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
}

// Turns `src` into `dst`, which is the same picture rotated a quarter turn.
//
// For a quarter turn the output is srcH wide by srcW tall, and the mapping is
// output(x, y) = input(srcW-1-y, x). That means an output row is built from a
// source *column*, so the inner loop strides across the source rather than
// copying contiguous rows.
void Rotate(const DWORD* src, int srcW, int srcH, DWORD* dst, int dstW,
            int dstH, Direction direction) {
    if (direction == kCounterClockwise) {
        for (int y = 0; y < dstH; ++y) {
            const DWORD* column = src + (srcW - 1 - y);
            DWORD* out = dst + y * dstW;
            for (int x = 0; x < dstW; ++x) out[x] = column[x * srcW];
        }
    } else {
        // Clockwise is output(x, y) = input(y, srcH-1-x), which does stay
        // within one source row, just in reverse.
        for (int y = 0; y < dstH; ++y) {
            const DWORD* row = src + y * srcW;
            DWORD* out = dst + y * dstW;
            for (int x = 0; x < dstW; ++x) out[x] = row[srcH - 1 - x];
        }
    }
}

DWORD WINAPI Worker(LPVOID) {
    const int period = g_options.fps > 0 ? 1000 / g_options.fps : 16;

    // `composed` is the finished frame, at window size. Drawing into it and
    // handing the window one single blit is what stops the picture flickering:
    // if the window's own DC were drawn on directly, the desktop could pick it
    // up part way through and show the cleared-but-not-yet-drawn frame.
    Bitmap turned, composed;
    bool sized = false;
    int lastX = -1, lastY = -1, lastW = -1, lastH = -1;

    LARGE_INTEGER freq, seed;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&seed);
    LONGLONG next = seed.QuadPart;

    long frames = 0;
    long reportAt = 120;
    bool wasForeground = false;

    for (;;) {
        if (!InterlockedCompareExchange(&g_running, 0, 0)) break;
        if (!g_overlay || !IsWindow(g_overlay)) break;

        // Sleep to the frame budget rather than spinning.
        LARGE_INTEGER raw;
        ::QueryPerformanceCounter(&raw);
        LONGLONG ticks = freq.QuadPart / 1000;
        LONGLONG due = next + ticks * period;
        if (due > raw.QuadPart)
            ::Sleep(static_cast<DWORD>((due - raw.QuadPart) / ticks));
        next = due;

        // Only present while the game has the foreground. Otherwise this would
        // sit on top of whatever the user has switched to.
        bool foreground = GetForegroundWindow() == g_game;
        if (foreground && !wasForeground) {
            static LONG loggedFg = 0;
            log::Write("rotate: game has the foreground (game=%p overlay=%p, "
                       "GetForegroundWindow=%p)",
                       g_game, g_overlay, GetForegroundWindow());
            InterlockedIncrement(&loggedFg);
        }
        wasForeground = foreground;
        if (!foreground) continue;

        // Capture from here rather than from the game's own Present. Doing it
        // from inside Present costs about a third of the frame rate - the blit
        // of a window whose surface the device is mid-way through presenting is
        // markedly slower than the same blit from outside - and it does not need
        // to be there: between presents the window surface is complete.
        RECT client{};
        if (GetClientRect(g_game, &client)) {
            int cw = client.right - client.left;
            int ch = client.bottom - client.top;
            if (cw > 0 && ch > 0) {
                HDC dst = BeginCapture(cw, ch);
                if (dst) {
                    HDC src = GetDC(g_game);
                    if (src) {
                        BitBlt(dst, 0, 0, cw, ch, src, 0, 0, SRCCOPY);
                        ReleaseDC(g_game, src);
                    }
                    EndCapture();
                }
            }
        }
        if (!g_hasFrame) continue;

        RECT overlayRect{};
        if (!GetClientRect(g_overlay, &overlayRect)) continue;
        int ow = overlayRect.right - overlayRect.left;
        int oh = overlayRect.bottom - overlayRect.top;
        if (ow <= 0 || oh <= 0) continue;

        int srcW = g_capture.width;
        int srcH = g_capture.height;

        EnsureLock();
        EnterCriticalSection(&g_lock);
        if (!g_hasFrame || !g_capture.pixels) {
            LeaveCriticalSection(&g_lock);
            continue;
        }
        if (!sized || turned.width != srcH || turned.height != srcW ||
            composed.width != ow || composed.height != oh) {
            turned.Destroy();
            composed.Destroy();
            HDC scratch = GetDC(g_overlay);
            bool ok = turned.Create(scratch, srcH, srcW);
            if (ok) ok = composed.Create(scratch, ow, oh);
            ReleaseDC(g_overlay, scratch);
            if (!ok) {
                LeaveCriticalSection(&g_lock);
                if (++reportAt > 200) log::Write("rotate: could not make the DIBs");
                continue;
            }
            sized = true;
            log::Write("rotate: turning %dx%d to %dx%d, composing %dx%d", srcW,
                       srcH, srcH, srcW, ow, oh);
        }
        if (turned.pixels && composed.pixels) {
            Rotate(g_capture.pixels, srcW, srcH, turned.pixels, turned.width,
                   turned.height, g_options.direction);

            // Uniform scale into the frame so the shape is preserved, and the
            // remainder stays black. Scaling up is allowed: the picture is only
            // ever 519x656, which would otherwise sit in the middle of the
            // desktop surrounded by black. The picture is normally the taller of
            // the two, so it is the height that gets filled and the sides end up
            // barred.
            int outW = turned.width;
            int outH = turned.height;
            if (ow * outH > oh * outW) {
                outH = oh;
                outW = oh * turned.width / turned.height;
                if (outW > ow) outW = ow;
            } else {
                outW = ow;
                outH = ow * turned.height / turned.width;
                if (outH > oh) outH = oh;
            }
            int outX = (ow - outW) / 2;
            int outY = (oh - outH) / 2;

            // The bars never change, so the frame is only cleared when the
            // picture moves or the window asks to be repainted. Clearing and
            // copying the whole 1920x1078 every frame was most of what held the
            // rate down.
            bool repaint = InterlockedExchange(&g_repaint, 0) != 0;
            if (repaint || outX != lastX || outY != lastY || outW != lastW ||
                outH != lastH) {
                RECT full{0, 0, ow, oh};
                FillRect(composed.dc, &full,
                         reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            lastX = outX;
            lastY = outY;
            lastW = outW;
            lastH = outH;

            SetStretchBltMode(composed.dc,
                              g_options.smooth ? HALFTONE : COLORONCOLOR);
            StretchBlt(composed.dc, outX, outY, outW, outH, turned.dc, 0, 0,
                       turned.width, turned.height, SRCCOPY);

            HDC dst = GetDC(g_overlay);
            if (dst) {
                // Normally only the picture is copied, in one operation, so the
                // desktop cannot pick the window up mid-update. The whole window
                // is copied when it has just been invalidated.
                BitBlt(dst, outX, outY, outW, outH, composed.dc, outX, outY,
                       SRCCOPY);
                if (repaint) {
                    BitBlt(dst, 0, 0, ow, oh, composed.dc, 0, 0, SRCCOPY);
                }
                ReleaseDC(g_overlay, dst);
            }
            if (++frames == reportAt) {
                log::Write("rotate: %ld frames shown, %dx%d at %d,%d", frames,
                           outW, outH, outX, outY);
                reportAt += 120;
            }
        }
        g_hasFrame = false;
        LeaveCriticalSection(&g_lock);
    }

    turned.Destroy();
    composed.Destroy();
    log::Write("rotate: compositing thread stopping");
    return 0;
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Anything invalidating the window means the next frame has to repaint all
    // of it, not just the part that changes.
    if (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_SIZE) {
        InterlockedExchange(&g_repaint, 1);
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

}  // namespace

// Puts the presentation window back above the game window if it has drifted
// below it, which activating the game causes.
//
// This must be called from the thread that created the window. SetWindowPos on
// a window owned by another thread sends that thread a synchronous message, and
// the window thread here does not pump messages for most of its life, so doing
// this from the compositing thread blocks that thread outright - which is what
// stopped frames appearing when it was tried there.
void KeepAboveGame() {
    if (!g_overlay || !IsWindow(g_overlay) || !g_game || !IsWindow(g_game)) return;
    // Only while the game is in front. Without this the correction keeps raising
    // the window back to the top after the user has switched to something else,
    // which is the always-on-top behaviour in all but name.
    if (GetForegroundWindow() != g_game) return;
    if (GetWindow(g_game, GW_HWNDPREV) == g_overlay) return;
    SetWindowPos(g_overlay, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

HDC BeginCapture(int width, int height) {
    if (!g_running || width <= 0 || height <= 0) return nullptr;

    static LONG loggedBusy = 0, loggedFail = 0;
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_hasFrame) {
        // The compositing thread has not taken the last one yet; skipping is
        // better than queueing, because only one buffer exists.
        if (InterlockedIncrement(&loggedBusy) == 1)
            log::Write("rotate: capture skipped, last frame still held");
        LeaveCriticalSection(&g_lock);
        return nullptr;
    }
    if (!g_capture.dc || g_capture.width != width || g_capture.height != height) {
        g_capture.Destroy();
        HDC scratch = g_game ? GetDC(g_game) : nullptr;
        bool ok = g_capture.Create(scratch ? scratch : GetDC(nullptr), width,
                                   height);
        if (scratch && g_game) ReleaseDC(g_game, scratch);
        if (!ok) {
            if (InterlockedIncrement(&loggedFail) == 1)
                log::Write("rotate: could not create the capture DIB (%dx%d, "
                           "err %lu)", width, height, GetLastError());
            LeaveCriticalSection(&g_lock);
            return nullptr;
        }
    }
    return g_capture.dc;  // lock still held until EndCapture
}

void EndCapture() {
    EnsureLock();
    g_hasFrame = g_capture.dc != nullptr;
    LeaveCriticalSection(&g_lock);
}

HWND Start(HWND gameWnd, const Options& options) {
    if (!options.enabled || !gameWnd) return nullptr;

    g_game = gameWnd;
    g_options = options;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Stop short of the bottom edge by the width of the sliver, so the bottom of
    // the game window stays uncovered and therefore still being presented into.
    int sliver = options.sliver > 0 ? options.sliver : 2;
    int overlayH = screenH - sliver;
    if (overlayH < 64) overlayH = screenH;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    // Deliberately not owned by the game window, and not WS_EX_TOPMOST. An
    // owned popup was the obvious way to say "float above the game and nothing
    // else", but it deadlocks the process as soon as focus moves between owner
    // and owned window - reproducible on the first focus cycle. Instead the
    // compositing thread raises this window with HWND_TOP when the game gains
    // focus: that is above the game window and other ordinary windows, but an
    // application the user switches to is brought above this one. No activate
    // and no taskbar entry, so it never takes focus from the game or shows up
    // as something the user has to close.
    HWND overlay = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 0, 0,
        screenW, overlayH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!overlay) {
        log::Write("rotate: could not create the presentation window (%lu)",
                   GetLastError());
        return nullptr;
    }

    SetWindowPos(overlay, HWND_TOP, 0, 0, screenW, overlayH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_overlay = overlay;

    HANDLE t = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!t) {
        log::Write("rotate: could not start the compositing thread");
        DestroyWindow(overlay);
        return nullptr;
    }
    CloseHandle(t);

    log::Write("rotate: presenting %dx%d, leaving %d px of the game window "
               "uncovered",
               screenW, overlayH, screenH - overlayH);
    return overlay;
}

}  // namespace rotate