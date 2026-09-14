# NV3D Vision Restoration Project

An open project, and a proof of concept: NVIDIA 3D Vision glasses and the original 3D Vision USB IR emitter working on a modern monitor and graphics card, without NVIDIA's discontinued 3D Vision driver.

Vision Restoration talks to the emitter directly and shows frame-sequential 3D itself. It uses standard Windows display APIs (Direct3D 11 / DXGI) instead of NVIDIA's stereo driver, so in theory it can work with any graphics card, not only NVIDIA ones. So far it has only been tested with an NVIDIA GPU.

> **Status: personal project, proof of concept.** There is no release.
>
> - **The only thing that works:** side-by-side 3D captured from a window into the app, on a **240 Hz OLED monitor**. It was proven with Dolphin set to side-by-side output.
> - **Does not work:** ReShade capture and the in-game hook.
> - **Never tested:** geo-11, and games with their own native stereo 3D.
> - **Not working yet:** LCD, QLED and Mini-LED displays show ghosting and crosstalk.
>
> Everything other than side-by-side window capture is future work. None of it has been proven.

## What works today

### 3D sources

| Source | Status |
|---|---|
| Window capture of side-by-side 3D output (proven with Dolphin set to side-by-side) | **Working.** The only proven source |
| ReShade capture (`VisionStereoSpout` add-on sending frames over Spout) | **Does not work** |
| In-game hook (`VisionGameHook`, `adapters/vision_hook.cpp`) | **Does not work, has never worked** |
| geo-11 | **Never tested** |
| Games with native stereo 3D (old 3D Vision titles, games with their own stereo mode) | **Never tested.** Unknown whether they work |
| DirectX 9 / 10, Vulkan | Not started |

Only side-by-side window capture works: the game has to produce its own left and right images side by side in a window. Vision Restoration does not add 3D to games. The goal is not to add support game by game; the plan is one general hook that works across games. That depends on who joins the project.

### Displays

The glasses close each lens while the other eye's image is shown. That only looks clean if the screen has fully switched from one eye's image to the other before the lens opens, so the display's pixel response time decides whether it works.

| Display | Status |
|---|---|
| OLED at 240 Hz, with software black frame insertion (SDR and HDR) | **Proven:** clean full-screen 3D through the glasses |
| OLED at 100, 120 or 144 Hz | Not proven |
| LCD, QLED, Mini-LED | **Not working yet.** Their slower pixel response leaves part of the other eye's image on screen, which shows up as ghosting and crosstalk. Being worked on. |

## Features

- **Original emitter, no NVIDIA stereo driver:** drives the 3D Vision USB emitter over WinUSB/libusb and uploads its firmware itself. The GPU driver is not replaced, patched or downgraded.
- **Frame-sequential output** with software black frame insertion (Left / Black / Right / Black), SDR and HDR.
- **Live timing controls:** phase, shutter width, phase sweep and eye swap while you look through the glasses.
- **Calibration aids:** built-in 3D test scene and per-eye test patterns.
- **Profiles:** timing saved per display and autosaved.
- **Diagnostics:** vblank jitter, present timing and emitter command timing.

## Requirements

- Windows 10 or 11 (64-bit)
- NVIDIA 3D Vision or 3D Vision 2 glasses
- NVIDIA 3D Vision USB IR emitter
- A 240 Hz OLED monitor (the only display type proven so far)
- A game that outputs side-by-side 3D in a window
- Visual Studio 2022 (Desktop development with C++), the Windows SDK and CMake 3.24+ to build it
- Your own copy of NVIDIA's 3D Vision USB driver package, for the emitter firmware. It is proprietary and not included.

## Installing

There is no release build, so installing means building from source. Third-party libraries and NVIDIA's firmware are not included in this repository; you download them yourself in the steps below.

### 1. Build the app

```powershell
git clone <URL from the green Code button above>
cd nv3d-vision-restoration-project
.\tools\Get-Dependencies.ps1   # downloads libusb, Dear ImGui, Spout2 and the ReShade headers from their official releases
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

1. Set the game to output **side-by-side** 3D in a window (in Dolphin: Graphics > Stereoscopic 3D Mode = Side-by-Side).
2. In **Sources & games**, pick its window with window capture.

Window capture is the only source proven to work. The ReShade Spout add-on and the in-game hook are in the app and the source tree, but neither works today.

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
- [ ] ReShade capture (built, does not work)
- [ ] geo-11 (never tested)
- [ ] Games with native stereo 3D (never tested)
- [ ] LCD, QLED and Mini-LED displays without ghosting and crosstalk
- [ ] Other refresh rates proven (100, 120, 144 Hz)
- [ ] A working general in-game hook, instead of per-game support
- [ ] Testing on AMD and Intel graphics cards
- [ ] DIY RP2040 emitter (firmware written, hardware untested)
- [ ] First release

## Documentation

- [Usage](docs/USAGE.md)
- [Emitter](docs/EMITTER.md)
- [Timing calibration](docs/TIMING-CALIBRATION.md)
- [Full screen at 120 Hz](docs/FULLSCREEN-120HZ.md)
- [DIY RP2040 emitter](docs/RP2040.md)
- [Development log](docs/STATUS.md)

## Contributing

This is a personal project built for one setup, published to show that the 3D Vision USB emitter can be unlocked for other monitors and newer graphics cards. Where it goes next depends on who jumps in. Help is most useful on LCD/QLED crosstalk, a general in-game hook, ReShade capture, native stereo games, and testing on other GPUs and displays.

Bug reports help most when they include your display model, refresh rate, GPU and `reports/session.log`.

## Credits

Built with [libnvstusb](https://github.com/eruffaldi/libnvstusb) (emitter protocol reference), [libusb](https://libusb.info/), [libwdi](https://github.com/pbatard/libwdi), [Dear ImGui](https://github.com/ocornut/imgui), [Spout2](https://github.com/leadedge/Spout2) and the [ReShade](https://github.com/crosire/reshade) add-on API. None of their code is redistributed here; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[GPL-3.0-or-later](LICENSE). NVIDIA firmware is not included and is not covered by this license.

Not affiliated with or endorsed by NVIDIA. "3D Vision" is a trademark of NVIDIA Corporation.
