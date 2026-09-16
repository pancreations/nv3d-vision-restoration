# NV3D Vision Restoration Project

An open project, and a proof of concept: NVIDIA 3D Vision glasses and the original 3D Vision USB IR emitter working on a modern monitor and graphics card, without NVIDIA's discontinued 3D Vision driver.

Vision Restoration talks to the emitter directly and shows frame-sequential 3D itself. Its Geo11 output adapter uses standard Windows display APIs (Direct3D 11 / DXGI), without NVIDIA's discontinued stereo driver. Game tests have used an RTX 5070 Ti; output-only tests also pass on AMD Radeon graphics. Other GPU families still need game tests.

> **Status: personal project, proof of concept.** There is no release.
>
> - **Proven game source:** side-by-side 3D captured from a window into the app, demonstrated with Dolphin on the 240 Hz OLED setup. The user also confirms OLED operation at 120 Hz (2026-09-15).
> - **Game integration unresolved:** a shared in-game Geo11 hook is allowed; game-specific adjustments are not the intended solution. Previous trials were rolled back after setup deviations and unresolved timing. A reliable shared integration preserving established fixes remains incomplete.
> - **Still incomplete:** broad Helix/3Dmigoto coverage, native NVAPI stereo, DX9/10, native DX12 and media-player integration. The earlier ReShade hook remains nonworking.
> - **Not working yet:** LCD, QLED and Mini-LED displays show ghosting and crosstalk.
>
> Window capture and AI desktop remain available. The failed Spout and Blender viewport inputs have been removed.

## What works today

### 3D sources

| Source | Status |
|---|---|
| Geo11 DX11 output, x86 and x64 | Shared-hook prototype withdrawn; compatibility with established fixes and stable gameplay timing remain unresolved. See [status](docs/STATUS.md). |
| Window capture of side-by-side 3D output (proven with Dolphin set to side-by-side) | **Working fallback.** |
| AI desktop | Available; converts captured 2D content using estimated depth |
| In-game hook (`VisionGameHook`, `adapters/vision_hook.cpp`) | **Does not work, has never worked** |
| Native NVAPI stereo / unconverted driver-dependent fixes | Separate rendering backend still required |
| DirectX 9 / 10, Vulkan | Not started |

The app can connect an existing Geo11 fix to its glasses output. The fix still renders both eyes and controls shaders, separation and convergence; our adapter presents those eyes in the game's original window. Window capture accepts existing side-by-side or top/bottom images, and AI desktop remains separate. See the [compatibility architecture](docs/STEREO-COMPATIBILITY.md); these two game tests do not establish support for the entire Helix catalogue.

### Displays

The glasses must admit the intended eye's light while excluding the other eye's light. Panel scanout, pixel transitions, backlight behavior and actual lens timing all affect separation; successful presentation and USB commands alone do not establish it.

| Display | Status |
|---|---|
| OLED at 240 Hz, with software black frame insertion (SDR and HDR) | **Proven:** clean full-screen 3D through the glasses |
| OLED at 120 Hz | User-confirmed working, 2026-09-15. Record the sequence and profile with each compatibility test. |
| OLED at 100 or 144 Hz | Not proven |
| LCD, QLED, Mini-LED | **Not working yet.** Ghosting and crosstalk remain. Panel response, scanout and synchronization need separate optical checks. |

## Features

