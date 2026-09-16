# Experimental RP2040 protocol and scheduler, version 1

Status: portable C++20 implementation, simulator, Windows USB client/application backend and RP2040 UF2 firmware are implemented. See [firmware build and setup guide](../firmware/rp2040/README.md). On 2026-09-15, actual hardware USB/clock checks passed after a device-specific WinUSB installation and host handling of TinyUSB's empty terminating packets. A ten-second app test recorded 1200 opens and 1200 closes. The user subsequently confirmed good OLED 120 Hz stereo. Firmware 0.4.0 adds frame-anchored LCD apertures and direct high-refresh cadence. See [generic LCD calibration](LCD-CALIBRATION.md) and [historical trial outcomes](RP2040-LCD-TRIAL.md). GPIO waveforms remain unmeasured. This protocol is NOT compatible with the NTM-3D NVIDIA-emulation firmware. The application must not send these packets to the original emitter or identify support from VID/PID alone.

Implementation: `src/rp2040_protocol.h`, `src/rp2040_protocol.cpp`. Validation: `tests/rp2040_tests.cpp`. Run `powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-Rp2040.ps1` from the workspace. The test runs 600 seconds of logical device time rapidly; it does not run a real ten-minute display test.

## Packet envelope

One message is exactly 64 bytes, unsigned fields little endian. Firmware assembles a full message before decoding and times out partial messages. The implemented transport is vendor bulk USB OUT 0x01 / IN 0x81 with MS OS 2.0 WinUSB descriptors. It is not HID. Windows transfers are bounded to 20 ms each and check cancellation between OUT and IN.

| Byte offset | Size | Meaning |
|---|---|---|
| 0 | 4 | ASCII VRP1 |
| 4 | 1 | Protocol version, 1 |
| 5 | 1 | Opcode |
| 6 | 2 | Payload length, 0 through 32 |
| 8 | 8 | Session epoch |
| 16 | 4 | Request ID, echoed by reply |
| 20 | 4 | Reserved, must be zero |
| 24 | 4 | IEEE CRC32 over all 64 bytes with these four bytes treated as zero |
| 28 | 4 | Reserved, must be zero |
| 32 | 32 | Payload followed by zero padding |

CRC uses reflected polynomial 0xEDB88320, initial/final XOR 0xFFFFFFFF. It detects corruption, not malicious senders. Invalid envelopes are discarded without a reply or command mutation. Output arguments remain unchanged on decode failure. Valid envelopes with invalid command lengths receive an error reply. Unknown versions, opcodes, reserved flags and nonzero padding are rejected.

## Commands

| Opcode | Name | Exact payload |
|---|---|---|
| 1 | Hello | Empty; starts a new session epoch, only after stop/disconnect/timeout |
| 2 | Clock | Empty; read-only clock exchange |
| 3 | Configure | u32 periodUs, u32 leftUs, u32 rightUs |
| 4 | Schedule | u64 eventSequence, u64 openAtDeviceUs, u8 eye (0 left, 1 right) |
| 5 | Stop | Empty; matching current session only |
| 6 | Status | Empty; read-only status |
| 7 | Diagnostics | Empty; read-only execution counters and last fault |
| 8 | ConfigureAperture | u32 periodUs, leftUs, rightUs, leftOpenUs, rightOpenUs, frameGuardUs (24 bytes) |
| 128 | Reply | 32 bytes, described below; never accepted as a command |

Firmware 0.3.0 accepts opening periods 8300-33367 us (approximately 30-120 eye openings/s, including held/black refreshes); 0.2.0 accepts only 8300-8367 us, intervals at least 250 us and no longer than period minus 500 us. The minimum keeps the longest IR token separate from the following token; it is not a measured optical limit. The host supplies absolute targets using the exact refresh ratio, e.g. 8333/8333/8334 us accumulation at 120 Hz. Phase shifts those targets; it is not an extra firmware delay. Changing timing after any scheduled event requires stop and a fresh session, preventing mixed configurations.

Reply payload offsets:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 1 | Result enum in header; Ok=0 |
| 1 | 1 | Original command opcode |
| 2 | 1 | Bit 0: configured; bit 1: extended cadence; bit 2: frame aperture; bit 3: fast cadence; bit 6: experimental hardware; bit 7: SIMULATION |
| 3 | 1 | Queue capacity, 16 |
| 4 | 4 | Queued frame windows, including an open window awaiting close |
| 8 | 8 | Device receive time, microseconds |
| 16 | 8 | Device reply timestamp, microseconds |
| 24 | 8 | Device boot identity |

Status replies override offsets 8/16 with last accepted epoch/current active epoch. Diagnostics replies override offset 4 with rejected-command count (u32), 8/16 with executed open/close command counts (u64), and 24 with the last scheduler fault (u64 enum). Neither is a clock sample. Other replies use the layout above. Clock timestamps bracket firmware request processing and reply enqueue; bus delay remains part of the host's uncertainty bound.

