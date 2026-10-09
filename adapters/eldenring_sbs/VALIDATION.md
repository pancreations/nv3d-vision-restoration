# Validation on 2026-09-22

Target: `F:\SteamLibrary\steamapps\common\ELDEN RING\Game`, executable 2.7.1.0.
Game SHA256:
`1A3547101327F65D0C76DA2F9190AC0AA66871EA42BAE2AECC61E11A8B597891`.

- Camera-copy signature is unique in `.text`, RVA `0x681970`. Local disassembly
  confirms copies of the 0x10 camera matrices into the three GameRend cameras.
- Build: MSVC 19.44, x64 Release. 3,333 camera, pair-integrity and GPU-compositor
  checks passed. Standalone project leaves the main app's build/source edits
  independent.
- Real DX12/ReShade fixture: near/far disparities were 32/8 pixels at 640x480
  and 40/10 at 800x500. Reversing source camera order produced -32/-8 and
  -40/-10. Stale camera stamps fell back to mono SBS, and resize recovered.
- Installer/uninstaller round trip in an isolated fixture restored all original
  graphics XML values and archived every installed runtime file.
- Live game: camera hook and 3840x2160 SBS resources initialized. The first
  gameplay run recorded 25,121 valid camera updates and 12,559 complete pairs,
  with no invalid camera matrices reported.
- ReShade screenshot of actual game output showed two distinct viewpoints.
  Static block matches in that image measured right-minus-left shifts of 33
  pixels on a foreground rock, 18 on a nearer tree, and 10 on distant buildings.
  This both establishes depth-dependent parallax and exposes reversed raw eye
  identity. The corrected build reverses source textures before convergence.
- Vision Restoration's existing window input received 3840x2160 stereo frames;
  its output logged approximately 239.98 Hz and an active USB emitter. Desktop
  capture showed the app's expanded eye view over the game, while the ReShade
  screenshot confirmed the underlying packed SBS source.
- The corrected module was installed through the hash-verified pending update
  path after the game exited. The offline launcher restarted successfully.

Local evidence is under `reports/eldenring-sbs-source.png`,
`reports/eldenring-sbs-source-preview.jpg`, `reports/session.log`, and
`build/eldenring-sbs-dx12-*`. Runtime logs are in
`Game\VisionEldenSBS\VisionEldenSBS.log` and `Game\ReShade.log`.

These checks do not prove shutter alignment, comfortable depth, correct HDR
luminance, temporal stability, or compatibility with every area/cutscene/menu.
The screenshot analysis was from the initial build before eye-order correction;
the corrected source order passed the real DX12 fixture and needs its own live
capture and optical confirmation. Alternate-frame views have different animation
times; no simultaneous two-view rendering or time-warp compensation is claimed.
