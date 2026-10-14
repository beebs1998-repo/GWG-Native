# Direct3D 8 findings

Notes from working out how `game.exe` actually uses Direct3D, and why the
rotation works the way it does. The short version: **the vtable order could not
be read off any header, so it was measured, and the measurement is below.**

## The vtable order does not match the headers

`deps/dx8/d3d8.h` is a trimmed, later revision of the DirectX 8 SDK and
disagrees with the classic 2002 layout. Neither can be trusted against the
Windows `d3d8.dll` that is actually loaded, so the layout was measured instead
of assumed.

## How it was measured

`analysis/callsites.py` inventories every `call dword ptr [reg+disp]` in
`.text`, which is the game's vtable dispatch. That alone gives the set of
offsets in play but not which is which, because it cannot see arguments.

`src/shim/devprobe.cpp` closes the gap at runtime. It wraps the game's device
in a proxy whose every slot is a stub that records the caller's return address
and then forwards unchanged:

```
mov  eax, [esp]                   ; return address
mov  ecx, <this slot's site list>
...record it if not already listed...
inc  dword ptr [counter]
mov  eax, <realDevice>
mov  [esp+4], eax
jmp  dword ptr [<realSlot>]
```

Only EAX, ECX and EDX are touched, all caller-saved, so the tail jump leaves the
real method with the state it expects. Nothing is called and no stack slot is
disturbed, so a stub cannot fault. Every distinct return address is recorded, up
to 16 per slot, because one method is normally reached through one dispatch
instruction per call site and the code is then disassembled offline.

Enable with `DeviceProbe = 1` under `[Diagnostics]`; it is off by default.

## What the game calls

Over 2.5 s of attract mode, per frame:

| Slot | Offset | Calls | Identified as |
| --- | --- | --- | --- |
| 3 | `0x0C` | per frame | `TestCooperativeLevel` |
| 4 | `0x10` | per frame | `GetAvailableTextureMem` |
| 15 | `0x3C` | per frame | **`Present`** |
| 34 | `0x88` | per frame | - |
| 35 | `0x8C` | per frame | - |
| 23 | `0x5C` | per frame | 4 arguments |
| 36 | `0x90` | per frame | 4 arguments |
| 70 | `0x118` | per frame | 3 arguments |
| 83 | `0x14C` | per frame | 3 arguments, not `GetBackBuffer` |
| 40 | `0xA0` | 3 per frame | `SetStreamSource` |
| 28 | `0x70` | ~0.4 per frame | 4 arguments |
| 50 | `0xC8` | 68 total | 2 arguments |
| 63 | `0xFC` | 82 total | 3 arguments |
| 79, 91 | | 7, 2 total | 3 arguments |
| 16, 24, 31, 33, 75, 76, 87, 88 | | 1-2 each | setup |

`TestCooperativeLevel` is identified because at `0x41DA0F` its result is
compared against `D3DERR_DEVICELOST` (`0x88760869`). That pins slot 3, and
therefore the start of the layout.

### `Present` is slot 15

The frame function at `0x41D9B0` is:

```asm
0x41d9e5  mov  eax, [edi+8]
0x41d9e8  mov  ecx, [eax]
0x41d9ea  push eax
0x41d9eb  call [ecx+0xc]      ; slot 3  TestCooperativeLevel
0x41d9ee  xor  ebx, ebx
0x41d9f0  cmp  eax, ebx
0x41d9f2  jl   0x41da0f       ; -> checks for D3DERR_DEVICELOST
0x41d9f4  mov  eax, [edi+8]
0x41d9f7  mov  edx, [eax]
0x41d9f9  push ebx            ; 0
0x41d9fa  push ebx            ; 0
0x41d9fb  push ebx            ; 0
0x41d9fc  push ebx            ; 0
0x41d9fd  push eax            ; this
0x41d9fe  call [edx+0x3c]     ; slot 15
```

