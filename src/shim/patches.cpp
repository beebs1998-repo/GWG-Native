#include "patches.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

namespace patches {
namespace {

// Parses "0043E469=74>EB" or "0043E469=EB" into an address, the bytes expected
// to be there, and the bytes to write. Either side may be several bytes, given
// as a run of hex digits.
bool ParseEntry(const char* text, unsigned long* address, unsigned char* expect,
                size_t* expectLen, unsigned char* write, size_t* writeLen) {
    const char* eq = strchr(text, '=');
    if (!eq || eq == text) return false;

    char addrText[32] = {};
    size_t addrLen = (size_t)(eq - text);
    if (addrLen >= sizeof(addrText)) return false;
    memcpy(addrText, text, addrLen);
    *address = strtoul(addrText, NULL, 16);
    if (!*address) return false;

    const char* spec = eq + 1;
    const char* arrow = strchr(spec, '>');

    auto HexToBytes = [](const char* hex, unsigned char* out,
                         size_t* outLen) -> bool {
        size_t n = 0;
        // Stop at the first non-hex character so that "74>EB" parses as one
        // byte, not as 74 followed by a byte mangled out of ">E".
        while (hex[0] && hex[1] && isxdigit((unsigned char)hex[0]) &&
               isxdigit((unsigned char)hex[1])) {
            if (n >= 16) return false;
            char pair[3] = {hex[0], hex[1], 0};
            out[n++] = (unsigned char)strtoul(pair, NULL, 16);
            hex += 2;
        }
        *outLen = n;
        return n > 0;
    };

    if (arrow) {
        // "OLD>NEW": verify the site still looks like we expect before writing.
        if (!HexToBytes(spec, expect, expectLen)) return false;
        if (!HexToBytes(arrow + 1, write, writeLen)) return false;
    } else {
        *expectLen = 0;
        if (!HexToBytes(spec, write, writeLen)) return false;
    }
    return true;
}

// Copies `len` bytes into `target`, making the page writable first. .text is
// read-execute once the loader is done with it, so the write needs a temporary
// PAGE_EXECUTE_READWRITE.
bool WriteBytes(void* target, const unsigned char* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(target, bytes, len);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    VirtualProtect(target, len, oldProtect, &oldProtect);
    return true;
}

}  // namespace

int Apply() {
    wchar_t spec[2048] = {};
    int len = GetEnvironmentVariableW(L"GWG_NATIVE_PATCHES", spec,
                                      sizeof(spec) / sizeof(spec[0]));
    if (len <= 0) {
        log::Write("patches: none configured");
        return 0;
    }

    char narrow[2048] = {};
    WideCharToMultiByte(CP_ACP, 0, spec, -1, narrow, sizeof(narrow), NULL, NULL);

    int applied = 0;
    char* context = NULL;
    for (char* entry = strtok_s(narrow, ";,\r\n", &context); entry;
         entry = strtok_s(NULL, ";,\r\n", &context)) {
        while (*entry == ' ' || *entry == '\t') ++entry;
        if (!*entry || *entry == '#') continue;

        unsigned long address = 0;
        unsigned char expect[16] = {}, write[16] = {};
        size_t expectLen = 0, writeLen = 0;
        if (!ParseEntry(entry, &address, expect, &expectLen, write, &writeLen)) {
            log::Write("patches: cannot parse \"%s\"", entry);
            continue;
        }

        void* target = reinterpret_cast<void*>(address);
        if (expectLen && memcmp(target, expect, expectLen) != 0) {
            log::Write("patches: %s skipped, site does not hold the expected bytes",
                       entry);
            continue;
        }

        if (WriteBytes(target, write, writeLen)) {
            log::Write("patches: %s applied", entry);
            applied++;
        } else {
            log::Write("patches: %s failed (VirtualProtect)", entry);
        }
    }
    return applied;
}

}  // namespace patches