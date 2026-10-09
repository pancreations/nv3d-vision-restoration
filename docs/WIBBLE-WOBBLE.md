# Wibble Wobble input

**Input/Games > Source > Wibble Wobble** feeds the active SteamVR session's left
and right compositor eye images into Vision Restoration. Selecting the source
connects automatically and waits for the first image. When the NVIDIA USB or
RP2040 emitter is ready and the display rate and timing are compatible, an
embedded 3D preview starts with the current calibration and drives the glasses.

## Use the running session

1. Leave SteamVR, Wibble Wobble/VRto3D and the VR application running.
2. Select **Input/Games > Source > Wibble Wobble** in Vision Restoration.
3. Wait for the first image and the embedded 3D preview. If the emitter is not
   ready, connect it, match the display rate, then click **Start 3D preview**.
4. Use **Windowed preview** or **Fullscreen 3D** to change the output window.
   **Fullscreen 3D / F11** starts stereo even after eye inspection. Vision
   Restoration uses the current display and emitter calibration to control
   presentation and glasses timing for its own output.

**Inspect eye images** explicitly opens a 2D side-by-side view without driving
the emitter; selecting Wibble Wobble does not open that view automatically.
**Reconnect input** reconnects the source while preserving an already running
output and its stereo or inspection mode.

During stereo output, Vision Restoration temporarily moves Wibble's own display
window outside the desktop so its topmost loop cannot cover the picture.
**Stop**, a source change, or closing Vision Restoration restores its position.
It stays visible to its renderer and is never closed or minimized. If you move
that window yourself, your new position is preserved. This handoff applies to
Wibble direct-eye input; packed VRto3D SBS/TAB capture keeps its source in place.

Selecting Wibble's frame-sequential window under **Capture window** and starting
capture switches to this direct-eye input automatically. Capturing its
alternating eye/black desktop frames cannot supply a packed stereo pair.

`VisionRestoration.exe --wibble-wobble` selects this input and follows the same
automatic connection and 3D-preview startup. **Launch-WibbleWobble.cmd** does
the same and is included beside the executable in the portable package. The
source checkout version also builds the app if its executable is missing.
Source selection leaves the installed Wibble Wobble configuration,
driver and running SteamVR session unchanged. If Wibble Wobble currently owns
the same physical USB emitter, release its direct emitter control before using
Vision Restoration's stereo output.

## Administrator SteamVR

If SteamVR is running as administrator and Vision Restoration is running with
normal permissions, Windows can block their connection. The source reports the
mismatch and shows **Connect to administrator SteamVR**.

Click that button and approve the Windows UAC prompt to reopen Vision Restoration
with matching permissions and Wibble Wobble input selected. Cancelling the prompt
keeps the current app running. SteamVR stays open during this reconnect.

## Image and timing limits

The input uses OpenVR's `GetMirrorTextureD3D11` eye textures. These contain the
SteamVR compositor scene used by the Wibble Wobble/VRto3D session. Wibble Wobble's
later desktop reprojection, postprocessing and on-screen controls are outside
this input. The frame-sequential desktop window is not the capture source.

SteamVR must have valid compositor images. An idle session can show the
compositor's idle view, and a waiting status alone does not establish that a VR
application is delivering changing frames.

OpenVR exposes each eye separately and provides no atomic pair with a common
frame ID through this mirror API. Temporal alignment between the eyes is
therefore not guaranteed. Successful texture acquisition and GPU tests do not
establish optical synchronization, crosstalk, sustained frame rate or end-to-end
latency. This input remains experimental until checked with actual content and
the glasses on the user's setup.

## Build and redistribution

`tools/Get-Dependencies.ps1` downloads Valve's OpenVR SDK v2.15.6, pinned to
revision `0924064316de3effbcd1acf1e309182a2deb1c05`, into `third_party/openvr`.
The build needs its `headers/openvr.h` and x64 `bin/win64/openvr_api.dll`.
Keep `openvr_api.dll` beside `VisionRestoration.exe`; the portable release
includes it and the SDK's BSD-3-Clause license under
`licenses/OpenVR-BSD-3-Clause.txt`.

SteamVR, Wibble Wobble/VRto3D, firmware and USB drivers remain separately
installed. Source selection requires no COM-port configuration or emitter bridge
helper.
