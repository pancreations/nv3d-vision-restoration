# Shared shutter calibration: LCD, OLED, RP2040 and NVIDIA

Phase, shutter, image brightness and HDR controls stay at the top of both the main and fullscreen panels. Saved manual profiles retain their original timing mode; loading one does not convert it to a guarded LCD aperture. **Advanced LCD calibration > Use guarded LCD timing** explicitly enables the optional frame-bounded exposure model. LCD/VA and OLED starting presets remain available. The selected emitter chooses the hardware implementation: scheduled RP2040 output or the official NVIDIA timing-register path.

## One screen of controls

Choose the actual output and load a saved profile or optionally choose a **Starting preset**. Presets do not recreate or close the preview. Choose the display refresh under Display; only presets matching the current refresh are offered. The everyday controls stay visible on the left. Expand **Advanced LCD calibration** for settling, scan compensation and per-eye corrections. Capture window, AI desktop and All inputs have direct buttons. Input, Profiles, Panel, Diagnostics, Settings and Display remain below the preview.

| Slider | Range | Meaning |
|---|---|---|
| Settle delay | 0–8 ms | Wait after predicted frame start before exposing the eye |
| Shutter duration | 0.25–8 ms, limited by the current refresh | Requested exposure length |
| Global phase | ±one refresh | Shift both eyes relative to the refresh prediction |
| Guard / blanking | 0.25–2 ms | Reserve closed time at each frame boundary |
| Left / right correction | ±1 ms each | Correct the physical lenses independently |
| Scan time | 0–50 ms | Optional progressive scan compensation; zero uses signal timing |
| Reference row | 0 top–1 bottom | Position used for scan compensation |

Use the slider handles, Ctrl+click to type, or the adjacent minus/plus buttons. RP2040 aperture changes are coalesced until a 60 ms pause in dragging, keeping the current device session running during a continuous drag. Buttons step by 0.1 ms, or 0.01 ms when the fine checkbox is selected. Eye swap and scan compensation are on the same screen. The timeline shows the requested openings for both lenses. HDR follows the chosen output and is calibrated separately.

## Fullscreen controls

Press **Fullscreen (F11)** for full-panel output, then **Home** to show the complete controls menu. The same menu works for test patterns, SBS sources and AI desktop. All timing, brightness, HDR, input, profile, display and diagnostic options remain available. The ordinary app window returns when fullscreen ends.

Drag the menu by its heading. **Home** shows/hides it without restarting or resizing output. **Windowed output** returns to the embedded preview. **Shortcuts** lets you rebind functions or disable individual/all shortcuts for gaming; shortcuts only activate during fullscreen output. The tray icon reopens controls when shortcuts are disabled. The default stop shortcut is **Ctrl+Alt+F8**. Timing edits and test selection do not recreate the output window. A missed LCD deadline reacquires synchronization automatically; it does not permanently disable the emitter. USB disconnects and hardware faults still surface as errors.

## Calibration procedure

1. Start with the default 4 ms settling delay and 0.75 ms duration at 120 Hz. These are search starting points, not a response-time measurement.
2. The preview starts with the familiar **Left / Right** rectangle/circle check. **3D shapes**, **Both eye targets**, **Left only** and **Right only** are beside it in both timing modes. For regional measurements, explicitly select **LCD rows**, then press **Fullscreen test (F11)**. The nine-row target includes bright and dark transitions. Through the left lens, columns 1, 3 and 5 should dominate; through the right, columns 2, 4 and 6.
3. In fullscreen, **S** starts or pauses the settle sweep. It advances through valid delays in 0.25 ms steps every three seconds, progressively shortening exposure. **Enter** keeps the current timing. Up/down adjusts settling; left/right adjusts duration; Shift uses 0.01 ms steps. **X** swaps the eyes.
4. Check the top, center and bottom through each lens. Keys **0/1/2/3** select the full panel/top/center/bottom. Selection highlights the region's edges; it does not crop the image or retime the glasses.
5. Press **Esc** to return to the same slider screen. Rate the timing you just viewed, then press **Record timing**. A dark or invisible intended target is not evidence of separation. Compare several settings.
6. **Use best observed** ranks the worst of the six regions first, then total ghosting, then exposure duration. Brightness only breaks a separation tie. **Save results** writes `reports/lcd-calibration.csv`; **Save profile** records the applied timing; old OLED and LCD profiles remain readable. Automatic saves use `autosave-timing-rp2040.ini` or `autosave-timing-nvidia.ini`.

Observation ratings reset when exposure, eye order or display context changes. Hardware and host counters are diagnostics, not optical measurements. If no setting works across all regions, record that result; the program does not turn a clean band into a claim of full-panel separation.

## Timing model

