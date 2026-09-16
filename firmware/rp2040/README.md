# Vision RP2040 emitter firmware 0.4.0 (experimental)

Original firmware for the RP2040-Zero and Adafruit IR module. The compiled UF2 is `build/rp2040/VisionEmitter.uf2`. On 2026-09-15 it was flashed to the received board and enumerated as `Vision RP2040 Emitter v1`, serial `53032847387B8B9C`, USB `cafe:3d02`. Automatic WinUSB binding did not occur; after the user approved the device-specific package, installation succeeded. The read-only clock test and the app's connection worker now pass (`State: Ready`). The user subsequently confirmed good OLED 120 Hz stereo. The 0.3.0 preload trial failed to sustain useful glasses activity; ordinary LR activated them but still ghosted. Firmware 0.4.0 adds generic frame apertures and direct high-refresh cadence. See [LCD calibration](../../docs/LCD-CALIBRATION.md). Electrical waveforms remain unmeasured. The NTM-3D UF2 uses a different USB protocol and cannot substitute for this firmware in the new application backend.

## What is built

- Raspberry Pi Pico SDK 2.2.0, its TinyUSB revision `86ad6e56c1700e85f1c5678607a762cfe3aa2f47`, ARM GNU 14.3.Rel1 (GCC 14.3.1), C++20 and `waveshare_rp2040_zero` board definition.
- Vendor bulk USB OUT 0x01 / IN 0x81, 64-byte VRP1 packets. MS OS 2.0 descriptors request WinUSB automatic binding on modern Windows. First hardware enumeration succeeded, but Windows did not attach WinUSB automatically; the cause is under investigation.
- First host transport fixes: accept TinyUSB's trailing zero-length USB packets within the existing reply deadline; discard replies left by an interrupted client before beginning a new connection. Native tests cover empty terminators, truncated replies, the shared timeout and cancellation. Hardware clock testing passed all 197 samples after the first three initialization samples, with about 154 us final estimated uncertainty. This measures clock communication, not photons.
- Core 0 handles USB; core 1 polls the hardware microsecond clock and runs the bounded scheduler. A critical section serializes command handling and timing. PIO emits each short token from its FIFO at 1 MHz; no DMA or external clock is necessary for these four-word-or-shorter waveforms. Core scheduling/lock contention jitter is not measured yet.
- Startup/stop/USB suspend/unmount force GPIO2 LOW. The scheduler's 100 ms host timeout stops queued work; a 500 ms chip watchdog recovers a stalled main loop or timing core. Lenses may take their own timeout to become transparent after IR stops.
- Hardware replies carry an experimental flag, not a calibrated flag. The firmware's source fingerprint is available in USB string descriptor 5 and is recorded in profiles. Device boot IDs change across reboot; flash serial identifies the board.
- Status/clock/diagnostic reads do not emit IR. Only an explicitly started/configured session with scheduled events emits tokens. Firmware 0.4.0 supports 4,000?33,367 us between openings and 4,000?10,050 us frame apertures, with 250 us minimum duration and guard. Capability flags distinguish older builds.

The private development USB identity is `cafe:3d02`, product `Vision RP2040 Emitter v1`, bcdDevice `0200`. It is not an assigned production VID/PID. The application verifies product, firmware string, serial, endpoints and VRP1 responses before operation; it does not treat the ID alone as authorization to send arbitrary commands. Obtain an assigned identity before distributing a hardware product.

## Build and verify

The Pico SDK, TinyUSB and ARM GNU toolchain are not included in this repository. Download the exact versions listed in `dependencies.json` (URLs and SHA-256 hashes) and extract them into `third_party/`; nothing needs to be installed into the system PATH.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Build-Rp2040.ps1 -Python3 'C:\path\to\python.exe'
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-FirmwareArtifact.ps1
```

Pass `-ToolchainRoot` if the ARM distribution is extracted somewhere else. Python 3 is needed by SDK boot2 packaging; Python 2 on this PC's PATH is unsuitable. CMake caches the Python path after the first successful configuration. `Build-Rp2040.ps1` creates ELF, BIN and UF2, prints SHA256, and never flashes or changes USB drivers. Firmware compilation needed execution outside this session's sandbox because Ninja's compiler probe stalled inside it.

The UF2 converter uses 256-byte data blocks for the RP2040 family at 0x10000000. The separate artifact test compares every payload byte against the linked binary, checks headers/family/addresses/padding, verifies the SDK boot2 checksum, and checks Cortex-M0+ reset/stack vectors.

## Flashing and connection

For the first flash, put the RP2040-Zero in BOOT mode using the button procedure below, then run this from the project folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Flash-Rp2040.ps1
```

