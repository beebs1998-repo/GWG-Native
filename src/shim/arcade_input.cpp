// Virtual DirectInput for GigaWing Generations.
//
// This mirrors the shape of OpenParrot's TypeX DirectInput shim
// (OpenParrot/src/Functions/Games/TypeX2/DirectInputWrapper.cpp), which is the
// proven approach for this title:
//
//   * DirectInput8Create never calls the real DirectInput at all - it just hands
//     back a static fake IDirectInput8.
//   * EnumDevices returns DI_OK *without ever invoking the game's callback*.
//
// The second point is the important one. game.exe was compiled against the
// classic 24-method IDirectInput8 and its enumeration callback reaches
// GetDefaultCompatGUID (vtable +0x2C) and GetDeviceType (+0x30/+0x34), which do
// not exist in the 9-method "create device" interface that modern SDKs - and
// OpenParrot - use. By never invoking the callback the game never touches those
// slots, so the short vtable is safe. Current Windows SDKs no longer ship the
// classic interface at all, so both vtables here are built by hand.
//
// Real gameplay input is delivered separately: this shim's job is to stop the
// game bailing out because it cannot find arcade hardware.

#define UNICODE
#define _UNICODE
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <stdio.h>
#include <string.h>

#include "log.h"

#ifndef DIERR_UNSUPPORTED
#define DIERR_UNSUPPORTED E_NOTIMPL
#endif
#ifndef DIERR_NOTREGISTERED
#define DIERR_NOTREGISTERED E_NOTIMPL
#endif
#ifndef DIERR_NOTATTACHED
#define DIERR_NOTATTACHED E_FAIL
#endif
#ifndef DI_NOEFFECT
#define DI_NOEFFECT S_OK
#endif

