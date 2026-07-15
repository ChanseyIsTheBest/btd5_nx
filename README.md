# Bloons TD 5 — Nintendo Switch port (Ninja Kiwi engine wrapper)
 
This is a native wrapper / loader that runs the original ARM64 build of Bloons TD 5 on Switch homebrew. It contains no game code and no game assets.

## Install & run
 
You need files from the Android build of Bloons TD 5 Version 4.7 (`com.ninjakiwi.bloonstd5`).
 
Copy the `.nro` to your SD card (e.g. `sdmc:/switch/btd5/btd5_nx.nro`), then place your game files next to the `.nro`, in the same folder:

 ```
sdmc:/switch/btd5
├── btd5_nx.nro
├── libnative.so                   <- from your APK: lib/arm64-v8a/
└── Assets/                        <- from your APK: the whole assets/ folder
```
Optionally, drop a `cursor.png` (64x64, transparency supported) in the same folder to replace the on-screen cursor with your own.

## Controls
 
| Input | Action |
| --- | --- |
| `+` | Toggle the on-screen cursor |
| `-` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked: move to control the cursor, left-click to tap, and use the scroll wheel to change 
sensitivity.
Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after in-game adjustment.

## Requirements (to build)
 
Install devkitPro with the Switch toolchain and these packages:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-freetype switch-libpng switch-zlib switch-bzip2
```

## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `imports`, `error`) derives from the SoLoader lineage — TheOfficialFloW's Vita/Switch loader tradition, by way of the open-source `colorsheep_nx` Switch port, all MIT-licensed.
