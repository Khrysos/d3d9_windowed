<p align="center">
  <h1 align="center">d3d9_windowed</h1>
  <p align="center">
    A lightweight Direct3D9 proxy DLL for windowed / borderless behavior.
  </p>
</p>
---

## What this does
- ✅ Forces **windowed / borderless** behavior for Direct3D9 games
- ✅ Helps with **alt-tab / focus** behavior (depending on game)
- ✅ [Add/remove bullets based on what your DLL actually does]

> This project is intended to be **non-invasive**: it does not modify game files beyond placing a proxy DLL next to the executable.

---

## Install (drop-in)
1. Build the DLL (see **Build** below)
2. Copy the output DLL into the same folder as the game EXE
3. Rename the DLL to `d3d9.dll` **if your game expects a D3D9 proxy**
4. Launch the game

### Uninstall
Delete the `d3d9.dll` you added (and any config file you created).

---

## Build
### Requirements
- Visual Studio Community (Desktop development with C++)
- Windows SDK installed via the VS installer

### Steps
1. Open `d3d9_windowed.slnx`
2. Select:
   - Configuration: `Release`
   - Platform: `x86`
3. Build:
   - `Build → Build Solution`

> Output location depends on your project settings. If you set a unified output folder, document it here.

---

## Configuration
If you have config toggles, document them here. Example:

- `BORDERLESS=1`
- `ALLOW_RESIZE=1`
- `IGNORE_FOCUS_LOSS=1`

(If you don’t have a config file yet, delete this section.)

---

## Compatibility notes
- ✅ Tested: Prince of Persia: Warrior Within (Steam)
- ✅ Likely compatible: many Direct3D9 titles
- ⚠️ Some games may behave differently depending on how they create their window/device.

---

## Troubleshooting
### Game doesn’t start / crashes immediately
- Confirm the DLL is next to the **correct** game EXE
- Ensure you built **x86** for 32-bit games
- Try removing overlays (Discord/RTSS) to test

### Nothing changes in-game
- The game may not be loading your proxy DLL
- Confirm the proxy filename should be `d3d9.dll` for this title

---

## Credits
- Uses **MinHook** (vendored in `third_party/minhook`). See its license in that folder.
- [Any other libraries/tools]

---

## License
[Your license choice here]
