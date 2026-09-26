# GPU-load flashing

## Refresh-scaled presentation queue (2026-09-22)

An on-display diagnostic through the production presenter reproduced the timing
fault while showing only a steady captured eye, with no physical emitter
connection. Window ordering remained stable and clicks reached the foreground
application. At 3840x2160 HDR / 239.98 Hz, the old three-frame queue recorded
800 delayed-present reports over 25 seconds. CPU drawing took only 0.006 ms at
the median; `Present` sometimes blocked for over 10 ms after the frame wait.
Three queued refreshes provided only 12.5 ms of buffering.

With six queued frames and seven back buffers, the same diagnostic recorded
zero timing misses in 5,997 presentations over 25 seconds. With the stream
mirror enabled, it recorded zero misses in 10,773 presentations over 45 seconds.
Sources: `reports/steady-probe-q3.csv`, `reports/steady-probe-q6.csv`,
`reports/steady-probe-q6-stream.csv`, and their `.csv.txt` summaries.
These are present-statistics tests with a steady picture, not optical
measurements through the glasses.

The final production-policy run with streaming and simulated shutter commands
completed 10,761 reported presentations and 5,387 simulated commands over 45
seconds. There were no late display frames or late commands. Its one nonzero
refresh delta was an early report during startup at 0.0703 s (present 13); all
10,702 other distinct reported presents matched their expected refresh.
See `reports/steady-probe-production-stream.csv` and its `.csv.txt` summary.
Core timing, fullscreen window order, actual capture routing, stream settings,
GPU transport, and the expanded simulated command queue tests pass.

The production presenter now reserves approximately 25 ms using the actual
display refresh: three frames at 120 Hz, six at 240 Hz, bounded at eight.
Swap-chain buffer count grows with that queue. The shutter-command FIFO also
holds the maximum queued eye frames plus two spare entries; its old four-entry
limit would discard valid commands after increasing presentation depth.
Eye selection and shutter deadlines still use the same display-refresh anchor.
At 240 Hz this adds approximately 12.5 ms of buffering compared with the old
three-frame queue; it does not lower capture rate or change calibration.

`vision_present_probe PID queue-depth seconds report.csv [stream] [simulate]`
repeats the bounded steady-image diagnostic. Queue depth `0` uses the production
policy. `simulate` exercises command scheduling with the simulated emitter;
the tool never connects to physical emitter hardware. It keeps input focus
with the application and writes per-frame timings after the run so disk writes
do not interrupt presentation.

## Focus loss and composed recovery (2026-09-22)

The window-capture overlay still flashed on the monitor and stream after the
capture routing repair. The session recorded `passthrough=1 gameOverlay=1`,
stable composed presentation and zero window-order repairs, but hundreds of
sampled black frames on image slots. The user also reported disappearance when
unfocused. The UI explicitly hid both the stereo window and mirror whenever
foreground belonged to another process, including Discord or a transient null
foreground window.

Focus no longer controls output visibility. Fullscreen output covers the selected
display independently of the game's position, focus or minimized state; hidden
or minimized sources retain the last captured pair. Both stereo and the display
stream mirror pass clicks through without activation. Repeated game activation
repairs their order instead of stopping output after three competing raises.
The composed timing path now
uses each actual `PresentRefreshCount` immediately, matching direct flip;
the previous two-report filter delayed real missed-refresh recovery. This
supersedes the composed-filter description in the September 18 entry below.

Fullscreen overlays are fully opaque (`alpha=255`). The compatibility setting
that used alpha 254 leaked the packed game image through black slots and forced
composition. Stream presents request `DO_NOT_WAIT`, and hidden controls no longer
draw or present their own swap chain alongside the stereo output.

Live read-only verification with Crimson Desert at 3840x2160 found the game in
the foreground, the stereo window visible/topmost with no covering windows, and
Windows hit tests at 10%, 50% and 90% of the screen all targeting the game
(`reports/overlay-focus-20260922-220850.log`). Earlier focus transitions to the
shell and back also left output visible. Presentation timing misses still
occurred with the game and stream running; window-order success does not prove
that GPU-load flashing is resolved.

The user confirmed continued flashing and eye discomfort. Live 3D was stopped
immediately. A stereo-only trace (stream mirror disabled) still recorded missed
deadlines while the overlay stayed above the focused application, so the mirror
is not the sole cause. This is an unresolved app presentation fault, not a
game-specific compatibility finding.

An interim automatic 2D fallback was removed at the user's explicit direction:
only the user decides when to disable 3D. Timing misses remain diagnostics and
update the refresh anchor without demoting running stereo to 2D. The
refresh-scaled queue repair above remains in place. The steady-image diagnostic
is an explicitly launched test tool, never an automatic playback mode.

Regression coverage includes focus/minimize/hidden-source window placement,
the real fullscreen capture/F11 route, repeated composed misses, and actual
stream shader pixels through L/B/R/B cycles. None of these tests measures the
receiving Discord stream or proves that GPU-load flashing is fully resolved.

