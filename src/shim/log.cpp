#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace log {
namespace {

HANDLE        g_file   = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool          g_lockReady = false;
bool          g_enabled   = false;

void Emit(const char* line) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(g_file, line, (DWORD)strlen(line), &written, NULL);
}

}  // namespace

BOOL Init() {
    if (g_lockReady) return g_file != INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    wchar_t path[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"GWG_NATIVE_LOG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        // No launcher-supplied path: fall back to a file beside the shim.
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(NULL, self, MAX_PATH);
        wchar_t* slash = wcsrchr(self, L'\\');
        if (slash) *slash = L'\0';
        wcscat_s(self, L"\\gwg-native.log");
        wcscpy_s(path, self);
    }

    g_file = ::CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) return FALSE;

    g_enabled = TRUE;
    SYSTEMTIME st;
    GetLocalTime(&st);
    Emit("\n================ shim start ================\n");
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u] shim loaded\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
    Emit(line);
    return TRUE;
}

void Write(const char* fmt, ...) {
    if (!g_enabled || g_file == INVALID_HANDLE_VALUE) return;

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

    EnterCriticalSection(&g_lock);
    Emit(line);
    LeaveCriticalSection(&g_lock);
}

void WriteHex(const char* tag, const unsigned char* data, DWORD len) {
    if (!g_enabled || g_file == INVALID_HANDLE_VALUE || !data) return;

    // 16 bytes per line keeps the log readable while still being greppable.
    for (DWORD off = 0; off < len; off += 16) {
        DWORD n = len - off;
        if (n > 16) n = 16;

        char line[256];
        int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "  %s +%04X: ", tag,
                               off);
        for (DWORD i = 0; i < n; ++i) {
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                                "%02X ", data[off + i]);
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        char full[320];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\r\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        EnterCriticalSection(&g_lock);
        Emit(full);
        LeaveCriticalSection(&g_lock);
    }
}

void Shutdown() {
    if (!g_lockReady) return;
    if (g_enabled) {
        Emit("shim unloading\r\n");
        FlushFileBuffers(g_file);
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_enabled = false;
    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
}

}  // namespace log