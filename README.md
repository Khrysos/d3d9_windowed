<p align="center">
  <h1 align="center">d3d9_windowed</h1>
  <p align="center">
    A lightweight Direct3D9 proxy DLL for windowed / borderless behavior.
  </p>
</p>

---

## What this does
- Forces **windowed / borderless** behavior for Direct3D9 games
- Ignores "focus-lost" behaviour
- Disables mouse-clipping 

> This project is intended to be **non-invasive**: it does not modify game files beyond placing a proxy DLL next to the executable.

---

## Installation (drop-in)
1. **Download** or **Build** the DLL (see **Build** below)
2. Copy `d3d9.dll` and `preferences.ini` into the same folder as the game EXE
4. Launch the game

### Uninstall
Delete `d3d9.dll`  and `preferences.ini` from your EXE folder

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

## Troubleshooting
### Nothing changes in-game
- Confirm the DLL is next to the **correct** game EXE
- Ensure you built **x86** for 32-bit games
- Confirm the dll filename is `d3d9.dll`

---

## Credits
- Uses **MinHook** (`third_party/minhook`). See its license in that folder.
