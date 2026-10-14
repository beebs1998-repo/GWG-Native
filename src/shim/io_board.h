// Emulation of the arcade I/O board.
//
// GigaWing Generations talks to its coin-counter / LED / service-switch board
// over a Win32 serial port. From static analysis of game.exe:
//
//   0x520933  CreateFileA("COM2", GENERIC_READ|GENERIC_WRITE, 0, NULL,
//                         OPEN_EXISTING, 0, NULL)  -> stored in global 0x5C4714
//   0x520952  SetupComm(h, 0x204, 0x204)
//   0x52096C  GetCommState(h, &dcb)
//   0x5209A9  SetCommState(h, &dcb)      -> 115200 baud, 8 data bits
//   0x5209BC  SetCommMask(h, EV_RXCHAR)
//   0x5209D2  GetCommTimeouts(h, &to)
//   0x5209FF  SetCommTimeouts(h, &to)    -> ReadTotalTimeoutConstant = 20ms
//   0x5207F0  GetCommModemStatus(h,&msr) -> returns (msr >> 4) & 1, i.e. CTS
//   0x52086C  ClearCommError / ReadFile   -> board -> game
//   0x52083B  WriteFile                   -> game -> board
//
// CTS is the "board present" signal: with no board on a PC it never asserts,
// and the game displays "I/O ERROR" and later exits.
//
// Each serial entry point in game.exe has exactly one call site, so we can
// intercept the whole surface by patching the game's IAT alone.

#pragma once

#include <windows.h>

namespace ioboard {

// Initialises the board. `self` is the shim module handle (unused today but
// kept so the board can later load external data files relative to the shim).
bool Init(HMODULE self);

void Shutdown();

// True when `h` is the handle we handed back for the emulated COM port.
bool Owns(HANDLE h);

// --- Win32 serial API surface used by game.exe -----------------------

HANDLE CreateFile(const char* path, DWORD access, DWORD share,
                  LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags,
                  HANDLE tmpl);
BOOL SetupComm(HANDLE h, DWORD inQueue, DWORD outQueue);
BOOL GetCommState(HANDLE h, LPDCB dcb);
BOOL SetCommState(HANDLE h, LPDCB dcb);
BOOL SetCommMask(HANDLE h, DWORD mask);
BOOL GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to);
BOOL SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to);
BOOL ClearCommError(HANDLE h, LPDWORD errors, LPCOMSTAT stat);
BOOL GetCommModemStatus(HANDLE h, LPDWORD msr);
BOOL EscapeCommFunction(HANDLE h, DWORD func);
BOOL ReadFile(HANDLE h, void* buffer, DWORD toRead, LPDWORD read,
              LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE h, const void* buffer, DWORD toWrite, LPDWORD written,
               LPOVERLAPPED overlapped);
BOOL CloseHandle(HANDLE h);

}  // namespace ioboard