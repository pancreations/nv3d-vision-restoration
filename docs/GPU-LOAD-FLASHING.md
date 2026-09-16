# GPU-load flashing: opaque output overlay (2026-09-15)

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
