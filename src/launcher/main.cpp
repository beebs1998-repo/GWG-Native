// gwg-native.exe - native launcher for GigaWing Generations.
//
// Starts game.exe, injects bin\gwgshim32.dll into it, and waits. No
// TeknoParrot and no external tool: the shim supplies the arcade hardware the
// game expects (see ..\src\shim\io_board.h for the details).
//
// Both binaries are built as 32-bit so the launcher can drive the 32-bit game
// process directly.
//
// Injection strategy
// ------------------
// CreateRemoteThread needs a callable address *inside* the target. The obvious
// shortcuts both fail here:
//   - CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) returns ERROR_PARTIAL_COPY
//     for a WOW64 target.
//   - a process created with CREATE_SUSPENDED has not built its loader list
//     yet, so PEB->Ldr is still NULL.
// So the launcher reads the target's own PEB with NtQueryInformationProcess +
// ReadProcessMemory (which works fine across WOW64), finds kernel32.dll in
// InMemoryOrderModuleList, walks that module's export table for
// LoadLibraryW, and starts a remote thread there.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "peb_debug.h"

namespace {

// ---------------------------------------------------------------------------
// Remote process inspection
// ---------------------------------------------------------------------------

bool ReadRemote(HANDLE process, uintptr_t addr, void* buffer, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(addr), buffer,
                             size, &got) &&
           got == size;
}

bool ReadRemoteDword(HANDLE process, uintptr_t addr, uint32_t* out) {
    return ReadRemote(process, addr, out, sizeof(*out));
}

bool ReadRemoteWord(HANDLE process, uintptr_t addr, uint16_t* out) {
    return ReadRemote(process, addr, out, sizeof(*out));
}

// Locates kernel32.dll's mapped base inside the target.
bool FindRemoteKernel32(HANDLE process, uint32_t* outBase) {
    typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, DWORD, PVOID, ULONG,
                                                      PULONG);
    struct PROCESS_BASIC_INFORMATION {
        void* ExitStatus;
        void* PebBaseAddress;
        void* AffinityMask;
        void* BasePriority;
        void* UniqueProcessId;
        void* InheritedFromUniqueProcessId;
    };

    auto ntqip = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQueryInformationProcess"));
    if (!ntqip) return false;

    PROCESS_BASIC_INFORMATION pbi{};
    if (ntqip(process, 0, &pbi, sizeof(pbi), nullptr) != 0 ||
        !pbi.PebBaseAddress)
        return false;

    uint32_t ldr = 0;
    if (!ReadRemoteDword(process, reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) +
                                       0x0C,
                         &ldr))
        return false;
    if (!ldr) return false;

    // InMemoryOrderModuleList; entry P maps to LDR_DATA_TABLE_ENTRY + 0x08.
    const uintptr_t head = static_cast<uintptr_t>(ldr) + 0x14;
    uint32_t node = 0;
    if (!ReadRemoteDword(process, head, &node)) return false;

    for (int guard = 0; guard < 512; ++guard) {
        if (!node || node == head) break;

        const uintptr_t p = node;
        uint16_t len = 0;
        if (!ReadRemoteWord(process, p + 0x24, &len)) return false;
        if (len == 24) {  // L"KERNEL32.DLL"
            uint32_t buf = 0;
            if (ReadRemoteDword(process, p + 0x28, &buf) && buf) {
                wchar_t name[16] = {};
                if (ReadRemote(process, buf, name, sizeof(name))) {
                    if (_wcsicmp(name, L"KERNEL32.DLL") == 0) {
                        uint32_t base = 0;
                        if (!ReadRemoteDword(process, p + 0x10, &base)) return false;
                        *outBase = base;
                        return true;
                    }
                }
            }
        }
        uint32_t next = 0;
        if (!ReadRemoteDword(process, p, &next)) return false;
        node = next;
    }
    return false;
}

