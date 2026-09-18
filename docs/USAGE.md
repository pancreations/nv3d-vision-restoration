# Usage

Windows frame-sequential stereo output for NVIDIA 3D Vision glasses. The original
USB emitter is driven through WinUSB/libusb and its RAM firmware. Clean stereo over the
whole screen was confirmed through the glasses on 2026-09-13, on a Samsung OLED at
4K 240 Hz in HDR with software black frame insertion. Other displays, refresh rates and
sequences are not optically verified.

> **Game integration:** the shared Geo11 hook and game-depth controls are available
> under **Games**. GPU output checks pass; broad game/optical validation remains
> incomplete and the real-Geo11 test fixture crashes at shutdown, including without
> our hook. Earlier game trials are historical and rolled back. Window capture
> remains a proven fallback. The old ReShade hook remains nonworking.

Double-click **Launch.cmd** to run `build/bin/Release/VisionRestoration.exe`.

## Live controls

During **fullscreen output**, **Home** shows or hides the complete controls
menu. This works with SBS sources, AI desktop and the test patterns. The normal
app window is hidden until fullscreen ends. Opening the menu does not replace
the source, restart the emitter or resize the output.

All input, profile, display and diagnostic options are available in the menu.
**Shortcuts** lets you rebind functions, disable individual bindings, or turn
all fullscreen shortcuts off for gaming. **Outside app** makes a binding work
while a game or the desktop has focus. Bindings are inactive outside fullscreen
and persist in `profiles/shortcuts.ini`. The tray icon (or launching `Launch.cmd`
again) reopens the controls even when shortcuts are disabled. **Tab** remains
available for normal control navigation.

**Phase**, **Shutter**, **Image brightness**, and **HDR output** are together at
the top of the main and fullscreen control panels. Drag a slider, Ctrl+click to
type, or use the phase/shutter minus and plus buttons. Brightness applies live;
HDR changes the output buffer format while retaining the output window.
**HDR highlights** controls the brightness test pattern's peak luminance.

Saved manual profiles, including the Dell preload profile, retain their original
timing mode when loaded. They are no longer automatically converted to guarded
LCD timing. That mode is optional under **Advanced LCD calibration**. Profile
loading also restores the saved image brightness and black level.

Stereo shaders are compiled when building the app. Switching between fullscreen
and preview no longer recompiles them or blocks Stop on shader compilation.
Stopping the renderer continues servicing synchronous Windows messages, so a
fullscreen transition cannot deadlock while DXGI waits for the control window.
AI desktop capture excludes the app's preview, fullscreen output and controls
in every view, preventing the converted image from capturing itself.

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
  The value is drawn in the output. The old subtraction-based LightBoost button has
  been removed: it did not control the backlight. **Legacy ghost subtraction** remains
  explicitly experimental and is not enabled by the frame-rate buttons.
- **Panel experiments** offers direct output, black reset, neutral gray reset and
  preload/hold sequences with the official NVIDIA emitter. Nine-row optical targets and
  measured phase ranges let you check both lenses across the whole screen without
  assumed response times. See [PANEL-EXPERIMENTS.md](PANEL-EXPERIMENTS.md).
- **Display type** labels the panel family; it no longer fills in guessed response times.
  **Advanced > Sequence** exposes holds of up to four refreshes and holds with resets.
  Rate buttons and Compare sequences still use the approximate model, not measurements.
  Neutral reset is excluded from that model. **LCD black floor** is a legacy image
  adjustment, not a demonstrated way to accelerate pixels. Use zero for calibration.
- **Software black frame insertion** gives 60 new frames/eye/s at 240 Hz, 36 at 144 Hz,
  and 30 at 120 Hz. Pixel persistence may remain on LCDs. **Maximize brightness** is a
  model-based suggestion; widening the shutter can admit unwanted light.
- **Illumination** (Advanced > Panel) tells the model whether the panel holds its rows or
  strobes (LightBoost, BFI, Motion Clearness). The Stereo area section predicts the
  other-eye leakage of the current phase for the top, center and bottom; **Suggest phase**
  and **Fit area to shutter** use the same model. **Diagnostics** shows vblank jitter,
  present-interval jitter and the eye command timing error, and **Measure vblank** times
  the blanking interval against the presentation timestamps. See
  [docs/TIMING-CALIBRATION.md](TIMING-CALIBRATION.md).

