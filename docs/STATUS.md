# Implementation and validation status

## Experimental v0.1.0 release, 2026-09-17

The portable release contains one app with the left **Tuning** and **Input/Games** tabs, prelaunch **Prepare game**, direct-eye transport, VLC stereo movie input, window capture, AI desktop, and emitter/calibration tools. Prepare watches the exact selected executable outside the visible Games panel, attaches when a provider appears, and starts frame-sequential output when complete eyes and a foreground window are available.

**Known failures remain:** Resident Evil 2 loaded the bridge and delivered pairs, then reported a GPU hang/device loss. The latest standalone geo-11 comparison was reported unsuccessful by the user; its failure has not been diagnosed. Metal Gear Solid V also has unresolved reported startup failures; isolated reproduction of a geo-11 deferred-context crash and an INI workaround do not establish working gameplay. Neither game is supported by this release. Classic NVIDIA-driver-dependent 3Dmigoto rendering is not supplied by the app. LCD/QLED/Mini-LED optical separation remains unproven/nonworking in reported tests.

The Prepare watcher UI test covers matching the full executable path, waiting for a provider, operation outside Input/Games, and cancellation. Transport tests check complete ordered eye pairs. These results do not prove real-game stability or optical timing. Entries below record earlier work and may describe superseded states.


## Combined left workspace, 2026-09-17

The left side has exactly **Tuning** and **Input/Games**. Game selection, adapter connection, source controls, matching game window and provider readiness now share one tab. Capture stays disabled until a live provider is available. Build, hidden UI/presenter smoke, connection regression and direct-eye transport tests pass. These checks do not establish optical or gameplay performance.

MGSV's original v1.6 fix matches all 67 author archive files. A capture add-on startup recursion with its 3Dmigoto 1.2.50 was reproduced and repaired; the isolated legacy-provider fixture now transfers pairs and exits cleanly. Original game/fix files were preserved. Classic NVIDIA stereo rendering remains a separate unsolved requirement.

## Geo11 direct-eye connection repaired, 2026-09-17

**Input/Games > Connect game** now recognizes an existing geo-11 fix and connects it through the existing VisionStereo11 proxy, without ReShade or a replacement renderer. The provider's DLL bytes, shader resources, `d3dxdm.ini` output mode and tuning are preserved. The app receives explicit full-resolution eyes from Katanga and owns sequential display; existing SBS/TAB modes remain supported at their supplied resolution.

The production x86/x64 runtimes passed real geo-11 0.7.11 geometry capture in all five supported output modes: 90 ordered stereo pairs per case, distinct left/right geometry, stalled-consumer backpressure and clean exit. The updated adapter keeps a paused reader's pending frames instead of treating consumer backpressure as a five-second GPU failure. The updated app is in its normal location, `build/bin/Release/VisionRestoration.exe`; the separate review executable was removed.

The earlier failure with ReShade is avoided by selecting geo-11's own proxy interface. The independently reproduced upstream shutdown crash was isolated to the sample help-overlay include: removing just that include from the isolated sample also produces a clean exit. User shader fixes are not stripped or altered. Gameplay, sustained load and optical shutter timing still require validation. See [Direct stereo capture](DIRECT-EYES.md) and `tools/Test-Geo11Capture.ps1` for the reproducible scope; earlier dated entries below describe earlier states.


## VLC stereo movie source implemented, 2026-09-16

The app now plays local SBS and top/bottom movies directly through an optional,
user-installed 64-bit VLC 3.x runtime. VLC supplies timed SDR video pixels and
audio; complete pairs feed the existing app presenter and emitter timing.
Input includes movie/VLC selection, pause/resume, volume, seeking and stop.
VLC preferences are ignored for this embedded session and are not changed.
Window capture is not used for this source.

The real installed VLC 3.0.16 passes generated AVI decoding and shared GPU
readback of distinct left/right colors in both layouts, a Unicode file path,
pause/resume, seeking, end-of-file retention, source restart/stop, missing files
and invalid packed dimensions. Independently encoded H.264 MP4 fixtures also
pass both SBS/TAB GPU pixel checks. All 12 CTest suites pass. Tests disable audio and
never instantiate an emitter; actual sound and optical movie playback require
user-desktop checks. The final VLC Input-tab smoke check passes without USB writes.