// Resolves an export by name inside a module already mapped in the target.
bool FindRemoteExport(HANDLE process, uint32_t moduleBase, const char* name,
                      uint32_t* outRva) {
    uint32_t lfanew = 0;
    if (!ReadRemoteDword(process, static_cast<uintptr_t>(moduleBase) + 0x3C,
                         &lfanew))
        return false;

    uint32_t exportRva = 0;
    const uintptr_t nt = static_cast<uintptr_t>(moduleBase) + lfanew;
    if (!ReadRemoteDword(process, nt + 0x78, &exportRva) || !exportRva)
        return false;

    const uintptr_t edt = static_cast<uintptr_t>(moduleBase) + exportRva;

    uint32_t numNames = 0, namesRva = 0, ordinalsRva = 0, funcsRva = 0;
    if (!ReadRemoteDword(process, edt + 0x18, &numNames)) return false;
    if (!ReadRemoteDword(process, edt + 0x20, &namesRva)) return false;
    if (!ReadRemoteDword(process, edt + 0x24, &ordinalsRva)) return false;
    if (!ReadRemoteDword(process, edt + 0x1C, &funcsRva)) return false;
    if (!numNames || !namesRva) return false;

    // kernel32 exports well under 2000 names; read them in modest chunks.
    const uint32_t kChunk = 256;
    std::vector<uint32_t> nameRvas;
    nameRvas.reserve(numNames);
    for (uint32_t i = 0; i < numNames; i += kChunk) {
        const uint32_t want = (numNames - i < kChunk) ? (numNames - i) : kChunk;
        std::vector<uint32_t> chunk(want);
        if (!ReadRemote(process, static_cast<uintptr_t>(moduleBase) + namesRva +
                                        i * sizeof(uint32_t),
                        chunk.data(), chunk.size() * sizeof(uint32_t)))
            return false;
        nameRvas.insert(nameRvas.end(), chunk.begin(), chunk.end());
    }

    const size_t wanted = strlen(name);
    for (uint32_t i = 0; i < nameRvas.size(); ++i) {
        if (!nameRvas[i]) continue;

        char candidate[64] = {};
        if (!ReadRemote(process, static_cast<uintptr_t>(moduleBase) + nameRvas[i],
                        candidate, sizeof(candidate)))
            continue;
        if (strncmp(candidate, name, wanted) != 0) continue;
        if (candidate[wanted] != '\0') continue;

        uint16_t ordinal = 0;
        if (!ReadRemoteWord(process,
                            static_cast<uintptr_t>(moduleBase) + ordinalsRva +
                                i * sizeof(uint16_t),
                            &ordinal))
            return false;

        uint32_t funcRva = 0;
        if (!ReadRemoteDword(process,
                             static_cast<uintptr_t>(moduleBase) + funcsRva +
                                 static_cast<uintptr_t>(ordinal) * sizeof(uint32_t),
                             &funcRva))
            return false;
        *outRva = funcRva;
        return true;
    }
    return false;
}

void* RemoteAllocCopy(HANDLE process, const void* data, size_t size,
                      DWORD protect) {
    void* remote =
        VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, protect);
    if (!remote) return nullptr;

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, data, size, &written) ||
        written != size) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return nullptr;
    }
    return remote;
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

