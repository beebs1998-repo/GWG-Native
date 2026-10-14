#include <windows.h>

#include "hooks.h"
#include "io_board.h"
#include "log.h"
#include "patches.h"
#include "present.h"
#include "trace.h"

namespace {

// Reports the first few unhandled faults, naming the module they came from.
// Without this the only symptom is an exit code; with it the crashing module
// is immediately obvious.
LONG WINAPI VehicleHandler(EXCEPTION_POINTERS* ep) {
    static LONG seen = 0;
    if (InterlockedIncrement(&seen) > 8) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &module);

    char moduleName[MAX_PATH] = "<unknown>";
    if (module) GetModuleFileNameA(module, moduleName, MAX_PATH);

    void* fault = nullptr;
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        fault = (void*)ep->ExceptionRecord->ExceptionInformation[1];
    }

    log::Write("EXC: code=0x%08lX at %p in %s (fault=%p)",
               (unsigned long)ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress, moduleName, fault);

    if (ep->ContextRecord) {
        CONTEXT* c = ep->ContextRecord;
        log::Write("EXC:   eip=%p esp=%p ebp=%p eax=%p ecx=%p edx=%p ebx=%p esi=%p edi=%p",
                   (void*)c->Eip, (void*)c->Esp, (void*)c->Ebp, (void*)c->Eax,
                   (void*)c->Ecx, (void*)c->Edx, (void*)c->Ebx, (void*)c->Esi,
                   (void*)c->Edi);
        // Scan the top of the stack for return addresses inside game.exe so the
        // call site that led here can be identified.
        HMODULE exe = GetModuleHandleA(NULL);
        uintptr_t base = (uintptr_t)exe, end = base + 0x1C8000;
        unsigned char* sp = (unsigned char*)c->Esp;
        int found = 0;
        for (int i = 0; i < 96 && found < 6; ++i) {
            uintptr_t v;
            memcpy(&v, sp + i * 4, sizeof(v));
            if (v >= base && v < end) {
                log::Write("EXC:   stack[%02d] = %p (in game.exe)", i, (void*)v);
                found++;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI Worker(LPVOID param) {
    HMODULE self = static_cast<HMODULE>(param);

    log::Init();
    AddVectoredExceptionHandler(1, &VehicleHandler);

    char gamePath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, gamePath, MAX_PATH);
    log::Write("shim: attached to %s", gamePath);

    ioboard::Init(self);
    hooks::Install();
    patches::Apply();
    trace::Start();
    present::Start();

    log::Write("shim: ready");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Never do real work from DllMain: the loader lock is held and the
            // game has not finished resolving its own imports yet.
            DisableThreadLibraryCalls(module);
            HANDLE t = CreateThread(NULL, 0, &Worker, module, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            log::Write("shim: detaching");
            hooks::Uninstall();
            ioboard::Shutdown();
            log::Shutdown();
            break;
    }
    return TRUE;
}