- **Original emitter, no NVIDIA stereo driver:** drives the 3D Vision USB emitter over WinUSB/libusb and uploads its firmware itself. The GPU driver is not replaced, patched or downgraded.
- **Frame-sequential output** with software black frame insertion (Left / Black / Right / Black), SDR and HDR.
- **Connect existing Geo11 fixes:** reversible x86/x64 installation, ordinary game launches, original window/input, and app-owned emitter timing. Completed eye pairs stay intact across the entire eye/black cycle.
- **Optional LCD/QLED experiments with the official emitter:** neutral gray reset, preload/hold sequences, nine-row optical targets and measured phase-range intersection. No LCD success is claimed; the previous subtraction-based "LightBoost" claim is withdrawn. See [panel experiments](docs/PANEL-EXPERIMENTS.md), or use `Launch-Panel-Experiments.cmd` for the separate experimental build.
- **Whole screen in 3D with AI depth (new, unverified through the glasses):** the desktop, a video or a 2D game is captured, a depth network estimates every pixel's depth and both eyes are resampled from it, the way SpaceWalker and Reality Hub convert 2D. The result is a click-through overlay, so the desktop stays usable. See [usage](docs/USAGE.md#whole-screen-in-3d-ai-depth).
- **Live timing controls:** phase, shutter width, phase sweep and eye swap while you look through the glasses.
- **Calibration aids:** built-in 3D test scene and per-eye test patterns.
- **Profiles:** timing saved per display and autosaved.
- **Diagnostics:** vblank jitter, present timing and emitter command timing.

## Requirements

- Windows 10 or 11 (64-bit)
- NVIDIA 3D Vision or 3D Vision 2 glasses
- NVIDIA 3D Vision USB IR emitter
- An OLED monitor with a working 120 Hz or 240 Hz setup (the only display type proven so far)
- A DX11 game with a working Geo11 fix, or an application that outputs side-by-side/top-bottom 3D for window capture
- Visual Studio 2022 (Desktop development with C++), the Windows SDK and CMake 3.24+ to build it
- Your own copy of NVIDIA's 3D Vision USB driver package, for the emitter firmware. It is proprietary and not included.

## Installing

There is no release build, so installing means building from source. Third-party libraries and NVIDIA's firmware are not included in this repository; you download them yourself in the steps below.

### 1. Build the app

```powershell
git clone <URL from the green Code button above>
cd nv3d-vision-restoration-project
.\tools\Get-Dependencies.ps1   # downloads build dependencies and the AI depth model
.\build.ps1
```

The app is built to `build\bin\Release\VisionRestoration.exe`.

### 2. Get the emitter firmware

The emitter has no permanent firmware: the PC uploads it every time the emitter is plugged in. That firmware lives inside NVIDIA's own driver file, `nvstusb.sys`.

