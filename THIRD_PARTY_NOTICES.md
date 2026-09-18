# Third-party components

Original application: GPL-3.0-or-later, see LICENSE. Protocol-related implementation was adapted from libnvstusb evidence; preserve its attribution. Proprietary NVIDIA firmware is user supplied and is not included.

**No third-party code is committed to this repository.** `tools/Get-Dependencies.ps1` downloads each component below from its official source into `third_party/`, where it keeps its own license. Binary release archives redistribute only the runtime components allowed by their licenses and include the applicable license and notice files under `licenses/`.

| Component | Version / source | License |
|---|---|---|
| libusb | [1.0.30](https://github.com/libusb/libusb/releases/tag/v1.0.30), official Windows binary | LGPL-2.1-or-later |
| LibVLC (optional, user-installed) | [64-bit VLC 3.x](https://www.videolan.org/vlc/), loaded dynamically for stereo movie playback; no VLC binaries or SDK files included | LibVLC LGPL-2.1-or-later; VLC plugins retain their upstream licenses |
| Dear ImGui | [v1.91.9b](https://github.com/ocornut/imgui/tree/v1.91.9b) | MIT |
| ReShade runtime and API headers | [v6.8.0](https://github.com/crosire/reshade/tree/v6.8.0), add-on API version 20 | BSD-3-Clause |
| MinHook | [v1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), used by the in-game DX11 output runtime | BSD-2-Clause; retain its LICENSE.txt in binary distributions |
| libwdi (optional) | [pbatard/libwdi](https://github.com/pbatard/libwdi); a locally built x64 `wdi-simple` enables in-app WinUSB installation, otherwise use [Zadig](https://zadig.akeo.ie/) | LGPL-3.0-or-later |
| libnvstusb protocol reference | [eruffaldi/libnvstusb](https://github.com/eruffaldi/libnvstusb), Bjoern Paetzel / Johann Baudy | LGPL-3.0 |
| ONNX Runtime (DirectML build) | [1.22.1 NuGet package](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.22.1); `onnxruntime.dll` is loaded at run time by `VisionDepth.exe` | MIT |
| DirectML | [1.15.4 NuGet package](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4); `DirectML.dll` redistributable | Microsoft DirectML license (redistributable binary) |
| 7-Zip standalone console | [26.03](https://github.com/ip7z/7zip/releases/tag/26.03); unmodified x64 `7za.exe` used to open user-supplied NVIDIA packages without running installers | LGPL-2.1-or-later; matching source archive, upstream license and LGPL text included under `licenses/7zip/` |
| Depth Anything V2 Small | [onnx-community/depth-anything-v2-small](https://huggingface.co/onnx-community/depth-anything-v2-small), ONNX export of the Small model by Lihe Yang et al.; weights only, downloaded into `models/` | Apache-2.0 |
| Depth Anything V2 Base / Large (optional) | [onnx-community/depth-anything-v2-base](https://huggingface.co/onnx-community/depth-anything-v2-base), [depth-anything-v2-large](https://huggingface.co/onnx-community/depth-anything-v2-large); weights only, fetched on request by `tools/Get-DepthModel.ps1` | CC-BY-NC-4.0 (non-commercial; not included in the public release) |

libusb is dynamically linked. ONNX Runtime, DirectML and the depth model are used only by the optional whole-screen conversion (`VisionDepth.exe`); none of them is in the repository. ImGui is built from the downloaded sources; the ReShade adapter uses upstream headers. `adapters/depth_tracker.h` follows the approach of ReShade's generic depth add-on (Copyright (C) 2021 Patrick Mours, BSD-3-Clause), as noted in that file. The former Spout2 dependency and adapter were removed on 2026-09-15; older builds used its BSD-2-Clause SDK. No code was copied from WibbleWobble, Open3DOLED, 3DVisionActivator or geo-11. Those projects remain research/compatibility references in PLAN.md.

# RP2040 firmware additions

Firmware source in `firmware/rp2040` and RP2040 host/model code in `src` are original GPL-3.0-or-later work. Protocol timing facts were studied from the AVR corrected IR table and NTM-3D references; their firmware source was not copied into this tree.

The firmware builds against Raspberry Pi Pico SDK 2.2.0 (BSD-3-Clause), TinyUSB revision 86ad6e56c1700e85f1c5678607a762cfe3aa2f47 (MIT) and the ARM GNU toolchain 14.3.Rel1. None of them is included; exact download URLs and SHA-256 hashes are in `firmware/rp2040/dependencies.json`. USB descriptors are authored using TinyUSB's public descriptor macros and the Microsoft OS 2.0 format. Retain dependency notices when distributing binaries. No proprietary NVIDIA firmware is included.
