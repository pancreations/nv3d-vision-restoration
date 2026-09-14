# Direct3D 12 and depth-based stereo (GTA V Enhanced) — 2026-09-14

The user asked for GTA V Enhanced in 3D. The game is Direct3D 12 only and renders one camera, and no Direct3D 12 two-camera stereo driver exists (geo-11 is Direct3D 11 only). The options offered were alternate-eye rendering, depth-based stereo, or GTA V Legacy with geo-11. The user chose depth-based stereo, which keeps the DLSS 5 Neural Rendering mod working.

## Design

- **Depth source (`depth_tracker.h`).**
  - Finds the game's scene depth from draw counts per depth-stencil, gathered per command list, merged when the list executes and collected at present. This is ported from ReShade's Generic Depth example (BSD-3-Clause).
  - Any depth buffer with the back buffer's aspect ratio qualifies, from a quarter to twice its width. So an upscaler's render resolution counts: DLSS Performance at 4K renders 1920x1080, which Generic Depth's default heuristic rejects.
  - The choice is sticky for 30 frames.
  - Reversed depth is detected from clear values; `[VISION] DepthReversed` overrides the detection.
- **Eye synthesis (`depth_stereo_shader.h`).**
  - Resamples each eye from the finished frame with parallax `separation * (1 - depth / convergence)` on the reversed depth value. For infinite-far perspective projections this is exact two-camera geometry and needs no near or far plane.
  - The pop-out side is compressed with tanh.
  - Each pixel marches its row nearest surface first, so foreground stays in front, and a disocclusion continues the background rather than the edge.
