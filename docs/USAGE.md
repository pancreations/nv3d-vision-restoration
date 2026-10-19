# User guide

This guide covers everything you'll do in the app day to day. For first-time setup (firmware, USB driver), start with the [README](../README.md#getting-started).

## The main window

The app has two tabs on the left:

- **Tuning**: display, refresh rate, timing and picture settings.
- **Input/Games**: what you want to see in 3D.

The 3D preview sits beside the controls. There are three ways to show 3D:

| Button | What it does |
|---|---|
| **Start 3D preview** | 3D inside the app window. Good for calibrating. |
| **Windowed preview** | 3D in a separate window you can move and resize. **F11** makes it fullscreen. |
| **Fullscreen 3D (F11)** | 3D over the whole display. This is how you'll normally watch. |

**Inspect eye images** shows the two eye images side by side in plain 2D, so you can check the source without the glasses.

## Calibrating your glasses

Do this once per monitor and refresh rate. Settings save automatically, and **Profiles** stores named setups.

1. Set the monitor to its highest refresh rate in Windows (240 Hz is ideal, 120 Hz also works).
2. On **Tuning**, choose the display and click **Match output rate**.
3. Turn on **Software black frame insertion**. It puts a black frame between the eyes, which gives the cleanest separation on OLED at 240 Hz.
4. Click **Start 3D preview**. The glasses start shuttering and you'll see a test scene with the words NEAR, SCREEN and FAR at different depths.
5. Close one eye and adjust **Phase** until that eye sees only its own image, then check the other eye. **Phase sweep** (or **S** in fullscreen) moves through every phase for you: press **M** to mark a good spot and **Enter** to keep it.
6. Widen **Shutter** for a brighter picture, until the other eye's image starts to show through. **Maximize brightness** suggests a setting.
7. If the eyes are reversed, use **Swap eye order** (**Ctrl+Alt+X**).

**Left only / Right only** test patterns make it easy to check one lens at a time.

Picture controls:

- **Image brightness** brightens the 3D picture to make up for the glasses' dark lenses.
- **Black level** at zero is neutral. Left of zero deepens shadows; right of zero lifts them.
- **Convergence** shifts the eye images horizontally to move the whole picture nearer or farther.

## Fullscreen and keyboard shortcuts

In fullscreen, press **Home** to open the full control menu inside the 3D picture. Every setting is available there.

| Key | Action |
|---|---|
| **Home** | Show / hide the menu |
| **F11** | Fullscreen / windowed |
| **Ctrl+Alt+F8** | Stop output |
| **Space** | Pause / resume |
| **Ctrl+Alt+↑ / ↓** | Phase |
| **Ctrl+Alt+← / →** | Shutter |
| **Ctrl+Alt+X** | Swap eyes |
| **Ctrl+Alt+R** | Re-lock sync |
| **B** | Black frame insertion on / off |
| **S**, **M**, **Enter** | Phase sweep, mark, keep |

The Ctrl+Alt shortcuts also work while a game has focus. Change or disable any shortcut under **Settings > Shortcuts**. The tray icon always brings the controls back.

## Choosing what to watch

On **Input/Games**, pick a **Source**.

### Capture Window (side-by-side or top/bottom 3D)

Use this for any program that already outputs 3D as two images in one window: emulators like Dolphin, Blender's stereo viewport, a video player showing an SBS movie, and so on.

1. Set the program to side-by-side (SBS) or top/bottom output.
2. Choose **Capture Window**, select the program's window, and set **Packing** to match:
   - **Half SBS**: both eyes squeezed into one normal-width picture (most common for video).
   - **Full SBS**: each eye at full width, so the window is twice as wide.
   - **Top / bottom**: one eye above the other.
3. Press **Fullscreen 3D (F11)**. The program keeps your mouse, keyboard and controller.

Run the program in windowed or borderless mode, not exclusive fullscreen. If the image looks flat or doubled, check Packing. If the eyes feel reversed, use Swap eye order.

**Capture method:** keep **Windows Graphics Capture** (the default). The alternative, DXGI Desktop Duplication, has not worked reliably with games.

### Stereo Image

Choose **Stereo Image**, then **Choose image...** to open a side-by-side or top/bottom photo (`.jps`, `.png`, `.jpg`).

### Ai 3D Desktop (2D to 3D)

Converts everything on a display into 3D using an AI depth model, similar to Leia's SpaceWalker. It needs a model file in the `models` folder:

