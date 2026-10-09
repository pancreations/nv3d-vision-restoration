# Schedule I Stereo SBS (experimental)

Game-only MelonLoader 0.7.3 / IL2CPP mod for Schedule I (Unity 2022.3.62f2).
It renders the game's main camera twice through URP StandardRequest, using parallel eye views and asymmetric projections, then packs left/right eyes into a borderless Half SBS game window. No Vision Restoration source, binaries, settings, or emitter code is changed.

## Use

These setup scripts are a local prototype and require adapting paths to your
installation. `Build.ps1` defaults to
`D:\SteamLibrary\steamapps\common\Schedule I` (override with `-GameDir`) and
hardcodes the Visual Studio 2022 Community C# compiler and .NET runtime
`6.0.36` under `C:\Program Files`. `Deploy.ps1` separately hardcodes the same
game directory. Check and update those paths before running either script.
This adapter is published as experimental source and is not included in the
Vision Restoration portable binary package.

Install MelonLoader x64 in Schedule I, run once for assembly generation, then run Build.ps1 and copy package/ScheduleIStereoSBS.dll into the game's Mods directory. Restart the game.
In your existing stereo capture app select the Schedule I window and **Half SBS** packing. Left eye is on the left by default.

- F6: toggle SBS / normal view; retry after a render failure.
- F7: swap eyes.
- F8: restore borderless at the current display resolution.
- Ctrl+Left/Right: decrease/increase eye separation by 5 mm.
- Ctrl+Down/Up: decrease/increase convergence distance by 0.5 m.

Settings persist in the game's UserData/MelonPreferences.cfg under ScheduleIStereoSBS. RenderScale defaults to 0.75 per eye; 1.0 increases sharpness and GPU cost. Each eye keeps the full window aspect and is squeezed into half its width.

## Current limitations

Experimental until verified in actual gameplay. Requires the game's main camera to be tagged MainCamera and support URP StandardRequest. It renders two extra camera passes per frame; expect a substantial GPU cost. Screen-space overlay canvases are not included in a camera render request, so HUD/menu overlays may be missing from SBS; use F6 for menus and inventory. World-space UI and camera-stack UI depend on the game's pipeline. Additional overlay cameras can remain monoscopic. Temporal effects, camera-specific culling, and reflections still require gameplay testing. F6 restores ordinary rendering without changing game saves.

## Remove

Close the game and remove only Mods/ScheduleIStereoSBS.dll to remove this mod. To disable MelonLoader too, rename the version.dll installed by this setup (only when it is the MelonLoader proxy); retain the loader and UserData if other mods use them.

## Implementation references

- Official MelonLoader: https://github.com/LavaGang/MelonLoader
- Unity render requests: https://docs.unity.com/en-us/engine/6000.7/script-reference/unityengine/rendering/renderpipeline/submitrenderrequest

These sources document the loader/render API; they do not establish compatibility with this game build.
