# Frame-to-shutter synchronization

The presenter and game host use the same mapping between a DXGI present identifier
and the refresh where that image reached the display. `PresentRefreshCount` already
includes composed presentation. Adding another refresh for DWM delayed the test
presenter's shutter command; subtracting one in the game host selected the previous
eye. Composition mode now describes the output path without modifying that mapping.

Slips (2026-09-13): a frame that lands one refresh late, a command with too little
lead, or a late USB transfer is now ridden through. The eye sequence is anchored to
the display refresh counter and the emitter free-runs one period without a command
(its own eye bit toggles at every boundary), so only the slipped refresh shows the
wrong eye and the next frame is correct again. Earlier builds blanked 16 frames and
re-locked the emitter on every slip; at 144 Hz fullscreen on the Hisense this happened
every few seconds (session.log: 4 to 7 misses per short run, maxIntervalMs about two
refresh periods) and was visible as the image jumping with no input. Ordinary phase,
shutter, depth and convergence edits apply live.

No resynchronization on slips (2026-09-14): the evening build still forced a full
resynchronization (16 black frames, emitter suspended and re-locked) after four slips
within one second, and on every composition-mode change. With a 4K game (Psychonauts 2)
captured as the source, the game's command buffers delayed the presenter's draw behind
them and it slipped 14 to 23 refreshes a second (session.log: lead bin below zero rising,
present interval jitter above 1 ms, mode changes flat). The rule then chained
resynchronizations three to six times a second, and the viewer saw the glasses go black
for seconds at a time. An emulated N64 game had never loaded the GPU enough to hit the
rule. A resync repairs nothing in that state: the eye for each present is derived from
the observed present-to-refresh mapping on every frame, and the emitter's own eye bit
toggles every period, so neither drifts. Slips are now counted only (misses, plus a
per-second figure in the status line and session.log as slipsPerSec); a composition-mode
change is the same case, since the queued presents land one refresh off and the mapping
has caught up by the next frame. Full resynchronization remains for disjoint statistics,
an occluded or resized output window, an unavailable emitter, and cadence changes in the
settings. To prevent the slips themselves the process requires the graphics kernel's
REALTIME scheduling class (logged as gpuPriority; a refusal stops the output with an
error) and the presenter device requests GPU thread priority 7; the present thread
also opens each shared source texture once instead of on every pair.

GPU contention (2026-10-19): HIGH was not enough. Under a Cycles OptiX render on the
RTX 5070 Ti, experiments/gpu-contention measured the presenter-sized GPU submission
waiting up to 57 ms at HIGH, with 54-124 waits per 10 s longer than the six-refresh
flip queue (25 ms at 240 Hz). Each one let the queue run dry, so the display repeated a
frame and a black slot of Left/Black/Right/Black reached an eye. At REALTIME the same
load gave at most 1.4 ms and no wait longer than one refresh. A D3D12 HIGH-priority
queue reached 25 ms; GLOBAL_REALTIME queues are refused without privilege.

Refresh clock (2026-09-13, night): the presenter and the game host fit a least-squares
line through the last 240 presentation timestamps (refresh counter against QPC time) and
predict every vblank from that line. A single timestamp's jitter, measured at 1 to 6 us rms
on this PC, no longer moves the next command by its full amount, and the residuals are
reported as vblank jitter. A sample that lands more than a quarter period from the line, or
a period that differs by more than 8 %, restarts the fit. The blanking interval and the
position of the timestamp inside it are measured separately with the kernel scan-line
counter (docs/TIMING-CALIBRATION.md).

Regression checks cover queued presents, both refresh parities, counter rollover,
and a refresh anchor updated after a delayed frame. These tests establish software
mapping behavior; they do not measure optical leakage through the glasses.

Reference: Microsoft's [DXGI flip model and glitch recovery](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model)
and [DXGI frame statistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics).
