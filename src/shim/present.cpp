// Presentation: windowed swap chain, borderless frame, centring, cursor.
//
// The game renders its 640x480 framebuffer containing portrait content turned on
// its side, and asks for an exclusive fullscreen display mode - which on a
// desktop monitor means dragging the whole screen down to 640x480. Forcing the
// swap chain into windowed mode avoids touching the desktop's resolution at all,
// after which the window can be made borderless and centred.
//
// How the interception works:
//
//   game.exe imports Direct3DCreate8, so the shim replaces it with a proxy
//   IDirect8 whose vtable is generated slot by slot. Slot 15 (CreateDevice) is
//   swapped for a wrapper that forces Windowed = TRUE before delegating; every
//   other slot is a tiny stub that rewrites `this` and tail-jumps into the real
//   method. Typed forwarders would not do: the DirectX 8 headers that have to be
//   vendored (the Windows SDK does not ship Direct3D 8) describe a trimmed,
//   later revision of the interface than the 2004 game was built against, so
//   their method order does not match the runtime d3d8.dll in SysWOW64.
//
// Two COM traps, both hit during development:
//
//   * A proxy object's first field must be the vtable pointer, and every
//     inherited method has to be answered by the proxy. Letting the real
//     QueryInterface/AddRef/Release run against the proxy reads the proxy's
//     fields as private D3D state.
//   * D3D8 rejects a NULL focus window outright, so any Direct3D probe has to
//     supply a real one - otherwise every attempt fails and the result says
//     nothing about the machine rather than about the query.
//
// ROTATION IS NOT IMPLEMENTED, and the reason is worth recording because it is
// not obvious. Rotating the portrait image means intercepting
// IDirect3DDevice8::Present, and that interface's real method order in the
// runtime d3d8.dll could not be established from anything available here:
//
//   * The vendored headers put Present at index 15; the classic 2002 layout puts
//     it at 17, with GetSwapChain at 15. They agree only up to index 14.
//   * Probing slot 15 with the known GetSwapChain signature does not return a
//     swap chain, which suggests the shorter layout.
//   * Installing the interception at 15 never fires. Installing it at 17 instead
//     faults inside d3d8.dll on the game's first device call (game.exe:0x41B8F8),
//     identically, with the hook never entered.
//   * Copying the device vtable with *no* slot replaced runs clean for as long as
//     the game does, so the proxy itself is sound and the failure is the slot,
//     not the mechanism. Reducing the forwarded slots from 128 to 32 changed
//     nothing, which rules out over-reading the real vtable.
//   * It also matters that the presentation device must not be created from
//     inside the game's Present: creating a second Direct3D device re-entrantly
//     faults inside d3d8.dll too.
//
// Guessing at this corrupts the stack rather than failing cleanly, so the game
// is left running upright. README.md records how to establish the index for
// real (identify slots by calling them with known signatures under
// __try/__except, or patch the game's own Present call sites and sidestep the
// vtable entirely).

#include <windows.h>

#include <d3d8.h>

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "present.h"
#include "devprobe.h"
#include "rotate.h"