bool Inject(HANDLE process, const std::wstring& dllPath) {
    uint32_t kernelBase = 0;
    if (!FindRemoteKernel32(process, &kernelBase)) {
        std::fwprintf(stderr, L"[gwg-native] could not locate kernel32.dll\n");
        return false;
    }

    uint32_t loadLibraryRva = 0;
    if (!FindRemoteExport(process, kernelBase, "LoadLibraryW", &loadLibraryRva)) {
        std::fwprintf(stderr, L"[gwg-native] could not locate LoadLibraryW\n");
        return false;
    }

    auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        static_cast<uintptr_t>(kernelBase) + loadLibraryRva);
    std::fwprintf(stderr, L"[gwg-native] kernel32=%p LoadLibraryW=%p\n",
                  reinterpret_cast<void*>(kernelBase),
                  reinterpret_cast<void*>(remoteLoadLibrary));

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath =
        RemoteAllocCopy(process, dllPath.c_str(), pathBytes, PAGE_READWRITE);
    if (!remotePath) {
        std::fwprintf(stderr, L"[gwg-native] failed to stage the DLL path\n");
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary,
                                       remotePath, 0, nullptr);
    if (!thread) {
        std::fwprintf(stderr, L"[gwg-native] CreateRemoteThread failed (%lu)\n",
                      GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    // LoadLibraryW returns the HMODULE on success and NULL on failure. The
    // handle is a real address, so only a null result means failure.
    if (exitCode == 0) {
        std::fwprintf(stderr, L"[gwg-native] remote LoadLibraryW failed (0x%08lX)\n",
                      exitCode);
        return false;
    }
    std::fwprintf(stderr, L"[gwg-native] shim loaded at 0x%08lX\n", exitCode);
    return true;
}

// ---------------------------------------------------------------------------
// Configuration / startup
// ---------------------------------------------------------------------------

// Collapses "." and ".." out of a path. This matters: the game resolves its own
// data files relative to GetModuleFileNameA(), and a path containing ".."
// makes that lookup fail, which cascades into a NULL Direct3D device and a
// crash at 0x0041B8F8.
std::wstring NormalizePath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 4] = {};
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 4, buffer, NULL);
    if (n == 0 || n >= MAX_PATH * 4) return path;
    return std::wstring(buffer, n);
}

std::wstring DirName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (b.empty()) return a;
    if (b[0] == L'\\' || (b.size() > 1 && b[1] == L':')) return b;
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    out += b;
    return out;
}

std::wstring ReadIni(const std::wstring& iniPath, const std::wstring& section,
                     const std::wstring& key, const std::wstring& fallback) {
    std::wifstream in(iniPath);
    if (!in) return fallback;

    std::wstring line, current;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\r' ||
                                 line.back() == L'\t'))
            line.pop_back();
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        line = line.substr(first);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            current = line.substr(1, line.size() - 2);
            continue;
        }
        if (!current.empty() && _wcsicmp(current.c_str(), section.c_str()) != 0)
            continue;

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq);
        std::wstring v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(0, 1);

        if (_wcsicmp(k.c_str(), key.c_str()) == 0) return v;
    }
    return fallback;
}

