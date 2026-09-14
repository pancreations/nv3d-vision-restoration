# NVIDIA 3D Vision Restoration — Implementation Plan

Implementation evidence and remaining hardware gates are tracked in [docs/STATUS.md](docs/STATUS.md). Launch the current prototype using `Launch.cmd`.

**Status:** Implementation in progress. Hardware and game validation remain pending. See README.md and reports/ for verified results.

## 1. Objective and constraints

Create a native Windows application that receives stereoscopic images, presents alternating left/right views, and controls the NVIDIA USB IR emitter without NVIDIA's discontinued stereoscopic graphics driver.

| Hardware | Initial configuration | Later experiments |
|---|---|---|
| NVIDIA 3D Vision 2 glasses and DIY RP2040-Zero IR emitter | Ordered RP2040-Zero, Adafruit IR module and STEMMA cable | See docs/RP2040.md |
| Samsung Odyssey OLED G8 G80SD | 4K, fixed 120 Hz | 144 Hz if available; 240 Hz |
| Hisense 65-inch U6 Pro Mini-LED/QLED | 4K, fixed 120 Hz | 144 Hz |
| Modern Windows PC | Current GPU driver | No graphics-driver downgrade |

**HDR support is required; enabling HDR is optional.** Maintain separate SDR and HDR calibration profiles.

Broad stereo-game compatibility is the goal. Distinguish applications that already produce two viewpoints from applications that relied on NVIDIA's old driver to generate stereo. Output software cannot automatically restore missing stereo rendering.

Test one display at a time. No optical sensors. Updated 2026-09-08: the original emitter was not included; the user authorized a DIY RP2040 emitter using ordered parts. [RP2040 preparation](docs/RP2040.md) supersedes original-emitter assumptions for the primary hardware path. A separate RP2040 backend, predictive presenter integration and UF2 firmware are now built; USB and optical validation await hardware. The original NVIDIA backend remains available.

## 2. Application architecture

Use C++20, CMake, Win32, D3D11/DXGI and a native control interface. Visual Studio 2022 and Windows SDKs are available.

| Component | Responsibility |
|---|---|
| Device discovery | Displays, active signal modes, GPU adapter, HDR capabilities, emitter |
| Emitter backend | USB access, firmware initialization, timing configuration, eye selection, recovery |
| Stereo sources | Synthetic scenes, stereo images, captured windows, shared GPU textures |
| Stereo presenter | Cadence, eye sequencing, HDR presentation, timing feedback |
| Calibration wizard | Guided tests, phase/duration adjustment, profiles |
| Diagnostics | Presentation misses, USB errors, timing distributions, reports |

### Emitter control

- Use libusb with Microsoft WinUSB as the primary transport.
- Inspect actual USB descriptors before selecting a supported protocol profile.
- Adapt established timing and eye-selection commands from libnvstusb and 3DVisionActivator.
- Keep firmware initialization separate from graphics-driver installation. Extract matching firmware from a local NVIDIA package when necessary; never bundle proprietary firmware.
- Use bounded, cancellable transfers and disconnected, initializing, ready, running and error states.
- Verify command-delay semantics before predictive scheduling. Undocumented registers remain internal.

Existing Windows implementations use NVIDIA-specific pipe paths; transport adaptation is required.

