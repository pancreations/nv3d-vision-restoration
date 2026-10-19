# Confirmed display-clock changes and NVIDIA emitter recovery

The user reports intermittent flashes and loss of glasses synchronization during
recent playthroughs. Most incidents recover automatically; some require manual
Re-sync. The active process uses the official NVIDIA emitter, Windows Graphics
Capture, 240 Hz and Left / Black / Right / Black.

Pre-change logs are preserved in `reports/sync-recovery-20261018/`. In the latest
snapshot, presentation misses reached 31, black-on-image observations reached 11,
and confirmed clock corrections reached 7. USB errors and late commands stayed
zero. The miss trace includes a 32.35 ms Present call and negative GPU completion
slack, exceeding the six-refresh buffer's approximately 25 ms reserve. These are
presentation timing faults; the counters are not optical measurements or a count
of every visible flash.

There is also a recovery gap in the NVIDIA path. `TimingTracker::observeContinuous`
accepts a replacement clock after eight consistent samples. The emitter formerly
learned only eye and deadline, and reset its firmware lock on repeated eyes or a
deadline gap greater than half an eye period. A confirmed 2 ms phase change at
240 Hz BFI passes neither condition: the eye period is approximately 8.33 ms.
The locked firmware's small phase nudges can therefore leave it catching up after
the display clock has already changed.

The previous session records a clock correction at QPC 40007.9 with no timing
block write, then manual resync increments at 40024.0 and 40026.0. The latest
session also records a correction at 41546.7 without a timing write. This supports
investigating the missing handoff; the logs do not record correction magnitude or
prove that this accounts for every reported incident.

The presenter now attaches its confirmed clock revision to each queued command.
The NVIDIA worker rewrites its existing timing block when it dispatches the first
command from a changed revision. Older queued commands retain their old revision.
A rejected duplicate or late command does not consume recovery. An explicit
suspend/new generation establishes a fresh baseline. No emitter disable, extra
black acquisition sequence, timing-profile edit, or firmware change is added.
RP2040 scheduling is unchanged.

Session logs expose `clockRelocks`, counting dispatched commands that requested
this recovery (also counted by the simulated emitter). The existing `writes`
counter continues to count actual NVIDIA timing-block transfers. A timing write
followed by a missed command deadline can be retried, so these counters need not
increase one-for-one.

The new `emitter_clock_recovery` test uses the real host FIFO and worker with
simulated USB. It covers queued old/new revisions, a 2 ms phase change missed by
the previous detector, duplicate rejection, late-command rejection and explicit
generation reset. Hardware optical behavior and sustained gameplay still require
a run with the updated application. Presentation starvation remains a separate
source of flashes.

Validation: Release application build passed. `core`, `emitter_clock_recovery`,
`captured_game_fullscreen_routing`, and `stream_view_picture` all passed, as did
the offscreen SDR/HDR rendering self-test. Build/test/pixel reports and installation
hash are in `reports/sync-recovery-20261018/`.

Installed at `build/bin/Release/VisionRestoration.exe`, with the previous executable
retained as `VisionRestoration.pre-clock-recovery-20261018.exe` in that directory.
The active application was left running; reopening it is required to load the
change. No post-change optical or gameplay result is claimed.
