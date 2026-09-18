# Vision Restoration v0.1.0 — experimental portable release

This release makes the current Windows app available for community testing. It is not a stable or universal game-compatibility release.

## Downloads and setup

Download **Vision-Restoration-Portable-v0.1.0.zip**, extract the entire ZIP to a writable folder, and run **Launch.cmd** or **Start Vision Restoration.cmd**. Both start the same app. The ZIP includes the updated **Vision Restoration README.pdf**, **README.md**, and **Vision Restoration Quick Start.pdf**. Both PDFs and **SHA256SUMS.txt** are also attached separately.

The app requires Windows 10/11 x64. Original NVIDIA emitters need user-supplied firmware and WinUSB setup; the PDFs explain both. Proprietary NVIDIA firmware, upstream geo-11/game fixes, AI model weights, saved profiles, and local diagnostic logs are not included. AI desktop needs a separately downloaded model. VLC movie playback needs an installed 64-bit VLC 3.x runtime.

## Included

- One app with the left **Tuning** and **Input/Games** tabs.
- **Prepare game**: select the actual game EXE before launch, arm the app, then launch normally. It watches the full EXE path, attaches to an available provider, and starts frame-sequential output when complete eyes and a foreground game window exist.
- x86/x64 geo-11 capture bridge, experimental direct-eye capture add-ons, and their required redistributable runtime files.
- Window capture, stereo images, VLC stereo movies, optional AI desktop, emitter utilities, calibration controls, and RP2040 firmware.

## Known limitations

- **Resident Evil 2 and Metal Gear Solid V remain unresolved and are not supported games in this release.** RE2 delivered stereo pairs before GPU device loss; MGS startup failures remain unverified after configuration changes. Prepare mode does not fix a crashing renderer.
- Classic 3Dmigoto fixes that depend on NVIDIA's discontinued stereo renderer still require that rendering backend. This app does not emulate it. Broad 3Dmigoto/geo-11/Katanga game compatibility is not established.
- Game output requires windowed/borderless mode on the selected output display. Native DX12, DX9/10, and Vulkan integration are not provided.
- Successful GPU/USB tests do not prove optical separation. OLED configurations have user-reported success; LCD/QLED/Mini-LED ghosting remains unresolved.
- VLC direct playback is SDR. The older depth-based ReShade hook is not working; the separate direct-eye capture add-on is experimental.

## Validation

All 13 CTest suites passed. Both runtime architectures and RP2040 firmware built successfully. The extracted public ZIP passed GPU and UI smoke tests, and the preparation watcher test covered exact-path matching, operation outside Input/Games, waiting for a provider, and cancellation. The ZIP's 55 internal checksums and public asset checksums were verified. Both PDFs were checked for the updated Prepare instructions and known game failures.

These checks do not establish crash-free gameplay or optical performance.
