# Usage

Windows frame-sequential stereo output for NVIDIA 3D Vision glasses. The original
USB emitter is driven through WinUSB/libusb and its RAM firmware. Clean stereo over the
whole screen was confirmed through the glasses on 2026-09-13, on a Samsung OLED at
4K 240 Hz in HDR with software black frame insertion. Other displays, refresh rates and
sequences need separate optical validation. RP2040 OLED operation at 120 Hz and
240 Hz with BFI has also been user-confirmed; see [RP2040 setup](RP2040.md).

> **Game integration:** the shared Geo11 hook and game-depth controls are available
> under **Input/Games**. Isolated renderer/transport tests pass; broad game and
> optical validation remains incomplete. Resident Evil 2 and Metal Gear Solid V
> retain unresolved failures. Earlier game trials are historical; see
> [current status](STATUS.md). Earlier successful window-capture trials used
> Windows Graphics Capture; they do not establish working DXGI game capture.
> The old depth-based ReShade hook remains nonworking.

Double-click **Launch.cmd** to run `build/bin/Release/VisionRestoration.exe`.
After replacing the executable, `VisionRestoration.exe --restart` asks the
running instance to save and close normally, then opens the replacement. It
waits up to 30 seconds and never force-terminates the old instance.

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
Stopping the renderer continues servicing synchronous Windows messages to avoid
blocking DXGI while it waits for the control window.
AI desktop capture requests Windows capture exclusion for the app's preview,
fullscreen output and controls. That request alone does not verify the captured
pixels or establish working capture beneath an output overlay.

## Output

- **Start 3D preview** presents alternating eye frames beside the controls and
  drives the emitter. The preview stays visible as you change tabs and settings.
- **Windowed preview** presents the same 3D output in a separate normal window.
  Drag its title bar to move it, resize its borders, or minimize/maximize it.
  The controls remain available in the main app. **F11** enters fullscreen and
  returns to the previous window size and position; **Esc** or the close button
  stops the preview. Keep it on the selected output display for calibrated timing.
- **Fullscreen 3D** opens the same stereo output fullscreen. **Stop** or **Esc**
  returns to the controls. **Fullscreen 3D / F11** starts stereo even after
  **Inspect eye images**; it does not retain the emitter-free inspection mode.
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

Starting a preview, eye inspection or fullscreen output also starts the selected
window source if capture is stopped. Choose the source window and packing first;
there is no separate **Start source** step required for window capture.

