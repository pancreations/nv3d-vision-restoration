# Vision Restoration Alpha 0.3.6

Released October 19, 2026 for Windows 10/11 (64-bit). Tag: **v0.3.6**.

## Highlights

### No more black flashing under heavy GPU load

If another program worked the GPU hard, for example orbiting a Cycles render in Blender's viewport or running a demanding game, the glasses could flash black or lose sync. Windows was making the 3D output wait behind the other program's GPU work for up to 57 ms, longer than the 25 ms of frames the app keeps queued. The app now asks Windows for realtime GPU priority, and in testing the wait dropped to about 1 ms with no flashes. Confirmed through the glasses while orbiting a Cycles viewport.

This applies to every source, not just Blender. If Windows ever refuses realtime priority, the app tells you instead of silently running at a lower priority.

### Other improvements

- **Steadier emitter sync:** when the display's timing shifts slightly, the original NVIDIA emitter now re-locks to the new timing straight away instead of drifting.
- **Smoother recovery from hiccups:** after a brief stall the display queue refills without doubling up frames.
- **Fullscreen menu inside the 3D picture:** pressing **Home** no longer opens a separate window over the output, which could cause flashes.
- **Black level slider:** zero is now neutral. Move it left to deepen shadows, or right to lift them as before.
- **AI desktop:** the depth search can run at 75, 50 or 25% resolution for speed, while the eye images stay at full resolution.
- **Remembers your stream view:** the 2D stream view, its cover-the-display mode and the chosen eye are saved between launches.
- **HDR is off by default** and marked experimental. When enabled it uses HDR10.
- **Friendlier documentation:** a rewritten README, user guide, quick start and project page.

### Also new since 0.3.0

- **Wibble Wobble / SteamVR input:** shows SteamVR's left and right eye images directly, connecting and starting the preview automatically. [Guide](WIBBLE-WOBBLE.md)
- **VRto3D input:** finds the VRto3D window and supports Half/Full side-by-side and top/bottom, with a separate game window for input. [Guide](VRTO3D.md)
- **AI depth controls:** optional depth map adjustments (levels, contrast, detail, invert) with presets, plus steadier depth that copes with noise and scene cuts.
- **Capture choices:** window capture and AI desktop each remember their own capture method.
- **Recovery after stalls:** presentation and emitter timing pick up again cleanly after a hiccup, for both the NVIDIA and RP2040 emitters.
- An application icon, and fullscreen (F11) now starts 3D straight from eye inspection.

## Upgrading

Download **Vision-Restoration-Portable-v0.3.6.zip**, extract it, and run **Start Vision Restoration.cmd**. Close the old version first. To keep your calibration, copy your `profiles` folder and `emitter.fw` from the old folder into the new one.

Not included, as before: NVIDIA's emitter firmware, the AI depth models and game fixes. The README explains where to get each.

## Validation

- Release build with all 26 automated test suites passing, including GPU rendering, capture, emitter clock recovery and window-order tests.
- Public ZIP checked after extraction: 95 files, internal hashes verified, GPU and UI smoke tests passed from the extracted copy.
- GPU-load fix measured with a Cycles OptiX render on an RTX 5070 Ti (worst wait 57 ms before, 1.4 ms after) and confirmed through the glasses on a 240 Hz OLED with black frame insertion.

## Known limitations

- LCD, QLED and Mini-LED screens still show ghosting between the eyes.
- Geo11 game fixes are experimental; Resident Evil 2 and Metal Gear Solid V still crash.
- VRto3D, Wibble Wobble, AI desktop and HDR output haven't been confirmed through the glasses yet.
- Some OLEDs buzz audibly with HDR at high brightness.
