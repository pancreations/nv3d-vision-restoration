# Stereo inputs and game integration

Updated 2026-09-15.

## Available inputs

| Input | Behavior |
|---|---|
| Built-in patterns | Calibration and rendered stereo test scenes. |
| Stereo image | PNG/JPEG/BMP/JPS decoded as SDR; select SBS or top/bottom packing. |
| Capture window | Capture an application's existing packed stereo output. Proven with Dolphin on the OLED. |
| Whole screen (AI depth) | Capture a display and estimate depth; retain the click-through desktop output. |

Spout texture and Blender viewport inputs, their receivers and Blender's automatic
attachment have been removed. Spout is no longer a build dependency.

For capture, select a window containing the packed picture. The app crops window
decorations to its client area. A media player's toolbar is still part of its
client; it is not stereo video. Window minimization and certain presentation modes
can stop new frames. Closing/reopening the source requires selecting/restarting it.

Capture automatically uses FP16 scRGB to preserve HDR data. Source encoding and
SDR white are carried to the presenter; SDR output tone-maps HDR content. WIC image
files are labeled SDR. Packing changes take effect when the source restarts.

## Primary game target

The intended experience is to open Vision Restoration, launch an existing 3D
Vision game with its established fix, and let an in-game compatibility backend
supply full eye images and sequential presentation. The game keeps focus and input;
our app owns activation, profiles and the emitter. SBS capture is the last resort.

This functionality is **not restored yet**. The existing VisionGameHook experiment
is not working and is not a general stereo/NVAPI replacement. A successful installer
or a connected timing channel is not evidence of game compatibility.

See [the app-hosted architecture and acceptance tests](STEREO-COMPATIBILITY.md).
Geo11 is the first candidate for reusing DX11 fixes; native stereo interfaces and
legacy fixes require their own backends. AI desktop remains a separate feature.

## Pair ownership and capture input

The source publishes complete packed pairs with dimensions, encoding, timestamp
and pair ID. Shared ownership retains consumer frames; keyed mutexes order GPU
access. The presenter accepts a new pair at a full stereo-cycle boundary and holds
it for both eyes. Capture/decoding runs on a separate worker.

AI desktop uses a nonactivating, click-through output window. Ordinary captured-game
fullscreen startup currently activates the output window; aligned game capture is
a future fallback improvement. Global shortcuts are configurable under Shortcuts.
No universal input or exclusive-fullscreen compatibility is claimed.
