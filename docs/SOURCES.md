# Stereo inputs

> **Status 2026-09-14:** only window capture of side-by-side output works, proven with Dolphin. ReShade capture does not work. The in-game hook does not work. geo-11 and games with native stereo 3D have never been tested.

Every input must already contain a complete left/right pair. Select SBS or top/bottom packing under **Stereo sources**, configure the source, then press **Start source**. Changing packing or sender settings takes effect when restarted.

| Input | Current path | Validation status |
|---|---|---|
| Built-in scenes | Direct shader output | Confirmed through the glasses on the G80SD, 4K 240 Hz HDR, black frame insertion, 2026-09-13 |
| PNG/JPEG/BMP/JPS | WIC, SDR only | Implemented; HDR image metadata is not supported |
| Dolphin | Native SBS output → Windows Graphics Capture | **Working** through the glasses. The only proven game source |
| Blender | Stereo SBS/TB image or native packed preview window | Application-specific test pending |
| SBS video | Player's packed stereo window → capture | Application-specific test pending |
| Spout | Packed shared texture on output GPU | Automated local sender test available |
| ReShade | DX11 packed backbuffer → included Spout add-on | **Does not work** |
| geo-11 / 3Dmigoto | Existing packed stereo output → capture or Spout adapter | **Never tested** |
| Games with native stereo 3D | Need the game's own stereo output in a form the app can read | **Never tested.** Unknown whether they work |

For capture, use a borderless window containing only the packed picture. Select **Capture HDR / scRGB** when capturing HDR content; Windows Graphics Capture uses an FP16 frame pool. A selected window may stop producing frames when minimized or under particular game presentation modes. The app holds the last complete pair and reports a stall. Closing/reopening the source requires selecting/restarting it.

For Spout, use the same GPU as the output display. Select the sender's actual encoding: SDR sRGB, linear scRGB, or HDR10 PQ/BT.2020. Spout texture format alone does not fully specify color interpretation. HDR output remains FP16; PQ is decoded into linear scRGB. HDR inputs shown on SDR output are explicitly tone-mapped by the shader; this is not HDR passthrough. Source images loaded through WIC are labeled SDR.

## Included ReShade adapter

**This adapter does not work.** The description below is the design, not a working path.

`build/Release/VisionStereoSpout.addon64` registers a DX11 sender named `VisionStereo_<process>_<runtime>`. Install it only in a test application's ReShade environment that supports add-ons. It copies the completed effect backbuffer into Spout. The backbuffer must already be SBS/TB; this adapter does not create stereo viewpoints. DX12/Vulkan/OpenGL output paths are not implemented in this adapter. Choose the sender in Vision Restoration and set the matching color encoding.

## Pair ownership

The producer keeps a pool of shared FP16 or SDR textures. Published frames carry packing, encoding, timestamp and pair ID. Shared ownership prevents recycling an in-use pair; keyed mutexes order GPU access. The presenter acquires new input only at a full stereo-cycle boundary and retains it for both eyes. A slow source repeats the previous complete pair. Source decoding/capture occurs on its own worker and GPU context.

Output can keep a captured application's focus while its fullscreen output window stays above it. Ctrl+Alt+F8 returns to setup. Mouse-relative games, exclusive fullscreen games and anti-cheat environments need individual tests; no universal input compatibility is claimed.
