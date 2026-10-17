# steamvr-arc-dx12-bridge

Makes DirectX 11 OpenVR games run in SteamVR on **Intel Arc** GPUs by handing their frames
to SteamVR as DirectX 12 textures.

[Русская версия](README.ru.md) ·
[GitVerse](https://gitverse.ru/kapitoxapux/steamvr-arc-dx12-bridge) (main) ·
[GitHub](https://github.com/kapitoxapux/steamvr-arc-dx12-bridge) (mirror)

> **Experimental.** Verified on Intel Arc B580 (driver 32.0.101.9150), Windows 10 22H2,
> SteamVR 2.17.10, with Drums Rock streamed to a PICO 4 via ALVR. Reports from other games
> and GPUs are very welcome.

## The problem

On Intel Arc, the SteamVR home (the empty room) works, but a DirectX 11 VR game freezes right
after it starts rendering: it hangs on its splash screen, and about 5 seconds later the SteamVR
compositor kills itself. What you find in `C:\Program Files (x86)\Steam\logs`:

- `vrclient_<game>.txt`: `Failed to open frame info buffer (D3D11)!`
- `vrcompositor.txt`:
  `Failed Watchdog timeout in thread Render in CGraphicsDevice::DoSwapchainPresent after 5.0 seconds. Aborting.`
- Windows Event Viewer → Application, event 1000: `vrcompositor.exe`, module `ucrtbase.dll`,
  exception `0xc0000409`.

The headset and the streaming software are not the cause: the same thing happens with SteamVR's
built-in virtual (null) headset and no real headset at all. DirectX 12 games are not affected.

## Try DirectX 12 first

If a game can render with DirectX 12, launch it that way and you need nothing from here.
For Unreal Engine games add `-dx12` to the Steam launch options (Properties → General →
Launch options). Beyond Sandbox, for example, works this way.

Not every game can: Drums Rock (Unity) starts in DirectX 12, but its OpenVR plugin then reports
`Unsupported graphics api` and VR does not start. That is the case this bridge is for.

## How it works

The bridge replaces the game's `openvr_api.dll`, a technique
[openvr_fsr](https://github.com/fholger/openvr_fsr) also uses. All calls are forwarded to the
original DLL, renamed `openvr_api.orig.dll`, except `IVRCompositor::Submit`:

1. On the first D3D11 frame the bridge creates a D3D12 device on the same GPU and a small ring
   of textures shared between D3D11 and D3D12, with shared fences.
2. Each eye texture is copied on the GPU into a free shared texture (one copy per eye per frame).
3. The frame is submitted to SteamVR as `TextureType_DirectX12`, the path that works on Arc.

Synchronization stays on the GPU through fences; the CPU only waits if all slots are busy.

## Status

| Game | Engine / VR plugin | Result |
|---|---|---|
| Drums Rock | Unity 2022.3, OpenVR XR plugin, `IVRCompositor_027` | works: menus and gameplay, 19 000 frames, 0 errors |

Limitations of version 0.1:

- Only games that request `IVRCompositor_027` are converted. With other versions, the log reads
  `WARNING: IVRCompositor_0xx requested ... not hooked`, and the game behaves as without the bridge.
- The C function table interface (`FnTable:IVRCompositor_027`) is not converted yet.
- 64-bit games only.
- If the game changes its render resolution on the fly, conversion stops until the game restarts.
- Depth textures (`Submit_TextureWithDepth`) are not passed on.
- OpenXR games (for example Beat Saber) are not covered: they do not use `openvr_api.dll`.
- Some games refuse a replaced `openvr_api.dll`. Do not use it in games with anti-cheat.
- "Verify integrity of game files" in Steam or a game update brings the original DLL back.
  Run the installer again.

## Install

1. Download this repository (Code → Download ZIP) and unpack it.
2. Close the game. In PowerShell, from the unpacked folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1 "C:\Program Files (x86)\Steam\steamapps\common\<Game>"
   ```

   The script finds `openvr_api.dll` inside the game folder, renames it to
   `openvr_api.orig.dll`, and puts `bin\openvr_api.dll` and `openvr_proxy.ini` next to it.
   If you get "access denied", run PowerShell as administrator.

To install by hand, do the same thing yourself: rename the game's `openvr_api.dll` to
`openvr_api.orig.dll`, then copy `bin\openvr_api.dll` and `openvr_proxy.ini` into that folder.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1 "C:\Program Files (x86)\Steam\steamapps\common\<Game>"
```

You can also use Steam → game Properties → Installed Files → Verify integrity of game files.

## Settings and log

`openvr_proxy.ini` next to the DLL:

```ini
[proxy]
convert=1   ; 0 = pass everything through untouched, to compare or disable without uninstalling
mirror=0    ; 1 = allow GetMirrorTextureD3D11 (desktop mirror in some games), untested
```

`openvr_proxy.log` is written next to the DLL. A healthy run looks like this:

```
==== steamvr-arc-dx12-bridge 0.1.0 loaded: convert=1 mirror=0
IVRCompositor hooked: ...
bridge: adapter 'Intel(R) Arc(TM) B580 Graphics'
bridge: sharing via d3d12->d3d11, slot format 29 (source 27)
stats: submits=1000 errors=0 ...
```

When reporting a problem, please attach this log together with `vrcompositor.txt` and
`vrclient_<game>.txt` from `Steam\logs`.

## Build

The DLL is cross-compiled with [zig](https://ziglang.org/download/) 0.13 (Linux or Windows):

```sh
ZIG=/path/to/zig ./build.sh
```

The code uses naked functions and GNU inline assembly for the raw export forwarders, so it needs
a clang-compatible compiler. MSVC will not build it as is.

## Technical notes

These are for driver and runtime developers.

- Reproduction without any headset: in `Steam\config\steamvr.vrsettings` set
  `"steamvr": {"forcedDriver": "null"}` and `"driver_null": {"enable": true}`, start SteamVR,
  then start any D3D11 VR app. D3D11 OpenVR (Drums Rock) and D3D11 OpenXR (Beat Saber) both kill
  the compositor within seconds. A D3D12 app (Beyond Sandbox `-dx12`) runs for minutes.
- D3D11 rendering itself is fine. Beat Saber in D3D11 ran stably for minutes on a minimal
  in-process OpenXR runtime that does not share textures with another process. The failure is in
  how the D3D11 frame is shared with the vrcompositor process.
- On the B580, a D3D12 committed resource with `D3D12_HEAP_FLAG_SHARED` in
  `DXGI_FORMAT_R8G8B8A8_TYPELESS` cannot be opened in D3D11: `ID3D11Device1::OpenSharedResource1`
  returns `E_INVALIDARG`. `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` works. The bridge tries both
  directions (D3D12→D3D11, D3D11→D3D12) and both formats, and logs what it picked.
- With ALVR, `vrcompositor.txt` also keeps logging
  `WaitForAcquire timed out (FAILED); rendering the next frame before the driver took the sync texture`.
  This is the D3D11 sync texture between the driver and the compositor, the same kind of
  cross-process D3D11 sharing.

Upstream reports: [Intel IGCIT #1590](https://github.com/IGCIT/Intel-GPU-Community-Issue-Tracker-IGCIT/issues/1590),
[SteamVR Bug Reports forum](https://steamcommunity.com/app/250820/discussions/3/569300439979348361/).

## License

MIT, see [LICENSE](LICENSE). This project does not include OpenVR SDK code: the few type
definitions it needs are declared in `src/proxy.cpp`.
