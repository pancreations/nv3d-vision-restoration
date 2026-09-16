# LCD / QLED experiments with the official NVIDIA emitter

Updated 2026-09-14. **Only the user's 240 Hz OLED with software black insertion is proven.**
U6 Pro at 144/240 Hz and Dell at 120 Hz remain optically unverified with these changes.
The old subtraction-based "LightBoost" button was removed. Its description incorrectly
equated subtracting a predicted ghost with switching off the backlight, and called assumed
Hisense response times measurements. Those claims are withdrawn.

## Start here

Close the currently running app, then use `Launch-Panel-Experiments.cmd`. This build lives
under `build/panel-experiments/bin/Release`; its profiles and reports are beside that EXE.
The build script seeds copies of existing profiles only when a destination does not exist.
The normal executable and original calibration files remain available through `Launch.cmd`.
Profile version 10 adds `guard_level`; old executables cannot load version 10 files.

Select the correct display and **Match output rate**, then open **Panel experiments**.
For optical calibration use fullscreen output, with the controls on another display.
The embedded preview covers only part of the panel and cannot establish full-panel behavior.

| Experiment | What is sent | 240 Hz: frames/eye/s | 144 Hz | 120 Hz |
|---|---|---:|---:|---:|
| Direct | L R | 120 | 72 | 60 |
| Black reset | L B R B | 60 | 36 | 30 |
| Neutral reset | L G R G, adjustable uniform gray G | 60 | 36 | 30 |
| Preload | L L R R, emitter trigger on second copy | 60 | 36 | 30 |
| Extended preload, Advanced > Sequence | L L L R R R | 40 | 24 | 20 |
| Hold plus reset, Advanced > Sequence | L L B R R B (or G) | 40 | 24 | 20 |

These rates are arithmetic, not promises about the TV's internal panel cadence or the
glasses' behavior at every emitter rate. The new buttons preserve the current phase modulo
the new cycle and use equal 1500 us starting shutters (or the emitter limit), full image
area, ordinary image gain, zero black lift, and no ghost subtraction. They do not guess
response times. Longer runs are deliberate low-rate experiments and can visibly flicker.

### U6 Pro at 240 Hz

1. Compare **Black reset** with **Preload** using **Nine-row optical targets**.
   Preload holds the same captured stereo pair through both copies of each eye.
2. Compare **Neutral reset**, initially gray code 0.5, with black reset at the same rate
   and fixed shutter. Try 0.25 and 0.75 separately, clearing measurements between trials.
3. If two-refresh runs still have no common good range, test the three-refresh sequences.
   Their 40 frames/eye/s is a lower-rate diagnostic and possible fallback.
4. Use the **Refresh-code camera test** without glasses to investigate whether every
   submitted frame survives the TV's 240 Hz mode. A refresh number in Windows alone does
   not answer that question.

### U6 Pro at 144 Hz; Dell at 120 Hz

Repeat direct, preload and neutral-reset comparisons. At these rates, extra settling
refreshes cost substantial per-eye cadence: 36 or 30 frames/eye/s. They do not secretly
deliver 72 or 60. If preload improves the whole screen, it is evidence that time between
eye changes matters. It does not prove the panel's exact transition time.