namespace present {
namespace {

// CreateDevice's index is the one thing that must be known up front, and both
// the classic DirectX 8 SDK and the vendored header agree it is 15.
constexpr int kFactoryMethodCount = 16;
constexpr int kCreateDeviceIndex = 15;

IDirect3DDevice8* WrapDevice(IDirect3DDevice8* device);

// Rotation settings, read once by Start().
rotate::Options g_rotation;

// The game's window, found by the window thread and used from the render thread
// to take the finished frame out of it.
HWND g_wnd = nullptr;

struct FactoryProxy {
    void** vtbl;  // must be first
    IDirect3D8* real;
    LONG refCount;
};

void** g_factoryVtbl = nullptr;

HRESULT STDMETHODCALLTYPE Proxy_QueryInterface(void* self, REFIID riid,
                                               void** out) {
    FactoryProxy* proxy = static_cast<FactoryProxy*>(self);
    if (!out) return E_POINTER;

    void* real = nullptr;
    HRESULT hr = proxy->real->QueryInterface(riid, &real);
    if (FAILED(hr)) return hr;

    if (real == reinterpret_cast<void*>(proxy->real)) {
        // Same interface: hand back the proxy so CreateDevice stays hooked.
        proxy->real->Release();
        InterlockedIncrement(&proxy->refCount);
        *out = self;
        return S_OK;
    }
    *out = real;
    return S_OK;
}

ULONG STDMETHODCALLTYPE Proxy_AddRef(void* self) {
    FactoryProxy* proxy = static_cast<FactoryProxy*>(self);
    InterlockedIncrement(&proxy->refCount);
    return proxy->real->AddRef();
}

ULONG STDMETHODCALLTYPE Proxy_Release(void* self) {
    FactoryProxy* proxy = static_cast<FactoryProxy*>(self);
    LONG n = InterlockedDecrement(&proxy->refCount);
    ULONG real = proxy->real->Release();
    if (!n) delete proxy;
    return real;
}

//     mov  eax, <realObject>
//     mov  [esp+4], eax
//     jmp  dword ptr [<slot>]
void* MakeForwarder(void* real, void** slot) {
    BYTE code[16];
    DWORD self = reinterpret_cast<DWORD>(real);
    DWORD slotAddr = reinterpret_cast<DWORD>(slot);

    int n = 0;
    code[n++] = 0xB8;  // mov eax, imm32
    memcpy(code + n, &self, sizeof(self));
    n += 4;
    code[n++] = 0x89;  // mov [esp+4], eax
    code[n++] = 0x44;
    code[n++] = 0x24;
    code[n++] = 0x04;
    code[n++] = 0xFF;  // jmp dword ptr [imm32]
    code[n++] = 0x25;
    memcpy(code + n, &slotAddr, sizeof(slotAddr));
    n += 4;

    void* mem =
        VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;
    memcpy(mem, code, n);
    FlushInstructionCache(GetCurrentProcess(), mem, n);
    return mem;
}

// A from-scratch windowed parameter set. Real d3d8 rejects the game's own set
// once Windowed is forced on and does not say which field it disliked, so
// building the parameters from zero sidesteps the question entirely.
void FillMinimalWindowed(D3DPRESENT_PARAMETERS* pp, HWND wnd, UINT w, UINT h) {
    ZeroMemory(pp, sizeof(*pp));
    pp->Windowed = TRUE;
    pp->hDeviceWindow = wnd;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferWidth = w;
    pp->BackBufferHeight = h;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->EnableAutoDepthStencil = FALSE;
}

// Real d3d8 rejects the game's fullscreen parameter set once Windowed is forced
// on (D3DERR_INVALIDCALL), so walk a ladder from "as given" outwards and keep
// the first rung that is accepted. Each attempt is logged, which also records
// which rung turned out to be needed.
struct Attempt {
    const char* name;
    D3DPRESENT_PARAMETERS params;
    HWND focusWindow;
    DWORD behavior;
};

HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirect3D8* self, UINT adapter,
                                             D3DDEVTYPE deviceType,
                                             HWND focusWindow, DWORD behavior,
                                             D3DPRESENT_PARAMETERS* params,
                                             IDirect3DDevice8** outDevice) {
    FactoryProxy* proxy = reinterpret_cast<FactoryProxy*>(self);
    IDirect3D8* real = proxy->real;

    if (!params) {
        return real->CreateDevice(adapter, deviceType, focusWindow, behavior,
                                  params, outDevice);
    }

    log::Write(
        "present: CreateDevice in: adapter=%u type=%d %ux%u fmt=%d count=%u "
        "swap=%u msaa=%u depth=%d/%d win=%d behaviour=0x%lX flags=0x%lX "
        "hwnd=%p focus=%p",
        adapter, (int)deviceType, (unsigned)params->BackBufferWidth,
        (unsigned)params->BackBufferHeight, (int)params->BackBufferFormat,
        (unsigned)params->BackBufferCount, (unsigned)params->SwapEffect,
        (unsigned)params->MultiSampleType,
        (int)params->EnableAutoDepthStencil, (int)params->AutoDepthStencilFormat,
        params->Windowed, (unsigned long)behavior, (unsigned long)params->Flags,
        (void*)params->hDeviceWindow, (void*)focusWindow);

    Attempt ladder[9];
    int n = 0;
    D3DPRESENT_PARAMETERS p;

    p = *params;
    p.Windowed = TRUE;
    ladder[n].name = "windowed, as given";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = behavior;
    ++n;

    // The game's behaviour flags are 0x50, which includes a bit D3D8 does not
    // define; plain software vertex processing is known to be accepted.
    p = *params;
    p.Windowed = TRUE;
    ladder[n].name = "windowed, software vertex processing";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    ++n;

    p = *params;
    p.Windowed = TRUE;
    ladder[n].name = "windowed, no depth buffer";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = behavior;
    ++n;

    p = *params;
    p.Windowed = TRUE;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    ladder[n].name = "windowed, X8R8G8B8 back buffer";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = behavior;
    ++n;

    p = *params;
    p.Windowed = TRUE;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.EnableAutoDepthStencil = FALSE;
    ladder[n].name = "windowed, X8R8G8B8, no depth";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = behavior;
    ++n;

    p = *params;
    p.Windowed = TRUE;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.EnableAutoDepthStencil = FALSE;
    p.MultiSampleType = D3DMULTISAMPLE_NONE;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    ladder[n].name = "windowed, X8R8G8B8, no depth, no MSAA, software VP";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    ++n;

    p = *params;
    p.Windowed = TRUE;
    p.hDeviceWindow = NULL;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.EnableAutoDepthStencil = FALSE;
    p.MultiSampleType = D3DMULTISAMPLE_NONE;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    ladder[n].name = "windowed, headless, minimal";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    ++n;

    // Exactly the set a standalone windowed creation was verified to accept on
    // this machine, built from zero rather than derived from the game's. This is
    // the rung that works here, but the earlier ones are kept because which of
    // them is needed depends on the driver and the game's own choices.
    FillMinimalWindowed(&p, focusWindow, params->BackBufferWidth,
                        params->BackBufferHeight);
    ladder[n].name = "windowed, from-scratch minimal set";
    ladder[n].params = p;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    ++n;

    ladder[n].name = "fullscreen, as given";
    ladder[n].params = *params;
    ladder[n].focusWindow = focusWindow;
    ladder[n].behavior = behavior;
    ++n;

    for (int i = 0; i < n; ++i) {
        HRESULT hr = real->CreateDevice(adapter, deviceType, ladder[i].focusWindow,
                                        ladder[i].behavior, &ladder[i].params,
                                        outDevice);
        if (SUCCEEDED(hr)) {
            log::Write("present: CreateDevice accepted [%s], hr=0x%08lX",
                       ladder[i].name, (unsigned long)hr);
            // Diagnostic only: counts which vtable slots the game calls.
            // Returns the device untouched unless GWG_NATIVE_DEVPROBE=1.
            if (outDevice && *outDevice)
                *outDevice = reinterpret_cast<IDirect3DDevice8*>(
                    devprobe::Wrap(*outDevice));
            // Rotation support: intercept Present at the slot the game really
            // uses, so the portrait framebuffer can be re-presented turned.
            if (outDevice && *outDevice) *outDevice = WrapDevice(*outDevice);
            return hr;
        }
        log::Write("present: CreateDevice rejected [%s], hr=0x%08lX",
                   ladder[i].name, (unsigned long)hr);
    }

    log::Write("present: every CreateDevice variant failed");
    return D3DERR_INVALIDCALL;
}

// --- device proxy and Present --------------------------------------------

// The game's device is wrapped so Present can be intercepted. Slot 15 is not a
// guess: counting stubs on a copy of the real vtable showed the game calling it
// with four NULL arguments immediately after TestCooperativeLevel, which is
// Present(NULL, NULL, NULL, NULL). Slot 3 is TestCooperativeLevel because its
// result is compared against D3DERR_DEVICELOST at 0x41DA0F. See
// Native/analysis/FINDINGS.md.
constexpr int kDeviceSlotCount = 128;
constexpr int kPresentIndex = 15;

struct DeviceProxy {
    void** vtbl;  // must be first
    IDirect3DDevice8* real;
    LONG refCount;
};

HRESULT STDMETHODCALLTYPE Device_QueryInterface(void* self, REFIID riid,
                                                void** out) {
    DeviceProxy* p = reinterpret_cast<DeviceProxy*>(self);
    if (!out) return E_POINTER;

    void* real = nullptr;
    HRESULT hr = p->real->QueryInterface(riid, &real);
    if (FAILED(hr)) return hr;

    if (real == reinterpret_cast<void*>(p->real)) {
        // The one interface it already holds: hand back the proxy so Present
        // stays hooked.
        p->real->Release();
        InterlockedIncrement(&p->refCount);
        *out = self;
        return S_OK;
    }
    *out = real;
    return S_OK;
}

ULONG STDMETHODCALLTYPE Device_AddRef(void* self) {
    DeviceProxy* p = reinterpret_cast<DeviceProxy*>(self);
    InterlockedIncrement(&p->refCount);
    return p->real->AddRef();
}

ULONG STDMETHODCALLTYPE Device_Release(void* self) {
    DeviceProxy* p = reinterpret_cast<DeviceProxy*>(self);
    LONG n = InterlockedDecrement(&p->refCount);
    ULONG real = p->real->Release();
    if (!n) {
        delete[] p->vtbl;
        delete p;
    }
    return real;
}

// The dirty-rect argument is passed as void* because the vendored DirectX 8
// headers predate RGNDATA. It is a pointer either way, so the ABI is identical.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(void*, const RECT*, const RECT*,
                                             HWND, const void*);

