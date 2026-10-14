// Diagnostic: map which IDirect3DDevice8 vtable slots the game calls, and from
// where in game.exe it calls them.
//
// The vendored DirectX 8 headers disagree with the runtime about the vtable
// order, so the order has to be measured rather than read off. Each slot is
// replaced by a stub that records the return address of the call and then
// forwards to the real method unchanged:
//
//     mov  eax, [esp]                    ; return address
//     ...record it against this slot...
//     inc  dword ptr [counter]           ; FF 05 <abs>
//     mov  eax, <realDevice>             ; B8  <abs>
//     mov  [esp+4], eax                  ; 89 44 24 04
//     jmp  dword ptr [<realSlot>]        ; FF 25 <abs>
//
// Only EAX, ECX and EDX are touched, all of which are caller-saved, so the tail
// jump leaves the real method with the state it expects. Nothing is called and
// no stack slot is disturbed, so the stub cannot fault.
//
// A method is normally reached through one dispatch instruction per call site,
// so every distinct return address is recorded (up to 16 per slot) and the code
// is disassembled offline afterwards. That is enough to tell the methods apart
// by argument count without having to guess.
//
// Enabled with GWG_NATIVE_DEVPROBE=1, so it is inert by default.
#include <windows.h>
#include <stdio.h>

#include "devprobe.h"
#include "log.h"

namespace devprobe {
namespace {

constexpr int kSlots = 128;

// Per-slot list of distinct call sites. 0x80000000 in the first word means the
// list filled up and was not fully recorded.
constexpr int kSiteWords = 16;
constexpr DWORD kSiteFull = 0x80000000u;

volatile LONG g_hits[kSlots];
volatile LONG g_callSites[kSlots * kSiteWords];
volatile LONG g_reported = 0;

struct Proxy {
    void** vtbl;  // must be first
    void* real;
    LONG refCount;
};

// Forward the query but hand back the proxy: letting a real device pointer
// escape would take calls outside the counting stubs.
HRESULT __stdcall Proxy_QueryInterface(void* self, REFIID riid, void** out) {
    Proxy* p = static_cast<Proxy*>(self);
    if (!out) return E_POINTER;
    *out = nullptr;

    void* queried = nullptr;
    void** vtbl = *reinterpret_cast<void***>(p->real);
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, REFIID, void**)>(
        vtbl[0])(p->real, riid, &queried);
    if (FAILED(hr) || !queried) return hr;

