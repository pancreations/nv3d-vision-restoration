# Direct stereo capture implementation status

Frame-sequential presentation is the priority. Window-captured SBS/TAB is a fallback. A packed transport containing two full-resolution eyes (such as Katanga) is distinct from capturing a half-width SBS game window.

## Implemented

- `vision_direct_eyes` is a D3D11 provider library. It accepts explicitly identified left/right textures or sequential eye submissions with a common pair ID. It transports a two-slice texture array without packing, scaling, or inferring eye identity from capture-frame counts.
- An incomplete pair is private to the producer. Duplicate eyes, mismatched pair IDs, changed dimensions within a pair, and incompatible devices are rejected. Both copies must finish on the GPU before the pair becomes readable.
- The consumer snapshots completed pairs into its own GPU resources. The presenter consumes its bounded queue in order at stereo-cycle boundaries. A full queue applies backpressure; it does not overwrite unread pairs. Pause and startup blanking do not drain this queue.
- The left **Input/Games** tab has **Game stereo**. Choose the executable; its running window is matched automatically, and capture is enabled when its stereo provider is available. Selecting an unmodified game does not install an adapter or expose its textures.
- The existing Geo11 output runtime now selects the oldest completed pair and never reclaims unread slots. Its blocking Present waits for a producer permit or reports a failure; the former 100 ms timeout no longer returns false success. Nonblocking Present reports `DXGI_ERROR_WAS_STILL_DRAWING` so its caller can retry.
- Ordinary capture no longer publishes an unfinished GPU copy after a 100 ms timeout or a failed query allocation. On a GPU error it retains the last completed pair and reports the error.

These changes do not prove that output will never miss a refresh under GPU load. Retaining complete pairs avoids an input-order error; presentation deadlines and optical emitter timing remain separate requirements.

## Provider contract

Include `src/direct_eyes.h` and link `vision_direct_eyes`. Open a `vision::direct::Producer` on the native D3D11 device, normally using the game PID as its channel. Call `submit` on the immediate-context thread after rendering each eye. Other users of that context must be synchronized by the adapter.

Use increasing, nonzero pair IDs and explicit `Eye::Left` / `Eye::Right`. Either eye may arrive first. Supply matching dimensions, format and encoding for the second eye. Single-sample RGBA8, BGRA8, RGB10A2 and RGBA16F are supported; resolve MSAA before submission. The source device must use the same adapter as the app output.

- `S_FALSE`: first eye copied and retained; submit its partner.
- `S_OK`: complete pair published.
- `DXGI_ERROR_WAS_STILL_DRAWING`: submission was not consumed; retry that same eye and ID when space is available.
- Failure: stop that publication and report it. A cancelled or failed GPU copy closes the channel; recreate the producer and reader before continuing.

`reset()` explicitly abandons a partial pair during renderer reinitialization. It cannot replace an unread complete pair. One producer and one consumer may own a channel. Destroy the reader and producer before recreating a channel; an existing mapping is never silently replaced. An abnormal process exit may require restarting the remaining endpoint.

## Connecting an adapter

The left **Input/Games** tab's **Connect game** action connects the selected game's existing stereo renderer. Geo11 is detected automatically and uses its established `proxy_d3d11` interface through the existing VisionStereo11 adapter; it does not load ReShade. Its renderer bytes, shader fixes, output mode and tuning are retained. The only provider configuration change establishes the proxy chain, recorded for restoration. Native stereo uses the matching ReShade capture add-on. Classic 3Dmigoto connection is rejected before any file writes: the capture add-on does not supply its stereo-rendering backend. Connect while the game is stopped, then launch it normally and use **Start game capture** in the same tab. Native and legacy inputs require an explicit output layout; detecting 3Dmigoto does not imply sequential backbuffers. This is general integration, with no DK64-specific configuration.

`tools/Get-CaptureRuntime.ps1` downloads the pinned official ReShade 6.8.0 full-add-on runtime. `tools/Build-StereoRuntime.ps1` builds both capture architectures. In the normal build these files are already under `bin/Release/runtime/x86` and `x64`.

The capture add-on hooks actual D3D11/D3D12 Presents. Sequential mode retains each declared eye, waits for its partner, and publishes only after the Present count confirms success. `DXGI_PRESENT_TEST` does not advance phase. A slow consumer applies backpressure; unread pairs are not overwritten. GPU copies must complete before publication. The adapter adds no game Presents. The app presents the received full-resolution eyes through its independent sequential output.

The first successful Present defines the selected left-first/right-first phase. A failed or ambiguous Present stops capture instead of guessing eye identity. Restart the game after such an error. Games whose backbuffer remains mono or SBS while internal textures contain separate eyes need another producer integration; selecting sequential cannot reconstruct those internal eyes. Array mode requires an actual stereo texture array. Vulkan capture is not implemented.

Geo11 capture reads its completed eyes below the existing renderer, after packing, and publishes separate eye slices on the native output device. With `katanga_vr`, both eyes retain full resolution. Its mapping is isolated to the game PID by the existing proxy hook. Existing SBS/TAB configurations, including reversed layouts, retain their supplied resolution. The capture route does not register as the in-game emitter host: the app owns sequential presentation. Consumer backpressure waits without replacing unread pairs; disconnect or shutdown releases the wait. The game's preview remains mono. The standalone ReShade Katanga adapter still uses the upstream session-wide mapping and is not the Geo11 integration path.

