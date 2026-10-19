# Vision Restoration

**Use your NVIDIA 3D Vision glasses again, on a modern PC.**

NVIDIA ended 3D Vision in 2019, and current graphics drivers can't run the glasses. Vision Restoration replaces that driver. It talks to the 3D Vision USB emitter directly, shows 3D frame by frame on a high-refresh monitor, and keeps the glasses in sync. Your graphics driver stays as it is: nothing is patched, downgraded or replaced.

> **Alpha software.** It works very well on the setup it was built on (a 240 Hz OLED monitor and an RTX 5070 Ti), and is still being tested elsewhere. The compatibility table below separates what has been confirmed through the glasses from what hasn't.

**[Download the latest release](../../releases/latest)** · [User guide](docs/USAGE.md)

## What you can watch in 3D

- **Any program that already shows side-by-side or top/bottom 3D in a window.** Examples are emulators such as Dolphin and Blender's stereo 3D viewport. The app captures the window and turns it into real 3D for your glasses.
- **Stereo photos** (side-by-side or top/bottom image files, including `.jps`).
- **Your whole desktop, converted from 2D** with an AI depth model: videos, 2D games, anything on screen.
- **VR games on a monitor** through VRto3D or Wibble Wobble.
- **DirectX 11 games with community 3D fixes** (Geo11), the same fixes 3D Vision players already use.

You can also record or stream a normal 2D picture while you play in 3D, using the *2D stream view* option.

## Compatibility

✅ **Works**: confirmed through the glasses · 🧪 **Experimental**: runs, not yet confirmed or has known issues · ❌ **Not yet**

### Displays

| Display | Status |
|---|---|
| OLED at 240 Hz (with black frame insertion) | ✅ Clean, full-screen 3D. The best experience. |
| OLED at 120 Hz | ✅ Works |
| OLED at other refresh rates (100, 144 Hz) | 🧪 Untested |
| LCD, QLED, Mini-LED | ❌ Not yet. These panels change pixels too slowly, so each eye sees a ghost of the other eye's image. |

### Hardware

| Hardware | Status |
|---|---|
| 3D Vision and 3D Vision 2 glasses | ✅ |
| Original NVIDIA 3D Vision USB emitter | ✅ |
| DIY RP2040 emitter ([guide](docs/RP2040.md)) | ✅ on OLED at 120 and 240 Hz |
| NVIDIA graphics cards | ✅ Tested on an RTX 5070 Ti; other generations untested |
| AMD graphics cards | 🧪 Output tests pass; not yet tried with the glasses |
| Intel graphics | 🧪 Untested |

### 3D sources

| Source | Status |
|---|---|
| Window capture of side-by-side / top-bottom 3D (Dolphin, Blender, …) | ✅ Works, including while the GPU is under heavy load |
| Stereo photos | 🧪 |
| Whole desktop in 3D (AI depth) | 🧪 Needs a free model download (see below) |
| VRto3D and Wibble Wobble (SteamVR) | 🧪 |
| Geo11 DirectX 11 game fixes | 🧪 Some games still crash (Resident Evil 2, Metal Gear Solid V) |
| HDR output | 🧪 Off by default. Some OLEDs buzz audibly at high HDR brightness. |
| Games that relied on NVIDIA's own 3D Vision driver, DirectX 9/10/12, Vulkan | ❌ Not supported |

Windows 10 or 11 (64-bit) is required.

## Getting started

The download also includes a printable **Quick Start** PDF covering these steps.

1. **Download and extract.** Get `Vision-Restoration-Portable` from [Releases](../../releases/latest), extract the whole ZIP to a normal folder, and run **Start Vision Restoration.cmd**.
2. **Give the emitter its firmware** (original NVIDIA emitter only). The emitter needs a small firmware file every time it's plugged in. NVIDIA owns it, so it can't be included here. Download NVIDIA's [3D Vision USB controller package](https://us.download.nvidia.com/Windows/Quadro_Certified/NV3DVisionUSB/390.41/3dvisioncontrollerdriver.exe). **Don't install it.** Drag it onto **Prepare NVIDIA Firmware.cmd** instead. The URL says Quadro, but any graphics card works. RP2040 owners skip this step: their firmware is included.
3. **Connect the emitter.** Plug it straight into the PC (not a hub) and start the app. When it offers to set up the USB driver, accept the administrator prompt. This happens once or twice per PC. If it doesn't work, see [manual driver setup](docs/EMITTER.md).
4. **Calibrate.** Set your monitor to 240 Hz (or 120 Hz). In the app, click **Match output rate**, turn on **Software black frame insertion**, and click **Start 3D preview**. Close one eye at a time and adjust **Phase** and **Shutter** until each eye sees only its own image. Your settings save automatically.
5. **Pick something to watch.** Under **Input/Games > Source**, choose **Capture Window** and select the program showing side-by-side 3D (in Dolphin: *Graphics > Stereoscopic 3D Mode > Side-by-Side*). Then press **Fullscreen 3D (F11)**. In fullscreen, **Home** opens the menu.