Window capture offers two backends. The default, Windows Graphics Capture, uses a two-buffer
pool and selects the newest available frame. DXGI Desktop Duplication captures
a display and crops it to the window; it does not provide an isolated window
image. See [Advanced capture method](#advanced-capture-method) for the current
limitations and validation status. For compatible stereo providers,
**Connect game** and the direct game source receive shared game textures without
either desktop capture backend.

Output uses a waitable `FLIP_DISCARD` swap chain: eye inspection queues at most
three presents, while frame-sequential stereo reserves about 25 ms, scaled to
the display refresh rate and clamped to three through eight queued presents
(six at 240 Hz) for either emitter. The buffer count is the queue depth plus
one (minimum four). Flip model permits independent
flip but does not force DWM bypass; window style, overlapping windows, display
configuration and driver support affect eligibility. Fullscreen input
passthrough currently uses a layered overlay. Check the presentation mode in the
session diagnostics or PresentMon on the actual system before assuming hardware
independent flip. Queue depth alone does not establish measured
capture-to-display latency.
**Profiles** saves named setups; timing and scene changes also autosave.
**Diagnostics** reports presentation/USB timing and runs checks of the rendered
pixels. **Advanced** holds per-eye timing, sequences, emitter connection and modes.

The software checks verify separate eye frames and black frames in SDR and HDR.
They do not establish that all visible leakage through a glasses lens is gone.
See the [emitter timing fixes](EMITTER-TIMING-FIX.md) and
[presentation synchronization](PRESENTATION-SYNC.md) for implementation details.

## Half and Full SBS inputs

**Stereo image**, **Capture window**, and **VLC stereo movie** all offer **Half SBS** and **Full SBS** in **Packing**. Half SBS restores horizontally squeezed eyes: a combined 1920 × 1080 frame displays each eye at 16:9. Full SBS uses each eye's native proportions: a combined 3840 × 1080 frame also displays each eye at 16:9. Select the format that produced the content; resolution alone cannot identify it.

SBS output preserves those proportions in previews, fullscreen, eye inspection and the 2D stream view, adding black bars when the output shape differs. Window capture uses the client picture without the title bar or borders. After changing packing, click **Start source** again. **Top / bottom** remains available with its existing output-area scaling.

## VLC stereo movies

1. Install [64-bit VLC 3.x](https://www.videolan.org/vlc/). The application loads its optional library; it does not modify VLC settings or require the legacy NVIDIA stereo driver.
2. Under **Input**, select **VLC stereo movie** and choose **Half SBS**, **Full SBS** or **Top / bottom** to match the file. **Choose movie...** opens a local video file. Half and Full SBS use the same eye split with different width restoration, preserving the intended eye aspect ratio.
3. Click **Start source**, then start your calibrated **3D preview** or fullscreen output. Use **Swap eyes** for right-first movies. Packing changes stop the source; start it again with the new setting.
4. **Pause movie / Resume movie** controls both VLC video and audio while the output retains a complete stereo pair. **Movie position** seeks when supported; **Movie volume** adjusts sound. **Stop movie** stops decoding. At end-of-file the last complete pair remains visible; **Start source** starts the movie again.

Installed VLC is detected in its standard Program Files location. A complete `vlc` folder beside the application also works, or **Choose VLC...** selects a portable installation's `vlc.exe`. Keep `libvlc.dll`, `libvlccore.dll` and `plugins` together. A 32-bit or VLC 4.x runtime is rejected with an error; other inputs continue to work without VLC installed.

The integration uses the documented [VLC 3 video callbacks](https://videolan.videolan.me/vlc-3.0/group__libvlc__media__player.html) to receive video pixels at VLC's presentation time, then publishes complete GPU-shared pairs to the existing output. Audio remains with VLC. The VLC window and controls are not part of the movie pixels. CPU decoding and copies can limit high-resolution playback; this first implementation provides SDR pixels and does not claim faithful HDR movie output. External subtitle composition, disc/MVC decoding, other stereo formats and movie playback through the glasses remain unverified. Ordinary 2D video requires the separate AI desktop conversion while playing in VLC.

## Screen sharing and recording (2D stream view)

Frame-sequential 3D cannot be captured. The output alternates left and right once
per refresh, and a black-frame sequence adds dark refreshes between them; a
recorder sampling that at 30 or 60 frames a second lands on a different eye each
time. That is what a viewer sees as the picture jumping sideways and flashing.
Only the glasses resolve the sequence, because only they are synchronized to it.
No encoder setting fixes this.

**2D stream view is disabled for Capture window with DXGI Desktop Duplication.**
The visible stream window could be captured as part of the desktop and feed its
image back into the source. Select **Windows Graphics Capture** to use the
stream view with a captured window.

The recorder gets its own picture. Under **Input**, tick **2D stream view**.
The presenter then draws one eye, at up to 60 pictures a second, into a second
window, without the stereo band, the eye shift, the crosstalk subtraction or the
brightness gain - those exist to make an alternating image look right through the
glasses and look wrong on a flat capture. Two options sit beside it:

- **Cover the display** (default): the stream view covers the output display
  underneath the 3D output, which requests Windows capture exclusion. Share **the screen**
  in Discord or OBS. The mirror stays between the stereo output and the game
  when the game gains focus. A source application may stop presenting its own
  window while covered; continued source updates need checking with that application.
- **Cover the display** off: the stream view is an ordinary window you can move
  and resize. Share the window named **Vision Restoration stream view (2D)**.

Built 2026-09-22 and not yet watched from the receiving end of a call. Whether
capture exclusion and covered-window updates work together in a screen share
has not been confirmed here. If a screen share
comes out frozen or black, untick **Cover the display** and share the stream
view window instead.

**Right eye** streams the other eye instead. The glasses keep the full stereo
sequence in every case; nothing about the calibration changes.

The stream view is drawn by the presenter, from the stereo pair it is already
holding, when the stream swap chain has room. Its Present call requests
`DXGI_PRESENT_DO_NOT_WAIT`; a busy stream frame is skipped instead of waiting
for the compositor. Drawing and resizing still cost GPU time. If it fails it
turns itself off and reports why; the 3D output continues.

Window-captured games now use the click-through, nonactivating game overlay in
fullscreen, including after F11 returns from the embedded preview. Earlier
builds only used that path for direct-eye providers, so captured games could
fight an activating fullscreen window and bypass overlay compatibility.
The captured window can be on another monitor; fullscreen covers the selected
output display regardless of the captured window's position or size.
The session log records `passthrough` and `gameOverlay` to identify that path.
Switching focus to Discord, another monitor or another application leaves both
output windows visible. Minimizing the game holds the last captured pair and
output position; it does not hide the stereo or stream window. Stop output with
the existing Stop control or shortcut.

For repeatable diagnosis of an already running SBS game, start the app with
`--capture-pid <game PID> --start-fullscreen`. With Windows Graphics Capture
selected, add `--stream-view` to open the 2D stream view, and `--stream-window`
for a separate stream window. These options do not launch or modify the game.

Whole-screen AI depth conversion requests capture exclusion for its output so
a screen recorder can receive the underlying 2D desktop. Verify the recorder's
image before relying on that exclusion.

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
  the desktop, and Windows draws the cursor above it at screen depth. The app requests
  Windows capture exclusion for the overlay to prevent feedback. **Ctrl+Alt+F8** stops it; **Ctrl+Alt+PageUp /
  PageDown** change the depth strength, **Ctrl+Alt+Home / End** move the screen plane, and
  **Ctrl+Alt+Insert** switches between 2D and 3D while the overlay is up. The controls under
  **Sources & games** stay clickable through the overlay.
- **Start source** alone feeds the conversion to the 3D preview. **Fullscreen / F11**
  uses the same click-through desktop overlay as **Start screen 3D**, including after
  returning from the windowed preview or selecting AI desktop in the fullscreen menu.
  **Home** opens the controls; hide them again to use the desktop underneath.
- Fullscreen output is fully opaque. The earlier translucent compatibility
  setting has been removed: it exposed the game underneath black slots and added
  compositor work. Mouse, keyboard and controller input still go to the game.
- Fullscreen output repairs its window order when a foreground app overlaps it
  or another fullscreen window covers it, without taking focus or repeatedly raising the
  controls. This applies to AI desktop, game overlays and other fullscreen output.
  Repeated game activation does not stop output. The overlay stays on the
  selected display when the game moves, loses focus, minimizes or closes.
  A separate overlay cannot guarantee display ownership over true exclusive
  fullscreen; use borderless/windowed mode. Window-order recovery does not fix
  missed GPU presentation deadlines. `reports/session.log` distinguishes
  `windowOrderRepairs`/`occluded` from timing `misses`/`blackOnImage`;
  `tools/Test-OverlayFocus.ps1` records actual foreground/overlapping HWNDs for
  30 seconds without moving windows or collecting their titles or contents.
  For true exclusive fullscreen, use borderless/windowed mode so that Windows
  can compose the overlay ([Microsoft's explanation](https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations/)).
- Timing warnings do not switch stereo to 2D or stop output. The user decides
  when to pause, stop or disable 3D. The overlay remains on top and passes input
  through when focus changes or the presenter repairs a missed deadline.
- Stereo presentation reserves about 25 ms of queued frames, scaled to the
  display refresh rate. At 240 Hz it queues six frames, with matching shutter
  commands, to absorb brief presentation stalls that exhausted the old
  three-frame queue. Capture rate and saved calibration remain unchanged.
- For **60 stereo frames per second**, use **240 Hz with Left / Black / Right / Black**
  or **120 Hz with Left / Right**. Desktop conversion follows that stereo frame rate;
  AI depth updates run independently and can be slower without limiting desktop motion
  to the depth update rate. Actual performance depends on GPU load and capture delivery.
- **Responsive desktop** selects Depth Anything V2 Small, a 518 px input, balanced
  temporal smoothing (0.5, the original default) and a saved limit of 60 depth updates/s.
  The faster update limit keeps temporal smoothing enabled at the original strength;
  the preset's model and input resolution are unchanged. **Actual depth** reports measured
  updates/s and map age separately from inference time and desktop frame rate. The
  depth helper reserves GPU time for stereo output, so Large at Fine resolution can
  update only a few times per second even while desktop rendering stays at 60 fps.
  Start with the responsive preset for desktop interaction; higher detail needs a
  measured comparison of depth update rate and latency. Unchanged content needs no
  new depth maps. `tools/Test-AiDesktopMotion.ps1 -Live` measures both rates in a
  running AI desktop session with temporary animated patches.
- **Depth strength** is the parallax at infinity as a share of the screen width (2 % default).
  **Screen plane** is the nearness that sits on the screen: 1 keeps everything behind the glass,
  lower values bring the nearest content out, limited by **Pop-out limit**. **Depth smoothing**
  suppresses isolated noise and keeps shared shifts in the network's depth scale from moving
  the whole scene. Smoothing follows elapsed time, so slower AI updates keep similar damping;
  sustained motion follows promptly and a scene cut resets the old depth. **Network input** trades speed for detail (518 px is
  about 5 ms per depth map with Small on an RTX 5070 Ti; the map can refresh up to 60 times a second and the
  eyes are redrawn for every new desktop frame). Choose **Fine (924 px)** or
  **Maximum detail (1036 px)** for a denser estimate, at the cost of slower updates.
- **Depth map controls**, available in both **Tuning** and the AI source panel, shape the
  estimated map only when **Enable map adjustments** is checked. It starts off each session, preserving the original normalized map and stereo effect. The original preview uses mid gray for the screen plane. **Show depth map** previews the adjusted
  map: black is far (0), white is near (1), independent of the screen-plane setting.
  **Far point** clips lower values to black; **Near point** clips higher values to white.
  Bringing those levels closer expands depth differences within that range. They are relative
  values, not physical distances. **Map contrast** expands differences around mid gray;
  **Midtone gamma** above 1 moves middle depths nearer, and below 1 moves them farther.
  **Local detail** enhances small variations already present in the estimate; **Detail radius**
  chooses their scale in map pixels. Start with a small amount: strong enhancement can amplify
  noise and halos, and cannot recover geometry the network missed. **Invert depth** swaps near
  and far before levels are applied. The buttons below the sliders offer **Original** (the
  original unadjusted map), **Medium detail** (gentle range expansion and detail enhancement),
  and **High detail** (stronger range expansion and enhancement). **Original** also resets
  map adjustments. Presets leave stereo strength, screen plane, model and resolution unchanged;
  every slider remains adjustable afterward. Adjustments apply
  live when enabled, including on an unchanged desktop. Slider values are saved with the display profile; enabling them requires explicit opt-in each session.
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

## VRto3D SBS / TAB input

Select **Input/Games > Source > VRto3D (SBS / TAB)**, choose its automatically
discovered **Window** (or use **Show all windows**), and match **Packing** to
**Half SBS**, **Full SBS**, **Half TAB (top / bottom)** or **Full TAB (top / bottom)**.
Optionally select the game's separate **Game input** window, click **Start
source**, then use eye inspection, preview or fullscreen output.

Before fullscreen, open VRto3D's **Ctrl+Home > System** menu, turn **Auto Focus
OFF**, and **Save Default Cfg** to release its own foreground lock. Keep the
captured window visible and unminimized behind the output. This source always
uses Windows Graphics Capture and does not focus the captured VRto3D window.
**Launch-VRto3D.cmd** or `VisionRestoration.exe --vrto3d` opens this source panel.
See [setup, packing and validation limits](VRTO3D.md); real VR gameplay and
optical performance remain unverified.

## Wibble Wobble input

Select **Input/Games > Source > Wibble Wobble** to connect to the running
SteamVR session. After the first image arrives, the embedded 3D preview starts
if the emitter is ready and the display rate and timing are compatible. Otherwise,
connect the emitter, match the display rate, then click **Start 3D preview**.
**Inspect eye images** explicitly opens a 2D side-by-side view without emitter
commands. **Fullscreen 3D / F11** starts stereo, and **Reconnect input** preserves
an already running output. While stereo runs, Wibble's competing output window
is moved off-screen and restored on Stop. Choosing that window in **Capture
window** automatically uses the direct eye feed instead of capturing alternating
eye and black frames. If SteamVR
has administrator permissions, **Connect to administrator SteamVR** reopens the
app with matching permissions after Windows UAC approval. Cancelling keeps the
current app running. See [input details and validation limits](WIBBLE-WOBBLE.md).

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

### Advanced capture method

**DXGI game capture was reported nonworking by the user on 2026-10-07.** Earlier
synthetic capture tests and latency reports do not establish that a real game
works beneath the stereo output. A passing fixture or a selected backend name
must not be reported as successful gameplay capture.

The **AI desktop** and **Capture window** panels have independent **Capture
method** selectors. **Capture window** defaults to **Windows Graphics Capture**
to isolate the source from overlapping windows; **AI desktop** defaults to
**DXGI Desktop Duplication**. Changing a source panel's selector restarts its
active capture. The two preferences are also available under **Settings >
Advanced capture** as **AI desktop capture** and **Window capture**; stop capture
before changing them there. Both preferences are saved with the display profile.
Older profiles retain their saved desktop preference and default window capture
to Windows Graphics Capture. **VRto3D** always uses Windows Graphics Capture.
There are no automatic backend fallbacks. **Active capture** shows the selected
backend and any acquisition or waiting status; a backend name alone does not
mean a frame was received. AI desktop always captures the selected monitor,
regardless of the previously selected window.

DXGI captures desktop pixels and crops window sources to their client area.
It cannot recover game pixels outside the captured display or independently
capture a window hidden behind ordinary covering windows. Covering windows
appear in the desktop crop; capture does not freeze based on an inferred list
of covering windows. Pixels outside the capture display are padded black in
the original client-sized image, including portions on another monitor, so the
stereo split does not shift. This implementation requires the capture display to use the
output GPU and an unrotated display. The app requests capture exclusion for its
controls and stereo output, but that does not guarantee usable live game frames
beneath them. A minimized or hidden source retains its last complete stereo
pair, if one was received.
Keep Windows Graphics Capture selected when window isolation is needed.
The 2D stream view is disabled for DXGI window capture to prevent feedback;
choose Windows Graphics Capture to use it.
Color handling follows the actual captured texture format; a display's HDR
setting alone does not establish captured HDR precision. Display changes and
temporary desktop access or session limits retry duplication using the same
backend, with a waiting status. Capture copies use GPU textures; actual latency
and frame rate require measurement on your setup.

Changing one capture preference leaves the other source's preference unchanged.
Stereo packing, AI conversion and direct shared-eye game providers remain
separate controls.

The reproducible capture benchmark is `vision_gpu_capture_bench.exe <report.csv> [idle|load] [waitable] [excluded-overlay]`. It displays a temporary 1280x720 animated window, using HDR when enabled on the display, and compares explicit DXGI and Windows capture through the real stereo-source pipeline. GPU-visible frame numbers tie measurements to a common producer clock. It also measures full-screen conversion with depth inference disabled. `load` adds 4K HDR GPU work. Each scenario runs twice in opposite backend order. The measured delay ends at the completed source copy; it excludes stereo output, emitter timing and the panel. The benchmark does not change display settings or enable fallback capture.

Use `vision_gpu_capture_bench.exe <report.csv> idle waitable excluded-overlay`
to check changing source pixels beneath a temporary full-display D3D overlay
with Windows capture exclusion. Each scenario requires at least ten distinct
source frame markers and verifies overlay presentation. The CSV records
`overlay_mode`, `overlay_presents`, `unique_capture_fps`,
`changing_pixels_verified` and `invalid_markers`. This tests the synthetic
window and desktop-conversion paths; it does not verify live-game capture.

The benchmark reports two separate latency measures: `producer_to_copy_*` includes the producer and desktop queues, while `capture_handling_*` measures from capture API delivery until the GPU-completed source texture is published. Desktop handling includes its conversion cadence. These numbers must not be substituted for each other. Window GPU completion uses a bounded 0.5 ms spin before sleeping to reduce scheduling delays; this trades some CPU time for lower typical latency and does not guarantee a 1 ms deadline. Flat desktop frames are copied directly into both shared eyes; depth reprojection still uses the stereo shader.

For presentation-queue diagnosis, use `vision_gpu_capture_bench.exe <report.csv> idle waitable` (or `load waitable`). This creates the test producer with a frame-latency waitable swap chain, limits its queue to one frame, and waits before rendering each frame. It does not change capture or the presentation settings of other applications. The CSV labels `producer_mode` and reports `producer_to_capture_*`, from marker submission until capture API delivery, separately from capture handling. Compare like producer modes; improving the test producer is not an improvement to the receiver. A locked or secure desktop prevents DXGI measurements; a header-only CSV from a failed run contains no latency result.

Capture startup applies the same process scheduling policy used by stereo output, requesting CPU execution and timer-resolution policies suitable for latency-sensitive work while the app is covered or minimized. This applies across source types; it does not change producer or compositor queues or guarantee a deadline. No capture backend is switched automatically.

Use `vision_direct_capture_bench.exe <report.csv> [load]` to measure an offscreen D3D11 FP16 producer through the actual native stereo source at 1280x720 and 3840x2160 per eye. It measures first-eye render submission through publication of the GPU-completed source snapshot, verifies both eyes and all four corners, and reports frames at or above 1 ms. It excludes desktop composition and physical output and cannot establish latency for arbitrary applications. The load case adds eight 4K HDR clears before each pair. Its short-timer diagnostic and producer/receiver completion columns help investigate long scheduling stalls; neither average nor percentile results guarantee a hard deadline.
