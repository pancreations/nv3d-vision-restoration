# Vision Restoration v0.2.0 - experimental portable release

This release brings together presentation-clock recovery, fullscreen/input fixes,
recording output and BFI controls.
Windows 10/11 x64; experimental prerelease.

## Downloads

- **Vision-Restoration-Portable-v0.2.0.zip**: extract the entire archive and run
  **Start Vision Restoration.cmd**. Includes the app, AI runtime/helper, x86/x64
  game adapters, emitter tools, RP2040 firmware, release notes and both PDFs.
- Both setup PDFs and **SHA256SUMS.txt** are available separately.

NVIDIA emitter firmware, AI model weights, Geo11/game fixes, user profiles,
recordings and diagnostic logs are not included. Keep your existing profiles
when upgrading. AI desktop requires a separate model; VLC playback requires
installed 64-bit VLC 3.x.

## Changes since v0.1.1

- **Presentation recovery:** retain an established clock through isolated
  timestamp outliers and unavailable/disjoint frame statistics. Eight distinct,
  consistent samples establish a replacement clock. Both direct and composed
  output follow actual displayed-refresh statistics.
- **High-refresh scheduling:** reserve approximately 25 ms of presentation
  frames (six at 240 Hz), increase the emitter queue accordingly and reject
  duplicate/backward shutter deadlines, including already-dispatched commands.
  Added clock-outlier, correction, queue and duplicate-command diagnostics.
- **Fullscreen and input:** keep opaque stereo output over the selected display,
  repair window ordering without taking game input focus, and route captured-game
  fullscreen/F11 output through the same passthrough behavior. Source loss holds
  the last complete pair until output is stopped.
- **Recording and screen sharing:** an optional steady single-eye SDR stream view
  provides a recordable window or desktop mirror while stereo continues. It
  removes black slots and shutter-specific brightness/band corrections, supports
  either eye and runs at up to 60 fps. AI desktop already excludes its overlay
  from capture and therefore does not use this extra view.
- **BFI controls:** checkbox and shortcut select true black reset frames after
  neutral-reset experiments and preserve the selected timing mode. Diagnostics
  distinguish requested black insertion from presentation activity.
- **RP2040 setup:** OLED 240 Hz with Left / Black / Right / Black was reconfirmed
  by the user on 2026-09-26. A saved profile had BFI off; restoring BFI recovered
  sync while retaining the original phase, shutter widths and eye order. This
  was a profile correction; no firmware or driver change was needed. The public
  ZIP now includes the read-only USB/clock probe.
- **Geo11 startup:** preload an existing local NVAPI wrapper before Geo11 when
  using the proxy chain, so the fix can resolve its own stereo implementation.
- Updated setup guides, website, PDFs, RP2040 troubleshooting and release tools.

## Validation

- Release application and x86/x64 shared runtimes built successfully.
- All 16 main CTest suites passed, covering timing, profiles, BFI controls,
  fullscreen ordering/routing, stream-view pixels, RP2040 scheduling and
  waveforms, GPU texture retention, direct-eye transport and VLC playback.
- RP2040 firmware built successfully. UF2 payload, headers, boot checksum and
  reset vectors passed artifact checks; hardware was not flashed.
- Real Geo11 capture passed all five output layouts on both architectures
  (10 cases), each delivering 90 ordered eye pairs and exiting cleanly.
- The public ZIP passed extracted GPU/UI smoke checks and verification of all
  57 internal file hashes. Both regenerated PDFs have the correct version and
  RP2040/BFI instructions (README: seven pages; quick start: five pages).

The pre-release RP2040 check passed its USB clock test, all 1,200 standalone
120 Hz shutter commands, and the short 240 Hz BFI test without reported emitter
errors or late commands. The user confirmed that BFI restored glasses sync.

## Known limitations

- Short optical checks and synthetic GPU/transport tests do not establish
  sustained gameplay stability or eliminate all visible flashes under load.
- LCD/QLED/Mini-LED ghosting remains unresolved. Direct 240 Hz Left / Right is
  a different cadence from the confirmed 240 Hz BFI setup.
- Resident Evil 2 and Metal Gear Solid V retain unresolved failures. Broad
  Geo11/3Dmigoto compatibility and native NVIDIA-driver stereo are not supplied.
- Generic DX12, DX9/10 and Vulkan stereo integration remain unsupported.
- AI desktop and VLC optical quality still need validation; direct VLC and the
  recording stream are SDR. The older depth-based ReShade hook remains nonworking.
