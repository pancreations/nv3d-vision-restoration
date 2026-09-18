# Geo11 game output

**Shared hook, experimental:** connection v3 preserves the installed Geo11
output mode and consumes SBS, TAB, their reversed variants, or Katanga. Both
architectures use the same code. Earlier game trials remain rolled back; broad
game/optical compatibility is not established. Real-renderer tests pass image
and live-depth checks but still reproduce a Geo11 shutdown crash also seen in
the unhooked baseline. See [current status](../docs/STATUS.md).

The game and its existing Geo11 fix render stereo. Vision Restoration replaces
the display endpoint and drives the original USB emitter. The game keeps its
window, mouse, keyboard and controller input. No external viewer is involved.

## Setup

Build with `build.ps1`. It packages `runtime/x86` and `runtime/x64` beside the app.
Install a Geo11-compatible fix from the maintained
[Geo11 releases](https://github.com/ThreeDeeJay/geo-11/releases),
[Helix Mod](https://helixmod.blogspot.com/) or
[3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) using its established instructions, close the
game, then use **Games > Enable shared Geo11 hook...**. Select the
actual rendering executable rather than a launcher. Leave the app open and
launch the game normally. The fix controls depth, convergence and shader effects.

The connector requires Geo11 with `force_stereo=2`; it does not convert arbitrary
legacy Helix/3Dmigoto fixes. It checks executable/DLL architecture and refuses
conflicting proxy chains or unrecognized existing installation files.

For scripts, the same connector is available as:

```powershell
.\build\bin\Release\vision_stereo_setup.exe inspect 'C:\Games\Example\game.exe'
.\build\bin\Release\vision_stereo_setup.exe connect 'C:\Games\Example\game.exe' '.\build\bin\Release\runtime'
.\build\bin\Release\vision_stereo_setup.exe disconnect 'C:\Games\Example\game.exe'
```

Disconnect with the game closed **before updating its community fix**. Connection
keeps original files and output settings in `VisionRestoration.OutputBackup`.
Disconnect restores the previous renderer/output settings and retains later
unrelated tuning. If someone changes the installed renderer or adapter, the
connector refuses to overwrite those changes.

## Rendering and loading

1. The connector retains the community `d3d11.dll` byte-for-byte as
   `VisionGeo11.dll`. It installs our early loader as `d3d11.dll` and our output
   adapter as `VisionStereo11.dll`. Shader files and other community DLLs remain.
2. Only `d3dx.ini`'s `[System] proxy_d3d11=VisionStereo11.dll` is changed.
   `d3dxdm.ini`, including the output mode and compatibility settings, is preserved.
3. Before Geo11 initializes, the loader installs the output hooks on the native
   DXGI factory. Geo11 then wraps our logical swapchain. Loader callbacks from
   Geo11 reach the native backend without re-entering the renderer.
4. The adapter reads the packed logical backbuffer for SBS/TAB, or the full-resolution
   Katanga texture when the fix already uses that mode. The Katanga mapping is
   isolated per process. At logical Present, the adapter copies a
   pair using the game's GPU context, in order after Geo11 rendering. It publishes
   that pair only after the GPU completion fence (or event query) confirms the
   copy finished. A successful CPU keyed-mutex acquisition alone is insufficient:
   consuming an unfinished copy can still stall the output device's GPU queue.
5. Three transport slots use keyed mutexes, or shared GPU fences when keyed
   textures are unavailable. The separate output device snapshots one completed
   pair at the beginning of a display cycle and retains it through both eyes and
   any black frames. A stalled producer repeats that pair while output continues.
6. Only the output worker physically presents. Logical Present is paced at one
   source frame per stereo cycle and waits for game GPU completion to prevent an
   unbounded producer from starving display output. Nonblocking Present accepts
   one pending submission and checks completion on its next call. At 240 Hz with
   Left/Black/Right/Black, this is up to 60 pairs/s.
   Four physical buffers hold up to three queued refreshes. The display thread
   uses MMCSS and relative GPU context priority +7; it never blocks on the game's
   producer mutex. Normal-range priorities require no vendor-specific driver API.
7. The presenter selects an eye before submission using the latest display
   refresh counter and queued presents. `HookRefreshSlots` declares a fixed
   `slot = refresh % cycle` rule in the existing v1 flags. The host uses that same
   rule and never learns a new eye phase from late frames during recovery.
8. DXGI presentation statistics travel over the existing host channel. The app
   owns emitter commands and its calibrated timing profile. Host disable/loss or
   game focus loss gives mono output; loaded hooks remain until game exit.

The internal packed texture does not change desktop mouse coordinates or turn
the game window into an SBS viewer. HDR metadata handling and the full set of
DXGI swapchain methods still need broader coverage.

## Validation

Results from 2026-09-15 on Windows, OLED at 240 Hz with Left/Black/Right/Black:

| Case | Result and scope |
|---|---|
| Batman: Arkham City GOTY, DX11 x86, official Geo11 0.7.11 | User confirmed stable 3D and normal controls on RTX 5070 Ti. Generic Geo11 setup, not a Batman-specific shader fix. Bloom/alpha defects remain. |
| Psychonauts 2, DX11 x64, Universal UE4 Fix 9.13 / Geo11 0.7.11 | User resolved double vision using the game mod. Recurring flashes persisted after scheduling and fixed-refresh-slot updates. GPU completion backpressure and GPU-ready pair publication are now installed; gameplay confirmation is pending. |
| Geo11 0.6.164 startup probes, x86/x64 | Both combined device/swapchain creation and factory-before-device creation produced distinct full-resolution eyes and exited successfully. |
| Output harness, NVIDIA and AMD Radeon graphics | Actual backbuffer pixels verify left/right/black slots, idle producer and a forced 500 ms stall holding the producer mutex, 80 paced source frames, resize/new texture, host disable and shutdown. Latest update also tests 17/53/131 ms display stalls and return to the same eye phase; passed x64 keyed/fence paths on NVIDIA and AMD, plus x86 NVIDIA fences. Some display-stall tests still observed 3-6 wrong slots during recovery, so this is not glitch-free physical output. |
| Application / connector | All 11 CTest suites passed; source/UI smoke test passed. Connector tests cover preservation, rollback, architecture, conflicts and disconnect. |

The output harness uses a private test mapping and never commands the emitter.
The GPU completion regression holds the game's GPU behind an unsignaled fence
for 200 ms, released by an independent device. Present must wait while physical
output continues from the old pair. Seven paths pass: x64 NVIDIA/AMD keyed and
shared-fence transport, x86 NVIDIA shared fences, and completion-query fallback
on x64/x86 NVIDIA. Forced physical-output stalls still produce up to three wrong
scanout slots before recovery in these runs. The harness consistently uses the
primary display; secondary-display timing and optical output need separate tests.
AMD results establish output operation, not AMD game or optical compatibility.
RTX 30/40-series and Intel game tests remain outstanding. There is no GPU vendor
or driver-version pin in this adapter; normal D3D11/DXGI feature support is needed.

Psychonauts used the fix linked by local 3D Fix Manager profile 1306. ReShade was
removed and backed up. The installed `dxgi.dll` is Geo11's early loader, not
ReShade. Its documented DX11 configuration and `allow_platform_update=2` were
applied. Batman's profile 534 did not prescribe this Geo11 route: that test used
the official generic renderer. Neither game's executable was patched.

## Boundaries

- This is a reusable DX11 Geo11 backend, not whole-catalogue compatibility.
- Original-driver-dependent NVAPI stereo, DX9/10, native DX12, Vulkan/OpenGL,
  other stereo mods and other media-player integrations require other backends.
- Window capture, AI desktop and VLC movie playback are separate sources. The
  direct VLC 3.x SBS/TAB source uses the app presenter, not the Geo11 game adapter;
  see [VLC setup](../docs/USAGE.md#vlc-stereo-movies).
- Fullscreen transitions, multiple simultaneous games/swapchains, unusual DXGI
  methods, D3D11On12 and HDR metadata need further compatibility tests.
- Geo11 0.7.11 shutdown behavior was not consistently clean in standalone probes;
  no general fix for upstream teardown is claimed.
- Shader stereo corrections, HUD/cursor depth and comfortable convergence remain
  responsibilities of the established fix. Distinct pixels and timing counters
  alone do not establish optical quality.

## Developer tools

`tools/Build-StereoRuntime.ps1` builds and packages both production architectures.
Configure `tools/stereo-runtime` directly to also build diagnostic targets:

- `vision_geo11_probe`: ordinary DX11 rendering to exercise the existing provider.
- `vision_output_tests` / `VisionStereo11Test`: private output harness. Run in a
  fresh directory; it creates a temporary provider configuration. Test-only
  environment variables `VISION_STEREO_TEST_FENCES=1` and
  `VISION_STEREO_TEST_VENDOR=AMD` select fallback transport and adapter.
  `VISION_STEREO_TEST_QUERY=1` selects the GPU completion event-query fallback.
- `vision_sync_inspect`: read-only host/game timing and foreground diagnostics.
  `--monitor 30` samples actual reported slots against fixed refresh slots. This
  does not observe the glasses or guarantee every refresh was sampled.
- `vision_pair_inspect`: diagnostic eye-texture snapshot, not synchronized proof
  of rendered output or glasses timing.

The game directory's `VisionStereo11.log` records startup, transport choice and
periodic pacing statistics. See [compatibility architecture](../docs/STEREO-COMPATIBILITY.md)
for future backends.

## Existing Geo11 direct capture

`VisionStereo11` also connects an existing geo-11 fix to the app's Direct 3D eyes input. `VisionStereoCapture.ini` with `[VISION_CAPTURE] Mode=geo11` selects this route, and the Games connector creates it automatically for detected geo-11 fixes. It uses the existing provider/proxy chain, preserves `d3dxdm.ini`, and does not require ReShade. Full-resolution Katanga eyes are copied into a two-slice shared texture after GPU completion. SBS/TAB modes retain the fix's existing resolution and eye order. The native preview stays mono while the app owns sequential display and emitter timing. See [direct capture status](../docs/DIRECT-EYES.md) and `tools/Test-Geo11Capture.ps1`.
