# gwg-native — running GigaWing Generations natively on Windows

A small self-contained launcher + injected shim that lets the arcade build of
**GigaWing Generations** (`game.exe`, 2004, Sega Chihiro) run on an ordinary
Windows 10/11 desktop, without TeknoParrot or any other third-party tool.

> **GigaWing Generations is © Takumi Corporation.** This project is not
> affiliated with, endorsed by, or sponsored by Takumi or Sega. No game data,
> code or assets are included here — you supply your own copy, and nothing in
> this repository modifies your game folder on disk.

```
Native\
  gwg-native.exe        launcher (32-bit)
  gwg-native.ini        configuration
  bin\gwgshim32.dll      injected shim (32-bit)
  deps\dx8\              DirectX 8 SDK headers - not redistributed, see deps\README.md
  build.cmd             one-shot build
  src\launcher\          launcher sources
  src\shim\             shim sources
```

## Build and run

```bat
cd Native
build.cmd
gwg-native.exe
```

`build.cmd` needs Visual Studio 2022 with the x86 toolset (it calls
`vcvarsall x86`), and it needs the **DirectX 8 SDK headers** in `deps\dx8\`.
Those are not redistributed here — see [`deps/README.md`](deps/README.md) for
what to fetch and why. Everything is built 32-bit so the launcher can drive the
32-bit game process directly.

The game data stays where it is — `game.exe`, `YOKUSHIN.mbd` and friends are
byte-for-byte untouched. The shim lives in `Native\bin\` and is injected by
absolute path.

**Do not put a ReShade / d3d8to9 `d3d8.dll` or `d3d9.dll` in the game folder.**
The shim is built against the real `SysWOW64\d3d8.dll`, and a middle-man DLL in
that chain changes which interface the game ends up talking to (see
"Rotation"). The CRT shader stack can be layered back on afterwards as a
separate step.

## Status

| Area | State |
| --- | --- |
| Injection (PEB-based, no toolhelp/ASLR assumptions) | **done** |
| IAT hooking of 16 Win32/DirectX entry points | **done** |
| Sega AM JVS I/O board emulation (the `I/O ERROR` cause) | **done** |
| Board self-test bypass | **done** |
| Virtual DirectInput so the game does not bail out | **done** |
| Coin insert / start / shoot / bomb from the keyboard | **done** |
| Borderless, centred, windowed (desktop resolution untouched) | **done** |
| Cursor hidden over the game window | **done** |
| Game reaches attract mode / demo gameplay | **done** |
| 90° rotation of the portrait framebuffer | **done** — see below |
| Frame cap | not done (the game self-limits to ~60 fps unaided) |


The game boots, completes the full arcade board bring-up, settles into attract
mode and plays its demo indefinitely.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys | stick |
| `Z` / `X` | shot / bomb |
| `F1` | start (player 1) |
| `F2` | **coin** |
| `F11` | test switch |
| Numpad `0`/`1`/`2` | player 2 start / shot / bomb |

Coins are credited from the board's **digital input byte**, not from its
`0x21` coin-counter command. This is worth stating because it looks like a
missing feature otherwise: wiring a key to the coin counter does nothing,
because this title never asks for the counter. The `0x21` path has been removed
rather than left as dead code.

## Why it needs a shim at all

The game is a perfectly ordinary 32-bit Win32 PE: MSVC 7.10, importing only
`KERNEL32/USER32/GDI32/COMCTL32/IMM32/ADVAPI32/SHELL32/ole32/WINMM` plus
`DSOUND`, `DINPUT8` and `d3d8`. Every one of those exists on Windows 10/11
(`d3d8.dll` lives in `SysWOW64`, which is what a 32-bit process sees as System32).
Nothing needs emulating at the OS level.

What is missing is the **Sega AM JVS I/O board** — the coin counter, LED board
and service switch. On real hardware that is a serial device on `COM2`. On a PC
there is no such device, the game notices, shows `I/O ERROR` and exits.

## The I/O board

### Serial API use

| Address | Call | Value |
| --- | --- | --- |
| `0x520933` | `CreateFileA` | `"COM2"`, `GENERIC_READ\|WRITE`, no sharing, `OPEN_EXISTING` |
| `0x520952` | `SetupComm` | 516-byte in/out queues |
| `0x5209A9` | `SetCommState` | **115200 baud, 8 data bits** |
| `0x48F9BC` | `SetCommMask` | `EV_RXCHAR` |
| `0x5209FF` | `SetCommTimeouts` | read total timeout 20 ms |
| `0x5207F0` | `GetCommModemStatus` | returns `(msr >> 4) & 1` — i.e. **CTS** |
| `0x520850` | read helper | `ClearCommError` + `ReadFile` |
| `0x520380` | packet parser | see below |
| `0x520820` | write helper | `WriteFile` |

Each has exactly one call site, so intercepting the whole surface through the
game's IAT is sufficient — no detour engine needed.

### Wire format

```
E0 <node> <len> <command> <args...> <checksum>          request
E0 <node> <len> <status> <report> <data...> <checksum>  reply
len      == bytes after the length byte, checksum included
checksum == node + len + sum(everything after the length byte, checksum excluded)
```

Confirmed against every packet in `gwg-native.log`, and the reply form matches
TeknoParrot's `JvsHelper.CraftJvsPackageWithStatusAndReport` exactly. The
board's identity string comes back byte-for-byte:

```
TX E0 01 02 10 13
RX E0 00 39 01 01 53 45 47 41 ...   "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10\0"
```

Two details that are easy to get wrong: the length byte seeds a countdown
decremented *before* each byte, so the last of them is the checksum and only
`len-1` bytes are payload; and the reply's payload must carry `0x01` at indices
**0**, **8** and **9**, because the game's parser dispatches on the first byte.

### Command handling

| Command | Reply |
| --- | --- |
| `0xF0` reset | **silence** — the game never reads a reply to it |
| `0xF1` address | empty payload; raises CTS once all channels are reported |
| `0x10` | the identification string above |
| `0x11` | `0x13` (command revision) |
| `0x12` | `0x20` (JVS version) |
| `0x13` | `0x10` (communication version) |
| `0x14` | 17-byte slave feature descriptor (2 players / 14 switches, 2 coin slots, 8 analog channels, 20 GPIO) |
| `0x20` | digital inputs — one cabinet byte plus 2 bytes per player |
| `0x04` | empty (unknown to every published JVS table; see below) |

`0x04` is polled to node 0 at the same rate as the digital read and is not a JVS
command in any published table — TeknoParrot treats it as Taito-specific and
answers it as an error. Serving it as a coin count was tried and had no effect,
so it is answered with an empty payload, which the game tolerates. Since coins
turn out to come from the digital byte, this no longer matters either way.

Digital input byte layout, matching TeknoParrot's `GetPlayerControls`:

```
0x80 start   0x40 coin 1   0x20 up    0x10 down
0x08 left    0x04 right    0x02 shot   0x01 bomb
```

### Handshake and CTS

`0x51FC40` – `0x520166` runs:

1. send `0xF0`, `Sleep(1000)`, then require CTS to go **low**;
2. loop up to 32 times: send `0xF1`, read a reply, stop as soon as CTS goes
   **high** — the number of completed rounds is the board's **channel count**;
3. read each channel with `0x10`–`0x14`, stored at `0x5BD220` with a `0x183`-byte
   stride.

CTS is the whole ball game, and getting it wrong is silent. The behaviour that
works:

* **reset** — drop CTS, raise it after `BusyMs` (the game polls about once a
  second);
* **`0xF1` rounds** — keep CTS low for the first `Rounds-1` rounds, raise it on
  the last, which is what makes the game enumerate two channels;
* **every other command** — leave CTS **asserted**. A real board answers in a
  couple of milliseconds at 115200 baud.

That last point was the single blocker between "boots and exits" and "runs":
the game's post-bring-up check at `0x520087` reads CTS, and while the shim held
it low for `BusyMs` the game concluded the board had failed, set the I/O status
to `-3` (`[0x5A9418]`) and gave up.

### Board self-test

Even with a perfect exchange the game still refused to continue, because its
hardware self-test compares a result code and, on failure, stores `"coinerror"`
or `"i/oerror"` into the error field at `game.exe + 0xFBC` (`0x43E3FC`–`0x43E482`).
`0x44A085` then compares that field against `"i/oerror"` and, on a match, prints
the `I/O ERROR` screen before quitting (`0x44A0C5`).

Both failure stores sit behind a short conditional jump:

```
0x43E426  74 17  je 0x43E43F   ; do not store "coinerror"
0x43E469  74 17  je 0x43E482   ; do not store "i/oerror"
```

Flipping the opcode from `74` (jump if equal) to `EB` (jump always) leaves the
displacement untouched and takes the same "hardware is fine" path the game takes
when the check passes. This is the same kind of fix OpenParrot applies
("error display routine noping"), applied through the generic patcher below
rather than hard-coded.

## DirectInput

`src/shim/arcade_input.cpp` mirrors the shape of OpenParrot's TypeX
`DirectInputWrapper`, which is the proven approach for this title:

* `DirectInput8Create` **never calls the real DirectInput** — it returns a
  static fake `IDirectInput8`;
* `EnumDevices` returns `DI_OK` **without ever invoking the game's callback**.

The second point is the important one. `game.exe` was compiled against the
classic 24-method `IDirectInput8`, and its enumeration callback reaches
`GetDefaultCompatGUID` (vtable `+0x2C`) and `GetDeviceType` (`+0x30`/`+0x34`),
which do not exist in the 9-method "create device" interface modern SDKs and
OpenParrot use. By never invoking the callback the game never touches those
slots, so the short vtable is safe. Gameplay input is delivered by other means;
this shim's job is only to stop the game bailing out when it cannot find arcade
hardware.

Traps recorded here, all hit during development:

* the game uses the **classic five-parameter** `DirectInput8Create`
  `(hinst, dwDataType, lpGUID, lplpDirectInput, dwReserved)` — there is no
  `rguidIid`, and the GUID is `CLSID_DirectInput8` at `0x59D03C`;
* a hand-built COM object's **first field must be the vtable pointer**. With
  `refCount` first the game read `1` as the vtable and faulted reading
  `[1 + 0x10]`;
* `game.exe` computes `edi = &slot` before the call and dereferences it again
  afterwards, so it depends on `edi` surviving the call — the real
  `dinput8.dll` happens to preserve it, and so must the hook.

## Presentation

`src/shim/present.cpp` replaces `Direct3DCreate8` with a proxy whose vtable is
generated slot by slot; slot 15 (`CreateDevice`) is swapped for a wrapper that
forces `Windowed = TRUE`. Without that the game drags the whole desktop down to
its own 640×480 mode. A tidying thread then strips the frame, centres the window
and hides the cursor.

Proxying a COM vtable by copying it has one sharp edge worth stating: the
inherited methods would run against the proxy object rather than the real one,
so they need their `this` rewritten. Typed forwarders are not safe here (see
"Rotation"), so the shim generates tiny stubs instead:

```
mov  eax, <realObject>
mov  [esp+4], eax
jmp  dword ptr [<slot>]
```

which is correct for any stdcall signature and any argument count.

### Forcing windowed mode

Real d3d8 rejects the game's own parameter set once `Windowed` is forced on —
`D3DERR_INVALIDCALL`, with no indication of which field it disliked. The shim
walks a ladder from "as given" outwards and keeps the first rung that is
accepted, logging each attempt. On this machine the rung that works is
**"from-scratch minimal set"**: the parameters built from zero rather than
derived from the game's, because one of the values the game fills in
(`AutoDepthStencilFormat`, and the sampler quality field the vendored header
cannot even name) is what this driver objects to.

That the windowed path works at all is worth recording, because a standalone
probe of `CheckDeviceType(windowed = TRUE)` says otherwise — the difference is
that d3d8 rejects a NULL focus window outright, so any probe has to supply a
real one.

### Rotation

The game renders a 640x480 back buffer whose contents are portrait and already
turned on its side, so it used to display sideways. Rotation turns it back and
shows it upright in a borderless window covering the desktop, scaled uniformly
and centred with black bars down the sides.

Set `Rotate = 1` under `[Presentation]`. The other settings there are
`RotateFps`, `RotateDir`, `RotateSmooth` and `RotateSliver`.

`src/shim/rotate.cpp` does the work with nothing but Win32 calls: capture the
game window with GDI, turn the picture a quarter turn, scale it into a
borderless `WS_EX_NOACTIVATE | WS_EX_TOPMOST` window. No Direct3D is involved in
the output half at all, which matters because the Direct3D 8 method order is
the one thing here that cannot be read off a header - see
`analysis/FINDINGS.md`.

Two things were found the hard way and are worth not rediscovering:

* **A fully covered Direct3D window cannot be captured.** Once nothing of the
  game window is visible the driver stops presenting into it and a GDI capture
  comes back blank. Moving the window off the desktop is worse: the device is
  lost and it goes black. So the game window is parked against the bottom of the
  screen and the presentation window stops short by `RotateSliver` pixels,
  leaving a sliver of the sideways picture visible along the bottom edge. That
  sliver is the one compromise in the design; it cannot simply be set to zero.
* **`FillRect` with a null brush draws nothing.** `RGB(0,0,0)` is 0, so using it
  as the bar colour silently leaves the bars showing whatever was in the window
  before - white. `GetStockObject(BLACK_BRUSH)` is what is wanted.

#### Known issues

* **A sliver of the unrotated picture is still visible**, 2 pixels of it along
  the bottom edge by default. It cannot simply be removed: see the capture notes
  above. `RotateSliver` adjusts it, and zero does not work - the picture goes
  blank. Removing it properly means reading the back buffer, which needs
  `GetBackBuffer`'s index established by probing the device vtable, the same way
  slot 15 was.

Fixed since first reported:

* **Flicker.** The picture is composed in an off-screen buffer and handed to the
  window in a single blit, and only the picture area is copied - the bars never
  change, so they are cleared only when the picture moves or the window is
  invalidated. Drawing straight onto the window let the desktop pick it up part
  way through a frame. Both were worth doing: the first stops a half-drawn frame
  being shown, the second is most of what the frame rate was being spent on.

* **Always on top.** The presentation window is an ordinary top-level window that
  is raised with `HWND_TOP` only while the game has the focus, and it only
  presents at all while the game has the focus. Switching to another application
  brings that one in front.

Two Windows behaviours cost real time here and are worth knowing about:

* **An owned popup window deadlocks the process.** Making the presentation window
  owned by the game window is the obvious way to say "float above the game and
  nothing else", and it worked right up until the moment focus moved between the
  two - at which point the whole process stopped, not just one thread.
  Reproducible on the first alt-tab cycle.

* **`SetWindowPos` on a window owned by another thread blocks if that thread does
  not pump messages.** The compositing thread raising the presentation window
  deadlocked in exactly this way, because the window thread that owns it only
  slept. Two fixes: raise it from the owning thread, and pump messages there.

### `Present` is slot 15

The earlier belief that `Present` could not be identified was wrong. It is slot
15, offset `0x3C`, and it is now hooked - `present.cpp` intercepts it and
forwards to the real method unchanged, which is enough to confirm it and is the
hook a future Direct3D-level rotation would use.

The frame function at `0x41D9B0` calls slot 3 and compares the result against
`D3DERR_DEVICELOST`, which identifies slot 3 as `TestCooperativeLevel`, and then
calls slot 15 with four NULL arguments. Four NULL arguments after a
cooperative-level check is `Present(NULL, NULL, NULL, NULL)`, the fullscreen
call the game was written to make.

The layout was measured rather than assumed, with counting stubs on a copy of
the device vtable that record the caller's return address per slot before
forwarding. Details, and the full call map, are in `analysis/FINDINGS.md`; the
probe is `DeviceProbe = 1` under `[Diagnostics]`.

Worth noting: **the classic layout was right and the "slot 17" figure recorded
earlier was wrong.** Slots 3, 4 and 15 all sit where the classic layout puts
them. The layout does diverge later - slot 16 takes one argument, and slot 83's
third argument is `0x24` rather than a pointer - so Windows' `d3d8.dll`, a
hand-written wrapper over Direct3D 9, is not the SDK header after about slot 15.

The game also **never calls `GetBackBuffer`** - it renders implicitly and never
asks for the surface. That rules out hooking it to obtain the back buffer, and is
why rotation captures the window instead.


## Patches

`[Patches]` holds `ADDRESS=OLD>NEW` entries (hex, virtual addresses at the
default image base `0x00400000`), separated by semicolons. `OLD` is verified
before writing and a mismatch is reported and skipped. They are applied in
memory only; the file on disk is never touched.

```
[Patches]
SkipBoardErrors = 0043E426=74>EB;0043E469=74>EB
```

## How injection works

`CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` returns `ERROR_PARTIAL_COPY` for
a WOW64 target, and a process created with `CREATE_SUSPENDED` has not built its
loader list yet (`PEB->Ldr` is `NULL`). Both shortcuts fail here, so the
launcher instead:

1. starts the game suspended, resumes it, and waits `InjectDelayMs`;
2. reads the target's PEB with `NtQueryInformationProcess` +
   `ReadProcessMemory` (which *does* work across WOW64);
3. walks `InMemoryOrderModuleList` to find `KERNEL32.DLL`;
4. walks that module's export table for `LoadLibraryW`;
5. starts a remote thread there with the shim's path as its argument.

`InjectDelayMs` defaults to 100 ms. It has to be short: the game creates its
window and its Direct3D device within a few hundred milliseconds of start, and
the presentation proxy only sees the call if it is installed first.

### Traps worth knowing about

* `sizeof(IMAGE_THUNK_DATA32)` is 8 (two unions), but each entry in an on-disk
  PE32 import table is 4 bytes. Indexing the thunk array with the struct stride
  walks twice as far and dereferences a bogus pointer.
* The IAT lives in `.rdata`, which the loader leaves **read-only** after
  resolving imports. `VirtualProtect` is needed before writing to it.
* A `..` in the game path makes the game fail its own data-file lookup (it
  resolves relative to `GetModuleFileNameA()`), which leaves the Direct3D device
  `NULL` and crashes the game at `0x0041B8F8`. The launcher normalises the path
  with `GetFullPathNameW`, so a relative `Exe=` in the ini is safe.

## Diagnostics

* `gwg-native.log` — hook installation, every serial packet in both directions,
  DirectInput and presentation activity, and any unhandled fault (including a
  stack scan that names the call site).
* `GWG_NATIVE_PEBDEBUG=1` — dump the target's module list before injection.
* `[Hooks] Skip=...` — leave individual hooks uninstalled to isolate one.
* `[Diagnostics] TracePC = 1` — sample the main thread's instruction pointer.
  This is what located the CTS wait at `0x51F845`; the spinning code calls
  nothing the shim hooks.
* `[Diagnostics] DeviceProbe = 1` — wrap the game's device in counting stubs and
  log every vtable slot it calls, with the call sites in `game.exe` each came
  from. This is how the `IDirect3DDevice8` method order was established; see
  `analysis/FINDINGS.md`. Inert by default, and safe to leave on.
* `[IoBoard] Echo=1` — echo everything the game writes, for comparing framings.
* `[IoBoard] Rounds=N` — change the reported channel count (2 is correct).
* `[IoBoard] BusyMs=N` — how long CTS stays low after a reset.
* `[IoBoard] Reply=<hex>` — override the canned reply packet.

## Credits

The board protocol, the DirectInput approach and the self-test bypass were all
cross-checked against [OpenParrot](https://github.com/teknogods/OpenParrot) and
[TeknoParrotUI](https://github.com/teknogods/TeknoParrotUI), whose JVS
implementation is the authoritative description of what this board expects.