The simulator takes a boot identity and timestamps from its caller; firmware generates a random nonzero boot identity at startup. Host request IDs correlate replies; event sequences prevent replay. Host session epochs strictly increase within a device boot, starting above zero. Status lets reconnecting clients choose the next epoch and explicitly stop an existing session. Stop retains the last accepted epoch. Old stop packets cannot interrupt a newer session. Discover a reboot before choosing epochs; handle eventual integer exhaustion by explicit reconnect/reboot, not wraparound.

## Scheduler contract

- Fixed storage, no heap allocation on enqueue/advance. Earliest accepted open is 500 us ahead; furthest is 50 ms ahead. With 120 Hz cadence the horizon limits usable queued frames below the 16-entry capacity.
- Eyes alternate, windows cannot overlap, and consecutive targets must match the configured period within 100 us. A missing refresh requires stop/blank and a new session. Each eye window expands into an abstract Open followed by Close.
- A timer adapter must call `advance` at deadlines and independently service the 100 ms watchdog even if USB stops. No late-event catch-up: lateness exceeding 100 us discards the queue, invalidates the session, and returns forceIdle. Clock regression has the same behavior. Read-only queries and rejected commands do not renew the watchdog.
- The adapter must honor every forceIdle from stop, disconnect or advance, cancel pending waveform work and ensure the IR output is LOW. forceIdle is not itself an IR token and does not prove the lenses are open; their optical response must be verified.
- An event already physically emitted cannot be recalled. USB stop is subject to delivery latency. The host should queue only a short prediction horizon, not as many events as possible.
- The portable model is single-threaded. A firmware integration must serialize USB command handling with timer callbacks and keep interrupt critical sections bounded. An application thread must not call these objects concurrently without synchronization.

The firmware uses the corrected eye-token mapping and PIO pulse generator documented in the firmware guide. PIO instruction simulation verifies token durations and final LOW; actual pin timing, core scheduling jitter and physical shutter latency remain unmeasured. Firmware tracks a conservative token completion time including FIFO drain and rejects overlapping execution.

## Firmware 0.4.0 capabilities

Reply flag `0x02` identifies the 0.3.0 repeated/reset cadence extension, `0x04` supports ConfigureAperture, and `0x08` supports direct high-refresh cadence. The latter two must be checked independently; absent flags are not inferred from USB VID/PID.

Ordinary Configure retains its 12-byte format and timestamp semantics. 0.4.0 accepts opening intervals of 4,000?33,367 us; 0.3.0 accepts 8,300?33,367 us; 0.2.0 is restricted to 120 Hz LR. Host capability checks reject unsupported configurations before Hello arms a session.

ConfigureAperture accepts frame periods of 4,000?10,050 us. Each opening offset must be at least frameGuardUs; each offset plus duration must end by periodUs minus frameGuardUs. The minimum guard and duration are 250 us. In this mode Schedule's timestamp represents the **frame start**, and the firmware adds the selected physical eye's opening offset. Consecutive cadence is validated using frame anchors, not the differently adjusted eye openings. Physical overlap and the previous exposure's end guard against the next actual anchor are also checked. Invalid combinations, overflow and missing frame cadence are rejected without inserting events.

The app supplies measured presentation periods, rounds the frame inward, rounds opening offsets later and rounds duration down. The scheduler does not accept a duration that extends into the next eye. The host cancels and reacquires LCD timing after recognized schedule misses; unexpected transport faults remain errors.

## Clock mapping

ClockMap uses host send/receive and device receive/send timestamps to bound device-minus-host offset. It does not assume symmetric USB latency. Offset intervals are widened by an assumed 1000 ppm drift budget and timer quantization, then intersected across samples. Disjoint intervals reset confidence. Four samples are required; stale, highly uncertain or invalid mappings cannot produce a schedule estimate.

Mapping is limited to 50 ms into the future, at most 250 us uncertainty, and at most one second since the last exchange. Drift widening often invalidates it sooner. These are initial engineering thresholds, not measured performance. Poll the clock often enough to retain a useful bound. Subtract uncertainty from available lead time before accepting a host-side target. Round device targets deliberately; do not silently drop uncertainty.

Clock mapping alone does not predict a display refresh. The presenter still needs presentation feedback, a future-refresh prediction, and a decision to blank/reacquire after missed presentation. No path here measures photons or makes GPU present and USB atomic.

## Verified and remaining

Release tests cover framing/CRC, every one-bit mutation of an example packet, truncated/oversized input, 20,000 malformed packets, typed command acknowledgments, per-session ordering, stale stop, queue draining, timing overlap, timestamp overflow, missed refresh, late executor, host timeout, clock regression, asymmetric USB and 100 ppm simulated clock drift. A 72,000-frame run uses 100-1200 us varying simulated USB delay and verifies every Open/Close eye and sequence.

Now implemented: pinned ARM build and UF2, vendor USB firmware and Windows transport, identity checks, PIO/IR generation, calibration UI/backend integration and a predictive presentation path. The standalone `vision_rp2040_probe --clock` collects read-only hardware timing when a flashed board is connected. Enumeration/binding and OLED 120 Hz stereo have been exercised on hardware. Still pending: LCD optical evaluation with frame apertures, pin timing measurements and hardware validation of missed-presentation recovery. No hardware validation is claimed from the simulated tests.
