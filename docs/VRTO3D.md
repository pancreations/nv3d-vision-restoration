# VRto3D SBS / TAB input

**Input/Games > Source > VRto3D (SBS / TAB)** captures VRto3D's packed stereo
window and feeds the two eyes to Vision Restoration's preview or fullscreen
output. SteamVR and VRto3D still run the VR game. Vision Restoration controls
its own frame-sequential presentation and emitter timing.

## Set up the running session

1. Start SteamVR, VRto3D and the VR game. Set VRto3D's output to **SbS** or
   **TaB**, and keep its output window visible and unminimized. It can remain
   behind Vision Restoration's output.
2. In VRto3D, press **Ctrl+Home**, open **System**, turn **Auto Focus OFF**,
   then choose **Save Default Cfg** and close the menu. In VRto3D V5.0.0,
   disabling Auto Focus through this menu also releases its current foreground
   lock. This prevents VRto3D from repeatedly putting its window over our
   fullscreen output. See the [upstream V5 implementation](https://github.com/oneup03/VRto3D/blob/V5.0.0/vrto3d/src/hmd_device_driver.cpp#L698-L706).
3. Select **Input/Games > Source > VRto3D (SBS / TAB)** in Vision Restoration.
   The **Window** list automatically discovers the V5 **VRto3D** window and
   older **SteamVR Headset** windows. Use **Refresh running windows**, or
   enable **Show all windows** and select the packed stereo window if its
   name differs.
4. Choose the matching **Packing** from the table below.
5. Optionally select the running game's window under **Game input**. Fullscreen
   output focuses that game once for keyboard/controller input; it does not
   focus the captured VRto3D window. **Keep current input focus** leaves game
   selection to you. Use windowed or borderless mode for the game.
6. Click **Start source**. Check for changing pictures with **Inspect eye
   images**, then use **Start 3D preview**, **Windowed preview** or
   **Fullscreen 3D** with your calibrated display and emitter.

| Packing | Arrangement | Example combined frame for 16:9 eyes |
|---|---|---|
| Half SBS | Left/right eyes squeezed horizontally | 1920 x 1080 |
| Full SBS | Left/right eyes at their native proportions | 3840 x 1080 |
| Half TAB (top / bottom) | Top/bottom eyes squeezed vertically | 1920 x 1080 |
| Full TAB (top / bottom) | Top/bottom eyes at their native proportions | 1920 x 2160 |

Select the format VRto3D actually produces; resolution alone cannot identify
it. The source restores the intended eye proportions and fits them to the
output. **Swap eyes** reverses left and right. Changing packing restarts an
active source. Stop capture before choosing another source window.

**Launch-VRto3D.cmd** opens the app with this source selected. The portable
package includes this launcher beside `VisionRestoration.exe`; the source
checkout version also builds the app if its executable is missing. The
equivalent command is `VisionRestoration.exe --vrto3d`. Select the window and
start the source in the app; the launcher does not launch a VR game.

## Capture and foreground behavior

This source always uses **Windows Graphics Capture**, regardless of the saved
capture preference for ordinary window capture or AI desktop. The captured
picture and the game's input window are separate selections. Source selection
does not rewrite VRto3D settings, install a driver, restart SteamVR or send a
foreground-lock toggle.

VRto3D V5's enabled lock reasserts its window as topmost about once a second.
Its [presenter code](https://github.com/oneup03/VRto3D/blob/V5.0.0/vrto3d/src/presenter/window_presenter.cpp)
explains why two fullscreen windows can keep covering each other. If that
continues, check **Auto Focus** in VRto3D again. For older versions,
**Ctrl+F8** toggles the foreground lock; use it only when the lock is enabled.
The [upstream setup instructions](https://github.com/oneup03/VRto3D#installation)
describe that shortcut. Closing or minimizing the captured window can stop
incoming pictures; restore it and restart the source if needed.

## Validation limits

GPU and synthetic fixture checks can verify eye packing, proportions and
capture behavior for their test windows. They do not establish working capture
for a particular VR game, sustained frame rate, input behavior, optical
synchronization or crosstalk through the glasses. This integration remains
experimental pending real gameplay and optical checks on the user's setup.
It does not establish that ordinary DXGI game capture is fixed.