The helper checks the `RPI-RP2` removable drive and its ROM identity, verifies the UF2 against the linked binary, programs flash, and waits up to 30 seconds for the runtime device. It then checks the product/serial/firmware identity and clock with the read-only probe and saves a dated report under `reports/`. It sends no IR commands. If several boards are in BOOT mode, specify the intended drive with `-Drive H:` (use its actual letter). Disconnect any other already-flashed Vision RP2040 emitter during this check. A failed clock check means the USB timing still needs investigation; it is not a successful bring-up.

Once WinUSB is available, the app now connects the single RP2040 at startup when no NVIDIA emitter was selected. For manual selection, use **Advanced > Refresh USB > Emitter > RP2040 candidate**, then **Reconnect emitter**. RP2040 timing autosaves to `profiles/autosave-rp2040.ini`, and startup profile loading keeps the emitter families separate. The backend retains fixed 120 Hz LR compatibility with 0.2.0; 0.3.0 enables repeated/reset sequences with 30-120 eye openings/s. `VisionRestoration.exe --emitter-check` tests the app's actual connection worker for a single supported emitter and writes `reports/emitter-check.txt`; it emits no IR without the separate `--emitter-pulse` option.

1. With power disconnected, verify the board is RP2040-Zero (not RP2350/Pi Zero) and check printed pin labels. For Adafruit 5639, intended connections are In to GPIO2, GND to GND, and V+ to USB-derived 5 V after confirming the received board's power path. The module's transistor drives the LEDs; GPIO2 supplies only logic. See [Adafruit specifications](https://www.adafruit.com/product/5639) and [Waveshare pinout](https://www.waveshare.com/wiki/RP2040-Zero). Use a USB data cable.
2. Initially flash the RP2040 by itself. Hold BOOT, connect USB, release BOOT and locate `RPI-RP2`. Confirm `INFO_UF2.TXT` identifies `Model: Raspberry Pi RP2` and `Board-ID: RPI-RP2`. Copy **only** `build/rp2040/VisionEmitter.uf2` to that drive. This programs board flash and reboots; it is not the NVIDIA RAM loader. BOOTSEL remains the recovery path.
3. Run `build/bin/Release/vision_rp2040_probe.exe --list`. Then, with exactly one emitter connected, run `--clock` and save its output. These commands are read-only. If USB opening fails, inspect Device Manager before considering any device-specific driver binding; the firmware is designed to request WinUSB automatically. Do not bind a driver to `RPI-RP2` or unrelated USB devices.
4. Disconnect power, attach the IR module using the verified wiring, then reconnect. Open `Launch.cmd`, select the intended output at fixed 120 Hz LR and Refresh USB / Connect emitter. The app does not flash firmware. Select SDR or HDR deliberately; both remain supported.
5. Open a preview first. When ready for the optical test, disable Preview and start the glasses-check pattern. Confirm actual lens response, test eye order, then adjust phase and duration. Esc/Ctrl+Alt+F8 stop output. A connected USB device, clock estimate or LED indication is not successful glasses calibration. The RP2040-Zero's onboard RGB LED is not driven by this firmware.
6. Collect the timing report, assess top/center/bottom separation and motion, and run a real ten-minute test. Repeat HDR separately and calibrate the Hisense independently at 120 Hz. Test OLED 240 Hz LR and 240 Hz BFI independently; these have not yet been optically validated.

## IR interpretation and remaining risks

The waveform table follows the corrected protocol facts in [b3nn's AVR fix](https://github.com/b3nn/3DVisionAVR/blob/fix-3dvision-irprotocol/3DVisionAVR/IRProtocols.h): open-left 43 us; open-right 23/46/31 us; close-left 23/21/24 us; close-right 23/78/40 us. Three-segment tokens alternate HIGH/LOW/HIGH. All finish LOW. The older AVR mapping differs. No AVR/NTM firmware source was copied into this implementation.

The host schedules the start of each open/close token. Token completion and lens transparency occur later. Requested duration is the separation between token starts, not measured optical opening. The physical glasses must verify token mapping, brightness/range and minimum useful intervals.

Clock bounds assume up to 1000 ppm drift and gate scheduling at 250 us estimated uncertainty. USB clock reads timestamp software boundaries; asymmetric delay remains uncertain. The host can refuse operation if the bound/lead is insufficient; gather `--clock` evidence instead of weakening checks blindly. The presenter sends a predicted future event before Present and checks actual refresh statistics afterwards. If the presentation misses, an already emitted event cannot be undone; output stops/blanks and reacquires. This is not atomic GPU-plus-USB synchronization.

The user has confirmed OLED 120 Hz stereo. The new LCD aperture and OLED 240 Hz modes still need optical validation; timing-core latency, pin waveforms and GPU-load recovery need hardware measurement. Simulation does not establish those results.
