# Vision Restoration v0.3.0 - Half/Full SBS and windowed preview

VLC movies, stereo images and captured windows now offer explicit Half SBS and
Full SBS input formats, preserving the intended proportions of each eye.
Windows 10/11 x64; experimental prerelease.

## Downloads

- **Vision-Restoration-Portable-v0.3.0.zip**: extract the entire archive and run
  **Start Vision Restoration.cmd** or **Launch.cmd**. Includes the app, AI
  runtime/helper, x86/x64 game adapters, emitter tools, RP2040 firmware,
  release notes and both setup PDFs.
- Both setup PDFs and **SHA256SUMS.txt** are available separately.
- The release tag includes the matching source code.

Exit the old application before launching the updated copy. Keep your existing
profiles when upgrading. NVIDIA emitter firmware, AI model weights, Geo11/game
fixes, user profiles, recordings and diagnostic logs are not included.
VLC playback requires a complete installed or portable 64-bit VLC 3.x runtime.

## Changes since v0.2.0

- **Half and Full SBS:** all three packed input sources share explicit packing
  choices. Half SBS restores horizontally squeezed eyes; Full SBS preserves
  native per-eye proportions. A combined 1920 x 1080 Half SBS frame and a
  3840 x 1080 Full SBS frame both display each eye at 16:9. Choose the format
  used by the content, then click **Start source** again after a packing change.
- **Aspect-correct output:** previews, fullscreen output, eye inspection and
  the 2D stream view preserve SBS proportions with black bars when necessary.
  Top / bottom retains its existing output-area scaling.
- **Windowed preview:** move, resize, minimize or maximize a separate 3D output
  window while keeping the main controls available. F11 switches to fullscreen
  and back to the previous placement. Esc or closing the preview stops it.
- **Capture across displays:** the captured window can be on another monitor;
  fullscreen stereo covers the selected output display regardless of the
  captured window's position or size.
- Updated source documentation, download page and setup PDFs.

## Validation

- Release build and all 17 main CTest suites, including real VLC decoding,
  Half/Full SBS image and Windows capture metadata, windowed/fullscreen
  transitions, direct-eye transport, timing and emitter simulation.
- Rendered-pixel checks for eye proportions, pillarboxing and letterboxing,
  SDR/HDR shader paths, inspection panes, eye boundaries and the 2D stream.
- Public archive hashes, allowed contents and extracted GPU/UI smoke checks.

These are software checks. They do not establish optical quality through the
glasses or sustained game stability. The emitter firmware and game adapters
are unchanged from v0.2.0; no hardware flashing is required for SBS support.

## Known limitations

- Direct VLC playback and the 2D stream view use SDR pixels.
- Movie optical quality and gameplay under sustained load need validation.
- LCD/QLED/Mini-LED ghosting remains unresolved.
- Resident Evil 2 and Metal Gear Solid V retain unresolved failures; broad
  Geo11/3Dmigoto compatibility and native NVIDIA-driver stereo are not supplied.
- Generic DX12, DX9/10 and Vulkan stereo integration remain unsupported.
