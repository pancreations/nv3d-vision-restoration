# Fullscreen stereo on 120 Hz displays

> Audit correction, 2026-09-14: the numerical panel response/scan values and exclusive
> attribution to the panel below are historical hypotheses, not independently established
> optical measurements. Successful USB transfers and stable DXGI statistics cannot rule
> out lens latency, emitter phase error, panel buffering or camera artifacts. The formula
> is a simplified sample-and-hold bound; it does not describe all BFI/strobed cases.
> Use [PANEL-EXPERIMENTS.md](PANEL-EXPERIMENTS.md) for the current evidence and procedure.

Written 2026-09-13 after fullscreen output failed to fuse on both the Hisense U6 Pro
and the Samsung G80SD while the embedded preview window fused on both. The session
log for those fullscreen runs shows the presenter locked in direct flip at the
measured refresh with single-digit misses and zero USB errors, so the emitter
command stream is not the difference. The emitter only ever sees eye commands
timed to the predicted vblank; it cannot tell a window from fullscreen.

## Why fullscreen fails at 120 Hz

A panel rewrites its rows top to bottom over most of each refresh. Rows that
have finished show the new eye; rows below the scan line still show the old
one. The shutter can only be clean while every lit row shows the same eye:

```
window = hold - response - scan * height
```

- `hold`: how long the panel keeps receiving the same eye image. One refresh for
  Left/Right and Left/Black/Right/Black, two refreshes for Left/Left/Right/Right.
- `response`: how long a rewritten row takes to settle. About 0.2 ms on the OLED;
  3 to 5 ms on the Hisense VA panel (measured 2026-09-13 with a 240 fps camera).
- `scan`: how long the active rows take to be rewritten. The signal timing gives
  active lines / total lines x refresh period (about 8.1 ms of 8.33 ms for a
  standard 4K 120 Hz mode). The Hisense re-times its panel and measured 6.0 ms at
  144 Hz; the app accepts a measured value under Advanced > Panel.
- `height`: the fraction of the screen height that carries the image.

| Display, mode | hold | response | scan | Whole screen | 40 % band |
|---|---|---|---|---|---|
| G80SD 4K 120 Hz, Left/Right | 8.33 | 0.2 | 8.1 | 0.0 ms | 4.9 ms |
| Hisense 4K 120 Hz, Left/Right | 8.33 | 3.0 | 6.0 | none | 2.9 ms |
| Hisense 4K 144 Hz, Left/Right | 6.94 | 3.0 | 6.0 | none | 1.5 ms |
| G80SD 4K 240 Hz, Left/Left/Right/Right | 8.33 | 0.2 | 4.05 | 4.1 ms | 6.5 ms |

The 1.6 to 1.8 ms shutters in the saved profiles need a window at least that
long. On both displays, at 120 Hz with the whole screen lit, there is none. The
embedded preview covered about a quarter of the screen height and therefore had
a window of several milliseconds. That is the entire difference.

## What the app now does

**Stereo area** (Live tab, under Image alignment; profile fields `band` and
`band_center`). The image is scaled about a chosen center and presented in a
band of the chosen height; every row and column outside is exactly black. Black
rows cannot leak into the other eye, so a band behaves like the small window that
already fused, on the real fullscreen path with direct flip and HDR. The same
band is sent to the game hook through shared memory and applied to both eye
blits. In fullscreen, PageUp/PageDown change the height and Home/End move it.

**Panel timing.** Displays report their active and total line counts from the
signal timing; the app derives the scan time and shows the settled window for
the whole screen and for the current area. Since 2026-09-13 (night) a numeric
model follows every row of the area through the cycle and predicts the other-eye
leakage of the current phase for the top, center and bottom. **Fit area to
shutter** picks the largest band whose best phase leaks at most 1 %. **Suggest
phase** picks the least-leakage phase of that model, converted to the emitter's
origin with the measured scan start and USB latency; lens latency is not known,
so sweep from there. Advanced > Panel holds the response time, an optional
measured scan time, the illumination type and the strobe pulse.

The self-test renders both eye frames with a 50 % band and requires every pixel
outside the band to be exactly zero in SDR and HDR, and the pattern to appear
inside it.

## Whole-screen routes

1. **G80SD at 240 Hz with Left/Left/Right/Right.** Each eye is scanned twice, so
   after the first 4 ms scan the whole panel holds that eye for another 4 ms.
   Set the display to 4K 240 Hz in Windows, press Match output rate, choose the
   sequence under Advanced, and start fullscreen. Optical results for this
   sequence are not yet verified; the whole-screen route actually confirmed
   through the glasses is black frame insertion, route 4 below.
2. **Custom 120 Hz mode with a large vertical total.** If a display accepts a
   120 Hz mode that keeps the 240 Hz line rate (vertical total about twice the
   standard value), the scan finishes in about 4 ms and the rest of the frame is
   blanking, which is how the original 3D Vision monitors worked. Create it in
   NVIDIA Control Panel > Change resolution > Customize with manual timing, keep
   the horizontal values of the native 4K 240 Hz mode, and double the vertical
   total. The app shows the resulting scan time under Advanced > Panel before any
   optical test. Whether the G80SD or the Hisense accepts such a mode is unknown.
3. **Hisense at 120 Hz.** The panel's 3 to 5 ms response leaves no whole-screen
   window at any 4K rate it offers. Use the stereo area; a 40 to 50 % band leaves
   2 to 3 ms for the shutter if the 6 ms scan holds at 120 Hz (a 5 s bare-TV
   240 fps clip of the Both eye targets pattern would confirm the scan time).
   Any TV-side black-frame-insertion or "motion clearness" setting is worth a
   try because it blanks the panel between frames; the app cannot control it.

4. **Software black frame insertion (Left/Black/Right/Black).** While the black frame
   scans in, no row anywhere shows the other eye, so the whole screen is clean. This
   sequence halves the emitter's rate, which doubles its period to two display refreshes,
   and the shutter may stay open across all of it: the model then collects 96 % of a fully
   lit frame at 4K 120 Hz with zero leakage, against 51 % under the one-refresh shutter
   limit this build removed. Press **Maximize brightness** after switching. Each eye still
   flashes at a quarter of the refresh rate, so 240 Hz gives the 60 per second that 3D
   Vision gave at 120 Hz, while 120 Hz gives 30 and visibly flickers. Output & timing >
   Software black frame insertion, or B in fullscreen. The game hook needs a build with
   sequence support for this. **Confirmed 2026-09-13:** the user reported this clean over
   the whole screen, with no stereo-area band, on the G80SD at 4K 240 Hz in HDR with the
   shutter at 4980 us and image brightness 3.05x.
5. **A strobed panel mode (BFI, Motion Clearness, LightBoost-style backlight).** The
   panel lights only during a pulse after the scan; if the rows have settled by then,
   the pulse is a whole-screen clean window. The pulse timing is found with the phase
   sweep; see [TIMING-CALIBRATION.md](TIMING-CALIBRATION.md).

What software cannot do: make a panel scan faster, settle faster, or show one
eye on rows it has not yet rewritten. The stereo area trades screen height for a
clean window; the 240 Hz and custom-mode routes trade nothing but require the
display to cooperate.
