# NV3D Vision Restoration Project

An open project, and a proof of concept: NVIDIA 3D Vision glasses and the original 3D Vision USB IR emitter working on a modern monitor and graphics card, without NVIDIA's discontinued 3D Vision driver.

Vision Restoration talks to the emitter directly and shows frame-sequential 3D itself. Its Geo11 output adapter uses standard Windows display APIs (Direct3D 11 / DXGI), without NVIDIA's discontinued stereo driver. Game tests have used an RTX 5070 Ti; output-only tests also pass on AMD Radeon graphics. Other GPU families still need game tests.

> **Status: personal project, experimental portable release.** Treat every display, game and timing profile as unverified until checked through each lens.
>
> - **Proven game source:** side-by-side 3D captured from a window into the app, demonstrated with Dolphin on the 240 Hz OLED setup. The user also confirms OLED operation at 120 Hz (2026-09-15).
> - **Shared Geo11 hook implemented, experimental:** x86/x64 output preserves the existing fix's SBS/TAB/Katanga mode, with game-depth sliders in the app. Prepare game arms automatic connection before launch. Isolated renderer/transport tests pass, but real-game compatibility is unresolved: Resident Evil 2 encountered GPU hangs and Metal Gear Solid V still has an unresolved user-reported startup failure. Neither game is listed as supported.
> - **VLC movie source implemented:** direct SDR playback of SBS/TAB files using installed 64-bit VLC 3.x; movie viewing through the glasses remains unverified.
> - **Still incomplete:** broad Helix/3Dmigoto coverage, native NVAPI stereo, DX9/10, native DX12 and other media-player integrations. The earlier depth-based ReShade hook remains nonworking; the separate direct-eye capture add-on is experimental.
> - **Not working yet:** LCD, QLED and Mini-LED displays show ghosting and crosstalk.
>
> Window capture and AI desktop remain available. The failed Spout and Blender viewport inputs have been removed.

## What works today

### 3D sources

| Source | Status |
|---|---|
| Geo11 DX11 output, x86 and x64 | Prepare/automatic connection and depth controls implemented; isolated tests pass, real-game crashes remain unresolved. See [status](docs/STATUS.md). |
| Window capture of side-by-side 3D output (proven with Dolphin set to side-by-side) | **Working fallback.** |
| VLC stereo movies (SBS or top/bottom) | Direct VLC 3.x source with audio, pause and seeking; SDR only, optical validation pending. |
| AI desktop | Available; converts captured 2D content using estimated depth |
| In-game hook (`VisionGameHook`, `adapters/vision_hook.cpp`) | **Does not work, has never worked** |
| Native NVAPI stereo / unconverted driver-dependent fixes | Separate rendering backend still required |
| DirectX 9 / 10, Vulkan | Not started |

The app can connect an existing Geo11 fix to its glasses output. The fix still renders both eyes and controls shaders, separation and convergence; our adapter transfers those eyes to the app, which presents a frame-sequential overlay over the game window. Window capture accepts existing side-by-side or top/bottom images, and AI desktop remains separate. See the [compatibility architecture](docs/STEREO-COMPATIBILITY.md); passing synthetic tests does not establish game compatibility.

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
- **Optional LCD/QLED experiments with the official emitter:** neutral gray reset, preload/hold sequences, nine-row optical targets and measured phase-range intersection. No LCD success is claimed; the previous subtraction-based "LightBoost" claim is withdrawn. See [panel experiments](docs/PANEL-EXPERIMENTS.md).
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
- Your own copy of NVIDIA's 3D Vision USB driver package, for the emitter firmware. It is proprietary and not included.

## Installing

### 1. Download the portable release

