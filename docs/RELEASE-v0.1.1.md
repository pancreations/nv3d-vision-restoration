# Vision Restoration v0.1.1 — experimental portable release

This maintenance release improves presentation recovery, game overlay behavior, and AI desktop responsiveness. It remains an experimental prerelease for Windows 10/11 x64.

## Downloads and setup

Download **Vision-Restoration-Portable-v0.1.1.zip**, extract the entire ZIP to a writable folder, and run **Start Vision Restoration.cmd** or **Launch.cmd**. The archive includes the application, AI helper and redistributable runtime, x86/x64 game adapters, emitter utilities, RP2040 firmware, README, and both setup PDFs. The PDFs and **SHA256SUMS.txt** are also attached separately.

Original NVIDIA emitters require user-supplied firmware and WinUSB setup. NVIDIA firmware, AI model weights, Geo11/game fixes, saved profiles, and diagnostic logs are not included. AI desktop needs a separately downloaded model; VLC movies need installed 64-bit VLC 3.x.

## Changes since v0.1.0

- Direct-flip presentation follows timing slips immediately instead of retaining a stale refresh mapping. Composed presentation retains its existing jitter filter. New `blackOnImage` and `clockReacquires` diagnostics help investigate flashes.
- High-resolution polling replaces waits that limited covered-game capture and AI desktop conversion to roughly 32 stereo pairs/s. Capture and depth scheduling retain a steady cadence after late wakes.
- The Geo11 adapter stops presenting its own swap chain while the app covers the game, and paces new pairs to the app's output interval. Fullscreen game overlays retain game input focus and tuning controls.
- AI desktop keeps mouse and keyboard passthrough across fullscreen/F11 and source changes. **Responsive desktop** selects Small at 518 px, low smoothing, and a saved limit of 60 depth updates/s.
- **Actual depth** shows measured depth updates, map age, and inference time separately. Desktop motion can remain smooth while depth updates run more slowly; actual rates depend on capture delivery and GPU load.
- Updated usage instructions, website, PDFs, and diagnostic tools. README PDF links now follow the release version automatically.
- Fixed an x86 compilation error in the diagnostic process runner used by runtime regression checks.

## Known limitations

- These fixes do not establish that every black flash is resolved. Sustained gameplay and optical validation remain pending.
- Resident Evil 2 and Metal Gear Solid V retain unresolved failures and are not supported games. Broad Geo11/3Dmigoto compatibility is not established; the discontinued NVIDIA stereo rendering backend is not supplied.
- Game output requires windowed/borderless mode on the selected display. Native DX12, DX9/10, and Vulkan integration are not provided.
- LCD/QLED/Mini-LED ghosting remains unresolved. AI desktop and VLC viewing through the glasses still require validation; VLC direct playback is SDR.
- The older depth-based ReShade hook does not work; the separate direct-eye add-on remains experimental.

## Validation

- All 13 CTest suites passed, including presentation-anchor regressions, profile persistence, GPU textures, and direct-eye transport.
- Release app, x86/x64 adapters and diagnostic helpers, and RP2040 firmware built successfully. UF2 structure, payload, boot checksum, and reset vectors passed artifact checks; hardware was not flashed.
- Real Geo11 capture passed all five output modes on both architectures (10 cases), plus covered-window SBS and Katanga capture on both architectures (4 cases). Every case delivered 90 ordered eye pairs and exited cleanly. Covered-window logs confirmed that the redundant swap chain stopped presenting.
- The extracted public ZIP passed GPU and UI smoke tests. Prepare-game watching, AI desktop fullscreen/F11 passthrough, and the game input overlay passed their dedicated UI smoke checks.
- Verified all 55 internal file hashes, the public ZIP checksum, excluded private/proprietary data, and version/content in both regenerated PDFs. The quick start has four pages and the README has seven.

Synthetic transport, GPU, and UI tests do not establish sustained gameplay or optical performance.
