# Historical RP2040 preload trial ? 2026-09-15

This trial preceded the generic [LCD / VA temporal aperture mode](LCD-CALIBRATION.md). It is not the recommended calibration starting point.

The user confirmed good OLED 120 Hz stereo with firmware 0.2.0. The backup is `profiles/oled120-rp2040-working.ini` and `reports/rp2040-oled120-working.uf2`.

Firmware 0.3.0 (`VRP1/0.3.0/bfc1c785fc97c286`) was flashed successfully. Its clock probe passed 197 of 200 samples after initialization, with no initial fault.

## Observed outcomes

- 120 Hz LCD preload L/L/R/R: the user reported no useful glasses activity, possibly one flicker. The board nevertheless executed 2,238 opens and closes without a reported scheduler fault. This demonstrates that command execution alone does not establish glasses operation.
- Ordinary 120 Hz L/R: the user reported activation, but the bleeding/ghosting remained. There was no verified full-panel improvement.

Reports: `reports/dell120-preload-first-20260915.txt` and `reports/dell120-direct-baseline-20260915.txt`. These names identify the local test hardware; the new calibration implementation does not depend on a monitor model.

The new 0.4.0 implementation keeps ordinary L/R refresh alternation and controls settling, exposure, guard time and per-eye timing within each frame. OLED 120/240 Hz and 240 Hz BFI remain available separately. Optical validation of the new mode is still required.
