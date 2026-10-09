# Vision Restoration Alpha 0.3.5

The project's first regular GitHub release, following the earlier prereleases.
The release tag is **v0.3.5** and GitHub's prerelease flag is off. The application
retains **alpha** maturity: game compatibility, sustained performance and optical
quality still depend on the source, display and calibrated emitter setup.

Windows 10/11 x64.

## Downloads

- **Vision-Restoration-Portable-v0.3.5.zip**: extract the entire archive and run
  **Start Vision Restoration.cmd** or **Launch.cmd**. Includes the app, AI
  runtime/helper, OpenVR runtime, x86/x64 game adapters, emitter tools, RP2040
  firmware, VR input launchers, usage guides, release notes and both setup PDFs.
- **Vision Restoration README.pdf**, **Vision Restoration Quick Start.pdf**
  and **SHA256SUMS.txt** are also available separately.
- The release tag includes the matching source code.

Exit the old application before launching the updated copy. Keep your existing
profiles when upgrading. NVIDIA emitter firmware, AI model weights, SteamVR,
Wibble Wobble/VRto3D, Geo11/game fixes, user profiles, recordings and diagnostic
logs are not included. VLC playback requires a complete installed or portable
64-bit VLC 3.x runtime.

## Changes since v0.3.0

- **Wibble Wobble / SteamVR eye input:** connects to the running compositor's
  left and right eye textures, with automatic connection and preview when the
  display and emitter are ready. Reconnection preserves an active output mode.
  During stereo, the competing Wibble display window moves off-screen and is
  restored on Stop. Administrator SteamVR sessions have an explicit reconnect
  action using Windows UAC. See the [input guide](https://github.com/pancreations/nv3d-vision-restoration/blob/v0.3.5/docs/WIBBLE-WOBBLE.md).
- **Dedicated VRto3D SBS / TAB capture:** discovers the VRto3D window, supports
  Half/Full SBS and Half/Full TAB with matching eye proportions, and lets the
  captured picture and game input window be selected separately. This route
  uses Windows Graphics Capture. See the [setup guide](https://github.com/pancreations/nv3d-vision-restoration/blob/v0.3.5/docs/VRTO3D.md).
- **Capture controls and diagnostics:** window capture and AI desktop have
  separate saved backend choices and report their active capture status.
  Windows Graphics Capture remains the default for window sources. Starting
  output also starts a selected window source. DXGI window capture disables
  the 2D stream view to avoid capturing its own visible output.
- **AI depth controls:** optional map levels, contrast, gamma, local detail
  and inversion, with live previews and presets. Adjustments require opt-in
  each session. Depth stabilization handles flat estimates, isolated noise,
  changing depth scale, scene cuts and varying inference rates.
- **Timing recovery:** retargets future presentation and shutter deadlines
  after stalls, reacquires the official emitter's phase after cadence gaps,
  and preserves accepted RP2040 events after recoverable scheduling misses.
  Timing-pass measurement restarts on detected misses. Stereo retains the
  refresh-scaled presentation reserve; these changes do not establish
  uninterrupted gameplay or optical synchronization.
- **Application integration:** adds an application icon and a graceful
  `--restart` path. Fullscreen/F11 starts stereo after eye inspection.
- Updated usage, source guides, download links and portable setup PDFs.

## Validation

- Release application and AI helper built successfully, together with both
  x86/x64 game integration runtimes and RP2040 firmware 0.4.1.
- All 25 CTest cases passed, covering capture backends, VRto3D packing and
  controls, Wibble eye layout and controls, SteamVR window handoff, depth
  stability, VLC decoding, presentation, timing and emitter simulation.
  Four desktop-capture cases required a rerun outside the restricted sandbox.
- Firmware was built and packaged; no hardware was flashed for this release.

These software checks do not establish sustained gameplay or optical quality.

## Known limitations

- Wibble Wobble input uses SteamVR compositor mirrors. The API exposes the
  eyes separately without a shared frame ID, so temporal alignment is not
  guaranteed. Wibble's later desktop reprojection and postprocessing are not
  included. Actual VR gameplay and optical synchronization remain unverified.
- VRto3D packed capture, AI desktop and movie viewing still need sustained
  gameplay/content and through-the-glasses checks on the target setup.
- DXGI Desktop Duplication gameplay capture remains unresolved. Synthetic
  capture tests do not establish working game capture beneath the output.
- Direct VLC playback and the 2D stream view use SDR pixels.
- LCD/QLED/Mini-LED ghosting remains unresolved.
- Resident Evil 2 and Metal Gear Solid V retain unresolved failures; broad
  Geo11/3Dmigoto compatibility and native NVIDIA-driver stereo are not supplied.
- Generic DX12, DX9/10 and Vulkan stereo integration remain unsupported.
