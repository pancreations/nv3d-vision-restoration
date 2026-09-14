# Usage

Windows frame-sequential stereo output for NVIDIA 3D Vision glasses. The original
USB emitter is driven through WinUSB/libusb and its RAM firmware. Clean stereo over the
whole screen was confirmed through the glasses on 2026-09-13, on a Samsung OLED at
4K 240 Hz in HDR with software black frame insertion. Other displays, refresh rates and
sequences, and stereo in games through the hook, are not optically verified.

Double-click **Launch.cmd** to run `build/bin/Release/VisionRestoration.exe`.

## Output

- **Start 3D preview** presents alternating eye frames beside the controls and
  drives the emitter. The preview stays visible as you change tabs and settings.
- **Fullscreen 3D** opens the same stereo output fullscreen. **Stop** or **Esc**
  returns to the controls.
- **Inspect eye images** displays the actual two eye images side by side in 2D.
- **Left image only / Right image only** makes the other image completely black.
- **Both eye targets** checks separation; **Stereo scene** returns to the depth scene.
  The scene words are 3D objects seen through the same stereo camera as the
  spheres: **NEAR** (red) floats in front of the screen, **SCREEN** (white) lies
  on it and **FAR** (blue) sits behind it. **LEFT** and **RIGHT** labels are
  reserved for the diagnostic patterns; two different words cannot fuse.
- **Stereo area** presents the image in a band of adjustable height and position
  with black outside it, in the app and in the game hook. A panel rewrites its rows
  top to bottom over most of a refresh, so at 120 Hz the whole screen is never one
  eye for long enough; a shorter band leaves a longer settled window for the
  shutter. **Fit area to shutter** and **Suggest phase** use the display's signal
  timing (Advanced > Panel holds the panel response and a measured scan time).
  Fullscreen keys: PageUp/PageDown height, Home/End position. See
  [docs/FULLSCREEN-120HZ.md](FULLSCREEN-120HZ.md) for the numbers and the
  whole-screen routes (240 Hz Left/Left/Right/Right, large vertical total).
- **Phase** on the NVIDIA emitter spans the full left/right cycle: about 20 ms
  at 100 Hz, 16.67 ms at 120 Hz, or 13.89 ms at 144 Hz. The second half
  changes the physical eye phase; it is not folded back into the first half.
  **Phase** and **Shutter** change emitter timing live. Arrows adjust timing;
  Shift uses fine steps. Fullscreen Space pauses, and X swaps the eye order.
- **Match output rate** sets the emitter timing from the selected output's rate.
  A mismatched rate blocks stereo startup. It does not change the output mode.
- **Phase sweep** (Output & timing, or **S** in fullscreen) advances the phase through
  the whole cycle while you watch; **M** marks a phase, **Enter** keeps the current one.
  The value is drawn in the output. **Software black frame insertion** (checkbox or **B**)
  switches to Left / Black / Right / Black so no row ever shows the other eye during a
  scan. It lights each eye on fewer refreshes, so recover the light with **Maximize
  brightness**: in this sequence the emitter's period is two display refreshes and the
  shutter may stay open across all of it, which a one-refresh limit had wrongly capped.
  The **Image brightness** slider spends HDR headroom on what is left. Each eye flashes at
  a quarter of the refresh rate: 60 per second at 240 Hz, 30 at 120 Hz.
- **Illumination** (Advanced > Panel) tells the model whether the panel holds its rows or
  strobes (LightBoost, BFI, Motion Clearness). The Stereo area section predicts the
  other-eye leakage of the current phase for the top, center and bottom; **Suggest phase**
  and **Fit area to shutter** use the same model. **Diagnostics** shows vblank jitter,
  present-interval jitter and the eye command timing error, and **Measure vblank** times
  the blanking interval against the presentation timestamps. See
  [docs/TIMING-CALIBRATION.md](TIMING-CALIBRATION.md).

The renderer has no cross-eye blending or image-subtraction path. Version 7
profiles add the illumination, strobe and scan-start fields; version 6 profiles add
the stereo area and panel timing; version 5 and older
profiles omit those controls; older profiles load their timing and scene settings
while ignoring retired blending fields. The embedded preview uses the actual
frame-sequential renderer and emitter timing, with no decorative image overlay.

**Image alignment** stays beside the preview: convergence shifts the eye images
horizontally, and scene depth changes the built-in stereo scene's camera separation.
Both apply live. Diagnostic eye-isolation targets stay fixed.
**Sources & games** selects stereo images, window capture, Spout or the game hook.
**Profiles** saves named setups; timing and scene changes also autosave.
**Diagnostics** reports presentation/USB timing and runs checks of the rendered
pixels. **Advanced** holds per-eye timing, sequences, emitter connection and modes.

The software checks verify separate eye frames and black frames in SDR and HDR.
They do not establish that all visible leakage through a glasses lens is gone.
See the [emitter timing fixes](EMITTER-TIMING-FIX.md) and
[presentation synchronization](PRESENTATION-SYNC.md) for implementation details.