## Game stereo: 32 pairs/s cap and the presenting game window (2026-09-20)

Psychonauts 2 through the geo-11 direct-eye runtime, 3840x2160 HDR / 240 Hz,
Left / Black / Right / Black. The user reported the 3D at about 30 fps, black
flashes visible on the screen itself, the game window "fighting" the 3D window, and
AI desktop also at 30 fps.

Evidence (`reports/framerate-repair-backup-20260920-154445`): the app presented at
240/s throughout while the provider delivered exactly 32.0 pairs/s (`max-gap`
33.5 ms) after delivering 120 pairs/s in the menu. Every direct-eye pair made two
GPU-completion polls with a 1 ms sleep. Windows 11 does not honour a raised timer
resolution for a process whose window is covered, so each sleep lasted one 15.625 ms
tick: 2 x 15.625 ms = 31.25 ms = 32.0 pairs/s. The screen conversion had the same
fault: a "1 ms" event wait per loop against a 16.7 ms pair interval renders on every
second tick, again 32 pairs/s. The emitter's USB side was healthy in the same log
(0 errors, 120 commands/s); `blackOnImage` rose to 405 and composition changes to 22.

Changes:

- `shortWait()` (`src/gpu_completion.h`): a 0.5 ms high-resolution waitable timer,
  optionally also waiting on an event. It replaces the 1-2 ms sleeps in the direct-eye
  producer and reader, the geo-11 output adapter, window capture and screen conversion.
- The reader announces that the app's output covers the game window and the pair
  interval it can show (see `DIRECT-EYES.md`). While covered, the in-game adapter stops
  presenting its own 4K swap chain at 240 Hz underneath the app's output and admits
  one game frame per announced interval (60/s for this sequence at 240 Hz).
- Fullscreen output with a game as the source on the same display now uses the
  click-through, non-activating game overlay and leaves the game focused, instead of
  taking the foreground. `tools/Set-LiveGameOverlay.ps1` is no longer needed for that.

Verification: Release build; core, direct-eye transport and conversion shader tests;
`Test-Geo11Capture.ps1` x64 (sbs uncovered; sbs and katanga_vr covered: 0 presents/s
while covered, 90 ordered pairs); `--smoke-test --source-smoke` and
`--smoke-test --games --game-overlay-smoke` pass. Installed the app, both runtime
architectures under `build/bin/Release/runtime`, and the x64 runtime in the
Psychonauts 2 folder. Not yet checked under gameplay: the pair rate in
`VisionStereo11.log` (`pairs/s`, `covered=1`), `blackOnImage` in `session.log`, and
what the user sees. Removing the second swap chain removes one known contender for
the display path; it does not establish that every black flash is gone.

## Direct-flip recovery repair (2026-09-18)

Window capture at 3840x2160 HDR / 240 Hz again produced black flashes visible
without glasses. The session recorded bursts up to 162 timing misses in a reported
second, including while direct flip remained active. Capture continued publishing
complete pairs. Misses are prediction/command diagnostics, not a flash count.

The presenter applied the DWM two-report offset filter to direct flip too. If the
present-to-refresh offset changes on successive reports during repeated missed
refreshes, that filter can keep the old mapping indefinitely. Future black/image
slots are then selected against a stale refresh prediction. Direct-flip feedback
now updates the anchor immediately; composed or unknown presentation retains the
two-report filter for the previously observed DWM wobble. Resize, cadence changes,
and resynchronization clear the anchor. No display mode, BFI sequence, queue depth,
or emitter firmware change is part of this repair.

The session log now includes `blackOnImage` (sampled reported scanouts where an
inserted black frame landed in an image slot) and `clockReacquires`. These counters
do not inspect source pixels or measure the physical screen. The status message no
longer incorrectly promises that a miss cannot cause blanking or affects only one
refresh: already queued frames can remain wrong until the corrected frames arrive.

Verification: Release build; core regression for sustained direct-flip slips,
composed wobble, persistent composed delay, mode transition, duplicate feedback,
and present-counter rollover; GPU texture integration; screen conversion shader;
and hidden UI/presenter startup smoke all pass. Build: `build/capture-flashing`.
The verified executable is installed at `build/bin/Release/VisionRestoration.exe`
and the app was restarted. SHA256:
`04E8E71CA0541ED5CB1F998E80522C60E626ABF4438086B0D451CC91C0B58B5D`.
Previous executable, profiles and log are in
`reports/capture-repair-backup-20260918-054437`.

The game/source window had closed before installation. Visual validation under
the original gameplay load remains pending. The repair removes a reproducible
recovery defect; it does not establish that all reported black flashes are fixed.

## Opaque output overlay (2026-09-15)

The user reported visible black flashes while watching a movie with AI desktop
conversion and a Blender render running. The live session used 3840x2160 HDR at
240 Hz, Left / Black / Right / Black, Depth Anything V2 Small, and an RTX 5070 Ti.
USB errors remained zero and resyncs remained at one while presentation timing
mismatches accumulated. The log's `misses` counter combines prediction mismatches,
short trigger lead and late USB commands; it is not a count of visible flashes.