// Collects every "key = value" line in a section and returns the values joined
// with semicolons, which is the form the shim's patch parser expects.
std::wstring ReadIniSection(const std::wstring& iniPath,
                            const std::wstring& section) {
    std::wifstream in(iniPath);
    if (!in) return L"";

    std::wstring line, current, joined;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\r' ||
                                 line.back() == L'\t'))
            line.pop_back();
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        line = line.substr(first);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            current = line.substr(1, line.size() - 2);
            continue;
        }
        if (_wcsicmp(current.c_str(), section.c_str()) != 0) continue;

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring v = line.substr(eq + 1);
        while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(0, 1);
        if (v.empty()) continue;
        if (!joined.empty()) joined += L';';
        joined += v;
    }
    return joined;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    const std::wstring selfDir = DirName(selfPath);
    const std::wstring iniPath = Join(selfDir, L"gwg-native.ini");

    std::wstring gameExe = ReadIni(iniPath, L"Game", L"Exe", L"..\\game.exe");
    std::wstring shimDll = ReadIni(iniPath, L"Shim", L"Dll", L"bin\\gwgshim32.dll");
    std::wstring logFile = ReadIni(iniPath, L"Logging", L"File", L"gwg-native.log");
    const std::wstring echoMode = ReadIni(iniPath, L"IoBoard", L"Echo", L"0");
    const int injectDelay = _wtoi(
        ReadIni(iniPath, L"Game", L"InjectDelayMs", L"600").c_str());

    if (argc > 1 && argv[1][0] != L'-') gameExe = argv[1];

    gameExe = NormalizePath(Join(selfDir, gameExe));
    shimDll = Join(selfDir, shimDll);
    logFile = Join(selfDir, logFile);

    if (GetFileAttributesW(gameExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"[gwg-native] game executable not found: %s\n",
                      gameExe.c_str());
        return 2;
    }
    if (GetFileAttributesW(shimDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"[gwg-native] shim not found: %s\n", shimDll.c_str());
        std::fwprintf(stderr, L"[gwg-native] run build.cmd first.\n");
        return 2;
    }

    SetEnvironmentVariableW(L"GWG_NATIVE_LOG", logFile.c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ECHO", echoMode.c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_IO_ROUNDS",
                           ReadIni(iniPath, L"IoBoard", L"Rounds", L"2").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_IO_BUSYMS",
                           ReadIni(iniPath, L"IoBoard", L"BusyMs", L"2500").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_IO_REPLY",
                           ReadIni(iniPath, L"IoBoard", L"Reply", L"").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_HOOK_SKIP",
                           ReadIni(iniPath, L"Hooks", L"Skip", L"").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_WINDOWED",
                           ReadIni(iniPath, L"Presentation", L"Windowed", L"1").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_BORDERLESS",
                           ReadIni(iniPath, L"Presentation", L"Borderless", L"1").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE",
                           ReadIni(iniPath, L"Presentation", L"Rotate90", L"1").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_MAXFPS",
                           ReadIni(iniPath, L"Presentation", L"MaxFps", L"60").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_SLOTPROBE",
                           ReadIni(iniPath, L"Diagnostics", L"SlotProbe", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_DEVPROBE",
                           ReadIni(iniPath, L"Diagnostics", L"DeviceProbe", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE",
                           ReadIni(iniPath, L"Presentation", L"Rotate", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE_FPS",
                           ReadIni(iniPath, L"Presentation", L"RotateFps", L"60").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE_DIR",
                           ReadIni(iniPath, L"Presentation", L"RotateDir", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE_SMOOTH",
                           ReadIni(iniPath, L"Presentation", L"RotateSmooth", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_ROTATE_SLIVER",
                           ReadIni(iniPath, L"Presentation", L"RotateSliver", L"2").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_PRESENTLOG",
                           ReadIni(iniPath, L"Presentation", L"PresentLog", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_HIDECURSOR",
                           ReadIni(iniPath, L"Presentation", L"HideCursor", L"1").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_TRACEPC",
                           ReadIni(iniPath, L"Diagnostics", L"TracePC", L"0").c_str());
    SetEnvironmentVariableW(L"GWG_NATIVE_PATCHES",
                           ReadIniSection(iniPath, L"Patches").c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(gameExe.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, DirName(gameExe).c_str(), &si,
                        &pi)) {
        std::fwprintf(stderr, L"[gwg-native] CreateProcess failed (%lu)\n",
                      GetLastError());
        return 3;
    }

    std::wprintf(L"[gwg-native] game  : %s\n", gameExe.c_str());
    std::wprintf(L"[gwg-native] shim  : %s\n", shimDll.c_str());
    std::wprintf(L"[gwg-native] log   : %s\n", logFile.c_str());
    fflush(stdout);

    // A process created with CREATE_SUSPENDED has not finished building its
    // loader list yet (PEB->Ldr is NULL), so resume first and give the loader a
    // moment to finish before we inspect it. The game spends seconds loading
    // the 174 MB YOKUSHIN.mbd before it touches the I/O board, so this is
    // nowhere near a race in practice.
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    Sleep(injectDelay > 0 ? injectDelay : 0);

    if (GetEnvironmentVariableW(L"GWG_NATIVE_PEBDEBUG", nullptr, 0) != 0)
        pebdebug::DumpRemotePeb(pi.hProcess);

    if (!Inject(pi.hProcess, shimDll)) {
        std::fwprintf(stderr, L"[gwg-native] injection failed; aborting.\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 4;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    std::wprintf(L"[gwg-native] game exited with code %lu\n", code);
    return static_cast<int>(code);
}