volatile LONG g_presents = 0;

HRESULT STDMETHODCALLTYPE Hooked_Present(void* self, const RECT* src,
                                         const RECT* dst, HWND hwnd,
                                         const void* dirty) {
    DeviceProxy* p = reinterpret_cast<DeviceProxy*>(self);

    LONG n = InterlockedIncrement(&g_presents);
    if (n <= 1) {
        log::Write("present: hook entered #%ld src=%p dst=%p hwnd=%p dirty=%p",
                   (long)n, (const void*)src, (const void*)dst, (void*)hwnd,
                   dirty);
    }

    void** realVtbl = *reinterpret_cast<void***>(p->real);
    PresentFn real = reinterpret_cast<PresentFn>(realVtbl[kPresentIndex]);
    return real(p->real, src, dst, hwnd, dirty);
}

// Wraps the game's device in a proxy whose only replaced slot is Present.
// Returns the device untouched if anything fails, so a proxy cannot be worse
// than running unproxied.
IDirect3DDevice8* WrapDevice(IDirect3DDevice8* device) {
    if (!device) return device;

    void** realVtbl = *reinterpret_cast<void***>(device);
    void** vtbl = new void*[kDeviceSlotCount];
    if (!vtbl) return device;

    for (int i = 0; i < kDeviceSlotCount; ++i) {
        void* fwd = MakeForwarder(device, realVtbl + i);
        if (!fwd) {
            delete[] vtbl;
            return device;
        }
        vtbl[i] = fwd;
    }
    vtbl[0] = (void*)&Device_QueryInterface;
    vtbl[1] = (void*)&Device_AddRef;
    vtbl[2] = (void*)&Device_Release;
    vtbl[kPresentIndex] = (void*)&Hooked_Present;

    DeviceProxy* proxy = new DeviceProxy{vtbl, device, 1};
    if (!proxy) {
        delete[] vtbl;
        return device;
    }
    log::Write("present: device proxied, slot %d hooked", kPresentIndex);
    return reinterpret_cast<IDirect3DDevice8*>(proxy);
}

