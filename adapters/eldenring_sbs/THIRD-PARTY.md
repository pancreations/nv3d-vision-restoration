# Third-party components and references

- ReShade 6.8.0, Patrick Mours: https://github.com/crosire/reshade/tree/v6.8.0.
  Supplies the Direct3D 12 hook and add-on API. Included binary comes from the
  official add-on installer, extracted without running setup. BSD-3-Clause;
  see ReShade-LICENSE.md in the built package.
- MinHook 1.3.4: https://github.com/TsudaKageyu/minhook/tree/v1.3.4.
  Linked into the camera add-on. See MinHook-LICENSE.txt in the built package.
- Logersnamed, FreecamMod, MIT, commit
  `99d44a75e9ebb316043635d4d8b98c007bffa7dd`:
  https://github.com/Logersnamed/FreecamMod/tree/99d44a75e9ebb316043635d4d8b98c007bffa7dd.
  Camera layout and UpdateCameraMatrix signature reference. See
  `src/core/game_data/field_area.h`, `src/core/game_data_manager.h` and
  `src/freecam.cpp`. The signature was independently matched and the function
  disassembled in the user's installed executable. The complete reference mod
  is not installed or shipped. Its license is included as FreecamMod-LICENSE.txt.

Vision Elden Ring SBS code is GPL-3.0-or-later, like Vision Restoration. No game
binary, proprietary VR mod, game data or saved game is redistributed.