namespace arcade {
namespace {

// GUID values, defined locally so the shim needs no uuid.lib.
const GUID kIUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIDirectInputDevice8W = {
    0x54D41081, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

bool SameGuid(const GUID& a, const GUID& b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// --- key mapping ---------------------------------------------------------

bool Down(int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Three player buttons on Z/X/C, then the cabinet controls on the remaining
// slots (coin, coin, start, service, test).
struct Mapping {
    LONG vkXNeg, vkXPos, vkYNeg, vkYPos;
    BYTE buttons[10];
};

Mapping g_map = {
    VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
    {'Z', 'X', 'C', VK_F3, VK_F4, '1', VK_F1, VK_F2, VK_F11, VK_SPACE},
};

// --- the fake device -----------------------------------------------------
//
// Plain __stdcall functions taking the object as the first argument, with the
// vtable as an array of them. The objects are static, so there is no lifetime
// management to get wrong.

struct VirtualDevice {
    void** vtbl;  // must be first: COM callers read the vtable from offset 0
    DWORD statePolls;
};

HRESULT __stdcall Dev_QueryInterface(VirtualDevice*, REFIID, void**);
ULONG __stdcall Dev_AddRef(VirtualDevice*);
ULONG __stdcall Dev_Release(VirtualDevice*);
HRESULT __stdcall Dev_GetCapabilities(VirtualDevice*, void*);
HRESULT __stdcall Dev_EnumObjects(VirtualDevice*, void*, void*, DWORD);
HRESULT __stdcall Dev_GetProperty(VirtualDevice*, REFGUID, void*);
HRESULT __stdcall Dev_SetProperty(VirtualDevice*, REFGUID, void*);
HRESULT __stdcall Dev_Acquire(VirtualDevice*);
HRESULT __stdcall Dev_Unacquire(VirtualDevice*);
HRESULT __stdcall Dev_GetDeviceState(VirtualDevice*, DWORD, void*);
HRESULT __stdcall Dev_GetDeviceData(VirtualDevice*, DWORD, void*, LPDWORD, DWORD);
HRESULT __stdcall Dev_SetDataFormat(VirtualDevice*, LPCDIDATAFORMAT);
HRESULT __stdcall Dev_SetEventNotification(VirtualDevice*, HANDLE);
HRESULT __stdcall Dev_SetCooperativeLevel(VirtualDevice*, HWND, DWORD);
HRESULT __stdcall Dev_GetObjectInfo(VirtualDevice*, void*, DWORD, DWORD);
HRESULT __stdcall Dev_GetDeviceInfo(VirtualDevice*, void*);
HRESULT __stdcall Dev_Poll(VirtualDevice*);
HRESULT __stdcall Dev_RunControlPanel(VirtualDevice*, HWND, DWORD);
HRESULT __stdcall Dev_Initialize(VirtualDevice*, HINSTANCE, DWORD, REFGUID);
HRESULT __stdcall Dev_CreateEffect(VirtualDevice*, REFGUID, void*, void**, LPUNKNOWN);
HRESULT __stdcall Dev_EnumEffects(VirtualDevice*, void*, void*, DWORD);
HRESULT __stdcall Dev_GetEffectInfo(VirtualDevice*, void*, REFGUID);
HRESULT __stdcall Dev_GetForceFeedbackState(VirtualDevice*, LPDWORD);
HRESULT __stdcall Dev_SendForceFeedbackCommand(VirtualDevice*, DWORD);
HRESULT __stdcall Dev_EnumCreatedEffectObjects(VirtualDevice*, void*, void*, DWORD);
HRESULT __stdcall Dev_Escape(VirtualDevice*, void*);
HRESULT __stdcall Dev_SendDeviceData(VirtualDevice*, DWORD, void*, LPDWORD, DWORD);
HRESULT __stdcall Dev_EnumEffectsInFile(VirtualDevice*, LPCWSTR, void*, void*, DWORD);
HRESULT __stdcall Dev_WriteEffectToFile(VirtualDevice*, LPCWSTR, DWORD, void*, DWORD);
HRESULT __stdcall Dev_BuildActionMap(VirtualDevice*, void*, LPCWSTR, DWORD);
HRESULT __stdcall Dev_SetActionMap(VirtualDevice*, void*, LPCWSTR, DWORD);
HRESULT __stdcall Dev_GetImageInfo(VirtualDevice*, void*);

void* const kDeviceVtbl[] = {
    (void*)&Dev_QueryInterface,           (void*)&Dev_AddRef,
    (void*)&Dev_Release,                  (void*)&Dev_GetCapabilities,
    (void*)&Dev_EnumObjects,              (void*)&Dev_GetProperty,
    (void*)&Dev_SetProperty,              (void*)&Dev_Acquire,
    (void*)&Dev_Unacquire,                (void*)&Dev_GetDeviceState,
    (void*)&Dev_GetDeviceData,            (void*)&Dev_SetDataFormat,
    (void*)&Dev_SetEventNotification,     (void*)&Dev_SetCooperativeLevel,
    (void*)&Dev_GetObjectInfo,            (void*)&Dev_GetDeviceInfo,
    (void*)&Dev_Poll,                     (void*)&Dev_RunControlPanel,
    (void*)&Dev_Initialize,               (void*)&Dev_CreateEffect,
    (void*)&Dev_EnumEffects,              (void*)&Dev_GetEffectInfo,
    (void*)&Dev_GetForceFeedbackState,    (void*)&Dev_SendForceFeedbackCommand,
    (void*)&Dev_EnumCreatedEffectObjects, (void*)&Dev_Escape,
    (void*)&Dev_SendDeviceData,           (void*)&Dev_EnumEffectsInFile,
    (void*)&Dev_WriteEffectToFile,        (void*)&Dev_BuildActionMap,
    (void*)&Dev_SetActionMap,             (void*)&Dev_GetImageInfo,
};

VirtualDevice g_device = {(void**)kDeviceVtbl, 0};

HRESULT __stdcall Dev_QueryInterface(VirtualDevice*, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (SameGuid(riid, kIUnknown) || SameGuid(riid, kIDirectInputDevice8W)) {
        *out = &g_device;
        return DI_OK;
    }
    *out = nullptr;
    return DIERR_UNSUPPORTED;
}

// Static objects: never actually freed. Report a non-zero refcount so a
// defensive Release() cannot tear anything down.
ULONG __stdcall Dev_AddRef(VirtualDevice*) { return 2; }
ULONG __stdcall Dev_Release(VirtualDevice*) { return 0; }

// DIDEVCAPS starts dwSize / dwDevType / wAxes / wButtons / dwFlags.
HRESULT __stdcall Dev_GetCapabilities(VirtualDevice*, void* capsRaw) {
    if (!capsRaw) return E_POINTER;
    BYTE* caps = (BYTE*)capsRaw;
    DWORD size = *(DWORD*)caps;
    if (size < 20) {
        *(DWORD*)caps = 220;
        return DIERR_INVALIDPARAM;
    }
    ZeroMemory(caps, size > 220 ? 220 : size);
    *(DWORD*)caps = 220;
    *(DWORD*)(caps + 4) = DI8DEVTYPE_JOYSTICK;
    *(WORD*)(caps + 8) = 0;
    *(WORD*)(caps + 10) = 10;
    return DI_OK;
}

HRESULT __stdcall Dev_EnumObjects(VirtualDevice*, void*, void*, DWORD) {
    return DI_OK;
}
HRESULT __stdcall Dev_GetProperty(VirtualDevice*, REFGUID, void*) { return DI_OK; }
HRESULT __stdcall Dev_SetProperty(VirtualDevice*, REFGUID, void*) { return DI_OK; }
HRESULT __stdcall Dev_Acquire(VirtualDevice*) { return DI_OK; }
HRESULT __stdcall Dev_Unacquire(VirtualDevice*) { return DI_OK; }

HRESULT __stdcall Dev_GetDeviceState(VirtualDevice* self, DWORD size, void* data) {
    if (!data) return E_POINTER;

    DIJOYSTATE2 js;
    ZeroMemory(&js, sizeof(js));
    js.lX = (Down(g_map.vkXPos) ? 0x7FFF : 0) - (Down(g_map.vkXNeg) ? 0x7FFF : 0);
    js.lY = (Down(g_map.vkYPos) ? 0x7FFF : 0) - (Down(g_map.vkYNeg) ? 0x7FFF : 0);
    for (int i = 0; i < 10; ++i)
        if (Down(g_map.buttons[i])) js.rgbButtons[i] = 0x80;

    DWORD n = size < sizeof(js) ? size : (DWORD)sizeof(js);
    memcpy(data, &js, n);

    if (self->statePolls < 5) {
        self->statePolls++;
        log::Write("arcade: GetDeviceState size=%u -> X=%d Y=%d btn=%02X%02X%02X",
                   size, js.lX, js.lY, js.rgbButtons[0], js.rgbButtons[1],
                   js.rgbButtons[2]);
    }
    return DI_OK;
}

HRESULT __stdcall Dev_GetDeviceData(VirtualDevice*, DWORD, void*, LPDWORD outBytes,
                                    DWORD) {
    if (outBytes) *outBytes = 0;
    return DI_NOEFFECT;
}

HRESULT __stdcall Dev_SetDataFormat(VirtualDevice*, LPCDIDATAFORMAT format) {
    log::Write("arcade: SetDataFormat (dwSize=%u)", format ? format->dwSize : 0);
    return DI_OK;
}

HRESULT __stdcall Dev_SetEventNotification(VirtualDevice*, HANDLE) { return DI_OK; }
HRESULT __stdcall Dev_SetCooperativeLevel(VirtualDevice*, HWND, DWORD) {
    return DI_OK;
}
HRESULT __stdcall Dev_GetObjectInfo(VirtualDevice*, void*, DWORD, DWORD) {
    return DI_OK;
}
HRESULT __stdcall Dev_GetDeviceInfo(VirtualDevice*, void*) { return DI_OK; }
HRESULT __stdcall Dev_Poll(VirtualDevice*) { return DI_NOEFFECT; }
HRESULT __stdcall Dev_RunControlPanel(VirtualDevice*, HWND, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_Initialize(VirtualDevice*, HINSTANCE, DWORD, REFGUID) {
    return DI_OK;
}
HRESULT __stdcall Dev_CreateEffect(VirtualDevice*, REFGUID, void*, void**,
                                   LPUNKNOWN) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_EnumEffects(VirtualDevice*, void*, void*, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_GetEffectInfo(VirtualDevice*, void*, REFGUID) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_GetForceFeedbackState(VirtualDevice*, LPDWORD state) {
    if (state) *state = 0;
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_SendForceFeedbackCommand(VirtualDevice*, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_EnumCreatedEffectObjects(VirtualDevice*, void*, void*, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_Escape(VirtualDevice*, void*) { return DIERR_UNSUPPORTED; }
HRESULT __stdcall Dev_SendDeviceData(VirtualDevice*, DWORD, void*, LPDWORD, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_EnumEffectsInFile(VirtualDevice*, LPCWSTR, void*, void*,
                                       DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_WriteEffectToFile(VirtualDevice*, LPCWSTR, DWORD, void*,
                                        DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_BuildActionMap(VirtualDevice*, void*, LPCWSTR, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_SetActionMap(VirtualDevice*, void*, LPCWSTR, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall Dev_GetImageInfo(VirtualDevice*, void*) {
    return DIERR_UNSUPPORTED;
}

// --- the fake IDirectInput8 ---------------------------------------------

struct FakeInput {
    void** vtbl;  // must be first
};

HRESULT __stdcall In_QueryInterface(FakeInput*, REFIID, void**);
ULONG __stdcall In_AddRef(FakeInput*);
ULONG __stdcall In_Release(FakeInput*);
HRESULT __stdcall In_CreateDevice(FakeInput*, REFGUID, void**, LPUNKNOWN);
HRESULT __stdcall In_EnumDevices(FakeInput*, DWORD, LPDIENUMDEVICESCALLBACK, void*,
                                DWORD);
HRESULT __stdcall In_GetDeviceStatus(FakeInput*, REFGUID);
HRESULT __stdcall In_RunControlPanel(FakeInput*, HWND, DWORD);
HRESULT __stdcall In_Initialize(FakeInput*, HINSTANCE, DWORD);
HRESULT __stdcall In_FindDevice(FakeInput*, REFGUID, LPCWSTR, LPGUID);
HRESULT __stdcall In_EnumDevicesBySemantics(FakeInput*, LPCWSTR, void*, void*, void*,
                                           DWORD);
HRESULT __stdcall In_ConfigureDevices(FakeInput*, void*, void*, DWORD, void*);
// Classic-interface slots past the 9-method set, present only so that a stray
// call cannot run off the end of the array.
HRESULT __stdcall In_Unused(FakeInput*);

void* const kInputVtbl[] = {
    (void*)&In_QueryInterface,      (void*)&In_AddRef,
    (void*)&In_Release,             (void*)&In_CreateDevice,
    (void*)&In_EnumDevices,         (void*)&In_GetDeviceStatus,
    (void*)&In_RunControlPanel,     (void*)&In_Initialize,
    (void*)&In_FindDevice,          (void*)&In_EnumDevicesBySemantics,
    (void*)&In_ConfigureDevices,
    // classic interface continues here
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
    (void*)&In_Unused,              (void*)&In_Unused,
};

FakeInput g_input = {(void**)kInputVtbl};

HRESULT __stdcall In_QueryInterface(FakeInput*, REFIID, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return DIERR_UNSUPPORTED;
}
ULONG __stdcall In_AddRef(FakeInput*) { return 0; }
ULONG __stdcall In_Release(FakeInput*) { return 0; }

HRESULT __stdcall In_CreateDevice(FakeInput*, REFGUID, void** out, LPUNKNOWN) {
    if (out) *out = &g_device;
    return DI_OK;
}

// Deliberately does NOT invoke the game's callback: see the file header. The
// classic-only vtable slots below EnumDevices are then never reached.
HRESULT __stdcall In_EnumDevices(FakeInput*, DWORD, LPDIENUMDEVICESCALLBACK, void*,
                                DWORD) {
    return DI_OK;
}

HRESULT __stdcall In_GetDeviceStatus(FakeInput*, REFGUID) { return DI_OK; }
HRESULT __stdcall In_RunControlPanel(FakeInput*, HWND, DWORD) {
    return DIERR_UNSUPPORTED;
}
HRESULT __stdcall In_Initialize(FakeInput*, HINSTANCE, DWORD) { return DI_OK; }
HRESULT __stdcall In_FindDevice(FakeInput*, REFGUID, LPCWSTR, LPGUID guid) {
    if (guid) *guid = kIDirectInputDevice8W;
    return DI_OK;
}
HRESULT __stdcall In_EnumDevicesBySemantics(FakeInput*, LPCWSTR, void*, void*, void*,
                                           DWORD) {
    return DI_OK;
}
HRESULT __stdcall In_ConfigureDevices(FakeInput*, void*, void*, DWORD, void*) {
    return DI_OK;
}
HRESULT __stdcall In_Unused(FakeInput*) { return DI_OK; }

}  // namespace

bool Install() { return true; }

}  // namespace arcade

// The game calls the classic five-parameter form
// (hinst, dwDataType, lpGUID, lplpDirectInput, dwReserved) - there is no
// rguidIid, and the GUID is the CLSID. We never call the real DirectInput.
HRESULT STDMETHODCALLTYPE Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dataType,
                                                    REFGUID guid, void** out,
                                                    LPUNKNOWN reserved) {
    (void)hinst;
    (void)dataType;
    (void)guid;
    (void)reserved;

    // game.exe computes `edi = &slot` before the call and dereferences it again
    // afterwards, so it relies on edi surviving - the real dinput8.dll happens
    // to preserve it, so we must too.
    DWORD savedEdi;
    __asm { mov savedEdi, edi }

    if (out) *out = &arcade::g_input;

    __asm { mov edi, savedEdi }

    log::Write("arcade: DirectInput8Create -> static fake interface");
    return DI_OK;
}