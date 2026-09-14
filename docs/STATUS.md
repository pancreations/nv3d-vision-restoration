# Implementation and validation status

Recorded 2026-09-08 on this workspace PC; updated 2026-09-12.

**Hardware change 2026-09-12:** the original NVIDIA emitter and glasses have arrived; the RP2040 board has not. The original NVIDIA backend is the active path. Plugged in with no driver, Windows shows `USB\VID_0955&PID_7003` "NVIDIA stereo controller", problem code 28. libusb (and therefore the app's probe) does not list a driverless device. The app now accepts `0955:7003` as the boot-state identity; runtime commands still require the `0955:0007` endpoint layout after firmware upload. Blockers before the first optical test: bind WinUSB to the emitter with Zadig (not installed on this PC), and obtain `nvstusb.sys` for RAM firmware extraction. Done 2026-09-12: the user supplied the standalone NV3DVisionUSB package (driver 6.14.13.9041, nvstusb64.sys); `vision_firmware.exe` extracted 7026 bytes, identical from the 32- and 64-bit .sys, to `STUFF/emitter.fw` (sha256 7348cfd7...). The NVIDIA driver was NOT installed and will not be; the app uploads this RAM image over WinUSB itself, independent of the GPU driver. Rebuilt 2026-09-12; all four CTest suites pass. **USB milestone reached 2026-09-12:** WinUSB bound to 7003 and 0007 via Zadig; `--emitter-check STUFF/emitter.fw` uploaded the RAM image; an elevated `pnputil /restart-device` re-enumerated the emitter as 0955:0007; `--emitter-check` then reported State Ready with the EP1/EP2 layout claimed (`reports/emitter-check.txt`). `--emitter-check --emitter-pulse` then drove 1200 alternating eye commands at 120 Hz with 0 errors and the user saw the glasses shutter. `--live 15` (headless fullscreen presenter on the G80SD, SDR) reached 119.993 Hz measured, 1728 presents, 1 miss, timing locked, 1718 emitter commands, 0 errors; `--live 10 --hdr` (FP16 scRGB surface) likewise locked at 119.996 Hz with 1120 commands. Earlier 60 Hz results were caused by a RivaTuner Statistics Server 60 fps cap on this PC, not the app; keep RTSS off or exclude VisionRestoration.exe. Swap chain now uses 3 buffers. The GUI auto-connects a runtime NVIDIA emitter at startup and defaults Preview off when one is present. Optical quality (eye mapping, phase, ghosting) is the next step and is judged by the user. Hisense was not connected during this session (G80SD and a Dell S3220DGF were).

**Hardware change:** glasses received without the original emitter. RP2040-Zero, Adafruit IR module and STEMMA cable are ordered. The original protocol/scheduler, Windows RP2040 backend, predictive presenter integration and ARM PIO/USB firmware are now built. Hardware behavior is untested. Follow the [firmware build/arrival guide](../firmware/rp2040/README.md); original-emitter steps below are historical/optional.

## 2026-09-14: black flashing with a game as the capture source

- The user reported the glasses going fully black for about three seconds at a time, in fullscreen and in the live preview, only with a captured game (Psychonauts 2, 4K) as the input; an emulated N64 game earlier had been fine. session.log for the run (240 Hz HDR, Left/Black/Right/Black) shows why: slips at 14 to 23 per second (trigger lead below zero, present interval jitter 0.8 to 1.5 ms while the game rendered), and the four-slips-per-second rule from the evening build resynchronizing 3 to 6 times a second (463 resyncs over 220 s; composition-mode changes stayed at 4 the whole run). Each resync blanked 16 frames and suspended the emitter, so the output was black most of the time in bursts. The game hook was not involved (hooked=0).
- Fix in src/renderer.cpp: slips and composition-mode changes never resynchronize any more; they are counted (misses, slipsPerSec in the log and the Live status line). Full resync remains for disjoint statistics, occlusion, resize, emitter loss and cadence changes. The process now requests the HIGH GPU scheduling class (D3DKMTSetProcessSchedulingPriorityClass, ABOVE_NORMAL if refused, logged as gpuPriority) and the presenter device GPU thread priority 7, so the presenter's draw is scheduled ahead of the game's; the present thread caches the opened shared source textures instead of reopening one per pair. Details in [PRESENTATION-SYNC.md](PRESENTATION-SYNC.md).
- Not yet verified with the user: whether the slip rate itself drops with the higher scheduling class. Remaining slips show one refresh with the wrong eye (brief ghosting), never black. The user's running instance was renamed so the new exe could link; restart with Launch.cmd.

## 2026-09-13 (night, last): first confirmed clean whole-screen stereo

The user viewed the built-in output through the glasses and reported it "perfect" on the
Samsung OLED. This is the project's first optical confirmation; every earlier result was a
software or host-timing check only. Configuration, from `reports/session.log` and the
autosaved profile at that moment:

| Setting | Value |
|---|---|
| Display | Samsung G80SD OLED, 3840x2160 at 239.991 Hz, HDR on |
| Sequence | Left/Black/Right/Black (software black frame insertion) |
| Stereo area | whole screen, 1.0 at centre 0.5, no band |
| Shutter | 4980.1 us, at the widened ceiling; phase 2380.4 us |
| Image brightness | 3.05x |
| Session health | 13562 emitter commands, 0 USB errors, 1 late; vblank jitter 2.7 us rms, 8.5 us max; 3 misses, 2 resyncs, direct flip |

What this establishes: the whole screen, not a band, is clean at 4K 240 Hz on this OLED
with black frame insertion and the widened shutter, and the brightness is acceptable with
the HDR gain. Two changes were jointly necessary: the black-frame sequence for separation
and the two-refresh shutter ceiling for the light.

What it does not establish: Left/Left/Right/Right, any 120 Hz mode, the Hisense, stereo in
games through the hook, and the ten-minute stability run are all still unverified. At
120 Hz this sequence flashes each eye 30 times a second and flickers; 240 Hz gives 60, the
rate 3D Vision gave at 120 Hz.

The confirmed values live in `profiles/autosave.ini`, which is rewritten on every settings
change, so they are recorded above as well.

## 2026-09-13 (night, later): the light a black-frame sequence throws away

- The user confirmed software black frame insertion removes the crosstalk ("perfect") but
  reported the image far too dark. Part of that is inherent: each row is lit with its own
  eye for one refresh in four. The larger part was a defect. `nvidiaMaxShutterUs`,
  `validate`, `clampTiming` and the shutter slider all bounded the shutter by one display
  refresh, while `emitter.cpp` already runs the emitter at half rate for a four-slot
  sequence, giving it a **two-refresh period** the shutter may use in full.
- Geometry: a row shows its eye from the moment the scan reaches it until the next scan a
  refresh later, and the other eye cannot appear for two refreshes, so a shutter about
  `period + scan` wide, phased at the scan start, collects every row's whole lit period
  with no leakage at all.
- Fixed: `sequenceEmitterHz` now drives every shutter limit; `brightestShutterUs` searches
  for the widest shutter whose best phase stays within the leakage limit, ranked by the
  light actually collected (`brightness x shutter`) rather than the lit fraction of the
  shutter, which falls as the shutter widens and would have argued for the dimmer setting.
  New **Maximize brightness** button, an **Image brightness** gain (1x to 8x, profile
  version 8, applied to the app's own output only, never to a black frame), and the stereo
  area now reports light reaching the eye as a percentage of a fully lit frame.

| Configuration (whole screen, model) | Old cap | New cap | Light before | Light after | Leakage |
|---|---|---|---|---|---|
| OLED 4K 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 51 % | 96 % | 0 % |
| OLED 4K 240 Hz Left/Black/Right/Black | 813 us | 4980 us | 19 % | 84 % | 0 % |
| OLED 4K 240 Hz Left/Left/Right/Right | 813 us | 4980 us | 19 % | 118 % | 0.6 % |
| Hisense 144 Hz Left/Black/Right/Black | 3592 us | 10499 us | 46 % | 95 % | 0 % |
| Hisense 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 55 % | 98 % | 0 % |

- Checks: core suite 37957 checks (18 new, covering the emitter period, the widened shutter
  through a profile round trip, the collected-light ranking and the gain), all four CTest
  suites, and `--gpu-test` in SDR and HDR including a new check that the gain brightens an
  eye frame and can never lift a black frame. Percentages above 100 are real for
  Left/Left/Right/Right, where a row is lit for two refreshes. Confirmed through the
  glasses the same night; see the entry below.

## 2026-09-13 (night): refresh clock, jitter, illumination model, strobe calibration

- Presenter and game host now predict the vblank from a least-squares line through the
  last 240 presentation timestamps instead of the newest one; a standalone probe
  (`experiments/vblank-probe`) showed those timestamps fit a line to 1 to 6 us rms on the
  Hisense at 144 Hz while the kernel scan-line estimate scatters 22 to 27 us. The probe is
  built into the app: **Diagnostics > Measure vblank** and `--vblank` (runs beside an open
  instance) report the blanking length (462 us), where the timestamp sits in it, and the
  scan start offset, which is stored in the profile and used by Suggest phase.
- Diagnostics and the session log carry vblank jitter (rms/max), present-interval jitter,
  the eye command timing error (write start minus deadline), the mean USB transfer and the
  count of timing-block writes; the timing tab shows the boundary and X the emitter receives.
- Core: numeric illumination model (`estimateLeakage`, `bestModelPhaseUs`,
  `bandHeightForLeakage`) that follows every row through the cycle for sample-and-hold or
  strobed panels and any sequence; Settings gain illumination, strobe start/length and scan
  start (profile version 7). For the whole screen at 4K 120 Hz Left/Right it finds about
  7 % leakage in the top and bottom thirds at best, which is the top/middle/bottom symptom;
  Left/Black/Right/Black at 240 Hz is clean for a 4 ms shutter.
- UI: phase sweep with marks (S / M / Enter in fullscreen, phase drawn in both eyes),
  software black frame insertion checkbox and B key, Illumination and strobe inputs,
  "Set strobe from the last two marks", predicted top/center/bottom leakage for the current
  phase, model-driven Suggest phase and Fit area, per-eye timing and highlight controls
  disabled with a reason when they cannot act, loaded profiles now restore the stereo area
  and panel fields. Details: [TIMING-CALIBRATION.md](TIMING-CALIBRATION.md).
- Game hook: the shared-memory contract carries the sequence and a `HookSequences` flag;
  the host applies four-slot sequences only to a hook that advertises them and otherwise
  drives Left/Right and says so. The hook source itself was being edited by another
  session and was left unchanged; four-slot presenting in the hook is still to do.
- Checks: core suite 37939 checks, all four CTest suites, `--gpu-test` (now also checks the
  phase readout: identical in both eyes, value-dependent, absent from black frames) and
  `--vblank` pass. `--smoke-test` could not run while the user's instance was open. No
  optical result is claimed; the model's predictions and the sweep are for the user to
  verify through the glasses.

## 2026-09-13 (evening): ride through slips instead of restarting

- The user saw the fullscreen image jump by itself on the Hisense at 144 Hz. session.log showed the presenter resynchronizing every few seconds: each frame that landed one refresh late (maxIntervalMs about two periods) was treated as a miss, followed by 16 black frames, emitter suspend and a firmware re-lock. The presenter now counts such slips and continues; the refresh-anchored eye sequence and the updated present-to-refresh mapping make the next frame correct. Four slips within a second, a composition-mode change or disjoint statistics still resynchronize. Details in [PRESENTATION-SYNC.md](PRESENTATION-SYNC.md). Why the occasional two-period frame interval occurs in fullscreen is not yet known.

## 2026-09-13 (later): fullscreen on 120 Hz panels, stereo area

- Fullscreen failed to fuse on both the Hisense and the G80SD while the embedded preview fused. The session log shows fullscreen locked in direct flip at the measured rate with single-digit misses and no USB errors, so this is not the presenter or the emitter: a whole 4K 120 Hz frame is scanned over ~8.1 ms (signal timing) and the panel needs 0.2 ms (OLED) to 3-5 ms (Hisense VA) to settle, which leaves no time when every row shows one eye. The preview covered about a quarter of the height. Analysis and routes: [FULLSCREEN-120HZ.md](FULLSCREEN-120HZ.md).
- Added the stereo area (Settings.bandHeight/bandCenter, profile version 6 fields band/band_center/panel_response/panel_scan): the presenter shader and the game hook blit scale the image into a band and keep everything outside exactly black; shared memory carries the band (former reserved fields, zero = whole output for older hosts). Live tab sliders, fullscreen keys PageUp/PageDown/Home/End, Fit area to shutter and Suggest phase from the settled-window model (core: settledWindow, bandHeightForShutter, suggestedPhaseUs). Displays now report active/total lines and the derived scan time; Advanced > Panel takes the response time and a measured scan override.
- Checks: core tests cover the window geometry, fit, phase suggestion, profile round trip and validation; `--gpu-test` (now runnable while the app is open) verifies both eye frames are exactly black outside a 50 % band in SDR and HDR. All four CTest suites pass. The hook build was copied to the test Dolphin folder. Optical results with the band, at 240 Hz Left/Left/Right/Right, or with a large-vertical-total mode are not yet verified.

## 2026-09-13: readable text in the stereo scene

- The stereo scene previously drew a different word in each eye (LEFT / RIGHT) as a screen-space overlay, which cannot fuse through the glasses. A later source edit (identical STEREO text at screen depth) had not been compiled, so the running exe still showed the per-eye words.
- The scene words are now geometry: NEAR (red, z=3, in front of the screen plane), SCREEN (white, z=4, zero disparity) and FAR (blue, z=6.5, behind) lie on camera-facing planes and are ray-traced through the same stereo camera as the spheres and floor, so camera separation and convergence move them exactly like the shapes. LEFT/RIGHT words remain only in the identification patterns.
- `--gpu-test` now reads both real eye frames of the scene at separation 0 and 0.12 and convergence -0.05/0/+0.05 and requires every word in both eyes with crossed, zero and uncrossed disparity respectively; SDR and HDR pass. Full build and all four CTest suites pass. Optical readability through the glasses is still judged by the user.

## 2026-09-12 (later): live calibration and the in-game hook

- Presenter: predictive emitter triggering for the NVIDIA backend (target = predicted vblank of the refresh the present lands on + host-side phase); the emitter's own delay register stays 0 because rewriting its timing block on every adjustment resets its timers (visible glitch). High-resolution waitable timer in the USB worker (Sleep(1) overshoot had dropped ~30% of triggers). Eye sequence anchored to the DXGI refresh counter, so a missed present repeats one eye instead of desynchronizing; misses no longer blank the screen. Phase/duration/swap/pattern changes apply live without a resync; only cadence/surface changes resync. DWM composition is read from `GetFrameStatisticsMedia` every frame and adds one refresh to eye selection and prediction (focus changes had shifted the eye by a whole refresh).
- Keys: Up/Down phase, Left/Right shutter, Shift = 10 us, X swap, Space pause. Profiles panel: list/load/save/export/import/reset; newest matching profile auto-loads. Tuned profile: `profiles/ODYSSSEY.ini` (HDR, phase 3600 us, shutter 1800 us, eyes swapped).
- Game hook (`adapters/vision_hook.cpp`, `src/gamesync.cpp`, `src/sync_protocol.h`): ReShade add-on that double-presents a side-by-side frame as L then R on the game's swap chain (sync interval forced to 1) and publishes present ids and frame statistics over shared memory; the app's GameSync thread drives the emitter from them with the same refresh-anchored, predictive logic. Installed into a Dolphin folder (ReShade 6.8.0 add-on build as dxgi.dll). Requires the game's D3D11 backend. First Dolphin run pending.
- RivaTuner Statistics Server must stay off or exclude the app: its 60 fps cap silently halves the presenter.

## RP2040 preparation checks

- Release build passes all four CTest suites: existing core, GPU texture integration, RP2040 protocol/scheduler, and RP2040 host/waveform tests.
- New suite: 312399 assertions across packet parsing, command acknowledgments, stale sessions, stop/disconnect, scheduling faults, clock uncertainty/drift and 72000 alternating frame windows representing ten minutes at 120 Hz.
- Host/waveform suite: 76 checks, including an injected transport connecting the real host client to the device endpoint and instruction-level PIO simulation for all four tokens.
- ARM GCC 14.3.1 / Pico SDK 2.2.0 release build produces `build/rp2040/VisionEmitter.uf2`. Artifact verification passes UF2 payload/header/family checks, boot2 CRC and Cortex-M0+ vectors. The latest hash is in `reports/rp2040-firmware-artifact.txt`.
- Native UI smoke test passed after RP2040 integration; screenshot inspected. USB probe enumerated no RP2040 firmware device, as expected before delivery. No app remains running from these tests.
- Profiles now include emitter serial identity and firmware source fingerprint, with separate SDR/HDR keys. Older v1 profiles remain readable and must be revalidated.
- This is logical-time simulation with injected USB delays, not a real USB or display timing test. No firmware was flashed, driver binding changed or optical profile validated.
- Reproduce with `tools/Test-Rp2040.ps1`. Report: `reports/rp2040-simulation.txt`. Wire format and remaining firmware work: [RP2040-PROTOCOL.md](RP2040-PROTOCOL.md).

## Software checks completed

- Release build: application, core tests, GPU integration tests, firmware extractor and ReShade/Spout add-on compile successfully with VS 2022 / Windows SDK 10.0.26100.
- Core suite: **331 checks passed**, covering sequences, eye swap, timing packets, phase arithmetic, profile replacement/round-trip, emitter/firmware/HDR profile separation, firmware record validation and presentation-clock discontinuities.
- GPU integration: local Spout sender/receiver, resize, source loss after draining in-flight frames, held-pair immutability, keyed-mutex access, FP16 value 4.0 preservation and simulated emitter trigger/shutdown passed.
- Offscreen WARP rendering: eight SDR/HDR pattern combinations passed. HDR ramp contains values above 1.0; black-frame RGB is zero in SDR and HDR.
- Native UI startup/render/shutdown passed on the installed GPU. Inspected the generated UI and stereo depth scene images; corrected high-DPI sizing and kept launch/stop controls visible outside the scrolling page.
- Live discovery reports RTX 5070 Ti, Odyssey G80SD at 3840×2160 120 Hz and HISENSE-TV at 3840×2160 143.988 Hz, HDR enabled on both. No display modes were changed during these checks.
- No NVIDIA USB device was enumerated at the last probe.

Evidence: `build/Testing/Temporary/LastTest.log`, `reports/gpu-test.txt`, `reports/ui-smoke.txt`, `reports/ui-preview.png`, `reports/depth-preview.png`, `reports/hardware.txt`.

## Milestones

| Milestone | Current state |
|---|---|
| 1 — Calibration foundation | Implemented; core/rendering/UI software checks passed. Full interactive wizard walkthrough and display transition matrix still pending. |
| 2 — Real emitter | Original NVIDIA emitter: USB binding, RAM firmware load, re-enumeration and runtime claim verified 2026-09-12. Shutter activity pending. RP2040 path built, hardware not arrived. |
| 3 — G80SD 120 Hz | Live mode detected; optical calibration and ten-minute stability test pending at 120 Hz. The confirmed clean configuration on this display is 240 Hz (milestone 6). |
| 4 — Hisense 120 Hz | Display detected at 143.988 Hz; explicit 120 Hz selection and independent optical calibration pending. |
| 5 — Live inputs | In-game hook (ReShade add-on + shared-memory sync) built and installed for Dolphin; first run pending. Capture/Spout paths kept but not the intended game route. |
| 6 — Higher refresh | G80SD at 4K 240 Hz with Left/Black/Right/Black confirmed clean over the whole screen by the user, 2026-09-13. Left/Left/Right/Right and 144 Hz physical results still unverified. |

## Next session with the emitter

1. Refresh discovery and record VID/PID, interfaces, endpoints and current driver binding.
2. Establish WinUSB access; initialize volatile firmware only if runtime endpoints are absent.
3. Verify repeatable connect/stop/disconnect before optical tuning.
4. At G80SD fixed 120 Hz, confirm visible shutters, eye mapping, and whether retrospective presentation feedback leaves a usable opening interval.
5. Calibrate phase/duration in SDR, then HDR; examine top/center/bottom and motion. Record failures and ghosting honestly.
6. Run the ten-minute stability test, then repeat independently on the Hisense at 120 Hz.

This is a working software prototype, not completed hardware restoration. Clean stereo and broad game compatibility cannot be established by the software checks above.
