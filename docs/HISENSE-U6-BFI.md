# Hisense 65U6SF-PRO: BFI investigation

2026-09-22. **The user reports MAJOR CROSSTALK after the control-fix build.**
The requested working U6 Pro BFI outcome has not been achieved. The user requests 240 Hz
Mini LED/QLED BFI and explicitly rejects the previous video-derived panel readings.
No such readings are inputs to this investigation or the BFI control fixes.

The last saved test-build session reports approximately 239.954 Hz, sequence 1
(L / black / R / black), zero gray reset and zero USB errors. Its final requested
phase is 858.8 us and shutter is 6980.9 us. These are host settings/counters, not
manufacturer panel specifications or measured lens openings. NVIDIA can clamp the
requested shutter depending on phase. This log does not identify the cause of
the crosstalk or prove the TV's internal scan cadence. No replacement timing is
derived from the old recordings or the generic panel-response model.

The user then reports a **dark band** when adjusting phase. This observation does
not establish a panel response time or uniquely distinguish panel scanning,
backlight behavior and shutter timing. Further widening based on the generic
model is not a demonstrated fix.

A separate `Hisense-240Hz-preload-isolation.ini` diagnostic profile is prepared
beside the isolated executable. It uses L/L/R/R, a 500 us manual shutter command
window, 1500 us initial phase, full image area, 1x gain, and no gray reset, black
lift or ghost subtraction. At the existing 239.97 Hz input it submits about 60
new images per eye per second. Those shutter/phase values are search starting
points, not manufacturer panel specifications. This compares preloaded images
against black insertion; it is not BFI or a verified solution. Use nine-row
targets and inspect each lens across the whole screen while sweeping phase.
If a whole-screen visible/clean range exists, record it; a dark image is no pass.

## Manufacturer evidence

The user identifies the TV as **65U6SF-PRO**, 65-inch US Fire TV, Best Buy SKU
**6673625**. The model is now resolved. Do not use specifications from U6SV,
U6Q PRO, U6N, or the non-Pro U6SF.

Reviewed primary manufacturer sources:

- [Hisense's exact model page](https://www.hisense-usa.com/product-page/televisions-65-class-u6-pro-series-uled-miniled-4k-fire-tv-65u6sf-pro).
- [Hisense U6SF-Pro Q&A, 55-85 inches](https://www.hisense-usa.com/_files/ugd/991681_3fc4051d8e234e989c61291101294659.pdf):
  native 144 Hz; the stated Game Mode VRR range is 48-144 Hz. It also documents
  full-array local dimming and two HDMI 2.1 inputs plus two HDMI 2.0 inputs.
- [Official Quick Setup Guide](https://www.hisense-usa.com/_files/ugd/991681_09bdcddff9bb408ea9e18c3e04b2b744.pdf),
  printed page 6 / PDF page 8: HDMI 3 and 4 are labeled 4K at 144 Hz; HDMI 1
  (eARC) and 2 are labeled 4K at 60 Hz. Printed page 12 / PDF page 14 explicitly
  includes 65U6SF Pro in the 3840 x 2160 model specification column.

The matching retailer SKU lists Game Booster 240, but the inspected Hisense
Q&A and setup guide do not supply its resolution/timing table. The app's existing
2560 x 1440 at approximately 240 Hz log is evidence of the host input mode,
not manufacturer documentation of internal panel scanning or pixel transitions.
The 144 Hz native specification does not establish that the accepted 240 Hz input
is dropped or interpolated; no such claim is made.

Neither inspected manufacturer document supplies gray-to-gray transition curves,
black-reset settling, local-dimming latency, or an API to synchronize backlight
strobes with shutter glasses. No numerical panel timing or a working BFI preset
can be derived from these documents. Lack of a documented backlight API is not
proof that a particular TV menu feature is absent.

Local reference copies and rendered pages are in
`reports/hisense-65u6sf-pro-manufacturer/`. The original setup-guide PDF uses
outlined text, so its port diagram and specification table were read visually.
Existing profile response/rise values remain unverified and are not treated as
manufacturer measurements. Saved user profiles are preserved.

## What the software establishes

At 240 input refreshes/s, L / black / R / black submits each eye 60 times/s,
with 4.167 ms per input refresh. This is sequence arithmetic. It does not establish
internal panel scanout, pixel settling, local-dimming response, or lens transparency.

The current changes fix two control-state bugs:

- Every BFI toggle clears gray reset and preserves the selected timing mode and
  phase/shutter values. Neutral reset no longer appears as enabled black insertion.
- The Panel drive experiments explicitly use manual timing, so their shutter and
  phase control the emitter instead of being overridden by a previous guarded
  aperture. Manual phase marks require manual timing and reset on a mode change.

No new Hisense response time, scan time, backlight-control capability or calibrated
BFI preset is claimed. Software build/tests cannot establish optical separation.
Manufacturer refresh and response specifications alone are not a measured
shutter-to-panel timing relationship.

## Build and verification

The isolated build is `build/u6-bfi-control-fix/bin/Release/VisionRestoration.exe`.
The ordinary running app and its saved calibration files are not replaced.

Build targets: `VisionRestoration`, `vision_panel_tests`, `vision_tests`, and
`vision_lcd_tests`. Regression coverage includes gray reset to black insertion,
preserved OLED timing, switching from a long guarded BFI aperture to manual panel
experiments, and optical-mark invalidation on timing-mode changes.

Validation on 2026-09-22: Release build passed; CTest `core`,
`lcd_temporal_aperture`, and `panel_experiments` passed (3/3). The app's
`--smoke-test --panel-smoke` exited successfully with no emitter commands.
No TV/lens result is inferred from those tests.

Selecting **Panel > Black reset L B R B** in the corrected build remains an
experiment, not a verified Hisense setup. Model identification is complete;
clean stereo remains unresolved. The pending optical result is whether the
500 us manual L/L/R/R preload comparison removes the dark band and produces
visible separation across both lenses and all screen rows. Manufacturer refresh
specifications alone cannot answer that question.
