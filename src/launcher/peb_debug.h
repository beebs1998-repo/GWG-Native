// peb_debug.h - dumps the *target* process's PEB module list.
//
// The injection stub walks InMemoryOrderModuleList to find kernel32.dll. If
// that walk fails inside the target but works locally, the difference is in
// the target's own PEB, so read it out of the target and look at it.
//
// Enabled by setting GWG_NATIVE_PEBDEBUG=1 before launching.
#pragma once

#include <windows.h>

#include <cstdio>

namespace pebdebug {

inline bool ReadRemote(HANDLE process, uintptr_t addr, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), buf, n,
                             &got) &&
           got == n;
}

inline void DumpRemotePeb(HANDLE process) {
    typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, DWORD, PVOID, ULONG,
                                                      PULONG);
    struct PROCESS_BASIC_INFORMATION {
        PVOID ExitStatus;
        PVOID PebBaseAddress;
        PVOID AffinityMask;
        PVOID BasePriority;
        PVOID UniqueProcessId;
        PVOID InheritedFromUniqueProcessId;
    };

    auto ntqip = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQueryInformationProcess"));
    if (!ntqip) {
        std::fwprintf(stderr, L"pebdebug: NtQueryInformationProcess missing\n");
        return;
    }

    PROCESS_BASIC_INFORMATION pbi{};
    LONG status = ntqip(process, 0, &pbi, sizeof(pbi), nullptr);
    std::fwprintf(stderr, L"pebdebug: status=0x%08lX peb=%p\n",
                  static_cast<unsigned long>(status), pbi.PebBaseAddress);
    if (status != 0 || !pbi.PebBaseAddress) return;

    uintptr_t ldr = 0;
    if (!ReadRemote(process, reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x0C,
                    &ldr, sizeof(ldr))) {
        std::fwprintf(stderr, L"pebdebug: read PEB->Ldr failed (%lu)\n",
                      GetLastError());
        return;
    }
    std::fwprintf(stderr, L"pebdebug: Ldr=%p\n", reinterpret_cast<void*>(ldr));
    if (!ldr) return;

    const uintptr_t head = ldr + 0x14;
    uintptr_t p = 0;
    if (!ReadRemote(process, head, &p, sizeof(p))) {
        std::fwprintf(stderr, L"pebdebug: read head.Flink failed (%lu)\n",
                      GetLastError());
        return;
    }
    std::fwprintf(stderr, L"pebdebug: head=%p first=%p\n",
                  reinterpret_cast<void*>(head), reinterpret_cast<void*>(p));

    for (int i = 0; i < 32 && p && p != head; ++i) {
        uintptr_t base = 0, buf = 0;
        unsigned short len = 0;
        ReadRemote(process, p + 0x10, &base, sizeof(base));
        ReadRemote(process, p + 0x24, &len, sizeof(len));
        ReadRemote(process, p + 0x28, &buf, sizeof(buf));

        wchar_t name[64] = {};
        if (buf && len < 120) ReadRemote(process, buf, name, len + 2);

        std::fwprintf(stderr, L"pebdebug: [%02d] node=%p base=%p len=%u name=%ls\n",
                      i, reinterpret_cast<void*>(p), reinterpret_cast<void*>(base),
                      len, name);

        uintptr_t next = 0;
        if (!ReadRemote(process, p, &next, sizeof(next))) break;
        p = next;
    }
}

}  // namespace pebdebug