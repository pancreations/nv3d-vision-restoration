# Timing, jitter and illumination calibration

Written 2026-09-13. This describes how the app times the shutters against the display,
what it measures about that timing, and how to align the glasses with a panel that strobes
(LightBoost-style backlight, black frame insertion, "Motion Clearness"). The numbers below
were measured on this PC with the Hisense U6 Pro at 4K 143.988 Hz; they are host timings,
not optical measurements.

## The timing chain

1. **Presentation timestamps.** For every refresh DXGI reports the refresh counter and a
   QPC timestamp (`DXGI_FRAME_STATISTICS::SyncQPCTime`). The presenter fits a straight line
   through the last 240 of them (least squares, `TimingTracker`). The line gives the period
   and the vblank phase; each new timestamp's distance from the line is the **vblank
   jitter** shown under Diagnostics. Earlier builds anchored the prediction on the newest
   timestamp alone, so its whole jitter went into the next command.
2. **Predicted vblank.** The eye command for the refresh a present will land on is due at
   the fitted time of that refresh. The USB worker waits on a high-resolution timer and
   sends the 8-byte eye command; the difference between the moment the write starts and the
   deadline is the **eye command timing error** (last / rms / max under Diagnostics).
3. **Emitter.** The command carries the distance to the emitter's next period boundary; the
   X register delays the open token after the boundary and Y closes it. The phase control
   is split into those two values (`nvidiaSchedule`), every phase or shutter change rewrites
   the timing block (counted as **timing writes**), and the Output & timing tab shows the
   boundary and X the emitter currently receives.
4. **Panel.** Rows are rewritten top to bottom over the scan time, settle over the panel
   response, and are lit continuously (sample and hold) or only during a pulse (strobed).

### Measured on 2026-09-13 (Hisense, 4K 143.988 Hz)

| Quantity | Value |
|---|---|
| Presentation timestamp jitter around the fitted line | 0.9 to 6 us rms, 4 to 24 us max |
| Scan-line estimate of the vblank start (kernel counter after a vblank wake) | 22 to 27 us rms |
| Wake latency of the vblank event | 140 to 150 us median, up to 400 us |
| Blanking interval (154 of 2314 lines) | 462 us |
| Presentation timestamp after the blanking interval starts | 105 to 150 us (varies by run) |
| Scan of row 0 after the presentation timestamp | 105 to 353 us (varies by run) |
| Mean USB transfer of an eye command | about 120 us |

The DXGI timestamps are therefore the better clock, and the scan-line probe is used only to
calibrate where the scan starts relative to that timestamp and to report the blanking
length. **Diagnostics > Measure vblank (3 s)** or `VisionRestoration.exe --vblank`
(reports/vblank.txt, runs beside an open app) performs that measurement and stores the
scan start offset in the profile (Advanced > Panel). The standalone probe that produced
these numbers is in `experiments/vblank-probe`.

## Frame jitter under Diagnostics

- **Vblank jitter rms / max** over the fitted refreshes: display timestamps against the
  refresh clock. Values above about 50 us rms mean the clock is not trustworthy (a
  composed window, a mode change, power saving).
- **Present interval jitter**: standard deviation of the presenter's present-to-present
  interval over the last 256 frames. Occasional two-period intervals show up as slips in the
  presentation counters, not here.
- **Eye command timing error**: how far from the predicted vblank the USB write started.
  This is host scheduling; the emitter's own period lock filters it further.
- **Mean USB transfer** is subtracted when a phase is suggested from the model.
- All of these are written to `reports/session.log` every two seconds and to the exported
  timing report.

## The illumination model

`estimateLeakage` follows 33 rows of the stereo area through one stereo cycle. Each row
receives a refresh's content when the scan reaches it, fades to it linearly over the panel
response, and is lit continuously or only during the strobe pulse. A shutter that is open
from the phase for the shutter length collects the eye's own light and the other eye's
light; their ratio is the predicted leakage, reported for the top, center and bottom third
of the area. The Stereo area section shows this prediction for the current phase; **Suggest
phase** searches the whole cycle for the least leakage (brightest among equals), and **Fit
area to shutter** picks the largest band whose best phase leaks at most 1 %. Black frames
and repeated frames go through the same model.

Model phases are measured from the start of the eye's first scan. The emitter phase is
measured from the presentation timestamp plus the USB latency, and for
Left/Left/Right/Right from the second refresh. `emitterPhaseFromModel` converts between the
two, using the measured scan start offset.

What the model says for the displays on hand:

| Configuration | Best whole-screen result |
|---|---|
| 4K 120 Hz Left/Right, OLED (scan 8.1 ms) | no clean phase; about 7 % leakage in the top and bottom thirds with a 1.8 ms shutter, center clean |
| 4K 120 Hz Left/Right, Hisense (scan 6 ms, response 3 ms) | worse; use the stereo area |
| 4K 240 Hz Left/Left/Right/Right, OLED | clean window of about 4 ms |
| 4K 240 Hz Left/Black/Right/Black, OLED | clean for shutters up to about 5 ms, 84 % of a fully lit frame once the shutter is widened, 60 flashes per eye per second |
| Strobed panel, pulse after the rows settled | clean whenever the shutter covers the pulse |

## Software black frame insertion

**Output & timing > Software black frame insertion** (or **B** in fullscreen) switches the
sequence to Left / Black / Right / Black. While a black frame is scanned in, no row anywhere
on the screen shows the other eye, so the whole screen can be clean on a sample-and-hold
panel. The costs are a per-eye flash rate of a quarter of the refresh rate (60 per second at
240 Hz, which is what 3D Vision delivered at 120 Hz, but only 30 per second at 120 Hz, which
flickers) and less light, most of which the wider shutter described under "Getting the light
back" recovers. It is a test tool at 120 and 144 Hz and a real route at 240 Hz.