1. Download NVIDIA's [3D Vision USB driver 390.41](https://www.nvidia.com/en-us/drivers/nv3dvisionusb/390_41/nv3dvisionusb-driver/) (a GeForce driver package up to 425.31 also works).
2. **Do not run the installer.** Open the package with [7-Zip](https://www.7-zip.org/) and copy `nvstusb.sys` out of the `NV3DVisionUSB.Driver` folder.
3. Extract the firmware next to the app:

   ```powershell
   .\build\bin\Release\vision_firmware.exe C:\path\to\nvstusb.sys .\build\bin\Release\emitter.fw
   ```

### 3. Give the emitter the WinUSB driver

The emitter appears to Windows as two different USB devices, one before its firmware is loaded and one after. The first number (`0955`, NVIDIA) stays the same; the second number changes:

| Emitter state | USB ID | Driver |
|---|---|---|
| Just plugged in, no firmware yet | `0955` **`7003`** | Install WinUSB once |
| Firmware running (after the app uploads it) | `0955` **`0007`** | Install WinUSB once |

Each needs WinUSB one time per PC. After that, plugging the emitter in again only needs the app to re-upload the firmware, which it does by itself.

1. Plug the emitter straight into a USB port on the PC (no hub). With no driver, Device Manager lists it as **NVIDIA stereo controller** with a warning.
2. Run [Zadig](https://zadig.akeo.ie/), choose **Options > List All Devices**, select the NVIDIA device with USB ID `0955 7003`, pick **WinUSB** as the target driver and click **Install Driver**.
3. Start the app (step 4). It uploads the firmware and the emitter restarts with the **other** ID, `0955 0007`. If the app then reports that it cannot open the emitter, run Zadig again, select `0955 0007`, install **WinUSB**, and press **Refresh USB** in the app.

Only these two emitter IDs get WinUSB. Don't install NVIDIA's 3D Vision driver or change your GPU driver.

### 4. Run and calibrate

1. Set the monitor to 240 Hz and run `Launch.cmd`.
2. Pick your display and refresh rate, click **Match output rate**, then **Start 3D preview**. The glasses start shuttering.
3. Turn on **Software black frame insertion** and use **Maximize brightness**.
4. Adjust **Phase** and **Shutter** until each eye sees only its own image.

### 5. Show a game in 3D

1. Install the game's established Geo11-compatible fix, following its instructions or 3D Fix Manager's profile. A legacy NVIDIA-driver-only fix needs a compatible stereo renderer first.
2. Close the game. In **Sources & games**, choose **Connect existing stereo fix...** and select the actual game executable.
3. Leave Vision Restoration open with your calibrated display profile, then launch the game normally. The game retains focus and its controls. Use the mod's own settings for depth, convergence and shader corrections.

To update or remove the community fix, close the game and choose **Disconnect stereo output...** first. See [game setup and troubleshooting](docs/USAGE.md#games-existing-geo11-fixes).

For the capture fallback, set the source to side-by-side (for example, Dolphin's stereoscopic graphics setting) and select its window under **Sources & games**. AI desktop remains available separately.

Every control is explained in [docs/USAGE.md](docs/USAGE.md). Emitter details and troubleshooting are in [docs/EMITTER.md](docs/EMITTER.md).

Self tests that need no emitter:

```powershell
.\build\bin\Release\VisionRestoration.exe --gpu-test
.\build\bin\Release\VisionRestoration.exe --smoke-test
```

## Roadmap

- [x] Original emitter control without the NVIDIA stereo driver
- [x] Clean full-screen 3D through the glasses on a 240 Hz OLED (black frame insertion, SDR and HDR)
- [x] Side-by-side capture from a window (proven with Dolphin)
- [ ] Whole screen converted with AI depth (built 2026-09-15, checked on the desktop, not yet through the glasses)
- [x] App-hosted Geo11 DX11 output in two games, keeping original window/input
- [ ] Broad compatibility with existing 3D Vision games and community fixes
- [ ] Games with native stereo 3D (never tested)
- [ ] LCD, QLED and Mini-LED displays without ghosting and crosstalk
- [x] OLED operation at 120 Hz (user confirmation, 2026-09-15)
- [ ] Other refresh rates proven (100, 144 Hz)
- [ ] A working general in-game hook, instead of per-game support
- [x] AMD output-only tests (keyed mutex and shared GPU fence transport)
- [ ] Game/optical tests on AMD, Intel and additional NVIDIA generations
- [ ] DIY RP2040 emitter (firmware written, hardware untested)
- [ ] First release

## Documentation

- [Usage](docs/USAGE.md)
- [Single-screen games and media: compatibility design](docs/STEREO-COMPATIBILITY.md)
- [Emitter](docs/EMITTER.md)
- [Timing calibration](docs/TIMING-CALIBRATION.md)
- [Full screen at 120 Hz](docs/FULLSCREEN-120HZ.md)
- [DIY RP2040 emitter](docs/RP2040.md)
- [Development log](docs/STATUS.md)

## Contributing

This is a personal project built for one setup, published to show that the 3D Vision USB emitter can be unlocked for other monitors and newer graphics cards. Help is most useful on established-fix compatibility, native stereo backends, LCD/QLED crosstalk, and testing on other GPUs and displays.

Bug reports help most when they include your display model, refresh rate, GPU and `reports/session.log`.

## Credits

Built with [libnvstusb](https://github.com/eruffaldi/libnvstusb) (emitter protocol reference), [libusb](https://libusb.info/), [libwdi](https://github.com/pbatard/libwdi), [Dear ImGui](https://github.com/ocornut/imgui) and the [ReShade](https://github.com/crosire/reshade) add-on API. None of their code is redistributed here; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[GPL-3.0-or-later](LICENSE). NVIDIA firmware is not included and is not covered by this license.

Not affiliated with or endorsed by NVIDIA. "3D Vision" is a trademark of NVIDIA Corporation.
