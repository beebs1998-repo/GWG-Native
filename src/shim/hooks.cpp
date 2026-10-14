// Win32 API hooks installed into game.exe's Import Address Table.
//
// Only game.exe's IAT is rewritten, so the shim's own calls to the real
// APIs (through its own IAT) are never intercepted.

#include "hooks.h"
#include "game_thread.h"
#include "present.h"
#include "arcade_input.h"

#include "iat.h"
#include "io_board.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

namespace hooks {
namespace {

// --- real originals -----------------------------------------------------
// Bound once, at load time, from our own module's IAT.

HANDLE(WINAPI* RealCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                DWORD, DWORD, HANDLE) = ::CreateFileA;
BOOL(WINAPI* RealReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) =
    ::ReadFile;
BOOL(WINAPI* RealWriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) =
    ::WriteFile;
BOOL(WINAPI* RealCloseHandle)(HANDLE) = ::CloseHandle;
BOOL(WINAPI* RealSetupComm)(HANDLE, DWORD, DWORD) = ::SetupComm;
BOOL(WINAPI* RealGetCommState)(HANDLE, LPDCB) = ::GetCommState;
BOOL(WINAPI* RealSetCommState)(HANDLE, LPDCB) = ::SetCommState;
BOOL(WINAPI* RealSetCommMask)(HANDLE, DWORD) = ::SetCommMask;
BOOL(WINAPI* RealGetCommTimeouts)(HANDLE, LPCOMMTIMEOUTS) = ::GetCommTimeouts;
BOOL(WINAPI* RealSetCommTimeouts)(HANDLE, LPCOMMTIMEOUTS) = ::SetCommTimeouts;
BOOL(WINAPI* RealClearCommError)(HANDLE, LPDWORD, LPCOMSTAT) = ::ClearCommError;
BOOL(WINAPI* RealGetCommModemStatus)(HANDLE, LPDWORD) = ::GetCommModemStatus;
BOOL(WINAPI* RealEscapeCommFunction)(HANDLE, DWORD) = ::EscapeCommFunction;

HWND(WINAPI* RealCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
                                  int, HWND, HMENU, HINSTANCE, LPVOID) =
    ::CreateWindowExA;

// --- hook entry points --------------------------------------------------

HANDLE WINAPI Hooked_CreateFileA(LPCSTR path, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES sa, DWORD creation,
                                DWORD flags, HANDLE tmpl) {
    if (path && _stricmp(path, "COM2") == 0) {
        return ioboard::CreateFile(path, access, share, sa, creation, flags, tmpl);
    }
    return RealCreateFileA(path, access, share, sa, creation, flags, tmpl);
}

BOOL WINAPI Hooked_ReadFile(HANDLE h, LPVOID buffer, DWORD toRead, LPDWORD read,
                            LPOVERLAPPED ov) {
    if (ioboard::Owns(h)) return ioboard::ReadFile(h, buffer, toRead, read, ov);
    return RealReadFile(h, buffer, toRead, read, ov);
}

BOOL WINAPI Hooked_WriteFile(HANDLE h, LPCVOID buffer, DWORD toWrite,
                             LPDWORD written, LPOVERLAPPED ov) {
    if (ioboard::Owns(h))
        return ioboard::WriteFile(h, buffer, toWrite, written, ov);
    return RealWriteFile(h, buffer, toWrite, written, ov);
}

BOOL WINAPI Hooked_CloseHandle(HANDLE h) {
    if (ioboard::Owns(h)) return ioboard::CloseHandle(h);
    return RealCloseHandle(h);
}

BOOL WINAPI Hooked_SetupComm(HANDLE h, DWORD inQ, DWORD outQ) {
    if (ioboard::Owns(h)) return ioboard::SetupComm(h, inQ, outQ);
    return RealSetupComm(h, inQ, outQ);
}

BOOL WINAPI Hooked_GetCommState(HANDLE h, LPDCB dcb) {
    if (ioboard::Owns(h)) return ioboard::GetCommState(h, dcb);
    return RealGetCommState(h, dcb);
}

BOOL WINAPI Hooked_SetCommState(HANDLE h, LPDCB dcb) {
    if (ioboard::Owns(h)) return ioboard::SetCommState(h, dcb);
    return RealSetCommState(h, dcb);
}

BOOL WINAPI Hooked_SetCommMask(HANDLE h, DWORD mask) {
    if (ioboard::Owns(h)) return ioboard::SetCommMask(h, mask);
    return RealSetCommMask(h, mask);
}

BOOL WINAPI Hooked_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to) {
    if (ioboard::Owns(h)) return ioboard::GetCommTimeouts(h, to);
    return RealGetCommTimeouts(h, to);
}

BOOL WINAPI Hooked_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to) {
    if (ioboard::Owns(h)) return ioboard::SetCommTimeouts(h, to);
    return RealSetCommTimeouts(h, to);
}

BOOL WINAPI Hooked_ClearCommError(HANDLE h, LPDWORD errs, LPCOMSTAT stat) {
    if (ioboard::Owns(h)) return ioboard::ClearCommError(h, errs, stat);
    return RealClearCommError(h, errs, stat);
}