- **Sequences (`vision_hook.cpp`).** Every game frame presents the host's whole sequence on both APIs, and the hook now sets `HookSequences`, so Left/Black/Right/Black works in games. On Direct3D 12 the slots are presented with blocking `Present(1,0)` inside the game's Present, each after `flush_immediate_command_list`, with explicit barriers for the back buffers, the frame copy and the depth copy.
- **No Direct3D 12 filler.**
  - A game fetches `GetCurrentBackBufferIndex` once per frame, so a present between frames would send its next frame into a buffer already queued for display.
  - A frame slower than the cycle repeats its last slot, which is black in Left/Black/Right/Black.
  - For the first 600 frames the hook watches which back buffer the game binds. If it is not the current one (a game that counts presents, like Dolphin's D3D12 backend), stereo stops with a message.
- **Settings.** The game's `ReShade.ini` `[VISION]` section holds StereoSource (auto, packed or depth), Separation, Convergence, PopOut and DepthReversed.
- **Hotkeys.** Ctrl+Shift with:
  - PageUp/PageDown: separation.
  - Home/End: convergence.
  - Insert: 2D.
  - Delete: defaults.
  - F8: diagnostics, now including a depth view.

## Evidence

`experiments/addon-regression/main_d3d12.cpp` is new. It renders a reversed-Z scene whose pixels encode their own position, then reads back the four FLIP_SEQUENTIAL buffers after a frame. At 10 % separation, 320x180 RGBA16F (the same checks pass again at 400x240 RGB10A2 after a resize):

| Surface | Half-parallax | Left / right eye source (uv) | Expected |
| --- | --- | --- | --- |
| Infinity | +16 px | 0.1765 / 0.0765 | 0.1766 / 0.0766 |
| Convergence plane | 0 | 0.5015 / 0.5015 | 0.5016 / 0.5016 |
| Near plane | -7.7 px | 0.1025 / 0.1506 | 0.1025 / 0.1507 |

The harness also checks the following:

- Black slots are black.
- Slots 0-3 run on consecutive present ids.
- Left/Right alternates.
- Disabling restores the game's image.
- Stereo keeps working after a resize. After the resize, 364 game frames produced 1092 injected presents: one Left/Black/Right/Black cycle per frame, about 60 fps on a 240 Hz display.

The D3D11 suite still passes.

Two harness pitfalls:

- **Start-up banner.** ReShade draws it over the top-left of the image for about five seconds after loading, and again after a resize. The pixel checks wait six seconds.
- **Filler count.** The D3D11 slow-producer count varies between runs, from 34 to 424 filled refreshes in 2 s. It barely passed (34 against 33 needed) on the final run, and it failed when a build ran at the same time.

## Not verified

Nothing has been checked in GTA V Enhanced itself:

- whether OptiScaler loads ReShade;
- where ReShade sits relative to OptiScaler's and Streamline's swap-chain wrappers (the extra presents go to whatever ReShade sees as the native swap chain);
- which depth buffer the tracker picks;
- how the HUD looks;
- anything through the glasses.

The app's installer path for OptiScaler games is compiled but not exercised; GTA was installed by hand.

# One image per refresh: the flashing/out-of-sync fix — 2026-09-13 (afternoon)

The user reported that with the hook finally enabled, "the glasses start flashing like crazy, out of sync". The 12:55 run was healthy by every counter the hook reports: 1500+ completed pairs, zero present errors, nonblack pixels in the packed source and both eyes. The app's session log showed the real problem: of 2905 emitter triggers, 401 were misses and 972 were guesses. A third of the refreshes had no known eye.

## Cause

A display shows one image per refresh and holds the last flipped image until the next flip. Dolphin runs at 60 frames a second and the hook presents one pair per frame, so 120 of the Hisense's 144 refreshes each second carry a new eye. The other 24 repeat whichever eye was last, while the glasses have already switched. The app's refresh-to-present mapping assumes one present per refresh, so it also drifts by those 24 presents a second. At 30 fps more than half of every second is wrong.

Measured directly on the display with `experiments/vblank-filler` (a standalone program, no emitter, no game): a 60 fps producer with a flip-model 3-buffer waitable swap chain covered 611 of 866 refreshes; a 30 fps producer covered 360 of 864.

## Change

`adapters/vision_hook.cpp` now runs a vblank thread. After each vblank it checks the flip queue, and when the game has not presented, it draws the opposite eye from the same packed source and presents it. Every refresh then carries a known eye and the host's mapping is exact. Game pairs also continue the alternation rather than always restarting on the left, so a filled refresh never causes a repeated eye at a frame boundary.

Lock discipline matters here and cost two deadlocks before it was right. The filler takes the D3D11 device lock first, while holding nothing, then tries for the addon's own state lock without ever waiting. The game's present path holds the state lock while it presents, and presenting takes the device lock, so a filler that waited for the state lock deadlocked the game. A refresh the filler cannot claim instantly is left to the game.

Multithread protection is enabled on the game's immediate context at swap-chain init, and the whole draw-and-present sequence runs inside the device lock. If either the containing output or the context lock is unavailable, filling is disabled and the hook logs why; splitting still works as before.

## Evidence

| Case | Refreshes | Carrying a new eye |
| --- | --- | --- |
| 60 fps producer, no filling | 866 | 611 |
| 60 fps producer, filling | 867 | 866 |
| 30 fps producer, no filling | 864 | 360 |
| 30 fps producer, filling | 866 | 864 |

Alternation was perfect in every filled run, and the producer held its exact frame rate (60.01 fps, worst frame period 17.05 ms), so filling does not feed VSync blocking back into the emulator's speed limiter. A frame latency of 2 was clearly better than 1.

The regression harness gained a slow-producer test: at 33 frames a second it now records about 178 eye entries where the producer itself supplied 66, with strict alternation and consecutive present ids. Three consecutive full runs passed. The earlier fixed left-first assertion was relaxed to the real invariant, alternation plus consecutive present ids.

Optical confirmation through the glasses is still the user's to make. The harness proves presentation order, never what the shutters do.

# Game hook status reporting — 2026-09-13 (afternoon)

The user's 05:44 Dolphin run loaded the hook (`Hook ready` three times, D3D11, 3840x2109, R10G10B10A2) and then logged nothing for 37 seconds: no completed pairs, no pixel probe, no warning. Every one of those requires the host's go-ahead, so the app never enabled the channel during that run (app closed, emitter not ready, or the app's own output/preview open). The hook was silent about it, and the app's session log did not include the hook status, so the run looked like a broken splitter.

Changes:

- `src/sync_protocol.h`: the first reserved host field is now `hostState` (0 = older host, 1 = ready, 2 = paused by the app's own output, 3 = emitter not ready). Layout and version unchanged.
- `src/gamesync.cpp` writes `hostState` on every loop.
- `adapters/vision_hook.cpp` logs `[Vision] ...` on every host transition: app not running, connected (with pid), paused with the reason, enabled, connection lost. A ReShade.log that shows the packed image on screen now names the cause.
- `src/main.cpp`: `reports/session.log` lines end with `| hook: hosting=.. hooked=.. driving=.. game=.. frames=.. triggers=.. misses=.. msg=".."`.
- `experiments/addon-regression/test_hook.cpp`: updated for the band parameters added to `drawEye` earlier today (the harness had not compiled since).

Regression run after the change: all PASS, and with the test window visible the sequential path completed one pair per frame (active=1, pairs=12 over 12 frames, no errors). Optical timing remains untested by the harness.

# Game hook repair — 2026-09-13

## Dolphin SBS profile

`adapters/dolphin/GLME01.ini` now explicitly selects D3D11 (`Core.GFXBackend=D3D`), SBS (`Video_Stereoscopy.StereoMode=1`), full per-eye resolution, unswapped input eyes and VSync. The existing default-post-shader workaround remains. This profile is installed in Dolphin's `User/GameSettings/GLME01.ini`; the previous file is backed up next to it.

The path is **Dolphin dual-view rendering -> SBS image -> VisionGameHook left/right sequential Presents -> app-owned emitter**. It does not request native sequential or quad-buffer output from Dolphin. The app's v1 channel uses packing 0 (SBS), and the addon defaults to sequential mode when the swap chain initializes. The app must be open with the emitter connected; its calibration/test output must be stopped so GameSync is enabled. The latest inspection found no host mapping, so no active game/emitter session was claimed.

Verified in isolated Dolphin with the global stereo setting deliberately set to QuadBuffer: the game profile overrides it with a non-array SBS back buffer; the addon initializes and the HDR source contains nonblack pixels. Evidence: `experiments/dolphin-black-screen/sbs-profile-override.log`. No app source or production addon binary change was needed for this profile update. Physical shutter alignment still requires live validation.

## Black-input trigger isolated and workaround verified

The 03:06 retest reported zero-valued samples in the packed source and both output eyes. The diagnostic keys were received (packed at 03:07:09, left-only at 03:07:18). Switching modes after startup did not restore the image.

An isolated copy of the user's Dolphin executable, Sys and graphics configuration reproduced this without the stereo addon: a small observer addon samples the back buffer before and after ReShade but performs no drawing, state changes, presentations, IPC or emitter commands. The observer found black source samples with Dolphin's **AutoHDR post shader + HDR enabled**. Disabling only that post shader restored source pixels with HDR still enabled. SDR also restored source pixels. This isolates the failing configuration; it does not establish the exact shader/compiler/driver defect inside AutoHDR.

| Isolated case | Source sample result |
| --- | --- |
| AutoHDR + HDR, stereo addon absent | All samples zero at frames 30, 120, 600 |
| AutoHDR + SDR, stereo addon absent | Nonblack source pixels |
| Default post shader + HDR, stereo addon absent | Nonblack source pixels, values above SDR range |
| Default post shader + HDR, stereo addon loaded on private test IPC | Nonblack packed source and left output |
| Original global AutoHDR + HDR + 6x settings, with GLME01 per-game override | Title-screen source: mean 0.382577, max 2.537109, 1903/2304 samples nonblack |

The workaround is staged in `adapters/dolphin/GLME01.ini` and installed as `<Dolphin folder>\User\GameSettings\GLME01.ini`. It overrides only `Video_Enhancements.PostProcessingShader` with an empty string, selecting Dolphin's default output shader. Global HDR, 6x resolution and stereo settings are unchanged. Stop/relaunch Luigi's Mansion to load the override. Removing this new per-game INI restores the previous selection.

Evidence: `experiments/dolphin-black-screen/observer-hdr.log`, `observer-sdr.log`, `observer-hdr-no-post.log`, `hook-hdr-no-post.log`, and `game-override-6x-hdr.log`. The isolated runs used their own user directory and no real emitter. Their window was occluded, so the successful native extra-Present/right-eye path and optical timing still require a user-desktop retest. The existing GPU shader regression separately checks both eyes in RGBA16F. The initial Dolphin frame dump showed the emulated title screen before final post-processing; it alone was not proof of a correct displayed back buffer.

The observer is an experiment only; it was not installed in the user's Dolphin. No further production addon binary change was made for this configuration workaround. Claude's app sources, emitter and calibration remain untouched.

## Retest result and diagnostic follow-up

The user retested the first repair and still got a black screen and flashing shutters. The 02:58 Dolphin log records roughly 60 completed pairs/second, with zero reported Present errors (pairs 300, 600, 900 at five-second intervals). This confirms the first patch did **not** resolve the reported problem; present counters do not establish image correctness.

The follow-up build samples small regions from the packed input, left output and right output at source frames 30, 120 and 600. It logs `Pixel probe source(...) left(...) right(...)` with average/maximum channel magnitude and nonblack sample counts. Readback is asynchronous and uses DO_NOT_WAIT; it does not save images. Black samples are diagnostic evidence, not automatic grounds for disabling a legitimately dark game scene.

With Dolphin focused and the host enabled, **Ctrl+Shift+F8** cycles sequential -> original packed image -> left eye only -> right eye only -> sequential. All three diagnostic modes publish inactive status to pause the emitter and submit no extra Presents. The selected mode appears in the app's hook message and ReShade.log. This makes image-vs-sequencing faults distinguishable without changing the app or game settings. The default on a fresh swap chain remains sequential.

Regression checks also pass for the asynchronous probe in all three tested formats and for inactive status/no extra Presents in each diagnostic mode. The actual black-screen cause and optical timing remain unresolved pending the diagnostic run.

The reported failure is Luigi's Mansion in a portable Dolphin build: black output, emulation slowing to a stop, glasses activating. The saved configuration selects D3D11 (`GFXBackend=D3D`), side-by-side stereo, VSync and HDR. The full game failure has **not** been reproduced or confirmed fixed in this tool session; the changes below fix identified addon defects and are ready for an interactive retest.

Only `vision_hook.cpp`, the addon-local `d3d11_state_scope.h`, and the separate regression project were changed. The app sources, shared protocol, emitter code, calibration, game configuration and GPU driver were not changed.

## Changes

- Save and restore the complete D3D11 context. The previous partial state block restored the PS constant-buffer pointer using `PSSetConstantBuffers`, losing suballocation offsets/counts set through `PSSetConstantBuffers1`. It also omitted class instances, predication and UAV state. The private context is cleared before restoring the game, releasing its back-buffer references for resize.
- Split at `reshade_present`, after ReShade's effects and overlay finish. The ordinary `present` event identifies the swap chain and rejects partial updates; it no longer modifies the stereo source before effects run.
- Submit one L/R pair per source frame. Remove the adaptive one/two-pair feedback loop that added more VSync blocking when the game slowed. The injected left Present uses `DO_NOT_WAIT`, with a 100 ms bound on queue-pressure retries. This bounds the retry loop; it cannot guarantee an arbitrary driver call or the game's own Present will never hang.
- On injected-present failure, restore the packed source and pause splitting. Occlusion remains inactive and automatically retries when the window is visible. Other presentation failures remain paused until the host disables/re-enables the hook or the swap chain is recreated.
- Keep eye records local until the entire ring and status are published under the existing seqlock. Publish the accepted left eye before the game's right Present. Count a completed pair only when the right Present counter advances once. ReShade does not expose that Present's HRESULT to `finish_present`; counters are submission evidence, not proof of scanout.
- Keep one swap-chain owner for this process's v1 channel so secondary windows do not interleave unrelated counters. This is not a new cross-process arbitration protocol.
- Preserve buffer count and waitable-swap-chain flags. Unsupported D3D12 creation is no longer modified. This repair remains D3D11 only and requires the D3D11.1 context-state interface.

## Validation

Run `experiments/addon-regression/Run-Tests.ps1`. It builds separately from the app, using the already installed ReShade add-on runtime. The test addon has a distinct IPC channel and does not contact the emitter or the real app. **Never deploy the test addon.** Deploy `experiments/addon-regression/build/package/Release/VisionGameHook.addon64`.

Passed with the actual ReShade runtime and hardware D3D11 device:

- GPU readback of the actual addon shader for left/right SBS and top/bottom, swapped eyes, center and eye-boundary pixels.
- RGBA8, RGB10A2 and RGBA16F formats, including resource recreation after resize.
- Constant-buffer subrange preservation and producer ring isolation before publication.
- Addon load, host-disabled pass-through, disable, and a three-buffer waitable flip swap chain.
- Occluded presentations remain inactive and produce zero stereo records.

Windows reports the test window as occluded in this tool session. Visible L/R presentation cadence is therefore explicitly **skipped**, not passed. The old addon accepted the same occlusion status as successful stereo. Glasses timing and Luigi's Mansion playability require a retest in the user's desktop session.

## App integration notes

`src/sync_protocol.h` and its v1 layout remain unchanged. The app still owns the emitter. The existing host estimates future eye timing from presentation statistics; that remains separate work for the app. Removing adaptive repetition means slow source frames can remain on screen for extra refreshes. This repair does not claim equal-duration optical L/R cadence for a 30 fps source on a 120 Hz display.

The addon still asks ReShade to force sync interval 1 when creating a D3D11 swap chain, including while the host is disabled; ReShade's creation event does not provide a per-Present toggle. Ordinary packed-image pass-through is preserved, but its VSync behavior is not identical to running without the addon.

ReShade's own state preservation is outside this patch. The new state isolation protects changes made by this addon. Context-state objects can interact with overlays that infer D3D10 support from a D3D11 device; ReShade's source mentions RTSS as one such compatibility case. RTSS was not part of the regression run.

Retest by restarting Dolphin normally with its current D3D11/SBS settings and the app connected to the emitter. First check that Luigi's Mansion renders and maintains emulation speed. Then inspect eye timing. The first accepted complete pair now writes `[Vision] Completed stereo pairs: 1` to `ReShade.log`; errors and occlusion have explicit messages. A `Hook ready` line alone proves only resource initialization.

References: [ReShade callback definitions](https://github.com/crosire/reshade/blob/main/include/reshade_events.hpp), [context-state preservation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate), [DXGI present flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
