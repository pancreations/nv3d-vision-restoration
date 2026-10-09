# Elden Ring SBS for Vision Restoration

Experimental **two-camera geometry stereo** for the inspected Elden Ring
executable version **2.7.1.0**. It renders the left and right views on consecutive
game frames, then packs the completed pair as **left | right** SBS. No depth map,
AI depth, Luke Ross component, or VRto3D component is used.

**This is alternate-frame stereo.** At 60 game frames/s the source supplies about
30 stereo pairs/s. Both views use the same central camera pose by default, but
world animation advances between them. This can cause motion disparity. It is
not a simultaneous two-view renderer. TAA, DLSS and frame generation need
separate eye histories; they are excluded from the initial test configuration.

## Start

1. Close Elden Ring and run `Install.ps1` from the built package. The default
   target is `F:\SteamLibrary\steamapps\common\ELDEN RING\Game`.
2. With Steam running, use **Launch Elden Ring SBS.cmd** in the Game folder.
   This launches the unprotected modded session directly. Play offline. Do not
   use the protected Steam launcher with this experimental camera add-on.
3. Load a character. Menus or missing camera updates may show identical images
   in both halves; those are explicitly mono, not evidence of stereo rendering.
4. In Vision Restoration choose **Input/Games > Source > Capture window**, select
   Elden Ring, choose **Side by side**, and start the source. Use your existing
   calibrated output/display profile. Do not use **Connect game / Prepare game**
   for this package; those buttons install a different provider adapter.
5. Start the app's 3D output. The game's borderless window supplies the SBS image;
   Vision Restoration handles the frame sequence and emitter as usual.

Each half is squeezed to half the game-window width, then expanded by Vision
Restoration. At 3840x2160, SBS contains 1920x2160 pixels per eye. The game renders
each camera at its full configured resolution before packing.

## Controls

| Key | Action |
| --- | --- |
| F6 | Toggle SBS / ordinary mono |
| F7 | Swap SBS eye order |
| F8 | Reload the INI settings and write camera/pair counters to `VisionEldenSBS.log` |
| Home | ReShade overlay |

Edit `Game\VisionEldenSBS\VisionEldenSBS.ini`, then press F8 in the game:

- `EyeSeparation`: distance between the camera positions in game world units;
  default 0.065. Increase cautiously after verifying eye order.
- `ConvergenceShift`: uniform horizontal translation per eye as a fraction of
  an eye image's width. Positive values bring the screen plane closer. Default
  0.0025. This changes convergence without warping depth.
- `LockPairPose=1`: use the same central camera pose for both renders. This does
  not freeze the simulation, enemies or animation.
- `ReverseCameraOrder=1`: source ordering calibrated for executable 2.7.1.0.
  It corrects the rendered eye identity before convergence is applied. Leave
  this set unless measuring camera/render correspondence for another build.

## Installation and removal

The installer refuses unknown game hashes and existing graphics loaders. It
adds a private ReShade 6.8.0 add-on directory, changes display mode to borderless
and disables anti-aliasing after backing up GraphicsConfig.xml. Other quality
settings and save files are preserved. ReShade loads only this add-on directory;
the existing ERSS frame-generation stub stays on disk but is not loaded.

To remove it, close the game and run
`Game\VisionEldenSBS\Uninstall.ps1`. It archives the installed files beside the
Game directory and restores only the graphics fields still equal to the values
it installed. The original `eldenring.exe` and `start_protected_game.exe` are
never replaced. Other pre-existing mods remain separate.

## Evidence and limits

The camera function's unique signature and RVA were checked against the local
2.7.1.0 executable. The function copies three camera structures with matrices at
offset 0x10. The add-on offsets their positions along the local camera right
axis after the copy. Unknown versions, signatures and invalid matrices are
rejected. Nothing writes to the game executable on disk.

Camera math/pair-integrity tests and the GPU SBS shader tests pass. A real DX12
swapchain running through the ReShade add-on reproduced the expected near/far
disparity at 640x480 and 800x500, rejected stale camera stamps, and recovered
after resize. These are synthetic renderer tests, not an Elden Ring gameplay or
glasses result. A live 3840x2160 Elden Ring scene also produced distinct views:
before source-order correction, a fixed foreground rock matched at 33 pixels
right-minus-left and distant scenery at 10 pixels. This confirms depth-dependent
parallax and establishes that raw camera identity needs reversing. Vision
Restoration received the SBS window and drove its 240 Hz output and emitter.
Cutscenes, menus, lock-on, effects, motion, HDR accuracy, sustained gameplay and
optical quality through the glasses still need validation. HUD elements receive
the same uniform convergence shift as the scene; automatic HUD isolation is not
implemented.

The add-on never inserts extra Presents or drives the emitter. It outputs a
complete stereo pair only after consecutive, distinctly tagged camera renders.
Missing/ambiguous camera updates fall back to mono SBS. Motion and camera/render
latency need further validation beyond the static scene checks above.

## Build and test

From the repository root:

```powershell
.\tools\Build-EldenRingSBS.ps1
.\tools\Test-EldenRingSBS.ps1
```

The production module is `build\eldenring-sbs\Release\VisionEldenSBS.addon64`.
`VisionEldenSBSTest.addon64` is only for the isolated regression fixture and is
never included in the installable package.
