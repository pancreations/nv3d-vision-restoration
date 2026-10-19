# Developer notes

## Build

Requirements: Visual Studio 2022 (Desktop development with C++), the Windows SDK and CMake 3.24+. Third-party code is never committed; `tools/Get-Dependencies.ps1` downloads it into `third_party/` from official sources.

```powershell
.\tools\Get-Dependencies.ps1
.\build.ps1            # configures, builds every target and runs CTest
```

The app is built to `build\bin\Release\VisionRestoration.exe`. A running copy can't be overwritten; `VisionRestoration.exe --restart` asks a running instance to save and close, then opens the new build.

## Self-tests (no emitter needed)

| Command | Checks |
|---|---|
| `--gpu-test` | Renders every pattern and sequence on WARP and checks the pixels, SDR and HDR |
| `--smoke-test` | Starts the real UI on the GPU and saves a screenshot of it |
| `--probe` | Lists displays, GPUs and emitters |
| `--vblank` | Measures each display's vblank timing (`reports/vblank.txt`) |
| `--screen-test 10` | Runs the AI desktop conversion for 10 s without output; writes timings and PNGs to `reports/` |
| `--capture-pid <pid> --start-fullscreen` | Captures a running SBS program straight into fullscreen output (add `--stream-view` for the 2D view) |

These checks verify rendering and timing in software. Only the glasses can confirm clean optical separation.

## Benchmarks and experiments

- `vision_gpu_capture_bench.exe <report.csv> [idle|load] [waitable] [excluded-overlay]` compares the capture backends against a synthetic producer.
- `vision_direct_capture_bench.exe <report.csv> [load]` times the direct shared-eye source.
- `experiments/gpu-contention` (local only) measures how long presenter-sized GPU work waits while another process saturates the GPU, for each Windows scheduling class. This is how the REALTIME scheduling requirement was found; see [PRESENTATION-SYNC.md](PRESENTATION-SYNC.md).

## Diagnostics written at runtime

| File | Contents |
|---|---|
| `reports/session.log` | Status line every two seconds: presents, misses, resyncs, GPU scheduling class, emitter and capture state. The previous run is kept as `session-previous.log`. |
| `reports/miss-events.log` | Frame-by-frame trace around every missed refresh, including GPU completion time and slack |

## Technical documents

- [Presentation sync](PRESENTATION-SYNC.md): how presents, refreshes and emitter commands are kept aligned
- [Timing calibration](TIMING-CALIBRATION.md) and [emitter timing fixes](EMITTER-TIMING-FIX.md)
- [Stereo compatibility architecture](STEREO-COMPATIBILITY.md) and [direct eye transport](DIRECT-EYES.md)
- [Full screen at 120 Hz](FULLSCREEN-120HZ.md), [panel experiments](PANEL-EXPERIMENTS.md), [LCD calibration](LCD-CALIBRATION.md)
- [RP2040 protocol](RP2040-PROTOCOL.md)
- [Development log](STATUS.md)

## Releasing

See [RELEASING.md](RELEASING.md).