// --- window tidying ------------------------------------------------------

struct Settings {
    bool windowed = true;
    bool borderless = true;
    bool hideCursor = true;
    rotate::Options rotation;
};

Settings ReadSettings() {
    Settings s;
    auto Flag = [](const wchar_t* name, bool fallback) {
        wchar_t buf[8] = {};
        if (GetEnvironmentVariableW(name, buf, 8) <= 0) return fallback;
        return buf[0] == L'1';
    };
    auto Number = [](const wchar_t* name, int fallback) {
        wchar_t buf[8] = {};
        if (GetEnvironmentVariableW(name, buf, 8) <= 0) return fallback;
        return _wtoi(buf);
    };
    s.windowed = Flag(L"GWG_NATIVE_WINDOWED", true);
    s.borderless = Flag(L"GWG_NATIVE_BORDERLESS", true);
    s.hideCursor = Flag(L"GWG_NATIVE_HIDECURSOR", true);

    s.rotation.enabled = Flag(L"GWG_NATIVE_ROTATE", false);
    s.rotation.fps = Number(L"GWG_NATIVE_ROTATE_FPS", 60);
    s.rotation.smooth = Flag(L"GWG_NATIVE_ROTATE_SMOOTH", false);
    s.rotation.sliver = Number(L"GWG_NATIVE_ROTATE_SLIVER", 2);
    s.rotation.direction =
        Number(L"GWG_NATIVE_ROTATE_DIR", 0) == 1 ? rotate::kClockwise
                                                : rotate::kCounterClockwise;
    return s;
}

bool g_tidied = false;
bool g_hideCursorEnabled = true;

BOOL CALLBACK FindWindowProc(HWND wnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(wnd)) return TRUE;
    if (GetWindow(wnd, GW_OWNER)) return TRUE;  // skip tool/owned windows

    RECT r{};
    if (!GetWindowRect(wnd, &r)) return TRUE;
    if ((r.right - r.left) <= 0 || (r.bottom - r.top) <= 0) return TRUE;

    *reinterpret_cast<HWND*>(param) = wnd;
    return FALSE;  // first visible top-level window of ours is the game's
}

