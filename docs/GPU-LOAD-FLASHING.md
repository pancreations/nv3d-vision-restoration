# GPU-load flashing

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