| Model | Speed | Best for |
|---|---|---|
| [Small](https://huggingface.co/onnx-community/depth-anything-v2-small/blob/main/onnx/model_fp16.onnx) | Fast | Desktop, games, anything moving |
| [Base](https://huggingface.co/onnx-community/depth-anything-v2-base/blob/main/onnx/model_fp16.onnx) | Medium | Films (non-commercial license) |
| [Large](https://huggingface.co/onnx-community/depth-anything-v2-large/blob/main/onnx/model_fp16.onnx) | Slow | Films, when nothing else uses the GPU (non-commercial license) |

Click **Start screen 3D**. The 3D picture lies over the desktop and lets clicks and typing pass through, so you can keep using your PC. **Responsive desktop** is a good starting preset.

| Key | Action |
|---|---|
| **Ctrl+Alt+PageUp / PageDown** | More / less depth |
| **Ctrl+Alt+Home / End** | Move the screen plane nearer / farther |
| **Ctrl+Alt+Insert** | Switch between 2D and 3D |
| **Ctrl+Alt+F8** | Stop |

**Depth strength**, **Screen plane** and **Depth smoothing** shape the effect. **Show depth map** shows what the AI sees (white is near, black is far). Optional **Depth map controls** let you fine-tune it.

### VRto3D and Wibble Wobble (VR games on a monitor)

Both play SteamVR games on your monitor in 3D. SteamVR and VRto3D or Wibble Wobble are installed separately.

- **VRto3D (SBS / TAB)**: select the VRto3D window and matching packing. In VRto3D's **Ctrl+Home > System** menu, turn **Auto Focus** off and save. [Setup guide](VRTO3D.md)
- **Wibble Wobble**: connects directly to SteamVR's eye images. The 3D preview starts automatically when ready. [Setup guide](WIBBLE-WOBBLE.md)

Both are experimental.

### Game Stereo (games with Geo11 fixes)

<a id="games-with-geo11-fixes"></a>
For DirectX 11 games with a community 3D fix. The fix renders both eyes; the app shows them on your glasses.

1. Install the game's fix from [Helix Mod](https://helixmod.blogspot.com/), [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) or the [Geo11 releases](https://github.com/ThreeDeeJay/geo-11/releases), following its instructions.
2. With the game closed, choose **Game Stereo**, click **Choose game...** and select the game's EXE (not its launcher).
3. Click **Prepare game**, then launch the game normally in windowed or borderless mode. 3D starts on its own once the game is running.
4. Adjust depth and convergence under **Game depth**.

Before updating or removing the fix, close the game and click **Disconnect adapter**.

This is experimental. Some games still crash (Resident Evil 2, Metal Gear Solid V), and old fixes that need NVIDIA's own 3D Vision driver won't work.

### Built in Pattern

Test scenes and eye patterns for calibration.

## Recording and streaming (2D stream view)

A recording or screen share of the 3D output flickers and jumps, because the picture alternates eyes and black frames many times a second. Only the glasses can make sense of it.

Tick **2D stream view** on Input/Games to get a steady single-eye picture for OBS, Discord and the like:

- **Cover the display** on (default): share **the screen**.
- **Cover the display** off: share the window named **Vision Restoration stream view (2D)**.
- **Right eye** streams the other eye.

Your glasses keep getting full 3D either way.

## HDR

**HDR output (experimental)** on Tuning is off by default; start in SDR. HDR needs Windows HDR turned on for that display.

## LCD, QLED and Mini-LED monitors

These panels can't yet give clean 3D: their pixels change too slowly, so each eye sees a ghost of the other. **Advanced LCD calibration** and **Panel experiments** are tools for people who want to experiment. See [Panel experiments](PANEL-EXPERIMENTS.md).

## Troubleshooting

| Problem | Try this |
|---|---|
| Glasses don't shutter | Check that the emitter is plugged in directly (no hub), then click **Refresh USB**. See [Emitter](EMITTER.md). |
| Each eye sees part of the other image | Adjust Phase, then narrow Shutter. Make sure black frame insertion is on at 240 Hz. |
| Picture flashes or loses sync | Press **Re-lock sync** (**Ctrl+Alt+R**). If it keeps happening, report it with the log files below. |
| Captured window is black or frozen | Keep the program's window visible and not minimized, and use Windows Graphics Capture. |
| Game's fix isn't detected | Select the game's real EXE, not the launcher, and make sure it's a Geo11 fix. |

**Reporting a problem:** include your display model, refresh rate and graphics card, plus `reports/session.log` and `reports/miss-events.log` from the app folder. These logs contain hardware details and folder paths, so check them before posting publicly.

## More detail

- [Emitter setup and troubleshooting](EMITTER.md)
- [RP2040 DIY emitter](RP2040.md)
- [Timing calibration in depth](TIMING-CALIBRATION.md)
- [How the app stays in sync](PRESENTATION-SYNC.md)
- [Developer notes](DEVELOPMENT.md)