Download `Vision-Restoration-Portable-v0.1.0.zip` from [GitHub Releases](https://github.com/pancreations/nv3d-vision-restoration/releases), extract the entire archive to a writable folder, and run `Start Vision Restoration.cmd`. The ZIP includes the complete application, AI helper and redistributable AI runtime, emitter utilities, x86/x64 game integration, and required runtime files. It never includes NVIDIA's proprietary emitter firmware, AI model weights, Geo11, or a game-specific fix.

Whole-screen AI depth needs a model file placed in the included empty `models\` folder. Download [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small/blob/main/onnx/model_fp16.onnx) for the fastest option. [Base](https://huggingface.co/onnx-community/depth-anything-v2-base/blob/main/onnx/model_fp16.onnx) and [Large](https://huggingface.co/onnx-community/depth-anything-v2-large/blob/main/onnx/model_fp16.onnx) are slower and licensed for non-commercial use. Save each model with a distinct filename. Normal stereo images, window capture, game integration and emitter operation do not need a model.

The release ZIP includes **Vision Restoration README.pdf** generated from this README, **README.md**, and **Vision Restoration Quick Start.pdf**. Both PDFs are also available beside the ZIP on the release page. This is an experimental prerelease, not a stable game-compatibility release.

### Downloads not included in the public ZIP

| Item | Official source | Why it is needed |
|---|---|---|
| NVIDIA emitter firmware source | [Direct download: NVIDIA standalone 3D Vision USB controller package 390.41](https://us.download.nvidia.com/Windows/Quadro_Certified/NV3DVisionUSB/390.41/3dvisioncontrollerdriver.exe) | Run `Prepare NVIDIA Firmware.cmd` and select the downloaded EXE. The helper opens it as an archive; it does not run NVIDIA's installer. |
| WinUSB fallback | [Zadig](https://zadig.akeo.ie/) | Only needed if the included automatic WinUSB setup fails. |
| Generic Geo11 binaries | [Maintained Geo11 releases](https://github.com/ThreeDeeJay/geo-11/releases) | Supplies stereo rendering for compatible DX11 fixes. |
| Game-specific fixes | [Helix Mod](https://helixmod.blogspot.com/) or [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) | Supplies per-game shader and compatibility fixes. |
| Fast AI depth model | [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small/blob/main/onnx/model_fp16.onnx) | Optional whole-screen AI depth. |
| Larger AI depth models | [Base](https://huggingface.co/onnx-community/depth-anything-v2-base/blob/main/onnx/model_fp16.onnx) · [Large](https://huggingface.co/onnx-community/depth-anything-v2-large/blob/main/onnx/model_fp16.onnx) | Optional slower models; non-commercial license. |

### Build from source

Building requires Visual Studio 2022 with Desktop development for C++, the Windows SDK and CMake 3.24+. Third-party build inputs are downloaded from their official sources:

```powershell
git clone https://github.com/pancreations/nv3d-vision-restoration.git
cd nv3d-vision-restoration
.\tools\Get-Dependencies.ps1   # downloads build dependencies and the AI depth model
.\build.ps1
```

The app is built to `build\bin\Release\VisionRestoration.exe`.

### 2. Get the emitter firmware

The emitter has no permanent firmware: the PC uploads it every time the emitter is plugged in. That firmware lives inside NVIDIA's own driver file, `nvstusb.sys`.

1. Download NVIDIA's official [standalone 3D Vision USB controller package 390.41 directly](https://us.download.nvidia.com/Windows/Quadro_Certified/NV3DVisionUSB/390.41/3dvisioncontrollerdriver.exe). This bypasses NVIDIA's legacy download-page button. The server path says Quadro, but this USB-only package supplies the same emitter firmware; Vision Restoration does not require a Quadro GPU or legacy graphics driver.
2. **Do not run NVIDIA's installer.** In the portable release, run `Prepare NVIDIA Firmware.cmd` and select the downloaded `3dvisioncontrollerdriver.exe`, or drag that EXE onto the helper. It includes a portable extractor, so no separate 7-Zip installation is needed. It writes `emitter.fw` beside the app and preserves any existing firmware.
3. An already extracted `nvstusb*.sys` also works. Source builders can extract the SYS from `NV3DVisionUSB.Driver` using [7-Zip](https://www.7-zip.org/) and run the low-level extractor directly:

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

1. Select a supported high-refresh mode and run `Start Vision Restoration.cmd` from the portable release (`Launch.cmd` in a source checkout).
2. Pick your display and refresh rate, click **Match output rate**, then **Start 3D preview**. The glasses start shuttering.
3. Turn on **Software black frame insertion** and use **Maximize brightness**.
4. Adjust **Phase** and **Shutter** until each eye sees only its own image.

### 5. Show a game in 3D

1. Install the game's established Geo11-compatible fix, following its instructions or [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html). Start with the maintained [Geo11 releases](https://github.com/ThreeDeeJay/geo-11/releases) and [Helix Mod community fixes](https://helixmod.blogspot.com/). A legacy NVIDIA-driver-only fix needs a compatible stereo renderer first.
2. In the left **Input/Games** tab, use **Choose game...** and select the rendering executable. With the game closed and display/emitter timing ready, click **Prepare game**. This connects the adapter if needed and arms automatic attachment. Geo11 uses its existing output settings automatically. Other providers require the layout they actually produce; frame sequential is preferred and SBS/TAB are fallbacks.
3. Launch the game normally in windowed or borderless mode on the selected output display. The app watches the exact EXE even while minimized or on **Tuning**, attaches when the provider appears, and starts output after a complete pair and foreground game window are available. **Cancel preparation / stop game 3D** stops watching. Preparation cannot repair an incompatible or crashing renderer.

Before updating or removing a community fix, close the game and use **Disconnect adapter**. See [game setup and troubleshooting](docs/USAGE.md#games-existing-geo11-fixes).

### 6. Play a stereo movie with VLC

Install [64-bit VLC 3.x](https://www.videolan.org/vlc/), then select **Input/Games > Source > VLC stereo movie**, choose the movie's **Packing** and **Choose movie...**, and click **Start source**. Start the calibrated **3D preview** or fullscreen output. Audio, pause/resume, volume and seeking are available under Input/Games. For portable VLC, use **Choose VLC...** and select its `vlc.exe` with the complete installation folder present. See [VLC playback details](docs/USAGE.md#vlc-stereo-movies).

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
- [x] Portable public release packaging with the redistributable AI helper and runtime

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

Built with [libnvstusb](https://github.com/eruffaldi/libnvstusb) (emitter protocol reference), [libusb](https://libusb.info/), [libwdi](https://github.com/pbatard/libwdi), [Dear ImGui](https://github.com/ocornut/imgui) and the [ReShade](https://github.com/crosire/reshade) add-on API. Third-party source is not committed to this repository; portable archives include the permitted runtime components and their notices. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[GPL-3.0-or-later](LICENSE). NVIDIA firmware is not included and is not covered by this license.

Not affiliated with or endorsed by NVIDIA. "3D Vision" is a trademark of NVIDIA Corporation.