## Tested compatibility and remaining work

| Provider | Evidence | Limitation |
| --- | --- | --- |
| Native D3D11/D3D12 sequential | Real ReShade add-on transfers four ordered full-resolution pairs; explicit reversed order and slow consumer tested; x86/x64 | Hidden synthetic renderer, not gameplay or optical validation |
| Stock 3Dmigoto | Real legacy DLL coexists with the final connector and transfers the probe's sequential eyes; clean exit and removal; provider DLL hash unchanged | Test disables NVIDIA stereo and supplies explicit eye frames. This does not restore the missing NVIDIA stereo-rendering backend |
| Katanga protocol | Real geo-11 publishes full-resolution eyes through its proxy hook; the separate ReShade protocol fixture also passes | No optical display validation |
| Geo11 | Production proxy transports 90 ordered pairs from an ordinary geometry draw; Katanga, SBS/TAB and reversed packing tested with the actual x86/x64 renderer | Synthetic scene, not a compatibility guarantee for every community fix or game |
| Stereo texture array | Transport and actual app shader tested | Native DXGI stereo-swapchain capture not integration-tested |
| SBS/TAB | Packed fallback supported; SBS actual add-on tested in D3D11/D3D12 | Retains source resolution; not the preferred sequential input |

The same existing Geo11 adapter supports either in-game presentation or independent app capture. The capture configuration selects the latter automatically; do not install both connections. The earlier ReShade/geo-11 chain failed to expose stereo and is no longer selected for geo-11. An unrelated exit crash was reproduced with the upstream sample help-overlay resources; a minimal fix configuration exits cleanly using the unchanged geo-11 binary. The regression harness omits those demo resources, and the connector does not remove or rewrite a user's shader resources to work around them.

## Provider contract additions

`Producer::submit` accepts an optional source region and deferred publication. With `deferPublication=true`, call `commit()` only after the source Present succeeds; a completed GPU copy alone does not establish a successful game Present. `connected()` reports the reader state. `reset(true)` may discard an unread pair only after the reader has disconnected; a connected reader's unread pair is never reclaimed.

## Normal build and validation

The app is `build/bin/Release/VisionRestoration.exe`. Updates use this normal launch location. The separate review executable was removed; game installations and user settings were not changed during consolidation.

`vision_direct_eyes_tests` checks GPU completion/error handling, producer waits, cross-device/process eye sharing, odd-sized full-resolution pixels, pair identity, deferred commits, ordering, resizing, backpressure, and cancellation. The app's offscreen `--gpu-test` checks the actual separate-eye shader and packed-eye paths. `tools/Test-CaptureAdapter.ps1` exercises real ReShade installation, pixel transfer, eye order, test Presents, and removal using hidden D3D11/D3D12 windows. Katanga in that script is a simulated provider, not geo-11.

`tools/Test-Geo11Capture.ps1` runs the actual geo-11 binary through the production proxy, with hidden windows and a real direct-eye reader. It verifies pair ordering/counts, distinct geometry in both eyes, slow-reader backpressure, provider hashes and clean shutdown. It does not use a simulated Katanga publisher. Both architectures also pass the separate-factory creation path with `VISION_CAPTURE_TEST_STALL=6000`: a six-second held pair resumes with all 90 pairs present and ordered, followed by clean shutdown.

No emitter or visible-game test was performed during the active DK64 session. Black-flash elimination and physical eye timing still require end-to-end display testing.

The left workspace has exactly two tabs: **Tuning** and **Input/Games**. The latter combines game selection, connection, source selection, live window/provider status and capture controls. Legacy in-game connection controls are no longer a competing setup flow.

The MGSV v1.6 installation was compared against the author archive: all 67 original files matched (`reports/mgsv-fix-verification.json`). Its 3Dmigoto 1.2.50 startup crash was reproduced in an isolated fixture and fixed by obtaining the existing D3D11 device adapter instead of recursively creating a DXGI factory from the capture add-on. The fixture now transfers sequential pairs and exits cleanly. This fixes startup compatibility; it does not supply the missing NVIDIA stereo rendering backend or prove MGSV gameplay stereo.

## Generic game output

`Input/Games > Start game 3D` starts the existing direct-eye source and a nonactivating, click-through output window over the selected game's client area. The window follows that area, hides when the game loses foreground focus or minimizes, and stops when the game closes. This path uses no game names, shader modifications or per-game profiles. Use windowed/borderless games on the selected display.

The build and hidden `--smoke-test --games --game-overlay-smoke` check pass (client placement, nonactivating styles, presenter startup, no emitter writes). The actual geo-11 x64 Katanga fixture also transfers 90 ordered distinct-eye pairs and exits cleanly. Gameplay and optical validation remain outstanding. Classic 3Dmigoto on a system without its rendering backend remains unsupported; no ReShade installation is offered as a substitute.