    reinterpret_cast<ULONG(__stdcall*)(void*)>(vtbl[2])(queried);  // Release
    InterlockedIncrement(&p->refCount);
    *out = self;
    return S_OK;
}

ULONG __stdcall Proxy_AddRef(void* self) {
    Proxy* p = static_cast<Proxy*>(self);
    InterlockedIncrement(&p->refCount);
    return static_cast<ULONG>(p->refCount);
}

ULONG __stdcall Proxy_Release(void* self) {
    Proxy* p = static_cast<Proxy*>(self);
    LONG n = InterlockedDecrement(&p->refCount);
    if (n == 0) {
        void** vtbl = *reinterpret_cast<void***>(p->real);
        reinterpret_cast<ULONG(__stdcall*)(void*)>(vtbl[2])(p->real);
        delete[] p->vtbl;
        delete p;
    }
    return static_cast<ULONG>(n);
}

// Records the return address against this slot's site list, then forwards.
//
//   mov  eax, [esp]                       ; 8B 04 24
//   mov  ecx, <sites>                     ; B9 <abs>
//   mov  edx, 0                           ; BA 00000000
// scan:
//   cmp  dword ptr [ecx+edx*4], 0         ; 83 3C 11 00
//   jz   store
//   cmp  dword ptr [ecx+edx*4], eax       ; 39 04 11
//   je   done
//   inc  edx                              ; 42
//   cmp  edx, 16                          ; 83 FA 10
//   jl   scan
//   or   dword ptr [ecx], 0x80000000      ; 81 09 00800000
//   jmp  done
// store:
//   mov  dword ptr [ecx+edx*4], eax       ; 89 04 11
// done:
//   inc  dword ptr [counter]              ; FF 05 <abs>
//   mov  eax, <realDevice>                ; B8  <abs>
//   mov  [esp+4], eax                     ; 89 44 24 04
//   jmp  dword ptr [<realSlot>]           ; FF 25 <abs>
void* MakeCountingStub(void* device, void** slot, volatile LONG* counter,
                       volatile LONG* sites) {
    BYTE code[96];
    const DWORD dev = reinterpret_cast<DWORD>(device);
    const DWORD cnt = reinterpret_cast<DWORD>(counter);
    const DWORD sitesAddr = reinterpret_cast<DWORD>(sites);
    const DWORD slotAddr = reinterpret_cast<DWORD>(slot);

    // Offsets within `code` that need a short jump displacement filled in once
    // the whole body is laid out.
    int atJz = 0, atJe = 0, atJl = 0, atJmp = 0;
    int scanAt = 0, storeAt = 0, doneAt = 0;

    int n = 0;
    code[n++] = 0x8B;
    code[n++] = 0x04;
    code[n++] = 0x24;  // mov eax, [esp]
    code[n++] = 0xB9;  // mov ecx, imm32
    memcpy(code + n, &sitesAddr, 4);
    n += 4;
    code[n++] = 0xBA;  // mov edx, 0
    code[n++] = 0x00;
    code[n++] = 0x00;
    code[n++] = 0x00;
    code[n++] = 0x00;

    scanAt = n;
    code[n++] = 0x83;
    code[n++] = 0x3C;
    code[n++] = 0x91;  // SIB: scale x4, index edx, base ecx
    code[n++] = 0x00;  // cmp dword ptr [ecx+edx*4], 0
    atJz = n;
    code[n++] = 0x74;  // jz store
    code[n++] = 0x00;
    code[n++] = 0x39;
    code[n++] = 0x04;
    code[n++] = 0x91;  // cmp dword ptr [ecx+edx*4], eax
    atJe = n;
    code[n++] = 0x74;  // je done
    code[n++] = 0x00;
    code[n++] = 0x42;  // inc edx
    code[n++] = 0x83;
    code[n++] = 0xFA;
    code[n++] = 0x10;  // cmp edx, 16
    atJl = n;
    code[n++] = 0x7C;  // jl scan
    code[n++] = 0x00;
    code[n++] = 0x81;
    code[n++] = 0x09;
    code[n++] = 0x00;
    code[n++] = 0x00;
    code[n++] = 0x00;
    code[n++] = 0x80;  // or dword ptr [ecx], 0x80000000
    atJmp = n;
    code[n++] = 0xEB;  // jmp done
    code[n++] = 0x00;

    storeAt = n;
    code[n++] = 0x89;
    code[n++] = 0x04;
    code[n++] = 0x91;  // mov dword ptr [ecx+edx*4], eax

    doneAt = n;
    code[n++] = 0xFF;
    code[n++] = 0x05;  // inc dword ptr [counter]
    memcpy(code + n, &cnt, 4);
    n += 4;
    code[n++] = 0xB8;  // mov eax, imm32
    memcpy(code + n, &dev, 4);
    n += 4;
    code[n++] = 0x89;  // mov [esp+4], eax
    code[n++] = 0x44;
    code[n++] = 0x24;
    code[n++] = 0x04;
    code[n++] = 0xFF;  // jmp dword ptr [imm32]
    code[n++] = 0x25;
    memcpy(code + n, &slotAddr, 4);
    n += 4;

    // Fill in the short-jump displacements now that every label is known. Each
    // fixup index points at the opcode byte and the displacement follows it, so
    // the byte written is the one after the opcode and the displacement is
    // measured from the end of the two-byte instruction.
    code[atJz + 1] = static_cast<BYTE>(static_cast<signed char>(storeAt - (atJz + 2)));
    code[atJe + 1] = static_cast<BYTE>(static_cast<signed char>(doneAt - (atJe + 2)));
    code[atJl + 1] = static_cast<BYTE>(static_cast<signed char>(scanAt - (atJl + 2)));
    code[atJmp + 1] = static_cast<BYTE>(static_cast<signed char>(doneAt - (atJmp + 2)));

    void* mem =
        VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;
    memcpy(mem, code, n);
    FlushInstructionCache(GetCurrentProcess(), mem, n);
    return mem;
}

void Report() {
    if (InterlockedCompareExchange(&g_reported, 1, 0) != 0) return;

    int order[kSlots];
    for (int i = 0; i < kSlots; ++i) order[i] = i;
    for (int a = 1; a < kSlots; ++a) {
        int key = order[a];
        int b = a - 1;
        while (b >= 0 && g_hits[order[b]] < g_hits[key]) {
            order[b + 1] = order[b];
            --b;
        }
        order[b + 1] = key;
    }

    log::Write("devprobe: slot usage (calls, then every distinct call site)");
    for (int k = 0; k < kSlots; ++k) {
        int slot = order[k];
        if (!g_hits[slot]) break;

        char line[512];
        int used = 0;
        int len = 0;
        len += _snprintf_s(line, sizeof(line), _TRUNCATE, "devprobe: %3d %8ld ",
                           slot, (long)g_hits[slot]);

        bool full = (static_cast<DWORD>(g_callSites[slot * kSiteWords]) &
                     kSiteFull) != 0;
        for (int w = 0; w < kSiteWords; ++w) {
            DWORD site =
                static_cast<DWORD>(g_callSites[slot * kSiteWords + w]);
            if (!site) break;
            ++used;
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE,
                               "%s0x%08X", used > 1 ? "," : "", site);
        }
        if (full) {
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, ",+");
        }
        log::Write("%s", line);
    }
}

DWORD WINAPI Worker(LPVOID) {
    Sleep(2500);  // a couple of seconds of steady running separates per-frame
                  // calls from one-time setup
    Report();
    return 0;
}

}  // namespace

void* Wrap(void* device) {
    if (!device) return device;

    wchar_t on[8] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_DEVPROBE", on, 8) <= 0 ||
        on[0] != L'1') {
        return device;
    }

    void** realVtbl = *reinterpret_cast<void***>(device);
    void** vtbl = new void*[kSlots];
    if (!vtbl) return device;

    for (int i = 0; i < kSlots; ++i) {
        void* stub = MakeCountingStub(device, realVtbl + i, &g_hits[i],
                                      &g_callSites[i * kSiteWords]);
        if (!stub) {
            delete[] vtbl;
            return device;
        }
        vtbl[i] = stub;
    }
    vtbl[0] = (void*)&Proxy_QueryInterface;
    vtbl[1] = (void*)&Proxy_AddRef;
    vtbl[2] = (void*)&Proxy_Release;

    Proxy* proxy = new Proxy{vtbl, device, 1};
    HANDLE t = CreateThread(NULL, 0, &Worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    log::Write("devprobe: counting stubs installed, results in a few seconds");
    return proxy;
}

bool Done() { return g_reported != 0; }

}  // namespace devprobe