The game hook must present four frames per game frame for this. The shared-memory contract
carries the sequence (`sequence` field) and a hook advertises support with the
`HookSequences` flag; the current hook build does not, so the app drives the game as
Left / Right whatever the profile says and reports that in the hook status.

## Getting the light back

A black-frame or repeated sequence lights each eye on fewer refreshes, so it is dimmer.
How much of that is recoverable is worth stating precisely, because two different effects
look the same through the glasses:

- **Inherent.** A row is lit with its own eye for one refresh out of the four-refresh
  cycle. No shutter setting changes that.
- **Recoverable, and previously thrown away.** The emitter runs one period per stereo slot
  pair, so a four-slot sequence gives it a period of **two** display refreshes, and the
  shutter may stay open for that whole period. Every shutter limit in the app used to be
  written against one display refresh, which cut the usable window roughly in half (and at
  240 Hz to a sixth). A row shows its eye from the moment the scan reaches it until the
  next scan one refresh later, and the other eye cannot appear before two refreshes, so a
  shutter of about `period + scan`, opened at the scan start, collects every row's entire
  lit period and still never sees the other eye.

**Maximize brightness** (next to Suggest phase) does this: it widens the shutter to the
largest value whose best phase stays within 1 % leakage and recentres the phase. It ranks
candidates by the light actually collected, not by the lit fraction of the shutter, which
falls as the shutter widens and would have chosen the dimmer setting. The stereo area
reports the result as "light reaching this eye" against one fully lit frame.

| Configuration (whole screen, model) | Old cap | New cap | Light before | Light after | Leakage |
|---|---|---|---|---|---|
| OLED 4K 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 51 % | 96 % | 0 % |
| OLED 4K 240 Hz Left/Black/Right/Black | 813 us | 4980 us | 19 % | 84 % | 0 % |
| OLED 4K 240 Hz Left/Left/Right/Right | 813 us | 4980 us | 19 % | 118 % | 0.6 % |
| Hisense 144 Hz Left/Black/Right/Black | 3592 us | 10499 us | 46 % | 95 % | 0 % |
| Hisense 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 55 % | 98 % | 0 % |

Above 100 % is real for Left/Left/Right/Right, where a row is lit for two refreshes.
Pressing Maximize brightness on a whole-screen Left/Right configuration does the opposite
and shrinks the shutter hard, because that is the only way that sequence stays inside the
leakage limit; the notice reports what it chose.

What is left after the shutter is as wide as it can go is panel luminance. **Image
brightness** (Output & timing) multiplies the image the app presents. In HDR it reaches
into the display's headroom; in SDR it clips to white, so use the display's own brightness
control instead. It applies to the app's own output only, never through the game hook, and
it can never lift a black frame.

## Strobed panels (LightBoost, BFI, Motion Clearness)

A strobed panel lights the whole screen at once for a short pulse after the scan. If the
rows have settled by then, the clean window is the pulse itself and it covers the whole
screen: this is how the original 3D Vision monitors worked. The pulse timing of a TV's
BFI mode is not published, so the app finds it with the glasses:

1. Enable the display's BFI (Hisense: Picture > Motion > Motion Clearness; Samsung:
   Clear Motion). Some TVs offer it only at 60 or 120 Hz input.
2. Advanced > Panel: set **Illumination** to strobed. Set the shutter to its minimum
   (250 us) so the glasses act as a narrow probe.
3. Start fullscreen 3D with **Both eye targets** and press **S**. The phase now advances
   through the whole cycle over 30 seconds (speed selectable under Output & timing) and its
   value is drawn in the output. Through one lens the targets are dark most of the time and
   bright twice per cycle: once with the correct image (the target on the correct side,
   the correct LEFT/RIGHT label) and once with the other eye's image.
4. Press **M** where the correct image first brightens and **M** again where it fades.
   Press **S** to stop. **Set strobe from the last two marks** stores the pulse (start =
   first mark + shutter, end = last mark).
5. Open the shutter to the length you want (it must cover the pulse; brightness comes from
   the pulse, not from a longer shutter) and press **Suggest phase**. The prediction under
   the stereo area should read 0 % top to bottom. Sweep from there if the lenses' own
   delay moves it.

If the bottom of the picture still shows the other eye at the pulse, the panel has not
settled when it strobes; the model then reports bottom leakage and a shorter area is the
only remedy. A high-speed camera clip of the bare panel (see the 2026-09-13 capture in
`reports/capture`) gives the same pulse timing directly.

## Which timing controls act on what

| Control | Acts on | Available when |
|---|---|---|
| Phase | boundary distance and X in every command; parity of the commanded eye | any connected emitter |
| Shutter | Y register (per-frame on NVIDIA); limited by the emitter period, which is two display refreshes in a four-slot sequence | any connected emitter |
| Image brightness | the app's own presented image, before the readouts, never a black frame | always; effective in HDR, clips in SDR |
| Maximize brightness | the shutter and the phase together | panel scan known |
| Left / right shutter, right eye offset | per-frame X/Y rewrite | NVIDIA emitter, Left / Right sequence only |
| Sequence, software black frame insertion | presenter cadence, emitter period | NVIDIA emitter (RP2040 is Left / Right only) |
| Stereo area | presenter and hook shaders | always |
| Panel response, scan, illumination, strobe, scan start | the model only (Suggest phase, Fit area, predicted leakage) | always |
| Phase sweep, marks | the phase, ten steps per second | 3D output running |
| Match output rate, manual rate | emitter period and scaled timing | always |

Controls that cannot act in the current configuration are disabled and say why. A simulated
emitter reports that its timing controls change nothing physical.
