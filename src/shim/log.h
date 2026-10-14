// Minimal file logger for the injected shim.
// The shim runs inside the game's process, so logging is intentionally
// dependency-free and self-contained: a private append handle plus a lock.
#pragma once

#include <windows.h>

namespace log {

// Opens the log file. Safe to call more than once; only the first call wins.
// Returns TRUE if a log file was successfully opened.
BOOL Init();

// Writes one formatted line. No-op if Init() failed or logging is disabled.
void Write(const char* fmt, ...);

// Appends a hex dump of `len` bytes as "TX"/"RX" tagged lines.
void WriteHex(const char* tag, const unsigned char* data, DWORD len);

void Shutdown();

}  // namespace log