### Optional: whole desktop in 3D

Download the free [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small/blob/main/onnx/model_fp16.onnx) model into the `models` folder, then choose **Ai 3D Desktop** as the source. [Base](https://huggingface.co/onnx-community/depth-anything-v2-base/blob/main/onnx/model_fp16.onnx) and [Large](https://huggingface.co/onnx-community/depth-anything-v2-large/blob/main/onnx/model_fp16.onnx) models give finer depth but are slower and for non-commercial use only.

### Optional: games with Geo11 fixes

Install the game's community fix from [Helix Mod](https://helixmod.blogspot.com/), [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) or the [Geo11 releases](https://github.com/ThreeDeeJay/geo-11/releases). In the app, choose **Game Stereo**, pick the game's EXE with **Choose game...**, click **Prepare game**, and launch the game in windowed or borderless mode. See the [user guide](docs/USAGE.md#games-with-geo11-fixes) for details.

## If something goes wrong

- **Flashing or lost sync:** press **Re-lock sync**, or **Ctrl+Alt+R** in fullscreen. If it keeps happening, open an issue and attach `reports/session.log` and `reports/miss-events.log`.
- **Each eye sees a bit of the other image:** adjust **Phase** first, then **Shutter**. On an LCD this is the panel limit described above.
- **Recording or screen sharing shows a jumping, flashing picture:** that's the 3D signal. Tick **2D stream view** and share that instead.

More help: [User guide](docs/USAGE.md) · [Emitter setup and troubleshooting](docs/EMITTER.md) · [RP2040 DIY emitter](docs/RP2040.md) · [VRto3D](docs/VRTO3D.md) · [Wibble Wobble](docs/WIBBLE-WOBBLE.md)

## What's new in 0.3.6

- **No more flashing under heavy GPU load.** Rendering in Blender's Cycles viewport, or any other GPU-heavy program, used to make the glasses flash black. The output now gets top GPU priority from Windows and stays in sync.
- More reliable emitter recovery when the display timing shifts.
- The fullscreen menu is drawn inside the 3D picture, so opening it no longer causes flashes.
- The black level slider can now darken shadows as well as lift them.
- HDR is off by default and marked experimental.

Full details are in the [release notes](docs/RELEASE-v0.3.6.md).

## Building from source

You need Visual Studio 2022 (Desktop development with C++), the Windows SDK and CMake 3.24 or newer.

```powershell
git clone <this repository's URL>
cd nv3d-vision-restoration
.\tools\Get-Dependencies.ps1   # downloads third-party libraries from their official sources
.\build.ps1                    # builds and runs the tests
```

The app is built to `build\bin\Release\VisionRestoration.exe`. Developer notes are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Contributing

This is a personal project, published to show that 3D Vision can live on. The most useful help is testing on other displays and graphics cards, LCD crosstalk, and game compatibility. Bug reports help most with your display model, refresh rate, graphics card and `reports/session.log`.

## Credits and license

Built with [libnvstusb](https://github.com/eruffaldi/libnvstusb) (emitter protocol reference), [libusb](https://libusb.info/), [libwdi](https://github.com/pbatard/libwdi), [Dear ImGui](https://github.com/ocornut/imgui) and the [ReShade](https://github.com/crosire/reshade) add-on API. No third-party source is stored in this repository; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Licensed under [GPL-3.0-or-later](LICENSE). NVIDIA's firmware is not included and is not covered by this license. Not affiliated with or endorsed by NVIDIA. "3D Vision" is a trademark of NVIDIA Corporation.
