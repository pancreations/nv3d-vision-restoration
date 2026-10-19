# Emitter setup and troubleshooting

This page covers the original NVIDIA 3D Vision USB emitter. For the DIY emitter, see [RP2040](RP2040.md); never use the NVIDIA firmware loader on an RP2040.

## How it works

The emitter has no permanent firmware. Each time it's plugged in, the PC has to load a small firmware file into it. Vision Restoration does this automatically, using `emitter.fw`, which you create once from NVIDIA's own package.

Windows sees the emitter as two devices:

| When | USB ID |
|---|---|
| Just plugged in (no firmware yet) | `0955 7003` |
| Firmware loaded and running | `0955 0007` |

Each needs the WinUSB driver once per PC. After that, unplugging and replugging just works: the app reloads the firmware and carries on without losing your calibration.

## Setup

1. **Firmware:** download NVIDIA's [3D Vision USB controller package](https://us.download.nvidia.com/Windows/Quadro_Certified/NV3DVisionUSB/390.41/3dvisioncontrollerdriver.exe). Don't run it. Drag it onto `Prepare NVIDIA Firmware.cmd` in the app folder, which writes `emitter.fw`. An already extracted `nvstusb*.sys` also works.
2. **Driver:** plug the emitter straight into the PC (no hub) and start the app. Accept the administrator prompt to install WinUSB. When the emitter restarts with its second ID, accept the prompt again if it appears.
3. **Check:** start the 3D preview and look through the glasses. They should shutter, and each eye should see its own image after calibration.

Don't install NVIDIA's 3D Vision driver, and don't change your graphics driver.

## If automatic driver setup fails

Use [Zadig](https://zadig.akeo.ie/):

1. Choose **Options > List All Devices**.
2. Select the NVIDIA device with USB ID `0955 7003`, choose **WinUSB**, and click **Install Driver**.
3. Start the app so it loads the firmware. When the device returns as `0955 0007`, repeat for that ID, then click **Refresh USB** in the app.

Only give WinUSB to those two IDs.

## Troubleshooting

| Problem | Try this |
|---|---|
| Emitter not listed | Plug it directly into the PC, then click **Refresh USB**. |
| "Firmware missing" | Make sure `emitter.fw` is in the app folder (see step 1). |
| Listed but can't connect | Install WinUSB for the ID shown (see above). |
| Glasses shutter but the image is wrong | That's calibration, not the emitter: see the [user guide](USAGE.md#calibrating-your-glasses). |

Command-line extraction for source builders (the output file must not exist yet):

```powershell
.\build\bin\Release\vision_firmware.exe 'path\nvstusb.sys' 'path\emitter.fw'
```

## Technical notes

The rest of this page documents the emitter protocol and firmware behaviour for developers.

Device handling: runtime descriptors must expose bulk OUT endpoints 1 and 2 in alternate setting zero; unknown layouts are rejected. After RAM upload the emitter does not re-enumerate on its own, so the app restarts the `0955:7003` PnP instance (elevated), waits for `0955:0007` and reconnects. The WinUSB packages are generated and signed per PC with [libwdi](https://github.com/pbatard/libwdi) for exactly these two IDs.

## Protocol and scheduling

### What the emitter firmware actually does (disassembled 2026-09-13)

See [the app/emitter timing correction](EMITTER-TIMING-FIX.md) for the corrected timer signs, eye parity and regression evidence.

The RAM firmware (8051 on the Cypress FX2) does not execute eye commands directly. Verified from the disassembly of `STUFF/emitter.fw`:

- Timer 2 is a free-running **period timer** (reload = the Z value, then adapted to the measured interval between host commands). Every period boundary toggles the emitter's own eye bit.
- Each `0xAA` eye command is a **phase-lock nudge**: the 4-byte value is the timer-2 count the host expects at that instant (`period/1.8` remaining). After the first 64 commands the correction is clamped to +/-64 ticks (5.3 us) per command, so phase moves slowly; a large jump takes hundreds of frames. The commanded eye only *checks* the emitter's alternation; it is forced only during acquisition or after 20 consecutive mismatches.
- At every boundary the firmware loads timer 0 with X (the "phase" register, plus a fixed 53-91 us refresh-band correction) and then runs: open token (~250 us) -> Y countdown - 196 us -> close token (~250 us). **If timer 0 is still busy at the next boundary, that whole period is skipped: the other eye gets no open or close token at all.** That makes one lens go dark or ghosted while the other looks fine.
- Consequence for the app (implemented 2026-09-13, profile version 4): the UI phase is now the **window start after the vblank of the commanded frame**, continuous over one period. `nvidiaSchedule()` splits it into the boundary distance sent in every eye command (kept within 1000 us of either end of the period) and the X register (kept within 300-2300 us), so the token sequence can never overrun the period and the whole frame is reachable. Every timing-block write makes the firmware re-acquire its lock (unclamped corrections), so phase changes take effect within a few frames. The shutter maximum is `period - 3300 us - bandCorrection`. Version 3 profiles are converted on load: `new = (period/1.8 - 6 + oldX + bandCorrection) mod period`.
- INT5/INT6 handlers implement the 3-pin DIN external sync path; they are inactive on USB-only use.
- Per-eye timing (2026-09-13): every register write runs firmware routine 0x140D, which resets the phase lock to unclamped acquisition and reloads the period from Z; X/Y take effect at the next boundary. The app uses this for a right-eye offset and independent durations: when either is non-zero it writes X/Y (12-byte write at block offset 4) about 0.6 ms before each eye command, i.e. after the previous boundary and before this eye's. Fixed cost: one extra EP2 transfer per frame and a permanently unclamped lock, which is fine with predicted-vblank command timing.

The internal packet layout follows [libnvstusb](https://github.com/eruffaldi/libnvstusb). Timing configuration goes to endpoint 2, explicit left/right commands to endpoint 1. Initialization, watchdog and enable/disable registers are kept internal. The W timing register retains the upstream constant because its physical meaning is not established. The X/Y register interpretation must be verified on this hardware before treating UI microseconds as calibrated optical values.

A dedicated worker handles USB; bulk transfers have a 20 ms timeout and stop cancellation. Firmware control writes are bounded to 200 ms each and checked between blocks. The presentation thread does not wait for USB. Pending triggers are replaced rather than accumulated; late replacements/rejections are counted. A trigger already handed to USB cannot be retracted atomically with a new presentation or stop request.

At startup, use equal 1500 µs durations and phase zero only as exploratory defaults. They are not claimed to fit either display. The application predicts presentation time and submits the USB eye command at that deadline. Feedback detects missed predictions; USB latency and optical timing remain unmeasured.

Do not install a legacy graphics driver or flash persistent firmware. The loader only supports the upstream volatile RAM record format and CPU reset/release addresses.


## Schedule change 2026-09-14: shortest X first

`nvidiaSchedule` used to hold X at 1300 us and move the boundary distance; X only left its
centre when the boundary would leave [1000, period - 1000]. The window could therefore never be
wider than period - 2300 - band correction - 1000 us guard, which at the 120 Hz emitter rate
of a four-slot sequence is 4980 us, less than a slow panel's refresh plus scan. The schedule
now starts from X = 300 us and moves the boundary; X grows only when the boundary would leave
its range (over about a quarter of the phase circle, by up to 2000 us), and there the shutter
is shortened to fit the period (`nvidiaEffectiveShutterUs`; the Live tab reports it). The
requested window start is unchanged for every phase, so tuned profiles keep their optical
position; only the split between boundary distance and X differs.
