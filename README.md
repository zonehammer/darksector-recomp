# Dark Sector Recompiled

A work-in-progress native Windows port of **Dark Sector** (Xbox 360, 2008) made by **static
recompilation**: the game's PowerPC code is translated to C++ and compiled for x64, then
run on a small runtime that plays the part of the console. Graphics and audio use
[Xenia](https://github.com/xenia-project/xenia)'s GPU and audio emulation; the
recompiler is [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

It is built on the runtime, launcher and tools of
[WET Recompiled](https://github.com/zonehammer/wet-recomp),
[Conan Recompiled](https://github.com/zonehammer/conan-recomp) and
[Fight Night Champion Recompiled](https://github.com/zonehammer/fightnight-recomp).

**This repository contains no game data.** You need your own copy of Dark Sector (title ID
`445007D4`). The build extracts your disc and generates the C++ on your machine;
nothing derived from the game is ever committed.

## Status

**First release (0.1.0): playable.** It boots, plays, saves and loads. It hasn't been
played through to the end yet; please report problems with `darksector.log` (next to the
game's exe).

## Features

- Native x64 Windows build of the game's own code, no emulator.
- Higher resolutions (`resolution_scale` 1-3: 720p, 1440p, 4K) with SMAA / FXAA, AMD FSR
  / CAS or NVIDIA Image Scaling, and 16x anisotropic filtering.
- 30 FPS as on the console, or higher frame rates (60, 120, 144, 240 ...); particle effects
  keep updating 30 times a second so they look as the game intended (F1 > Game).
- Ultrawide (21:9, 32:9) with the HUD and menus kept 16:9 in the middle, and a field of
  view slider.
- Controllers through SDL (Xbox, PlayStation and others) with rumble; keyboard and mouse
  with native mouse look and rebindable keys.
- Button prompts that match what you play with: Xbox, PlayStation (the game's own PS3
  art), or keyboard / mouse keys that follow your bindings.
- Skips the startup logos; the game's network checks answered offline (no 30-second wait).
- Some of the game's own graphics settings as switches (F1 > Game): cascaded shadows
  (a big speed-up at high resolutions), soft shadow edges, depth of field in play.
- Testing cheats: god mode and infinite ammo (the game's own).
- Saves in `game\hdd` with automatic backups; achievements with pop-ups.
- Settings overlay (F1), screenshots (F12), brightness / contrast / gamma / colour.
- Launcher with settings, save backups and one-click updates.

## Installing (Windows 10/11, 64-bit)

You need your own Dark Sector disc image (tested with *Dark Sector (Europe)*), or the folder of an already extracted disc, and about 6 GB of free space.

1. Download **DarkSectorSetup.exe** from the
   [latest release](https://github.com/zonehammer/darksector-recomp/releases/latest).
2. Run it, choose your disc image (or extracted folder) and a folder to install to, and press
   **Install**. It downloads the build tools it needs (Git, CMake, Ninja, Clang, Python),
   gets this source code, extracts your disc and builds the game on your PC. The first build
   takes a while (10-30 minutes).
3. Start **Dark Sector Recompiled** from the desktop shortcut: the launcher has the settings,
   then press **Play**. In game, F1 opens the settings overlay.

The launcher's **Check for updates** / **Update now** gets and builds new releases; your
saves and settings stay.

## Building by hand

1. Install [Python 3](https://www.python.org/downloads/) (tick "Add python.exe to PATH"),
   [Git for Windows](https://git-scm.com), CMake and Ninja (`pip install cmake ninja`) and
   the latest **llvm-mingw** release (`llvm-mingw-<date>-ucrt-x86_64.zip` from
   [github.com/mstorsjo/llvm-mingw/releases](https://github.com/mstorsjo/llvm-mingw/releases)),
   unzipped with its `bin` folder on your `PATH`.
2. Build:

   ```bat
   git clone --recursive https://github.com/zonehammer/darksector-recomp.git
   cd darksector-recomp
   python tools\build.py --iso "D:\path\to\DarkSector.iso"
   ```

   This extracts the disc into `game\extracted`, recompiles `default.xex` into
   `generated\ppc` and builds everything.
3. Run `build\runtime\DarkSectorLauncher.exe` (settings, then Play) or
   `build\runtime\DarkSectorRecomp.exe`. Problems: include `darksector.log` (next to the exe).

## How it works

1. `tools/prepare_game.py` extracts the disc into `game/extracted/`.
2. A patched XenonRecomp translates `default.xex` into C++ files in `generated/ppc/`,
   guided by `config/DarkSector_config.toml` and `config/DarkSector_switch_tables.toml`
   (`tools/fix_switch_bounds.py` sizes functions whose jump tables run past the
   analyser's estimate).
3. `runtime/` provides what the console did: guest memory, threads and synchronisation,
   files, input, achievements and the kernel/XAM functions.
4. The GPU commands the game submits run through Xenia's command processor and shader
   translator; XMA audio is decoded by Xenia's decoder and played with XAudio2.

## Credits

- Icon art: zonehammer.

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the recompiler.
- [Xenia Canary](https://github.com/xenia-canary/xenia-canary): the resolve readback /
  downscale and the reference for host occlusion queries.
- [FNC Reborn](https://github.com/whompay/FNCReborn): fixes carried over from Fight Night
  (vector multiply-add rounding, EDRAM ranges).
- [Xenia](https://github.com/xenia-project/xenia): GPU and audio emulation, and the
  reference for the console's behaviour.
- [SDL2](https://www.libsdl.org/), [Dear ImGui](https://github.com/ocornut/imgui),
  [FFmpeg](https://ffmpeg.org/), [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling).

## Legal

Dark Sector Recompiled's own code is released under the [MIT licence](LICENSE); the third-party
projects keep their own licences. This project contains no game code or assets and is
intended for people who own a legitimate copy of Dark Sector. It is not affiliated
with or endorsed by Digital Extremes or D3 Publisher. Dark Sector is a trademark of its
respective owner.