## Games (in-game hook)

> **The in-game hook does not work and has never worked.** This section documents the design and code as they stand. To get a game into 3D today, set it to side-by-side output and capture its window, or send it from ReShade with the `VisionStereoSpout` add-on, under **Sources & games**.

Leave the app open. A game with the hook installed takes over the glasses automatically and releases them when it exits, the way the NVIDIA driver did. `VisionGameHook.addon64` is a ReShade add-on that runs inside the game: each game frame it presents the profile's whole sequence (Left/Right, Left/Black/Right/Black or Left/Left/Right/Right) on the game's own swap chain, one slot per refresh, and reports DXGI present statistics through shared memory. The app maps those presents to refreshes and times the emitter with the current profile. Nothing is captured.

The hook handles two kinds of game:

- **Games that render both eyes** (Direct3D 11). Dolphin with Backend = Direct3D 11 and Stereoscopic 3D Mode = Side-by-Side, or geo-11 with side-by-side output. This is true two-camera stereo; the hook only splits the halves.
- **Games that render one camera** (Direct3D 12, for example GTA V Enhanced). The hook finds the game's depth buffer and resamples both eyes from the finished frame. Every visible surface gets its correct depth, but both eyes come from one camera position: what the camera could not see is filled from the background beside it, and glass, smoke and the HUD take the depth of whatever is behind them.

`ReShade.ini` in the game folder chooses between them with `[VISION] StereoSource=auto|packed|depth`. Auto means packed on Direct3D 11 and depth on Direct3D 12.

While the game has focus:

| Keys | Action |
| --- | --- |
| Ctrl+Shift+PageUp / PageDown | Depth strength (separation at infinity, percent of the screen width) |
| Ctrl+Shift+Home / End | Convergence. Home brings the screen plane nearer, so less comes out of the screen; End pushes it away, so more comes out |
| Ctrl+Shift+Insert | 2D / 3D, with the glasses still running |
| Ctrl+Shift+Delete | Depth defaults |
| Ctrl+Shift+F8 | Diagnostics: original image, left eye only, right eye only, depth view (screen depth light grey, sky black, twice as near white) |
| Ctrl+Alt+Up/Down, Left/Right, X | Phase, shutter, swap eyes; Save in Profiles keeps them |

Depth settings are written to the game's `ReShade.ini` at once.

On Direct3D 12 each game frame is presented as one whole sequence inside the game's own Present, so the game runs at the display rate divided by the sequence length (60 fps at 240 Hz with Left/Black/Right/Black). There is no refresh filler on Direct3D 12. A frame that takes longer repeats its last slot, which is black in Left/Black/Right/Black. A game that counts its own presents instead of asking for the current back buffer (Dolphin's D3D12 backend) is detected and left in 2D.

Install with **Games > Install hook next to a game...**. It copies the ReShade add-on build as `dxgi.dll`. If OptiScaler already owns `dxgi.dll`, it copies ReShade as `ReShade64.dll` and sets `LoadReshade=true` in `OptiScaler.ini` instead. Then it copies the add-on and writes a minimal `ReShade.ini`. ReShade's add-on installer must have been run once on the PC. Do not enable ReShade effects with the hook.

### GTA V Enhanced

- Play story mode with BattlEye off: Steam launch option `-nobattleye`. GTA Online is unavailable while it is off.
- In the game's graphics settings, turn VSync on and DLSS Frame Generation off. DLSS, the DLSS 5 Neural Rendering mod and HDR can stay on.
- If the scene looks inside out or flat, press Ctrl+Shift+F8 until the depth view appears. Near objects should be bright and the sky black. `[VISION] DepthReversed=0` or `1` overrides the automatic detection.

## Build and check

Requires Visual Studio 2022 C++ desktop tools, Windows SDK and CMake 3.24+. Third-party dependencies are not part of the repository; `tools/Get-Dependencies.ps1` downloads them into `third_party/` from their official releases.

```powershell
.\tools\Get-Dependencies.ps1
.\build.ps1
# Hardware-independent pattern rendering and native UI checks:
Start-Process .\build\bin\Release\VisionRestoration.exe --gpu-test -Wait
Start-Process .\build\bin\Release\VisionRestoration.exe --smoke-test -Wait
Start-Process .\build\bin\Release\VisionRestoration.exe --probe -Wait
# Vblank timing of every output against the presentation timestamps (reports/vblank.txt):
Start-Process .\build\bin\Release\VisionRestoration.exe --vblank -Wait
```

CTest includes core tests and an integration test using the installed GPU and a local Spout sender. It makes no emitter writes or display-mode changes. `--gpu-test` uses WARP; `--smoke-test` uses the actual GPU and writes an image of the application's own UI. These checks do not establish optical performance.

Original application code is provided under GPL-3.0-or-later; preserve the upstream notices and licenses. Proprietary firmware is not bundled.