For measured refresh `h`, the frame period is `P = 1,000,000 / h` microseconds. With scan compensation enabled, `scanShift = scanTime × referenceRow`; otherwise it is zero.

```
open[eye]  = ceil(settle + globalPhase + scanShift + eyeCorrection)
close[eye] = open[eye] + duration

guard <= open[eye]
close[eye] <= floor(P) - ceil(guard)
```

Sliders keep the values you request, including temporarily invalid combinations. Only a complete valid set replaces the applied timing. A **PENDING** notice explains what does not fit, displays the last applied values, and offers **Restore applied values**. The glasses and preview continue using the last valid set while you finish changing other sliders. Scan time and reference position remain editable before scan compensation is enabled. Saves record applied timing. Only an explicitly started sweep fits both settle and duration together. The UI uses integer device guards and 0.10 ms scheduling headroom before the end guard. Phase never silently wraps an LCD exposure into the other eye's frame. The 0.25 ms minimum guard leaves room for the IR tokens. Requested exposure is time between token starts; actual lens transparency can lag those commands and must be judged through the glasses.

The presenter supplies its measured refresh period to the emitter backend. RP2040 firmware 0.4.0 receives frame anchors and per-eye offsets separately, so independent eye corrections do not masquerade as irregular frame cadence. It checks guards, exposure bounds, overlap, event order and deadlines again on the board. A detected LCD scheduling miss cancels pending work and reacquires the presentation clock; USB faults still require a working connection. An already emitted command cannot be undone after late presentation feedback.

The model is independent of refresh rate; automated cases cover 100, 119.88, 120, 144, 165, 180, 200, 239.991 and 240 Hz. At 120 Hz, LRLR means 8.333 ms per refresh and 60 complete stereo pairs per second. Faster refresh leaves less time for settling. Scan compensation shifts one global exposure, since each glasses lens exposes all display rows together. Signal scan timing may differ from the monitor's internal scan.

## OLED and firmware compatibility

OLED/general remains a separate mode with 120 Hz LR, 240 Hz LR and 240 Hz BFI controls. At 240 Hz, LR gives 120 stereo pairs/s; software L/B/R/B gives 60 pairs/s. These options describe supported scheduling, not a completed optical test.

- Firmware 0.2.0: original 120 Hz LR path; the user confirmed good OLED stereo.
- Firmware 0.3.0: repeated/reset cadence, including 240 Hz BFI. A 120 Hz LCD preload trial failed to sustain useful glasses operation; ordinary LR activated them but still showed ghosting.
- Firmware 0.4.0: adds bounded frame apertures and direct high-refresh cadence. Read capability flags before offering these paths.

OLED profiles continue using the existing version 11 format. LCD profiles use version 12, a distinct `_LCD` profile key and separate autosave files. The working OLED profile and firmware backup remain available. See [RP2040 protocol](RP2040-PROTOCOL.md) and [firmware flashing](../firmware/rp2040/README.md).

The new firmware and app require physical optical testing on the user's monitors. Narrowing exposure trades brightness for a chance at better isolation; it cannot guarantee that a slow panel has a clean full-screen window.

Build verification (2026-09-15): all six targeted CTest suites pass; LCD and OLED UI smoke checks exercise the embedded preview without USB writes. Firmware 0.4.0 `719175976901ec47` passed the UF2 artifact checks. SHA256: `7A3C2E0F359D404D1FE31F5D8869AEC789C829EF530BEAD9AA1E0F1875F6593E`. This build was flashed successfully at 14:42 local time. The board reports both new capabilities; the clock probe passed 197/200 samples after initialization with no device faults. See `reports/rp2040-flash-20260915-144212.txt`. Optical evaluation is still pending.


## Sequences and compatibility

The model derives both times from measured refresh: the interval between eye triggers is `(held refreshes + black refreshes) / refresh`, while the available exposure after the last held refresh is `(1 + black refreshes) / refresh`. It must close before the other eye appears. Thus 240 Hz BFI has 4.167 ms display refreshes and up to an 8.333 ms eye exposure interval, still producing 60 complete stereo pairs/s.

RP2040 firmware 0.4 uses frame-anchored scheduling for intervals up to 10.05 ms, including 120 Hz LR and 240 Hz LR/BFI. Longer intervals retain its existing timestamped-open protocol; at those cadences, independent eye corrections may differ by at most 0.05 ms to leave room inside the firmware's cadence tolerance. An unsupported request remains pending and does not stop the current session. No new flash is required for this update. The official emitter retains its existing driver, RAM firmware loading and timing-register implementation; firmware-imposed exposure limits still apply.

Native exceptions now produce `reports/native-crash.dmp` so an unexpected app exit can be investigated rather than losing its context. Normal timing validation never deliberately exits the application.