Direct playback currently uses CPU decoding and 8-bit SDR output. Faithful HDR
movies, subtitles, disc/MVC and other stereo formats are not established. Ordinary
2D VLC playback can use the existing separate AI desktop feature. This is direct
VLC library playback in the app, rather than a replacement output module inside
the standalone VLC interface. See [movie setup](USAGE.md#vlc-stereo-movies).

## Shared Geo11 hook and game-depth controls implemented, 2026-09-15

The app now has a **Games** tab with **Enable shared Geo11 hook...**, disconnect,
fix inspection, a **Depth (%)** slider and **Game convergence**. It connects an
existing fix through the same loader/output code for x86 and x64. No game names,
shader hashes, compatibility-flag changes or replacement fix packages are used.

The output adapter consumes the fix's existing `sbs`, `tab`, reversed variants,
or `katanga_vr` output. Connection v3 changes only the common proxy chain in
`d3dx.ini`; it does not write `d3dxdm.ini`. The original renderer bytes and fix
settings remain recoverable. Legacy v1/v2 connections can still be removed.
Unsupported output modes and conflicting wrapper chains are reported unchanged.
Packed modes retain the source's eye resolution; Katanga provides full-resolution
eyes. This is not a claim of full-catalogue game compatibility.

Explicit depth-slider edits save `dm_separation` and, for manual convergence,
`dm_convergence` in the selected fix. A process-specific control channel asks the
hook to pulse the fix's configured reload chord only to Geo11's key-state reads.
No keyboard input is sent to the game or other apps. Return focus to the game to
apply; unsupported reload bindings use the next launch/manual reload instead.
Reloading may pause rendering. Auto-convergence stays under the fix's control.

Validation:

- Connection/tuning regression passes, including read-only provider configuration,
  preservation, rollback, both architectures, unsupported modes and auto-convergence.
- All ten emitter-free GPU output cases pass: five modes in x86 and x64. Tests
  inspect physical pixels, original HWND/input, producer/GPU stalls, resize,
  disable and clean adapter shutdown. Forced display stalls still show transient
  wrong slots before recovery; optical/gameplay stability is not established.
- Real Geo11 0.7.11 geometry reaches sequential physical output in SBS, TAB and
  Katanga for both architectures. Depth edits reduce geometry disparity to below
  0.04 pixels, then restore more than four pixels, without restarting the fixture.
- These real-Geo11 integration tests still **fail at process shutdown** inside
  Geo11. An unhooked x64 baseline reproduces the same crash at `d3d11+0x214f68`.
  Rendering/depth checks passing must not be reported as a passing complete suite.
- App build and Games-tab smoke test pass. No actual game installation was changed
  during this implementation.

Artifacts: `build/shared-geo11-output-654fa3920cef46b1bebd4ce31a22e881`,
`build/shared-geo11-integration-be8a07580d8742a19b206a21101d6cd3` (x64 depth),
`build/shared-geo11-integration-c49040e4d7004d59a41ae9aa47c5f6ff` (x86 depth),
`build/shared-geo11-integration-ca6b256a662f45fb986e4a98a96385cb` (TAB/Katanga),
and `build/geo11-baseline-da48bbd77d6e4ddda4c8c722c240665c` (unhooked baseline).

## Corrected requirement: shared game hook allowed, 2026-09-15

The user clarified: "i dont mind if we hook the game, but you were making game
specific adjeustments". Hooking the game is allowed. The earlier interpretation
that our code must never attach to the game was incorrect and is superseded.

Build one reusable Geo11 output integration, with x86/x64 builds selected by
architecture. Keep the original game window/input and app-owned emitter timing.
Reuse each game's established community stereo fix. Our integration must not
depend on title-specific configuration changes, replacement fix packages, shader
patches, or graphics-setting workarounds. Shared hook loading/output connection
mechanisms are distinct from such adjustments; preserve existing fix behavior
and make any shared connection changes explicit and reversible.

The loader/output implementation has no game-name branches, but the previous
trials included manual setup deviations: Psychonauts' allow_platform_update was
changed, and Batman used a generic Geo11 installation outside its manager profile.
Those trials do not establish compatibility with unchanged community setups.
Game-specific testing remains necessary; game-specific adjustments are not the
solution to failures of the shared hook.

This clarification does not establish stable output. Both trials remain rolled
back, the UI offers inspection/removal, and gameplay timing remains unresolved.
No runtime or game installation was changed while recording this correction.

## Superseded interpretation of presentation constraint, 2026-09-15

**Historical interpretation, corrected above.** The claimed ban on attaching our
code was a misunderstanding, not the user's current requirement.

The user requires the original game window to render stereo and explicitly
rejects both a separate viewer/overlay and attaching our code to the game in
memory. Use only the established fix's published external output. This supersedes
the earlier proposed app-owned in-process attachment architecture.

An external Geo11 receiver/overlay was briefly implemented in source, then removed
after this clarification. It was never deployed or tested against either game.
No replacement attachment code was installed. The custom game-folder connection
button has been replaced by read-only fix inspection; rollback remains available.

Feasibility remains unresolved under these combined constraints. Geo11 publishes
full-resolution eyes via Katanga for external consumers, but that interface does
not send converted frames back into the original game swapchain. The documented
native sequential routes use the NVIDIA 3D Vision driver stack. No documented
driver-independent original-window route meeting all these constraints was found.
Do not silently substitute an overlay, inject code, reinstall the old adapters,
or claim the requested integration works. Sources:

- https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html
- https://github.com/oneup03/NV3D-Glass
- https://oneup03.github.io/3DVision4All/docs/Native

## Deployment withdrawn and rolled back, 2026-09-15

**Historical rollback record.** The blanket restriction on our loaders below is
superseded by the shared-hook clarification above. The rollback facts and the
objection to title-specific setup deviations still apply.

The user reiterated a constraint that the custom adapter deployment violated:
game installations must follow their published community-fix instructions.
Compatibility belongs in our app. Do not install our loaders/DLLs, add custom
output overrides, or substitute a generic game setup to satisfy an app test.
The shared adapter code and two-game demonstrations below do not meet that
requirement and must not be described as established-ecosystem compatibility.

Both games are now disconnected using the recorded rollback. Custom loader/output
DLLs and connection overrides are removed. The original community Geo11 renderer
is restored in Psychonauts; its additional trial dxgi loader was archived and
allow_platform_update reverted from 2 to the pre-trial value 1. Its user depth
settings were hash-verified unchanged. Batman's 19 recorded generic Geo11 trial
files were archived outside its installation because that trial did not match
the game's 3D Fix Manager profile. Both game EXEs match original recorded hashes.
No saves were modified. Batman's current user configuration was preserved and
backed up; differences from the old backup have not been attributed, so do not
claim that every setting is restored to its original state.

Rollback copies and verification:
`build/game-backups/remove-custom-adapter-20260915-220339`.
No game was relaunched after rollback. Optical/gameplay stability and the requested
app-only compatibility remain unresolved. All deployment descriptions below are
historical and superseded by this section.

## GPU completion and unwanted SteamVR launches, 2026-09-15

The user reports that the fixed-refresh-slot update below still flashes and
breaks stereo during gameplay. VRR is disabled. Stability remains unconfirmed.

A new regression reproduced two problems: logical game Present returned before
GPU completion, allowing unfinished work to accumulate; publishing a copied eye
pair on CPU completion let the display queue inherit an unfinished GPU dependency.
Keyed-mutex acquisition returning successfully on the CPU did not prevent that.
Present now bounds unfinished work with a GPU completion fence (event query
fallback), and transport slots become readable only after GPU completion. The
display worker retains its previous completed pair during the wait. The query
fallback must allow GetData to flush; DONOTFLUSH caused excessive polling delay
on the tested NVIDIA driver even after initial End/Flush.

Seven emitter-free GPU paths pass: x64 NVIDIA/AMD keyed and shared-fence
transport, x86 NVIDIA shared fences, plus completion queries on x64/x86 NVIDIA.
An independent device releases a 200 ms GPU fence gate: game Present waits while
display output continues. Existing pixel/input/CPU-stall/resize/host-loss checks
also pass. Forced physical-display stalls still produce up to three wrong
scanout slots before recovery; these results do not establish glitch-free output.
The test now consistently uses the primary OLED; its earlier phase check could
not obtain usable scanout observations on the secondary monitor.

Both games now contain the GPU completion update. Connector disconnect/reconnect
preserved exact hashes of the game EXEs, Geo11 renderers, both connection INIs,
and saved mod tuning. Rollback files are in
`build/game-backups/gpu-ready-20260915-215801`. Installed output DLL hashes:

- x64: `9FA5027C7AF1FC61A3541DABD5CF3424A472AF24A6A278649AFEA4511C43624E`
- x86: `E231F110728145D55365B98501A72D353AE8BFF3567F7F0638FF6E02057068D5`

Host remains the fixed-refresh-slot build below. User calibration is retained:
239.991 Hz, L/B/R/B, phase 9390 us, shutters 6980 us. Batman was launched directly
with the host open; it reports full-resolution stereo, 240 physical presents/s,
and the host reports active emitter driving. A 20-second sample observed 1283
reported scanouts with zero wrong slots; this is sampled software evidence,
not optical confirmation. User gameplay feedback is pending. Psychonauts has the
same x64 repair installed but has not yet been relaunched with this update.

SteamVR's `vrclient_3DFixManager.txt` identifies 3DFixManager PID 41052 as the
OpenVR overlay client starting vrserver at 21:24:51 and 21:42:38. Its saved VR
mode was already disabled; no global VR configuration was changed. Normal Close
left the manager running, and Windows denied termination of the elevated process.
After asking the user to exit it, both manager and SteamVR were observed gone.
Direct Batman launch did not restart SteamVR. Use the manager for established fix
installation, exit it, and launch connected games normally with our host open.

## Frame-drop recovery follow-up, 2026-09-15

The user reports that the earlier queue/priority fix still breaks 3D on frame
rate drops. That supersedes any implication that the clean intervals below meant
the problem was resolved.

Replaced retrospective slot skipping with pre-submission selection from current
DXGI refresh/queue statistics. Added `HookRefreshSlots` in the existing v1 flag
word: the host and presenter both use `slot = refresh % cycle`. The host no longer
relearns eye phase from late frames for this backend. Legacy hook behavior remains
unchanged. A game source stall retains the completed eye pair.

New tests deliberately stall physical output for 17, 53 and 131 ms, verify return
to the same refresh/eye rule, and reject prolonged recovery beyond eight observed
wrong slots. Five GPU runs passed (x64 NVIDIA/AMD keyed/fences; x86 NVIDIA fences).
Observed recovery errors ranged from zero to six displayed slots; this does not
prove glitch-free output if physical presentation itself stalls. The 500 ms game
producer stall test and all 11 CTest suites also pass. A read-only timing monitor
now samples reported eye slots directly.

Installed both runtimes and the matching host; Psychonauts restarted with user
approval after saving. User mod and timing settings retained. The user subsequently
reported continued flashing and stereo breaks. Historical SHA256 values:

- App: `72CB982AB0D698F8E8B4642CF8172C05D32359CB7FC099BFF07E915FA68FC646`
- x64 output: `7B03865B878108E1C2D562509332DB758A8C51574582163B3BB3A68734E24B10`
- x86 output: `8EEA36CC5756BC448114E92B2C4DA5AD11423FC973F9BF127C9B4463038FC875`

Launching a connected game without the app remains supported. The installed
Geo11 renderer still loads, but our output adapter shows only its left eye until
the host allows stereo. Closing the host does not uninstall the game fix.

## Geo11 in-game output and load-related flashing, 2026-09-15

Implemented a reusable x86/x64 Geo11 display endpoint with the existing game HWND
and input. The app owns emitter timing. An early loader preserves the community
renderer bytes; the reversible connector changes only two output/proxy keys and
retains shader fixes and later tuning. No per-game executable or shader patching.
See [runtime details and scope](../runtime/README.md).

- **Batman: Arkham City GOTY:** generic Geo11 0.7.11, DX11 x86. User confirmed
  stable 3D and normal controls. Bloom/alpha defects remain. This generic route
  differs from the old NVIDIA-dependent route in local 3D Fix Manager profile 534.
- **Psychonauts 2:** Universal UE4 Fix 9.13 / Geo11 0.7.11, DX11 x64, linked by
  profile 1306. ReShade was removed/backed up. User resolved double vision using
  the mod; saved depth settings were retained.
- **Timing regression:** despite initially clean samples, the user later reported
  recurring black flashes and loss of 3D. Logs showed bursts of missed refreshes.
  The host was stopped and progress saved before restarting the game.
- **Further fix:** output no longer blocks on the producer mutex. Four buffers
  support three queued refreshes instead of one. The output worker uses MMCSS
  and relative GPU context priority +7. It skips render work when the latency
  wait times out, and reuses its render-target view. Game frame pacing remains
  one source pair per display cycle. The first half-minute still contained a few missed refreshes; the subsequent
  five-minute sample had none. Latest gameplay confirmation is pending.

Validation: all 11 application CTest suites and the source/UI smoke test passed
before this output update. The updated runtime then passed five emitter-free
GPU runs: x64 NVIDIA and AMD, keyed mutex and shared fences, plus x86 NVIDIA
fences. These verify actual eye/black pixels, paced production, resize, host
loss and shutdown. A new test holds the producer mutex for 500 ms while checking
that physical output continues. Early-load Geo11 0.6.164 probes previously passed
combined and separate factory/device startup on both architectures.

Installed the app and both runtime architectures under `build/bin/Release`,
including `vision_stereo_setup.exe`; both games are connected to the updated
runtime. Main executable SHA256:
`322F75992368C3DCAA444C63EB9839DB7E79BACA024F505AC19D5F43C4B23C9D`.
Previous scheduling-update x64 runtime SHA256:
`F0AEEE39AA2F645314EAA5E94E3F5AE4F29A031F1D4B9C6D6FA32E7709184D75`.
Previous scheduling-update x86 runtime SHA256:
`3EF447F7E2580526C3A15617284576371FD4C83D08BE83D418E4B47989BF779B`.
The prior app is retained as `VisionRestoration.pre-geo11.*.exe`.

AMD output tests are not AMD game/optical validation. RTX 30/40-series, Intel,
native NVAPI stereo, DX9/10, native DX12 and native media-player integration remain
unverified or unimplemented. There is no GPU vendor or driver-version pin in the
adapter. The historical ReShade hook below remains nonworking.

## Earlier input cleanup and compatibility direction, 2026-09-15

The user confirms OLED operation at 120 Hz and 240 Hz with BFI in the current
setup, and defers LCD work. This updates the older 240-Hz-only statements below;
record the actual sequence and profile when testing each source.

[App-hosted stereo compatibility](STEREO-COMPATIBILITY.md) records the corrected
priority: our app stays open, existing games/fixes use in-process stereo backends,
and the game retains its own window and input. This supersedes the earlier
no-open-control-app requirement. SBS capture is the last-resort game route.

Removed the Spout and Blender viewport source choices, their receiving backends,
Blender's automatic attachment, the Spout exporter target and SDK dependency.
Window capture, stereo images, calibration and AI desktop remain. The existing
game hook is explicitly labeled experimental and not working. No legacy stereo
compatibility backend or successful Geo11/ReShade/VLC test is claimed.

Validation: clean Release build in `build/stereo-compat`; all ten CTest suites
pass. The GPU integration test now uses actual image sources and checks that a
consumer's frame remains valid across restart/stop, without the removed transport.
The `--smoke-test --source-smoke` UI check also passes. Installed the verified app
at `build/bin/Release/VisionRestoration.exe` (SHA256
`8B8F17FF0D901E225F1A4ADFE38F073C7CF31147596194F1C80AC04AFC4619BB`).
The previous executable is retained as
`VisionRestoration.pre-input-cleanup.20260915-175436.exe`. The active process was
left running; the cleaned input list takes effect on the next app restart.

## GPU-load overlay flashing, 2026-09-15

A live opacity A/B/A check with the movie, AI desktop Small model and Blender
running sharply reduced timing mismatches when the overlay changed from alpha 254
(forced DWM composition) to 255 (direct flip in the measured interval). Restoring
254 restored the mismatches. App overlays now default to 255; full evidence, scope
and remaining optical validation are in [GPU-LOAD-FLASHING.md](GPU-LOAD-FLASHING.md).

## LCD audit correction, 2026-09-14

The user confirms only 240 Hz OLED with black insertion works. U6 Pro at 144/240 Hz
and Dell at 120 Hz remain unresolved. Historical explanations below that attribute
failures exclusively to panel response, or declare optical results from signal timing,
host counters, model calculations or tests, are hypotheses rather than established facts.
The former LightBoost subtraction implementation did not control a physical backlight.
Its assumed panel timings and custom-mode recipe were not optical measurements.

New optional work is documented in [PANEL-EXPERIMENTS.md](PANEL-EXPERIMENTS.md): uniform
neutral reset, explicit preload/extended holds, nine-row targets, refresh-code diagnostics,
and intersection of user-observed phase ranges at fixed shutter width. A reversed-argument
bug in the frame-rate buttons was fixed. Panel family no longer overwrites response times
with guesses, and frame-rate selection no longer silently enables ghost subtraction.
The official emitter scheduling/firmware implementation is not changed by this work.
Rendered-pixel and program tests must not be reported as optical validation.

Recorded 2026-09-08 on this workspace PC; updated 2026-09-12.

## Historical source status, 2026-09-14

| Source | State |
|---|---|
| Window capture of side-by-side output (Dolphin) | Working through the glasses, 240 Hz OLED |
| ReShade capture (Spout add-on) | Does not work |
| In-game hook | Does not work, has never worked |
| geo-11 | Never tested |
| Games with native stereo 3D | Never tested |
| Whole screen with AI depth (SpaceWalker-style, 2026-09-15) | Built; capture, network and eye images checked on the desktop (`--screen-test`), not yet through the glasses |

Entries below that call the hook or ReShade paths built, installed or passing tests describe software checks only. None of them is proof that those paths work.

**Hardware change 2026-09-12:** the original NVIDIA emitter and glasses have arrived; the RP2040 board has not. The original NVIDIA backend is the active path. Plugged in with no driver, Windows shows `USB\VID_0955&PID_7003` "NVIDIA stereo controller", problem code 28. libusb (and therefore the app's probe) does not list a driverless device. The app now accepts `0955:7003` as the boot-state identity; runtime commands still require the `0955:0007` endpoint layout after firmware upload. Blockers before the first optical test: bind WinUSB to the emitter with Zadig (not installed on this PC), and obtain `nvstusb.sys` for RAM firmware extraction. Done 2026-09-12: the user supplied the standalone NV3DVisionUSB package (driver 6.14.13.9041, nvstusb64.sys); `vision_firmware.exe` extracted 7026 bytes, identical from the 32- and 64-bit .sys, to `STUFF/emitter.fw` (sha256 7348cfd7...). The NVIDIA driver was NOT installed and will not be; the app uploads this RAM image over WinUSB itself, independent of the GPU driver. Rebuilt 2026-09-12; all four CTest suites pass. **USB milestone reached 2026-09-12:** WinUSB bound to 7003 and 0007 via Zadig; `--emitter-check STUFF/emitter.fw` uploaded the RAM image; an elevated `pnputil /restart-device` re-enumerated the emitter as 0955:0007; `--emitter-check` then reported State Ready with the EP1/EP2 layout claimed (`reports/emitter-check.txt`). `--emitter-check --emitter-pulse` then drove 1200 alternating eye commands at 120 Hz with 0 errors and the user saw the glasses shutter. `--live 15` (headless fullscreen presenter on the G80SD, SDR) reached 119.993 Hz measured, 1728 presents, 1 miss, timing locked, 1718 emitter commands, 0 errors; `--live 10 --hdr` (FP16 scRGB surface) likewise locked at 119.996 Hz with 1120 commands. Earlier 60 Hz results were caused by a RivaTuner Statistics Server 60 fps cap on this PC, not the app; keep RTSS off or exclude VisionRestoration.exe. Swap chain now uses 3 buffers. The GUI auto-connects a runtime NVIDIA emitter at startup and defaults Preview off when one is present. Optical quality (eye mapping, phase, ghosting) is the next step and is judged by the user. Hisense was not connected during this session (G80SD and a Dell S3220DGF were).

**Hardware change:** glasses received without the original emitter. RP2040-Zero, Adafruit IR module and STEMMA cable are ordered. The original protocol/scheduler, Windows RP2040 backend, predictive presenter integration and ARM PIO/USB firmware are now built. Hardware behavior is untested. Follow the [firmware build/arrival guide](../firmware/rp2040/README.md); original-emitter steps below are historical/optional.

## 2026-09-14: LightBoost mode for LCD panels (Hisense)

- The user rejected black frame insertion for the Hisense U6 Pro: at 144 Hz it gives each eye 36 new frames per second. It stays a 240 Hz OLED feature.
- Built instead, from the measured panel numbers (scan 6 ms, fall 4 ms, rise 1.5 ms): a **LightBoost (LCD)** button (Left/Right at the display's own rate, shutter and phase from the model, crosstalk cancellation on), **per-row crosstalk cancellation** in the presenter shader (the other eye's modelled leak is subtracted in linear light; strength slider, **C** key), a **panel rise time** in the model (profile v9: `panel_rise`, `cancel`, `cancel_strength`), and the fast-scan custom display mode (144 Hz line timing at 120 or 100 Hz) documented in [LIGHTBOOST-LCD.md](LIGHTBOOST-LCD.md). Model prediction for the Hisense: 15 % mean leak at 144 Hz native, 6 % at 120 Hz with the fast scan, 1 % at 100 Hz, all before cancellation.
- Verified: core tests (38009 checks), `--gpu-test` SDR and HDR including the cancellation checks, all CTest suites. Not verified through the glasses; the Hisense was not connected during the build.
- Later the same day: the user reported the cancellation "did not help at all" at 144 Hz (parts of the image synchronized, parts not) and found the Hisense accepts 1440p at 240 Hz; Left/Black/Right/Black there did not help either. Sequences are now any hold/black pattern (`makeSequence`, codes 16..47, hook flag `HookPatterns`, profile field `sequence` unchanged): Advanced > Sequence picker, quick buttons, **Compare sequences for this panel** (model table with frames per eye, clean shutter, light, leak, Use). The user then rejected any rate below refresh/4 ("40 at 240 is useless"); the UI offers refresh/2 and refresh/4 only (L R, L B R B, L L R R). Core tests 38742 checks, all CTest suites; not verified through the glasses.
- Then the user asked for explicit choices, since the app cannot detect the panel: Live tab **Display type** (Custom / OLED / QLED VA / IPS-TN; sets response and rise, profile field `panel_kind`) and **frames per eye** buttons (refresh / 2 and / 4) that apply the model's best pattern, shutter and phase for that panel (`bestSequenceForRun`; clean black-frame patterns preferred). A regression test loads the confirmed OLED profile text and checks it still means Left/Black/Right/Black, phase 2618, shutter 4501, no cancellation. The user reported the QLED "looks worse" with the earlier build; cause unknown (phase, panel numbers or the panel itself); the OLED was not retested.
- The user then reported the same ghost with Left/Black/Right/Black at 240 Hz on the QLED (session.log: 2560x1440 at 239.96 Hz, direct flip, locked, 2 to 4 misses in 21800 presents, phase 2357, shutter 2129) and asked for an LCD-specific black frame insertion, with the 2026-09-13 Hisense camera numbers disregarded. Added the **LCD black floor** (`Settings.blackFloor`, profile field `black_floor`, shader `extra.y`): eye images are lifted to a fraction of the panel drive (10 % for the QLED / VA kind, 8 % IPS, 0 OLED) while black frames stay at zero, so the slow crawl to full black is never asked of the image. GPU test checks every eye-frame pixel is lifted and black frames stay black. Not verified through the glasses.
- The user then described a dark band scrolling up and down with the phase on the QLED at 240 Hz. That is the black frame's scan edge seen through a shutter shorter than the panel scan, and the shutter could not be widened because `nvidiaSchedule` parked the emitter's X register at its 1300 us centre, capping the window at period - 2300 - 1053 us (4980 us at the 120 Hz emitter rate of Left/Black/Right/Black). The schedule now prefers the shortest X (300 us) and moves the boundary; the ceiling is period - 1353 us (6980 us at 120 Hz, 2813 us at 240 Hz Left/Right). Near the two ends of the boundary range X still has to grow and the window is shortened to fit; `nvidiaEffectiveShutterUs` reports that and the Live tab shows it with the phase shift that avoids it. The confirmed OLED phase 2618 keeps its window start and full 4501 us shutter (tested). Not verified through the glasses.

## 2026-09-14: black flashing with a game as the capture source

- The user reported the glasses going fully black for about three seconds at a time, in fullscreen and in the live preview, only with a captured game (Psychonauts 2, 4K) as the input; an emulated N64 game earlier had been fine. session.log for the run (240 Hz HDR, Left/Black/Right/Black) shows why: slips at 14 to 23 per second (trigger lead below zero, present interval jitter 0.8 to 1.5 ms while the game rendered), and the four-slips-per-second rule from the evening build resynchronizing 3 to 6 times a second (463 resyncs over 220 s; composition-mode changes stayed at 4 the whole run). Each resync blanked 16 frames and suspended the emitter, so the output was black most of the time in bursts. The game hook was not involved (hooked=0).
- Fix in src/renderer.cpp: slips and composition-mode changes never resynchronize any more; they are counted (misses, slipsPerSec in the log and the Live status line). Full resync remains for disjoint statistics, occlusion, resize, emitter loss and cadence changes. The process now requests the HIGH GPU scheduling class (D3DKMTSetProcessSchedulingPriorityClass, ABOVE_NORMAL if refused, logged as gpuPriority) and the presenter device GPU thread priority 7, so the presenter's draw is scheduled ahead of the game's; the present thread caches the opened shared source textures instead of reopening one per pair. Details in [PRESENTATION-SYNC.md](PRESENTATION-SYNC.md).
- Not yet verified with the user: whether the slip rate itself drops with the higher scheduling class. Remaining slips show one refresh with the wrong eye (brief ghosting), never black. The user's running instance was renamed so the new exe could link; restart with Launch.cmd.

## 2026-09-13 (night, last): first confirmed clean whole-screen stereo

The user viewed the built-in output through the glasses and reported it "perfect" on the
Samsung OLED. This is the project's first optical confirmation; every earlier result was a
software or host-timing check only. Configuration, from `reports/session.log` and the
autosaved profile at that moment:

| Setting | Value |
|---|---|
| Display | Samsung G80SD OLED, 3840x2160 at 239.991 Hz, HDR on |
| Sequence | Left/Black/Right/Black (software black frame insertion) |
| Stereo area | whole screen, 1.0 at centre 0.5, no band |
| Shutter | 4980.1 us, at the widened ceiling; phase 2380.4 us |
| Image brightness | 3.05x |
| Session health | 13562 emitter commands, 0 USB errors, 1 late; vblank jitter 2.7 us rms, 8.5 us max; 3 misses, 2 resyncs, direct flip |

What this establishes: the whole screen, not a band, is clean at 4K 240 Hz on this OLED
with black frame insertion and the widened shutter, and the brightness is acceptable with
the HDR gain. Two changes were jointly necessary: the black-frame sequence for separation
and the two-refresh shutter ceiling for the light.

What it does not establish: Left/Left/Right/Right, any 120 Hz mode, the Hisense, stereo in
games through the hook, and the ten-minute stability run are all still unverified. At
120 Hz this sequence flashes each eye 30 times a second and flickers; 240 Hz gives 60, the
rate 3D Vision gave at 120 Hz.

The confirmed values live in `profiles/autosave.ini`, which is rewritten on every settings
change, so they are recorded above as well.

## 2026-09-13 (night, later): the light a black-frame sequence throws away

- The user confirmed software black frame insertion removes the crosstalk ("perfect") but
  reported the image far too dark. Part of that is inherent: each row is lit with its own
  eye for one refresh in four. The larger part was a defect. `nvidiaMaxShutterUs`,
  `validate`, `clampTiming` and the shutter slider all bounded the shutter by one display
  refresh, while `emitter.cpp` already runs the emitter at half rate for a four-slot
  sequence, giving it a **two-refresh period** the shutter may use in full.
- Geometry: a row shows its eye from the moment the scan reaches it until the next scan a
  refresh later, and the other eye cannot appear for two refreshes, so a shutter about
  `period + scan` wide, phased at the scan start, collects every row's whole lit period
  with no leakage at all.
- Fixed: `sequenceEmitterHz` now drives every shutter limit; `brightestShutterUs` searches
  for the widest shutter whose best phase stays within the leakage limit, ranked by the
  light actually collected (`brightness x shutter`) rather than the lit fraction of the
  shutter, which falls as the shutter widens and would have argued for the dimmer setting.
  New **Maximize brightness** button, an **Image brightness** gain (1x to 8x, profile
  version 8, applied to the app's own output only, never to a black frame), and the stereo
  area now reports light reaching the eye as a percentage of a fully lit frame.

| Configuration (whole screen, model) | Old cap | New cap | Light before | Light after | Leakage |
|---|---|---|---|---|---|
| OLED 4K 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 51 % | 96 % | 0 % |
| OLED 4K 240 Hz Left/Black/Right/Black | 813 us | 4980 us | 19 % | 84 % | 0 % |
| OLED 4K 240 Hz Left/Left/Right/Right | 813 us | 4980 us | 19 % | 118 % | 0.6 % |
| Hisense 144 Hz Left/Black/Right/Black | 3592 us | 10499 us | 46 % | 95 % | 0 % |
| Hisense 120 Hz Left/Black/Right/Black | 4980 us | 13276 us | 55 % | 98 % | 0 % |

- Checks: core suite 37957 checks (18 new, covering the emitter period, the widened shutter
  through a profile round trip, the collected-light ranking and the gain), all four CTest
  suites, and `--gpu-test` in SDR and HDR including a new check that the gain brightens an
  eye frame and can never lift a black frame. Percentages above 100 are real for
  Left/Left/Right/Right, where a row is lit for two refreshes. Confirmed through the
  glasses the same night; see the entry below.

## 2026-09-13 (night): refresh clock, jitter, illumination model, strobe calibration

- Presenter and game host now predict the vblank from a least-squares line through the
  last 240 presentation timestamps instead of the newest one; a standalone probe
  (`experiments/vblank-probe`) showed those timestamps fit a line to 1 to 6 us rms on the
  Hisense at 144 Hz while the kernel scan-line estimate scatters 22 to 27 us. The probe is
  built into the app: **Diagnostics > Measure vblank** and `--vblank` (runs beside an open
  instance) report the blanking length (462 us), where the timestamp sits in it, and the
  scan start offset, which is stored in the profile and used by Suggest phase.
- Diagnostics and the session log carry vblank jitter (rms/max), present-interval jitter,
  the eye command timing error (write start minus deadline), the mean USB transfer and the
  count of timing-block writes; the timing tab shows the boundary and X the emitter receives.
- Core: numeric illumination model (`estimateLeakage`, `bestModelPhaseUs`,
  `bandHeightForLeakage`) that follows every row through the cycle for sample-and-hold or
  strobed panels and any sequence; Settings gain illumination, strobe start/length and scan
  start (profile version 7). For the whole screen at 4K 120 Hz Left/Right it finds about
  7 % leakage in the top and bottom thirds at best, which is the top/middle/bottom symptom;
  Left/Black/Right/Black at 240 Hz is clean for a 4 ms shutter.
- UI: phase sweep with marks (S / M / Enter in fullscreen, phase drawn in both eyes),
  software black frame insertion checkbox and B key, Illumination and strobe inputs,
  "Set strobe from the last two marks", predicted top/center/bottom leakage for the current
  phase, model-driven Suggest phase and Fit area, per-eye timing and highlight controls
  disabled with a reason when they cannot act, loaded profiles now restore the stereo area
  and panel fields. Details: [TIMING-CALIBRATION.md](TIMING-CALIBRATION.md).
- Game hook: the shared-memory contract carries the sequence and a `HookSequences` flag;
  the host applies four-slot sequences only to a hook that advertises them and otherwise
  drives Left/Right and says so. The hook source itself was being edited by another
  session and was left unchanged; four-slot presenting in the hook is still to do.
- Checks: core suite 37939 checks, all four CTest suites, `--gpu-test` (now also checks the
  phase readout: identical in both eyes, value-dependent, absent from black frames) and
  `--vblank` pass. `--smoke-test` could not run while the user's instance was open. No
  optical result is claimed; the model's predictions and the sweep are for the user to
  verify through the glasses.

## 2026-09-13 (evening): ride through slips instead of restarting

- The user saw the fullscreen image jump by itself on the Hisense at 144 Hz. session.log showed the presenter resynchronizing every few seconds: each frame that landed one refresh late (maxIntervalMs about two periods) was treated as a miss, followed by 16 black frames, emitter suspend and a firmware re-lock. The presenter now counts such slips and continues; the refresh-anchored eye sequence and the updated present-to-refresh mapping make the next frame correct. Four slips within a second, a composition-mode change or disjoint statistics still resynchronize. Details in [PRESENTATION-SYNC.md](PRESENTATION-SYNC.md). Why the occasional two-period frame interval occurs in fullscreen is not yet known.

## 2026-09-13 (later): fullscreen on 120 Hz panels, stereo area

- Fullscreen failed to fuse on both the Hisense and the G80SD while the embedded preview fused. The session log shows fullscreen locked in direct flip at the measured rate with single-digit misses and no USB errors, so this is not the presenter or the emitter: a whole 4K 120 Hz frame is scanned over ~8.1 ms (signal timing) and the panel needs 0.2 ms (OLED) to 3-5 ms (Hisense VA) to settle, which leaves no time when every row shows one eye. The preview covered about a quarter of the height. Analysis and routes: [FULLSCREEN-120HZ.md](FULLSCREEN-120HZ.md).
- Added the stereo area (Settings.bandHeight/bandCenter, profile version 6 fields band/band_center/panel_response/panel_scan): the presenter shader and the game hook blit scale the image into a band and keep everything outside exactly black; shared memory carries the band (former reserved fields, zero = whole output for older hosts). Live tab sliders, fullscreen keys PageUp/PageDown/Home/End, Fit area to shutter and Suggest phase from the settled-window model (core: settledWindow, bandHeightForShutter, suggestedPhaseUs). Displays now report active/total lines and the derived scan time; Advanced > Panel takes the response time and a measured scan override.
- Checks: core tests cover the window geometry, fit, phase suggestion, profile round trip and validation; `--gpu-test` (now runnable while the app is open) verifies both eye frames are exactly black outside a 50 % band in SDR and HDR. All four CTest suites pass. The hook build was copied to the test Dolphin folder. Optical results with the band, at 240 Hz Left/Left/Right/Right, or with a large-vertical-total mode are not yet verified.

## 2026-09-13: readable text in the stereo scene

- The stereo scene previously drew a different word in each eye (LEFT / RIGHT) as a screen-space overlay, which cannot fuse through the glasses. A later source edit (identical STEREO text at screen depth) had not been compiled, so the running exe still showed the per-eye words.
- The scene words are now geometry: NEAR (red, z=3, in front of the screen plane), SCREEN (white, z=4, zero disparity) and FAR (blue, z=6.5, behind) lie on camera-facing planes and are ray-traced through the same stereo camera as the spheres and floor, so camera separation and convergence move them exactly like the shapes. LEFT/RIGHT words remain only in the identification patterns.
- `--gpu-test` now reads both real eye frames of the scene at separation 0 and 0.12 and convergence -0.05/0/+0.05 and requires every word in both eyes with crossed, zero and uncrossed disparity respectively; SDR and HDR pass. Full build and all four CTest suites pass. Optical readability through the glasses is still judged by the user.

## 2026-09-12 (later): live calibration and the in-game hook

- Presenter: predictive emitter triggering for the NVIDIA backend (target = predicted vblank of the refresh the present lands on + host-side phase); the emitter's own delay register stays 0 because rewriting its timing block on every adjustment resets its timers (visible glitch). High-resolution waitable timer in the USB worker (Sleep(1) overshoot had dropped ~30% of triggers). Eye sequence anchored to the DXGI refresh counter, so a missed present repeats one eye instead of desynchronizing; misses no longer blank the screen. Phase/duration/swap/pattern changes apply live without a resync; only cadence/surface changes resync. DWM composition is read from `GetFrameStatisticsMedia` every frame and adds one refresh to eye selection and prediction (focus changes had shifted the eye by a whole refresh).
- Keys: Up/Down phase, Left/Right shutter, Shift = 10 us, X swap, Space pause. Profiles panel: list/load/save/export/import/reset; newest matching profile auto-loads. Tuned profile: `profiles/ODYSSSEY.ini` (HDR, phase 3600 us, shutter 1800 us, eyes swapped).
- Game hook (`adapters/vision_hook.cpp`, `src/gamesync.cpp`, `src/sync_protocol.h`): ReShade add-on that double-presents a side-by-side frame as L then R on the game's swap chain (sync interval forced to 1) and publishes present ids and frame statistics over shared memory; the app's GameSync thread drives the emitter from them with the same refresh-anchored, predictive logic. Installed into a Dolphin folder (ReShade 6.8.0 add-on build as dxgi.dll). Requires the game's D3D11 backend. First Dolphin run pending.
- RivaTuner Statistics Server must stay off or exclude the app: its 60 fps cap silently halves the presenter.

## RP2040 preparation checks

- Release build passes all four CTest suites: existing core, GPU texture integration, RP2040 protocol/scheduler, and RP2040 host/waveform tests.
- New suite: 312399 assertions across packet parsing, command acknowledgments, stale sessions, stop/disconnect, scheduling faults, clock uncertainty/drift and 72000 alternating frame windows representing ten minutes at 120 Hz.
- Host/waveform suite: 76 checks, including an injected transport connecting the real host client to the device endpoint and instruction-level PIO simulation for all four tokens.
- ARM GCC 14.3.1 / Pico SDK 2.2.0 release build produces `build/rp2040/VisionEmitter.uf2`. Artifact verification passes UF2 payload/header/family checks, boot2 CRC and Cortex-M0+ vectors. The latest hash is in `reports/rp2040-firmware-artifact.txt`.
- Native UI smoke test passed after RP2040 integration; screenshot inspected. USB probe enumerated no RP2040 firmware device, as expected before delivery. No app remains running from these tests.
- Profiles now include emitter serial identity and firmware source fingerprint, with separate SDR/HDR keys. Older v1 profiles remain readable and must be revalidated.
- This is logical-time simulation with injected USB delays, not a real USB or display timing test. No firmware was flashed, driver binding changed or optical profile validated.
- Reproduce with `tools/Test-Rp2040.ps1`. Report: `reports/rp2040-simulation.txt`. Wire format and remaining firmware work: [RP2040-PROTOCOL.md](RP2040-PROTOCOL.md).

## Software checks completed

- Release build: application, core tests, GPU integration tests, firmware extractor and ReShade/Spout add-on compile successfully with VS 2022 / Windows SDK 10.0.26100.
- Core suite: **331 checks passed**, covering sequences, eye swap, timing packets, phase arithmetic, profile replacement/round-trip, emitter/firmware/HDR profile separation, firmware record validation and presentation-clock discontinuities.
- GPU integration: local Spout sender/receiver, resize, source loss after draining in-flight frames, held-pair immutability, keyed-mutex access, FP16 value 4.0 preservation and simulated emitter trigger/shutdown passed.
- Offscreen WARP rendering: eight SDR/HDR pattern combinations passed. HDR ramp contains values above 1.0; black-frame RGB is zero in SDR and HDR.
- Native UI startup/render/shutdown passed on the installed GPU. Inspected the generated UI and stereo depth scene images; corrected high-DPI sizing and kept launch/stop controls visible outside the scrolling page.
- Live discovery reports RTX 5070 Ti, Odyssey G80SD at 3840×2160 120 Hz and HISENSE-TV at 3840×2160 143.988 Hz, HDR enabled on both. No display modes were changed during these checks.
- No NVIDIA USB device was enumerated at the last probe.

Evidence: `build/Testing/Temporary/LastTest.log`, `reports/gpu-test.txt`, `reports/ui-smoke.txt`, `reports/ui-preview.png`, `reports/depth-preview.png`, `reports/hardware.txt`.

## Milestones

| Milestone | Current state |
|---|---|
| 1 — Calibration foundation | Implemented; core/rendering/UI software checks passed. Full interactive wizard walkthrough and display transition matrix still pending. |
| 2 — Real emitter | Original NVIDIA emitter: USB binding, RAM firmware load, re-enumeration and runtime claim verified 2026-09-12. Shutter activity pending. RP2040 path built, hardware not arrived. |
| 3 — G80SD 120 Hz | Live mode detected; optical calibration and ten-minute stability test pending at 120 Hz. The confirmed clean configuration on this display is 240 Hz (milestone 6). |
| 4 — Hisense 120 Hz | Display detected at 143.988 Hz; explicit 120 Hz selection and independent optical calibration pending. |
| 5 — Live inputs | Window capture of side-by-side output works (Dolphin). ReShade capture and the in-game hook do not work. geo-11 and native stereo games never tested. |
| 6 — Higher refresh | G80SD at 4K 240 Hz with Left/Black/Right/Black confirmed clean over the whole screen by the user, 2026-09-13. Left/Left/Right/Right and 144 Hz physical results still unverified. |

## Next session with the emitter

1. Refresh discovery and record VID/PID, interfaces, endpoints and current driver binding.
2. Establish WinUSB access; initialize volatile firmware only if runtime endpoints are absent.
3. Verify repeatable connect/stop/disconnect before optical tuning.
4. At G80SD fixed 120 Hz, confirm visible shutters, eye mapping, and whether retrospective presentation feedback leaves a usable opening interval.
5. Calibrate phase/duration in SDR, then HDR; examine top/center/bottom and motion. Record failures and ghosting honestly.
6. Run the ten-minute stability test, then repeat independently on the Hisense at 120 Hz.

This is a working software prototype, not completed hardware restoration. Clean stereo and broad game compatibility cannot be established by the software checks above.
