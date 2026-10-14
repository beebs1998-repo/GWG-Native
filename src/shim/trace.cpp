// Diagnostic: samples the instruction pointer of the game's main thread.
//
// Symptom it was written for: the game runs at 100% CPU, renders a static
// screen and never advances. The trace log alone cannot distinguish "spinning
// in the I/O retry loop" from "spinning waiting for an input device", because
// the spinning code calls nothing we hook. Suspending the main thread and
// reading its CONTEXT answers it directly.
//
// Enabled with GWG_NATIVE_TRACEPC=1 (the launcher's [Diagnostics] TracePC key).
#include <windows.h>

#include "game_thread.h"
#include "log.h"
#include "trace.h"

namespace trace {
namespace {

const DWORD kSampleIntervalMs = 400;

void SampleOnce(DWORD tid) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (!th) {
        log::Write("trace: cannot open thread %lu (err %lu)",
                   (unsigned long)tid, (unsigned long)GetLastError());
        return;
    }
    if (SuspendThread(th) == (DWORD)-1) {
        log::Write("trace: cannot suspend thread %lu (err %lu)",
                   (unsigned long)tid, (unsigned long)GetLastError());
        CloseHandle(th);
        return;
    }

    DWORD eip = 0, esp = 0, eax = 0;
    CONTEXT ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(th, &ctx)) {
        eip = (DWORD)ctx.Eip;
        esp = (DWORD)ctx.Esp;
        eax = (DWORD)ctx.Eax;
    }
    ResumeThread(th);
    CloseHandle(th);

    log::Write("trace: tid=%lu eip=%08lX esp=%08lX eax=%08lX",
               (unsigned long)tid, (unsigned long)eip, (unsigned long)esp,
               (unsigned long)eax);
}

DWORD WINAPI Sampler(LPVOID) {
    // The main thread records itself in the CreateWindowExA hook; its Win32
    // start address points into kernel32's thread init thunk, so enumerating
    // threads cannot identify it. Wait for the window to exist first.
    for (int i = 0; i < 100 && !GameMainThreadId(); ++i) Sleep(100);

    DWORD tid = GameMainThreadId();
    if (!tid) {
        log::Write("trace: no main thread recorded (window never created)");
        return 0;
    }
    log::Write("trace: sampling main thread %lu", (unsigned long)tid);

    for (int i = 0; i < 25; ++i) {
        SampleOnce(tid);
        Sleep(kSampleIntervalMs);
    }
    return 0;
}

}  // namespace

void Start() {
    wchar_t on[8] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_TRACEPC", on, 8) <= 0 || on[0] != L'1')
        return;
    HANDLE t = CreateThread(NULL, 0, &Sampler, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

}  // namespace trace