Profiles now use version 10 for neutral reset. Older profiles retain their timing; old executables cannot read the new version. The separate experimental build keeps its own profile copies beside its EXE.

**Image alignment** stays beside the preview: convergence shifts the eye images
horizontally, and scene depth changes the built-in stereo scene's camera separation.
Both apply live. Diagnostic eye-isolation targets stay fixed.
**Sources & games** selects stereo images, VLC stereo movies, window capture or the whole screen converted with AI depth. Spout and Blender viewport inputs have been removed. The game-output controls connect existing Geo11 fixes; broader API coverage remains in development.
**Profiles** saves named setups; timing and scene changes also autosave.
**Diagnostics** reports presentation/USB timing and runs checks of the rendered
pixels. **Advanced** holds per-eye timing, sequences, emitter connection and modes.

The software checks verify separate eye frames and black frames in SDR and HDR.
They do not establish that all visible leakage through a glasses lens is gone.
See the [emitter timing fixes](EMITTER-TIMING-FIX.md) and
[presentation synchronization](PRESENTATION-SYNC.md) for implementation details.

## VLC stereo movies

1. Install [64-bit VLC 3.x](https://www.videolan.org/vlc/). The application loads its optional library; it does not modify VLC settings or require the legacy NVIDIA stereo driver.
2. Under **Input**, select **VLC stereo movie** and choose **Side by side** or **Top / bottom** to match the file. **Choose movie...** opens a local video file. Both half and full packed layouts use the same eye split; the image expands to the output area. The current presenter stretches to that area, so use a matching display aspect ratio or a movie with encoded letterboxing.
3. Click **Start source**, then start your calibrated **3D preview** or fullscreen output. Use **Swap eyes** for right-first movies. Packing changes stop the source; start it again with the new setting.
4. **Pause movie / Resume movie** controls both VLC video and audio while the output retains a complete stereo pair. **Movie position** seeks when supported; **Movie volume** adjusts sound. **Stop movie** stops decoding. At end-of-file the last complete pair remains visible; **Start source** starts the movie again.

Installed VLC is detected in its standard Program Files location. A complete `vlc` folder beside the application also works, or **Choose VLC...** selects a portable installation's `vlc.exe`. Keep `libvlc.dll`, `libvlccore.dll` and `plugins` together. A 32-bit or VLC 4.x runtime is rejected with an error; other inputs continue to work without VLC installed.

The integration uses the documented [VLC 3 video callbacks](https://videolan.videolan.me/vlc-3.0/group__libvlc__media__player.html) to receive video pixels at VLC's presentation time, then publishes complete GPU-shared pairs to the existing output. Audio remains with VLC. The VLC window and controls are not part of the movie pixels. CPU decoding and copies can limit high-resolution playback; this first implementation provides SDR pixels and does not claim faithful HDR movie output. External subtitle composition, disc/MVC decoding, other stereo formats and movie playback through the glasses remain unverified. Ordinary 2D video requires the separate AI desktop conversion while playing in VLC.

## Whole screen in 3D (AI depth)

> Built 2026-09-15 and checked only on the desktop without glasses: the capture, the depth
> network and the eye images work (`--screen-test`). Whether it looks right through the
> glasses, and whether the network's GPU work disturbs the shutter timing, is not yet verified.

**Input/Games > Source > Whole screen (AI depth)** converts everything on a display into 3D,
the way Leia's SpaceWalker and Samsung's Reality Hub do: the display is captured, a depth
network (Depth Anything V2 Small, through ONNX Runtime on DirectML) estimates the depth of every
pixel, and both eyes are resampled from the picture with that depth. The network runs in its own
process, `VisionDepth.exe`, at a low GPU scheduling class so its work queues behind the presenter.

- **Start screen 3D** captures the chosen display and lays the stereo output over the output
  display as a click-through window: mouse and keyboard go through to
  the desktop, and Windows draws the cursor above it at screen depth. The overlay is hidden from
  screen capture so it never captures itself. **Ctrl+Alt+F8** stops it; **Ctrl+Alt+PageUp /
  PageDown** change the depth strength, **Ctrl+Alt+Home / End** move the screen plane, and
  **Ctrl+Alt+Insert** switches between 2D and 3D while the overlay is up. The controls under
  **Sources & games** stay clickable through the overlay.
- **Start source** alone feeds the conversion to the 3D preview and the fullscreen output instead.
- **Depth strength** is the parallax at infinity as a share of the screen width (2 % default).
  **Screen plane** is the nearness that sits on the screen: 1 keeps everything behind the glass,
  lower values bring the nearest content out, limited by **Pop-out limit**. **Depth smoothing**
  averages the network's depth over time. **Network input** trades speed for detail (518 px is
  about 5 ms per depth map on an RTX 5070 Ti; the map is refreshed up to 30 times a second and the
  eyes are redrawn for every new desktop frame). **Show depth map** shows what the network sees:
  brighter is nearer, mid grey is the screen plane.
- The depth map is a few hundred pixels across and sampled bilinearly, so depth edges are soft
  over about 7 screen pixels; a moving edge also lags the picture by one network run. Guided
  upsampling of the depth to the picture's edges is the next step if that shows.
- **Setup:** the portable release includes the AI helper and runtime. Download the
  [Small model](https://huggingface.co/onnx-community/depth-anything-v2-small/blob/main/onnx/model_fp16.onnx)
  into `models/`. Source builders can instead run `tools/Get-Dependencies.ps1`, which downloads
  the same runtime and model before `build.ps1` builds `VisionDepth.exe`.
- **Stronger models for films.** Download [Base](https://huggingface.co/onnx-community/depth-anything-v2-base/blob/main/onnx/model_fp16.onnx)
  or [Large](https://huggingface.co/onnx-community/depth-anything-v2-large/blob/main/onnx/model_fp16.onnx),
  or use `tools\Get-DepthModel.ps1 -Size base` / `-Size large` in a source checkout. Base and Large
  are non-commercial. Save each download with a distinct name in `models/`; pick one under **Model** (a change restarts the
  helper while the picture stays up; the choice is saved with the profile). **Choose model...**
  accepts any other ONNX depth model with an NCHW RGB input. Measured on an RTX 5070 Ti at the
  518 px input, with the app running at the same time:

  | Model | Per depth map | Size | License | Use |
  |---|---|---|---|---|
  | Small (default) | 5 ms | 50 MB | Apache-2.0 | games, desktop, anything moving fast |
  | Base | 16 ms | 195 MB | CC-BY-NC-4.0 | films and video: finer edges, small objects |
  | Large | 38 ms | 670 MB | CC-BY-NC-4.0 | films where the GPU has nothing else to do |

  The network never takes more than half of the GPU: it runs at most **Depth updates per
  second** times a second and never more often than twice its own run time, so Large refreshes the
  depth about 13 times a second, which is fine for a film and too slow for a fast game. Watch
  **Diagnostics > Slips** after choosing a bigger model: slipped refreshes there mean the network's
  GPU bursts are delaying the shutter timing, and the answer is a smaller model or a lower rate.
- **Check without glasses:** `VisionRestoration.exe --screen-test 10` converts the first display
  for ten seconds without any overlay or emitter, and writes `reports/screen-test.txt` with the
  timings, `reports/screen-sbs.png` (both eyes) and `reports/screen-depth.png` (the depth view).
  `--display N` picks another display.

## Games (existing Geo11 fixes)

The left workspace has **Tuning** and **Input/Games** tabs. Keep the calibrated
output selected. The existing community fix renders the eyes; the app receives
completed pairs and presents them in frame sequence.

1. Install the game's compatible community fix according to its instructions.
2. In **Input/Games**, use **Choose game...** to select the actual rendering EXE.
   With the game closed, click **Connect game**. Geo11 is detected automatically.
   Native and legacy inputs require their actual output layout; selecting frame
   sequential cannot turn a mono backbuffer into stereo.
3. Launch normally. The same tab matches the game's window and shows whether its
   stereo provider is available. Click **Start game 3D** for output over the game with input passed through.
   Use windowed/borderless mode on the selected output display. **Start game capture**
   remains available for feeding the separate preview.
4. Geo11 depth controls appear under **Game depth**. They save only when edited
   or applied; the community fix retains shader corrections and auto-convergence.

The connector supports x86 and x64. Geo11 uses its existing proxy interface;
other supported providers use the capture add-on. Classic 3Dmigoto capture does
not restore its NVIDIA stereo-rendering backend. See [tested capabilities](DIRECT-EYES.md).

Before updating or removing a fix, close the game and use **Disconnect adapter**.
Connection ownership and restoration records live in
`VisionRestoration.CaptureBackup` beside the game EXE.

Existing SBS/TAB modes retain their supplied eye resolution. Katanga supplies
full-resolution eyes. The adapter preserves the Geo11 output mode.

### Historical trials and troubleshooting

Both game deployments below were rolled back. They are not results for the new
shared hook; see [current validation](STATUS.md) for the isolated test results.

- **Batman: Arkham City GOTY:** DX11 x86, generic Geo11 0.7.11. User confirmed
  stable 3D and normal controls. Bloom/alpha defects remain; this test did not
  install a Batman-specific shader fix.
- **Psychonauts 2:** DX11 x64, Universal UE4 Fix 9.13 / Geo11 0.7.11, linked by
  local 3D Fix Manager profile 1306. ReShade was removed. The user resolved double
  vision with the mod. Recurring flashing persisted after both scheduling and
  fixed-refresh-slot updates. A GPU completion repair is installed; gameplay
  confirmation is pending.
- **SteamVR starts unexpectedly:** this output route does not need SteamVR.
  The tested Katanga edition of 3D Fix Manager repeatedly initialized OpenVR,
  even with its saved VR mode disabled. Exit that manager after installing fixes,
  leave Vision Restoration open, and launch the game normally.
- **Double images or uncomfortable depth:** check the fix's separation and
  convergence controls. Shader defects and glasses alignment need different fixes.
- **Black flashes or lost stereo:** record the scene and `VisionStereo11.log`
  beside the game, plus the app's `reports/session.log`. Short clean samples do
  not establish stability through an entire game session.
- **Fix not detected:** the connector needs Geo11 with `force_stereo=2`. An old
  NVIDIA-driver-dependent Helix/3Dmigoto fix needs a compatible renderer first.
  Select the game executable rather than its launcher.
- **Proxy conflict:** the connector leaves the installation unchanged. That
  wrapper chain needs explicit compatibility work.

Native NVAPI stereo, DX9/10, native DX12 and other media-player integrations need
separate backends. VLC SBS/TAB files use the direct movie source above. The old ReShade hook remains nonworking; historical notes are
in [adapters/README.md](../adapters/README.md). Window capture and AI desktop remain.
See [runtime documentation](../runtime/README.md) for implementation and test scope.

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

CTest includes core tests and an integration test using the installed GPU and stereo image fixtures. It checks retained GPU frames across source restart/stop and HDR output format changes without physical emitter writes or display-mode changes. `--gpu-test` uses WARP; `--smoke-test` uses the actual GPU and writes an image of the application's own UI. These checks do not establish optical performance.

Original application code is provided under GPL-3.0-or-later; preserve the upstream notices and licenses. Proprietary firmware is not bundled.


### Prepare a game before launch

In the left **Input/Games** tab, choose the game's EXE and click **Prepare game**. The app checks the current display/emitter prerequisites and connects the existing supported capture adapter if needed. Initial adapter connection requires the game to be closed. Existing classic 3Dmigoto installations still require their NVIDIA stereo rendering backend; preparing capture does not provide that backend.

Launch the game normally through its launcher. While prepared, the app watches the exact executable path every 100 ms, including while minimized or on Tuning. It attaches the stereo reader when the matching process publishes its provider, without requiring a game window first. Once a complete pair and a foreground game window exist, the app starts its frame-sequential output automatically. Use windowed/borderless mode on the selected output display. Detection is automatic, not a guarantee of zero startup latency.

The status distinguishes waiting for launch, waiting for the provider, connected/waiting for eyes, and active 3D. When the game process exits, the app remains prepared for its next launch. **Cancel preparation / stop game 3D**, Stop capture, switching sources/games, or Escape from output cancels preparation. A provider/startup failure is shown as an error, not reported as active 3D. Preparation does not repair a renderer that crashes before providing usable eyes.
