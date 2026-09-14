# Third-party components

Original application: GPL-3.0-or-later, see LICENSE. Protocol-related implementation was adapted from libnvstusb evidence; preserve its attribution. Proprietary NVIDIA firmware is user supplied and is not included.

**No third-party code is redistributed in this repository.** `tools/Get-Dependencies.ps1` downloads each component below from its official source into `third_party/`, where it keeps its own license.

| Component | Version / source | License |
|---|---|---|
| libusb | [1.0.30](https://github.com/libusb/libusb/releases/tag/v1.0.30), official Windows binary | LGPL-2.1-or-later |
| Dear ImGui | [v1.91.9b](https://github.com/ocornut/imgui/tree/v1.91.9b) | MIT |
| Spout2 | [SDK 2.007.017](https://github.com/leadedge/Spout2/tree/2.007.017) | BSD-2-Clause |
| ReShade API headers | [v6.8.0](https://github.com/crosire/reshade/tree/v6.8.0), add-on API version 20 | BSD-3-Clause |
| libwdi (optional) | [pbatard/libwdi](https://github.com/pbatard/libwdi); a locally built x64 `wdi-simple` enables in-app WinUSB installation, otherwise use [Zadig](https://zadig.akeo.ie/) | LGPL-3.0-or-later |
| libnvstusb protocol reference | [eruffaldi/libnvstusb](https://github.com/eruffaldi/libnvstusb), Bjoern Paetzel / Johann Baudy | LGPL-3.0 |

libusb is dynamically linked. Spout and ImGui are built from the downloaded sources; the ReShade adapter uses upstream headers. `adapters/depth_tracker.h` follows the approach of ReShade's generic depth add-on (Copyright (C) 2021 Patrick Mours, BSD-3-Clause), as noted in that file. No code was copied from WibbleWobble, Open3DOLED, 3DVisionActivator or geo-11. Those projects remain research/compatibility references in PLAN.md.

# RP2040 firmware additions

Firmware source in `firmware/rp2040` and RP2040 host/model code in `src` are original GPL-3.0-or-later work. Protocol timing facts were studied from the AVR corrected IR table and NTM-3D references; their firmware source was not copied into this tree.

The firmware builds against Raspberry Pi Pico SDK 2.2.0 (BSD-3-Clause), TinyUSB revision 86ad6e56c1700e85f1c5678607a762cfe3aa2f47 (MIT) and the ARM GNU toolchain 14.3.Rel1. None of them is included; exact download URLs and SHA-256 hashes are in `firmware/rp2040/dependencies.json`. USB descriptors are authored using TinyUSB's public descriptor macros and the Microsoft OS 2.0 format. Retain dependency notices when distributing binaries. No proprietary NVIDIA firmware is included.