## Controlled live check

`tools/Test-OverlayOpacity.ps1` changed only the workspace app's output window
alpha, then restored the exact previous attributes in a `finally` block. Playback,
the model and Blender were left running. Evidence: `reports/overlay-opacity-ab.log`.
Session timestamps below are QPC seconds, not wall-clock times.

| Window alpha | Session interval | Additional timing misses | Presentation path |
|---|---|---:|---|
| 254, original | 69796.2–69812.3 | 398 | DWM composed |
| 255, opaque | 69812.3–69826.4 | 9, including transition | Direct flip by the ending sample |
| 254, restored | 69826.4–69842.5 | 194, including transition | DWM composed again |

The opaque interval's later samples reported zero slips per second. Restoring 254
returned to 24 slips in the ending second. Resyncs stayed at one and USB late
commands at 16 throughout this A/B/A check. These short windows support removing
forced composition; they do not prove that every GPU workload will be glitch-free.
Later desktop activity can still cause composition transitions and timing misses.

## Change

App output overlays now use alpha 255. Alpha 254 deliberately forced composition
to avoid an older behavior that resynchronized on composition-mode changes. That
resync rule has already been removed, so keeping the workaround imposed unnecessary
composition work on every stereo refresh. `WS_EX_LAYERED | WS_EX_TRANSPARENT`,
no-activate behavior and capture exclusion remain in place. Both screen conversion
and Blender viewport overlays use this creation path. Plain fullscreen output and
the game hook do not use this alpha setting.

Microsoft describes how independent flip and hardware overlay planes can bypass
desktop composition in [DXGI flip-model performance guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model).
Windows still decides which path is available; opaque output does not guarantee
independent flip under every desktop/window configuration.

The diagnostic also accepts `-ApplyOpaque` to apply the same change to an already
running workspace app without restarting playback. It must run on the user's
interactive desktop. The default mode always restores the prior opacity.

Optical alignment, visible flashing and mouse interaction require the user's live
check. Counters alone cannot establish them. No emitter firmware or timing change
is part of this repair.

## Verification and installation

- Release app and test targets built in `build/opacity-fix`.
- All seven CTest tests passed: core, depth, screen conversion shader, panel
  experiments, RP2040 scheduler, RP2040 host/waveforms, and GPU texture integration.
- After applying opaque output again, misses rose by 7 over session seconds
  69908.8–70029.3 (120.5 seconds); composition changes rose from 14 to 50, with no
  new resync. Some misses remain. Snapshot: `reports/gpu-load-presentation-live.log`.
- Installed the verified executable at `build/bin/Release/VisionRestoration.exe`;
  retained the previous binary as `VisionRestoration.pre-opacity.20260915-010731.exe`.
  Installed SHA256: `B28B2326610E406EA9330A75A0C2EAC4DF671E0FD83F347EA20A76FECFC7E2C2`.
  The original app process stayed running with the live opacity adjustment applied.
- User confirmation of visible flashing, optical alignment and controls is pending.

## AI desktop clock interruptions (2026-09-22)

The user reported moving black bands in the glasses and repeated manual resyncs
after switching to AI desktop. The running app used HDR 3840x2160 at about
239.982 Hz, L/B/R/B, and six queued presentation frames. A pre-change log snapshot
is in `reports/ai-desktop-before-clock-fix.log`. It records both presentation
misses and clock reacquisitions. In one interval the clock reacquisition count
increased with no increase in presentation misses or composition transitions.
This is evidence of an additional clock-recovery problem; it is not an optical
measurement or proof that every reported black band has the same cause.

Two recovery paths inserted extra black frames: one unusual timestamp discarded
the fitted refresh clock, and `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` suspended the
emitter and restarted a sixteen-frame acquisition blank. The presenter now keeps
the current clock across an isolated timestamp outlier or unavailable statistics.
Eight distinct, consistent samples must establish a replacement clock. The
presentation anchor follows only statistics in the accepted clock's counter
domain. Explicit user resync still clears timing state.

An anchor correction could also enqueue the same refresh twice with slightly
different predicted times. Emitter submission now rejects repeated/backward
deadlines and spacing below half an eye period, including commands already taken
out of the FIFO. This prevents duplicate commands from perturbing the firmware's
period lock. A new explicit synchronization generation clears that history.

Diagnostics now distinguish `clockOutliers`, `clockCorrections`, `statsDisjoints`,
and emitter `repeated` commands from ordinary misses. No automatic 2D fallback,
calibration adjustment, emitter firmware change, or AI quality reduction was added.

Verification: core, GPU texture integration (simulated emitter), fullscreen window
ordering, and stream-picture tests passed. The clock test covers three minutes of
synthetic 239.982 Hz timing with recurring outliers, duplicate reports, confirmed
phase/rate/counter changes, and missing-statistics intervals. Queue tests cover
duplicate deadlines before and after dispatch and explicit resync. These tests do
not establish optical stability through physical glasses. The current live app
was left running; the new executable takes effect on the user's next relaunch.