The saved hardware report identifies the Dell as **S3220DGF**. Its manual identifies a VA
panel and Fast / Super Fast / Extreme response controls. Compare those settings separately,
including inverse ghosts, and clear the optical marks each time. The manual does not
document a LightBoost or strobe-backlight control. [Dell manual, pp. 14 and 35](https://dl.dell.com/manuals/all-products/esuprt_electronics_accessories/esuprt_electronics_accessories_monitors/dell-s3220dgf-monitor_user%27s-guide_en-us.pdf).

For the TV, compare fixed-rate Game/PC operation, interpolation off, and local dimming off
versus on if its actual menu permits. This is an isolation experiment, not a claim about
which settings this particular U6 Pro firmware exposes. Record resolution, input, HDR,
overdrive, dimming and motion settings; changing them invalidates comparisons.

## Measure the whole screen instead of chasing a moving clean band

The nine-row target has three pairs of columns in every row: white/black, middle-gray/black,
and light-gray/dark-gray. Left-eye content is brighter in columns 1/3/5; right-eye content
reverses each pair. This tests different transitions and reaches close to both screen edges.

At one fixed shutter width, inspect one lens at a time. Find one contiguous acceptable
phase range for each screen third. Stop the sweep and let the output settle before marking
its start and end. Desired targets must remain visible throughout the range; darkness is
not successful isolation. Inspect all three rows in each third and all gray pairs.

**Use common measured phase** intersects all six ranges (two lenses times three thirds),
including ranges that wrap through phase zero. It chooses the center of the widest overlap,
keeps the shutter unchanged, and refuses a setting where the emitter would clamp its width.
No overlap is reported as no overlap, not replaced by the model's least-bad phase. This
calibration does not treat phase-range width as light-pulse width.

Marks reset when output stops or relevant app settings change. Clear them manually after
changes in the display's own controls, glasses position, or hardware. These are visual
observations, not photodiode measurements. **Export optical observations** records the
configuration, six ranges, intersection, notes and host timing counters. Recheck the chosen
phase with the captured game: passing a static pattern does not establish moving-content quality.

## What neutral reset is testing

Black reset asks the previous image to transition toward black. Neutral reset instead asks
all pixels to converge toward one intermediate level before the next eye. The hypothesis is
that a different transition may erase image-dependent residuals sooner on some LCD modes.
It may also increase haze, activate local dimming differently, or fail completely. There is
no preselected response-time claim or guarantee of improvement.

The gray slider is an **sRGB code value**, not a voltage sent to liquid crystals. SDR sends
that code; HDR converts it to linear scRGB against the app's 200-nit reference white.
It does not modify the eye images' blacks or emitter packets. Startup, pause and
reacquisition frames stay black. The old leakage model is disabled for this experiment
because it has no neutral-reset dynamics. The game hook does not implement neutral reset;
use the app's presenter and the proven window-capture source.

## What actual LightBoost would require

Backlight strobing and shutter-glasses timing must cooperate with pixel settling. Sending a
black image, increasing vertical blanking, or changing a model's illumination flag does not
itself switch the physical backlight off. Multipass refresh before viewing is a documented
approach to LCD settling; our preload experiment uses duplicate scans and glasses timing,
without claiming the monitor's internal overdrive or backlight is controlled. [Blur Busters'
engineering discussion](https://blurbusters.com/faq/creating-strobe-backlight/).

For direct L/R, an approximate full-screen settled interval is `T - S - R`: refresh period
minus actual panel scan duration minus settling time. The requested shutter and timing
margin must also fit. Preloading N identical refreshes changes this to `N*T - S - R`.
This is an explanatory bound for a simple sample-and-hold model, not a measured property
of either display. Gray transitions, overdrive and scanning backlights need richer measurements.

If the goal is to keep 60 frames/eye/s at a **120 Hz** input, investigate accelerated panel
scanout/longer blanking or a real synchronized backlight mode. Repeated frames alone cannot
retain that cadence. A custom mode must be derived from the actual pixel clock and totals,
then checked optically: a TV can buffer and re-time the input. The previous fixed VT 2777 /
3332 instructions are withdrawn; their acceptance and optical effect were never established.

If a TV menu offers a backlight strobe, measure whether it is synchronized and whether it
flashes once per input refresh. PWM, local dimming and interpolation are not interchangeable
with a clean stereo strobe. The app's **Illumination** setting only changes its model.

## What 3DVision4All contributed

Its [native-display guide](https://oneup03.github.io/3DVision4All/docs/Native) restores NVIDIA's
legacy stereo path and links a modified Strobelight utility. Strobelight's author lists
specific ASUS/BenQ LightBoost monitors and DVI/DisplayPort requirements, not generic LCDs,
the Dell S3220DGF or Hisense TVs. [Strobelight requirements](https://www.monitortests.com/forum/Thread-Strobelight-LightBoost-Utility-for-AMD-ATI-and-NVIDIA).

The inspected [D3D11 compositor](https://github.com/oneup03/3DVision4All/blob/main/src/Compose_D3D11.cpp)
converts stereo into spatial display formats. Its [overlay presenter](https://github.com/oneup03/3DVision4All/blob/main/src/Output_Overlay.cpp)
uses a separate D3D11 output path. These files do not supply a generic LCD backlight-control
implementation. Their useful distinction is between generating stereo images and driving
a display already capable of separating them. No code was copied from that project.

## Audit of the earlier recordings

`Footage240fps/U6_PRO` contains LEFT, RIGHT and TV MP4 recordings and extracted images.
The LEFT and TV video streams report 240000/1001 fps (about 239.76), 1920x1080. Their frame
spacing is about 4.171 ms. The inspected TV sample and left-lens contact image show
different relative target brightness at different screen heights. They support investigating
spatial/temporal mixing. They do not independently separate camera exposure/readout,
display scanout, liquid-crystal response and shutter-glasses timing. Periodic sampling can
sometimes reveal sub-frame structure, but requires a justified capture model. No such
validated fit or exposure calibration accompanies the previously asserted 1.5 ms rise /
4 ms fall values in the inspected material; those values are not used as measured inputs
to the new experiments.

## Further method worth pursuing after the measurements

A measured, gray-to-gray **pre-emphasis pass followed by a target pass** could extend preload:
write a deliberately chosen first image, then the actual eye image, and open the lens after
the second pass. This acts on transition history rather than subtracting the opposite eye's
visible ghost. It needs measured transitions for this panel/mode, including overdrive and
clipping limits; otherwise another invented LUT simply repeats the previous mistake.
This is a research direction, **not implemented or validated in this build**.

The official emitter remains the active backend. Replacing it with an RP2040 does not by
itself establish faster LCD settling, panel scan behavior, or physical backlight control.
