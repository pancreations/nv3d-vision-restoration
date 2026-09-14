# Vision Restoration

Bring NVIDIA 3D Vision glasses back to life on modern Windows PCs. No discontinued NVIDIA 3D Vision driver, no GPU driver downgrade.

Vision Restoration drives the original 3D Vision USB IR emitter directly and shows frame-sequential stereo on today's high-refresh displays. The goal is to play stereo 3D games again, the way 3D Vision used to.

> **Status: early development.** There is no release yet. Clean full-screen stereo has been confirmed through the glasses on a 4K 240 Hz OLED. Game support is in progress.

## Features

- **Original emitter support:** talks to the NVIDIA 3D Vision USB emitter over WinUSB/libusb and uploads its firmware itself. The NVIDIA stereo driver is never installed.
- **Works with current GPU drivers:** nothing is replaced, patched or downgraded.
- **High-refresh output:** 100, 120, 144 and 240 Hz, with Left/Black/Right/Black (black frame insertion) for clean separation on modern panels.
- **SDR and HDR**
- **Live timing controls:** phase, shutter width, phase sweep and eye swap.
- **Stereo sources:** built-in test scene, stereo image pairs, window capture and Spout.
- **In-game hook:** syncs the glasses to a game's own frames. Direct3D 11 games that render both eyes (tested with Dolphin), and Direct3D 12 games that render one camera through depth-based 3D from the game's own depth buffer (built for GTA V Enhanced, not yet tested in the game).
- **Profiles:** timing is saved per display and autosaved.
- **Diagnostics:** live vblank jitter, present timing and emitter command timing.

## Requirements

- Windows 10 or 11 (64-bit)
- NVIDIA 3D Vision or 3D Vision 2 glasses
- NVIDIA 3D Vision USB IR emitter (a DIY RP2040-based emitter is also in development)
- A display running at 100 Hz or higher (a 240 Hz OLED is recommended)
- Visual Studio 2022 (Desktop development with C++), the Windows SDK and CMake 3.24+ to build it
- Emitter firmware extracted from your own copy of NVIDIA's 3D Vision USB driver package. It is proprietary and not included.

## Installing

There is no release build yet, so installing means building from source. Third-party libraries and NVIDIA's firmware are not included in this repository; you download them yourself in the steps below.

### 1. Build the app

```powershell
git clone <URL from the green Code button above>
cd nv3d-vision-restoration
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

1. Run `Launch.cmd`.
2. Pick your display and refresh rate, click **Match output rate**, then **Start 3D preview**. The glasses start shuttering.
3. Adjust **Phase** and **Shutter** until each eye sees only its own image. On OLEDs, turn on **Software black frame insertion** and use **Maximize brightness**.

Every control is explained in [docs/USAGE.md](docs/USAGE.md). Emitter details and troubleshooting are in [docs/EMITTER.md](docs/EMITTER.md).

Self tests that need no emitter:

```powershell
.\build\bin\Release\VisionRestoration.exe --gpu-test
.\build\bin\Release\VisionRestoration.exe --smoke-test
```

## Game support

| Type | Status |
|---|---|
| Direct3D 11 games with side-by-side stereo output (e.g. Dolphin, geo-11) | In progress |
| Direct3D 12 games without stereo, depth-based 3D from the game's depth buffer (e.g. GTA V Enhanced) | In progress |
| Games with native stereo 3D support | Planned |
| DirectX 9 and 10, and Vulkan | Planned |
| Stereo mods for games without built-in 3D | Planned |

## Roadmap

- [x] Original emitter control without the NVIDIA driver
- [x] Frame-sequential output at 100 to 240 Hz, SDR and HDR
- [x] Clean full-screen stereo confirmed through the glasses (4K 240 Hz OLED)
- [ ] Stable game hook
- [ ] Compatibility with all stereo-capable games
- [ ] One central install, with no files copied into game folders
- [ ] Mod support for games without native stereo
- [ ] DIY RP2040 emitter
- [ ] First release

## Documentation

- [Usage](docs/USAGE.md)
- [Emitter](docs/EMITTER.md)
- [Timing calibration](docs/TIMING-CALIBRATION.md)
- [Full screen at 120 Hz](docs/FULLSCREEN-120HZ.md)
- [DIY RP2040 emitter](docs/RP2040.md)
- [Development status](docs/STATUS.md)

## Contributing

Issues and pull requests are welcome. Bug reports help most when they include your display model, refresh rate, GPU and `reports/session.log`.

## Credits

Built with [libnvstusb](https://github.com/eruffaldi/libnvstusb) (emitter protocol reference), [libusb](https://libusb.info/), [libwdi](https://github.com/pbatard/libwdi), [Dear ImGui](https://github.com/ocornut/imgui), [Spout2](https://github.com/leadedge/Spout2) and the [ReShade](https://github.com/crosire/reshade) add-on API. None of their code is redistributed here; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[GPL-3.0-or-later](LICENSE). NVIDIA firmware is not included and is not covered by this license.

Not affiliated with or endorsed by NVIDIA. "3D Vision" is a trademark of NVIDIA Corporation.
