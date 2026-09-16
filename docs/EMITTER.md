# Connecting the actual emitter

**Hardware update 2026-09-15:** both emitters have arrived. The RP2040 is flashed, its approved WinUSB driver is installed, and the app's connection check passes. A ten-second 120 Hz command test recorded 1200 opens and closes; the user subsequently confirmed good OLED 120 Hz stereo. LCD/QLED separation and OLED 240 Hz still need optical testing. See [RP2040 setup and lower-refresh experiments](RP2040.md). The instructions below describe the original NVIDIA emitter; do not use its RAM firmware loader to flash an RP2040.

With no driver, Windows lists the emitter as **`USB\VID_0955&PID_7003`**, bus description "NVIDIA stereo controller", problem code 28. The app uses live Windows PnP discovery to find that driverless boot identity; `0955:0007` is the runtime identity after firmware upload.

1. Connect the USB IR emitter directly to the PC. Open **Glasses check → Refresh USB**. The supported protocol profile recognizes `0955:0007` (runtime) and `0955:7003` (boot state); other NVIDIA devices are listed but receive no writes.
2. This backend uses **WinUSB via libusb**, not NVIDIA's old stereoscopic graphics driver. The application ships an x64 [libwdi](https://github.com/pbatard/libwdi) bootstrap and generates, signs and installs an exact-device WinUSB package for `0955:7003` and `0955:0007` on each target PC. Approve the UAC prompt for each identity the first time it appears. No machine-specific instance, USB port, GPU identity, local path or `oem*.inf` name is copied between PCs.
3. Press **Connect emitter**. Runtime descriptors must expose bulk OUT endpoints 1 and 2 in alternate setting zero. Unknown endpoint layouts are rejected.
4. A boot interface with no endpoints needs matching RAM firmware. The app loads `emitter.fw` from its own folder (or `STUFF\emitter.fw`); create it with `vision_firmware.exe` (below) from a locally obtained NVIDIA 3D Vision USB driver package. Known sources: the standalone [NV3DVisionUSB driver 390.41](https://www.nvidia.com/en-us/drivers/nv3dvisionusb/390_41/nv3dvisionusb-driver/) or a GeForce package up to 425.31 (extract with 7-Zip; the file is in the `NV3DVisionUSB.Driver` folder). Do not run the NVIDIA installer; only the `.sys` file is needed. Extraction validates the PE data section and firmware records; it does not install that package. Proprietary firmware is not distributed here.
5. **Verified 2026-09-12:** after RAM upload the emitter does not re-enumerate on its own. The application finds the active `0955:7003` PnP instance, requests an elevated Windows PnP restart, waits for `0955:0007`, and reconnects automatically. Port moves use live PnP discovery and the portable installer for both identities. An unplug loses RAM firmware and starts the boot state again; the app reloads it without resetting the stereo session or calibration.
6. Start the 120 Hz glasses test. Confirm shutter activity through the actual lenses, then eye order, phase and duration. A successful USB transfer is not a successful optical test.

The application detects and connects the emitter automatically, including after unplug/replug. USB device-change messages no longer close fullscreen stereo or discard calibration. Calibration changes autosave immediately and are not tied to window focus. When `0955:7003` returns on any port, the app installs WinUSB if necessary, reloads the volatile RAM firmware, restarts to `0955:0007`, and resumes shutter commands.

CLI extraction (output must not exist):

```powershell
.\build\bin\Release\vision_firmware.exe 'path\nvstusb.sys' 'path\emitter.fw'
```

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