// Strips the frame and centres the window, then keeps the cursor out of the way.
void TidyWindow() {
    if (!g_wnd) {
        HWND found = nullptr;
        EnumWindows(&FindWindowProc, reinterpret_cast<LPARAM>(&found));
        if (!found) return;
        g_wnd = found;
        log::Write("present: game window is %p", (void*)g_wnd);
    }

    if (!g_tidied) {
        LONG style = GetWindowLongW(g_wnd, GWL_STYLE);
        LONG want = style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                              WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER);
        if (style != want) SetWindowLongW(g_wnd, GWL_STYLE, want);

        RECT r{};
        GetWindowRect(g_wnd, &r);
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y;
        if (g_rotation.enabled) {
            // Parked against the bottom of the screen so the presentation
            // window, which stops short of the bottom edge, never covers all of
            // it: a fully covered Direct3D window stops being presented into and
            // cannot be blitted. See rotate.cpp.
            y = GetSystemMetrics(SM_CYSCREEN) - h;
        } else {
            y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        }
        SetWindowPos(g_wnd, HWND_TOP, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_tidied = true;
        log::Write("present: window %dx%d placed at %d,%d", w, h, x, y);
    }
}

void KeepCursorHidden() {
    if (g_hideCursorEnabled && g_wnd && GetForegroundWindow() == g_wnd)
        SetCursor(NULL);
}

DWORD WINAPI WindowThread(LPVOID) {
    bool rotating = false;
    // This thread owns the presentation window, and anything posted to it has to
    // be dispatched here or cross-thread calls to that window block forever.
    auto Pump = [] {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    };
    for (int i = 0; i < 400; ++i) {  // wait up to 20s for the window
        Sleep(50);
        Pump();
        TidyWindow();
        if (g_wnd && g_rotation.enabled && !rotating) {
            // Rotation needs the window to exist, and creating a window from
            // inside the game's own Present faults in d3d8.dll, so it happens
            // here rather than on the render thread.
            if (rotate::Start(g_wnd, g_rotation)) rotating = true;
        }
        if (rotating) break;
    }
    for (;;) {
        Sleep(250);
        if (g_wnd && !IsWindow(g_wnd)) break;
        Pump();
        TidyWindow();
        KeepCursorHidden();
        if (rotating) rotate::KeepAboveGame();
    }
    return 0;
}

}  // namespace

void* STDMETHODCALLTYPE Hooked_Direct3DCreate8(UINT version) {
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) d3d8 = LoadLibraryA("d3d8.dll");
    if (!d3d8) {
        log::Write("present: d3d8.dll not found");
        return nullptr;
    }

    typedef IDirect3D8*(STDMETHODCALLTYPE* CreateFn)(UINT);
    CreateFn realCreate =
        reinterpret_cast<CreateFn>(GetProcAddress(d3d8, "Direct3DCreate8"));
    if (!realCreate) {
        log::Write("present: Direct3DCreate8 not exported by d3d8.dll");
        return nullptr;
    }

    IDirect3D8* real = realCreate(version);
    if (!real) return nullptr;

    if (!g_factoryVtbl) {
        g_factoryVtbl = new void*[kFactoryMethodCount];
        if (!g_factoryVtbl) return real;

        void** realVtbl = *reinterpret_cast<void***>(real);
        for (int i = 0; i < kFactoryMethodCount; ++i) {
            void* fwd = MakeForwarder(real, realVtbl + i);
            if (!fwd) {
                delete[] g_factoryVtbl;
                g_factoryVtbl = nullptr;
                return real;  // cannot proxy; run unmodified rather than crash
            }
            g_factoryVtbl[i] = fwd;
        }
        g_factoryVtbl[0] = (void*)&Proxy_QueryInterface;
        g_factoryVtbl[1] = (void*)&Proxy_AddRef;
        g_factoryVtbl[2] = (void*)&Proxy_Release;
        g_factoryVtbl[kCreateDeviceIndex] = (void*)&Hooked_CreateDevice;
    }

    FactoryProxy* proxy = new FactoryProxy{g_factoryVtbl, real, 1};
    if (!proxy) return real;
    log::Write("present: Direct3DCreate8 proxied");
    return reinterpret_cast<void*>(proxy);
}

void Start() {
    Settings s = ReadSettings();
    g_hideCursorEnabled = s.hideCursor;
    g_rotation = s.rotation;
    if (!s.windowed && !s.borderless && !s.hideCursor && !s.rotation.enabled)
        return;

    HANDLE t = CreateThread(NULL, 0, &WindowThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

}  // namespace present