References: [libnvstusb](https://github.com/eruffaldi/libnvstusb), [3DVisionActivator](https://github.com/FlintEastwood/3DVisionActivator).

### Presentation and timing

- Fixed-refresh, VSync, borderless fullscreen flip-model presentation; VRR off initially.
- Waitable swap chain, QPC timestamps and DXGI presentation statistics.
- Track intended eye, presentation ID and actual refresh count. Do not derive eye order solely from loop iterations.
- Source acquisition independent of display cadence; repeat complete stereo pairs on stalls.
- Preallocate rendering resources. Keep blocking USB operations, decoding and logging off the presentation thread.
- On timing discontinuity, blank output, reacquire timing and restart with explicit eye order.

No atomic GPU-present-plus-USB API is assumed. Scheduling feedback describes host timing, not emitted light or shutter transparency.

Reference: [DXGI timing](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics).

### HDR

Preserve HDR precision using floating-point textures and configure the presentation color space according to display capabilities. Do not silently pass HDR through SDR or label SDR as native HDR. SDR sources may be displayed within the HDR output space with explicit SDR interpretation.

## 3. Guided calibration and compatibility

The calibration tool is built in and directly launchable. It should resemble a modern stereo setup wizard: clear instructions, large targets, Back/Next navigation and advanced timing controls.

1. **Display and mode:** model, active resolution, actual refresh, HDR and emitter status; recommend supported 4K 120 Hz.
2. **Glasses operation:** neutral image and user confirmation of shutter activity; USB detection is not optical confirmation.
3. **Identify each eye:** distinct left/right shapes and labels; check one lens at a time; swap-eyes control.
4. **Phase:** white/gray-on-black targets at top, center and bottom; coarse/fine controls; evaluate all regions.
5. **Duration:** brightness/ghosting tradeoff, independent eye durations in advanced settings.
6. **Depth and motion:** original scene behind/on/in front of the screen plane, moving objects and readable labels. Camera separation is independent of emitter timing.
7. **Validate/save:** sustained timing test, user visual assessment, named profile and report.

Immediate pause/exit controls. Without an emitter, clearly labeled preview/simulation only; never report successful glasses calibration.

### Profiles

Store display identity, connection, resolution, exact refresh, SDR/HDR, sequence, eye order, phase, duration and validation status. Record monitor settings manually where software cannot read them.

Samsung's stored EDID identifies the G80SD. Published 4K 120 Hz input timing gives an 8.333 ms starting interval, but not panel latency or shutter phase. Prefer live signal information. [Samsung manual](https://device.report/m/ecba48306e2060bdf3558fa456588ef6b593f9d88ce0bfc7a6e82a67f9f0c4fe_optim.pdf)

Calibrate the Hisense separately; compare local dimming settings where available.

| Mode | Sequence | Cadence |
|---|---|---|
| 120 Hz baseline | L R | 60 per eye per second |
| 144 Hz experiment | L R | 72 per eye per second |
| 240 Hz experiment | L Black R Black | 60 per eye per second |
| 240 Hz experiment | L L R R | 60 pairs per second; shutters target settled intervals |

144 Hz glasses operation and optical benefits of 240 Hz remain unverified until tested.

### Input compatibility

Implement in order: synthetic patterns/scene; SBS/top-bottom SDR image files; Windows Graphics Capture for packed stereo windows (Dolphin, Blender, video); Spout GPU sharing and ReShade export; tested geo-11/legacy-game configurations.

Normalize input into complete stereo pairs with dimensions, eye order, color interpretation, timestamp and pair ID. Synchronize GPU resource ownership. Keep game focus and input working while the presenter owns stereo output. Detect closure, resizing and stalls.

Study [WibbleWobble](https://github.com/PHARTGAMES/WibbleWobbleCore), [Open3DOLED](https://github.com/open3doled/open-3d-oled), [Spout2](https://github.com/leadedge/Spout2), and [PresentMon](https://github.com/GameTechDev/PresentMon). Audit licenses before reuse. [geo-11](https://github.com/ThreeDeeJay/geo-11) is an external integration initially.

## 4. Milestones and acceptance tests

### 1 — Foundation without emitter

Application structure, display discovery, synthetic stereo, wizard, simulated emitter, profiles, SDR/HDR render paths. Verify navigation, preview labels, eye swap, sequences, profile round-trip, timing arithmetic and pair ownership.

### 2 — Real emitter

Identify hardware, establish USB access, initialize firmware and verify visible shutter activity. Repeat initialization, shutdown and unplug/replug without hangs. Actionable missing-firmware/unsupported-device messages.

### 3 — G80SD at 120 Hz

Calibrate eye order, phase and duration in SDR/HDR. Assess separation over the screen and run ten minutes without persistent inversion. Record ghosting/brightness limitations; no invented optical score.

### 4 — Hisense at 120 Hz

Independent profiles and visual validation. Determine whether backlight settings affect results.

### 5 — Live stereo sources

Demonstrate Dolphin, Blender, SBS video and one compatible PC game using the same output engine. Complete-pair repetition under changing source rates, resize/loss recovery, working game input.

### 6 — Higher refresh

144 Hz where available, then both 240 Hz sequences on G80SD. Label each configuration tested, experimental or unsuitable. Retain the best validated default.

Across milestones: Alt-Tab, mode/HDR changes, source stalls, GPU device loss, USB disconnects. Hardware acceptance stays pending until glasses and emitter are connected. Deliver a working calibration/output application with documented compatibility, not a promise that every game/display supports clean stereo.