BOOL WINAPI Hooked_GetCommModemStatus(HANDLE h, LPDWORD msr) {
    if (ioboard::Owns(h)) return ioboard::GetCommModemStatus(h, msr);
    return RealGetCommModemStatus(h, msr);
}

BOOL WINAPI Hooked_EscapeCommFunction(HANDLE h, DWORD func) {
    if (ioboard::Owns(h)) return ioboard::EscapeCommFunction(h, func);
    return RealEscapeCommFunction(h, func);
}

// Window creation is not intercepted yet (that is Phase 4) but we record it so
// the game's native window geometry - and therefore the backbuffer aspect it
// wants - is captured from the very first run.
HWND WINAPI Hooked_CreateWindowExA(DWORD exStyle, LPCSTR className,
                                   LPCSTR windowName, DWORD style, int x, int y,
                                   int w, int h, HWND parent, HMENU menu,
                                   HINSTANCE inst, LPVOID param) {
    // The thread that creates the window is the game's main thread.
    NotePossibleMainThread();

    HWND wnd = RealCreateWindowExA(exStyle, className, windowName, style, x, y, w,
                                   h, parent, menu, inst, param);
    if (wnd) {
        log::Write("win: CreateWindowExA class=\"%s\" title=\"%s\" style=0x%08X "
                   "%dx%d -> hwnd=%p",
                   className ? className : "(null)",
                   windowName ? windowName : "(null)", (unsigned)style, w, h,
                   (void*)wnd);
    }
    return wnd;
}

struct HookDef {
    const char* dll;
    const char* fn;
    void* hook;
    void** real;
};

const HookDef kHooks[] = {
    {"kernel32.dll", "CreateFileA",         (void*)&Hooked_CreateFileA,       (void**)&RealCreateFileA},
    {"kernel32.dll", "ReadFile",           (void*)&Hooked_ReadFile,         (void**)&RealReadFile},
    {"kernel32.dll", "WriteFile",          (void*)&Hooked_WriteFile,        (void**)&RealWriteFile},
    {"kernel32.dll", "CloseHandle",        (void*)&Hooked_CloseHandle,      (void**)&RealCloseHandle},
    {"kernel32.dll", "SetupComm",          (void*)&Hooked_SetupComm,        (void**)&RealSetupComm},
    {"kernel32.dll", "GetCommState",       (void*)&Hooked_GetCommState,     (void**)&RealGetCommState},
    {"kernel32.dll", "SetCommState",       (void*)&Hooked_SetCommState,     (void**)&RealSetCommState},
    {"kernel32.dll", "SetCommMask",        (void*)&Hooked_SetCommMask,      (void**)&RealSetCommMask},
    {"kernel32.dll", "GetCommTimeouts",    (void*)&Hooked_GetCommTimeouts,  (void**)&RealGetCommTimeouts},
    {"kernel32.dll", "SetCommTimeouts",    (void*)&Hooked_SetCommTimeouts,  (void**)&RealSetCommTimeouts},
    {"kernel32.dll", "ClearCommError",     (void*)&Hooked_ClearCommError,   (void**)&RealClearCommError},
    {"kernel32.dll", "GetCommModemStatus", (void*)&Hooked_GetCommModemStatus, (void**)&RealGetCommModemStatus},
    {"kernel32.dll", "EscapeCommFunction", (void*)&Hooked_EscapeCommFunction, (void**)&RealEscapeCommFunction},
    {"user32.dll",   "CreateWindowExA",    (void*)&Hooked_CreateWindowExA,  (void**)&RealCreateWindowExA},
    {"dinput8.dll",  "DirectInput8Create", (void*)&Hooked_DirectInput8Create, nullptr},
    {"d3d8.dll",     "Direct3DCreate8",    (void*)&present::Hooked_Direct3DCreate8, nullptr},
};

}  // namespace

// Comma-separated list of function names to skip, from GWG_NATIVE_HOOK_SKIP.
bool IsSkipped(const char* fnName) {
    char skip[1024] = {};
    DWORD n = GetEnvironmentVariableA("GWG_NATIVE_HOOK_SKIP", skip, sizeof(skip));
    if (n == 0 || n >= sizeof(skip)) return false;
    char* p = skip;
    while (*p) {
        char* comma = strchr(p, ',');
        if (comma) *comma = '\0';
        if (_stricmp(p, fnName) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

bool Install() {
    int ok = 0, total = 0;
    for (const HookDef& h : kHooks) {
        if (IsSkipped(h.fn)) {
            log::Write("hook: %s!%s -> skipped by config", h.dll, h.fn);
            continue;
        }
        ++total;
        if (PatchIAT(h.dll, h.fn, h.hook)) {
            ++ok;
        } else {
            log::Write("hook: %s!%s -> NOT FOUND in game.exe", h.dll, h.fn);
        }
    }
    log::Write("hook: %d/%d entry points patched", ok, total);
    return ok == total;
}

void Uninstall() {
    for (const HookDef& h : kHooks) {
        if (h.real && *h.real) PatchIAT(h.dll, h.fn, *h.real);
    }
}

}  // namespace hooks