Four NULL arguments immediately after a cooperative-level check is
`Present(NULL, NULL, NULL, NULL)` — a fullscreen call, which is what the game
was written to do. The first three are `pSourceRect`, `pDestinationRect` and
`hWndOverride`; the fourth is `pDirtyRegion`. Confirmed by forwarding slot 15 to
the real method and watching the game run unchanged.

**The classic layout was right and the "slot 17" figure recorded earlier was
wrong.** Slots 3, 4 and 15 all sit where the classic layout puts them.

The layout does diverge later, though. Slot 16 is called once with a single
argument, and slot 83 takes three arguments whose third is `0x24` - not a
pointer - so slot 83 is not `GetBackBuffer`. Windows' `d3d8.dll` is a
hand-written wrapper over Direct3D 9 and its method order is not required to
match the SDK header.

## The game never asks for its back buffer

Worth recording, because it rules out the obvious way to read pixels: no
`GetBackBuffer` call exists to hook. The game renders implicitly and never
queries the surface. Combined with the layout above, the back buffer cannot be
had by calling a looked-up slot, so rotation does not use it.

## A covered Direct3D window cannot be blitted

Measured, not assumed:

| Game window | GDI capture |
| --- | --- |
| visible and unobscured | correct picture |
| fully covered by our window | **blank** (uniform white) |
| moved off the desktop | black - the device is lost |

Once a window is completely covered the driver stops presenting into it, so
there is nothing left to read. That is why `rotate.cpp` parks the game window
against the bottom of the screen and stops its own window a few pixels short of
the bottom edge, leaving a sliver of the game window uncovered. Without that
sliver there is nothing to capture. `RotateSliver` controls its width.

## PrintWindow solves it, and then deadlocks

`PrintWindow` with `PW_RENDERFULLCONTENT` goes through DWM rather than sending
`WM_PRINT`, and it **does** return the correct picture for a fully covered
Direct3D window. Verified with the game window entirely behind an opaque topmost
window: the picture came back intact. That would remove the sliver altogether.

It cannot be used. It deadlocks the *whole process* - zero CPU, every thread
stuck, no further log output - when the game is switched away from and back, and
did so on the first focus cycle after several thousand successful captures. The
process has to be killed to recover. The capture itself is not slow; it is
returning from it that never happens.

## An owned popup deadlocks the process on focus change

Making the presentation window owned by the game window is the obvious way to
say "float above the game and nothing else", and it worked until focus moved
between the two. After that the process was dead in the same way as above.

Isolated by removing the ownership alone: 6 focus cycles passed. `WS_EX_TOPMOST`
is not needed either - an ordinary window raised with `HWND_TOP` while the game
has focus ends up above the game window and below whatever the user switches to.

## SetWindowPos across threads needs the owning thread to pump messages

The compositing thread stopped compositing entirely once the z-order work went
in. The cause was calling `SetWindowPos` on the presentation window from the
compositing thread: that window belongs to the window thread, which only ever
slept, and the call sends a synchronous message nothing was going to process.

Two changes fix it, and both are worth having anyway: raise from the thread that
created the window, and pump messages on it.

## Capturing from inside Present is expensive

Capturing inside the hooked `Present`, so the frame is guaranteed complete, runs
at about 38 fps. The identical capture from the compositing thread runs at 62.
The blit of a window whose surface the device is mid-way through presenting is
markedly slower than the same blit from outside, and between presents the surface
is complete anyway.

## Notes on the two binaries

`game.exe` and `backups/game.exe` are the same program. They differ in 58 bytes
across 2 runs, both inside `.rdata`; `.text` is byte-identical, so every code
address quoted here is valid in either.

## Bugs hit while measuring, for the record

Both were in the stub generator, and both produced a fault that looked like a
Direct3D problem:

- The SIB byte for `[ecx+edx*4]` was `0x11`, which decodes as `[esp+ecx]`. It
  needs `0x91` (scale x4, index edx, base ecx).
- Short-jump fixups were written at the opcode byte instead of the displacement
  byte, overwriting the opcode, and were measured from one byte too early.