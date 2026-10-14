# deps/

Third-party headers the shim builds against. Nothing here is ours to licence.

## DirectX 8 SDK

`dx8/` holds `d3d8.h`, `d3d8caps.h` and `d3d8types.h` from the **DirectX 8 SDK**
(Takumi, 2000). They are **not** redistributed in this repository, and the
`dx8/` directory is ignored by git for that reason.

The Windows SDK dropped Direct3D 8 long ago - there is no supported install
that provides these any more - so the only source is the old SDK itself. It is
preserved on the Internet Archive; look for the `DirectXSDK_*` archive.

## Which files

The build needs all three:

| File | Why |
| --- | --- |
| `d3d8.h` | `IDirect3D8`, `IDirect3DDevice8`, `D3DPRESENT_PARAMETERS` |
| `d3d8types.h` | `D3DLOCKED_RECT`, enums, `D3DLOCK_*` |
| `d3d8caps.h` | included by `d3d8.h` |

Drop them into `deps/dx8/` and `build.cmd` will find them. It checks for them
before it does anything else, so a missing header is a one-line message rather
than a wall of compiler errors.

## Note on the revision

The copy this was developed against is a **trimmed, later revision** of the SDK
headers, not the 2000 originals. It is missing `D3DTSS_ALPHAPASS`, `RGNDWORD`
and `IDirect3DDevice8::SetSamplerState`, which is a large part of why this
project's Direct3D work is the shape it is - see `../analysis/FINDINGS.md`.

If you supply a fuller revision the shim should still build; nothing here
depends on the missing declarations being absent. If something fails to compile,
that is the first thing to look at.