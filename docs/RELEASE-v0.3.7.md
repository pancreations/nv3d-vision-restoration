# Vision Restoration Alpha 0.3.7

Hotfix released October 19, 2026 for Windows 10/11 (64-bit). Tag: **v0.3.7**.

## Fixed

- **The fullscreen menu could not be hidden.** In 0.3.6 the control menu is drawn inside the 3D picture. Pressing **Hide menu** or the menu shortcut hid the control window, but the menu's image stayed on the 3D output and could not be removed until fullscreen was stopped. Hiding the menu now removes it from the picture straight away, and the shortcut or tray icon brings it back.

Nothing else changed from 0.3.6.

## Upgrading

Download **Vision-Restoration-Portable-v0.3.7.zip**, extract it, and run **Start Vision Restoration.cmd**. Close the old version first. To keep your calibration, copy your `profiles` folder and `emitter.fw` from the old folder into the new one.

Not included, as before: NVIDIA's emitter firmware, the AI depth models and game fixes. The README explains where to get each.

## Known limitations

Unchanged from [0.3.6](RELEASE-v0.3.6.md).
