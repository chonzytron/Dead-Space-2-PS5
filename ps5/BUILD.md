# Building Dead Space 2 for PS5

How to build the release app (`PPSA94108`) from this repository. The build follows the
[GTA IV PS5 port](https://github.com/elripalda/gta4-ps5)'s: the same toolchain, the same kind
of static program, the same link and packaging steps.

> **Status:** release 1.0 is built, linked and packaged this way and played on a PS5: native
> 4K at 60 frames a second in play (cutscenes at the game's own 30). Rare short hitches
> remain where the game moves into a new area.

No game files are needed to build, and none are in the repository: the recompiled game code
is already here (`src_tu2/`). Players supply their own files on the console.

## What you need

A Linux PC (built on Fedora and Ubuntu 24.04) with:

- git, wget, curl, unzip, xz, tar, make, rsync, sha256sum, python3 (with mako, ply, yaml),
  CMake 3.25+, Ninja, meson **1.4 or later**, glslang (`glslangValidator`), bison, flex,
  gcc/g++, pkg-config, clang 18 or 19 and lld
- Ubuntu: `apt-get install git wget curl unzip xz-utils make rsync python3 cmake ninja-build
  meson glslang-tools bison flex gcc g++ pkg-config clang-18 lld-18 python3-mako python3-ply
  python3-yaml` (then a newer meson, e.g. `pip install meson`)
- Several GB of memory per compiler job: the recompiled game is 157 large files

## Build

```
git clone https://github.com/chonzytron/Dead-Space-2-PS5
cd Dead-Space-2-PS5
ps5/tools/build_ps5.sh
```

That one command runs five steps and leaves the release in
`out/ps5/DeadSpace2-PS5-<version>.zip` (the app folder alone in `out/ps5/ps5-app/PPSA94108`):

| Step | What it does |
|---|---|
| `toolchain` | Clones Nazky's [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) at a pinned commit into `thirdparty/` and builds it: the PS5 payload SDK, the PS5 Vulkan driver (mihawk-99's PS5_Mesa RADV), the app start-up code and the signer |
| `sdk` | `ps5/tools/fetch_sdk.sh`: clones ReXGlue at the pinned commit (`sdk/REXGLUE_BASE.txt`) into `thirdparty/rexglue-sdk-ps5` and applies `sdk/rexglue_ds2.patch` (the game's fixes, as on the PC), `sdk/rexglue_ps5.patch` (the PS5 platform layer and renderer work) and `sdk/ffmpeg_ps5.patch` |
| `compile` | CMake and Ninja with `ps5/cmake/ps5-toolchain.cmake`: the SDK, the recompiled game and the app (`ps5/src/`) |
| `package` | `ps5/tools/ps5_package.sh`: links one static program, signs it as `eboot.bin`, and lays out the app folder with `sce_sys/` (icon, home screen background, `param.json`), `install/README.txt` and `ds2.example.toml` |
| `zip` | Zips the app folder |

The first run takes a long time (the toolchain, then the recompiled game). After that, a
change only needs some of the steps, for example:

```
ps5/tools/build_ps5.sh compile package zip
```

Each step's output is in `out/ps5/logs/<step>.log`; a failed step prints the end of its log.
`DS2_PS5_JOBS=4 ps5/tools/build_ps5.sh ...` limits the compiler jobs if memory runs out.

Options: set them once on the build folder (`cmake -B out/ps5 -DDS2_PS5_THINLTO=ON`, after
a first `compile`), then run `compile package zip` again; CMake keeps them.

| Option | Effect |
|---|---|
| `-DDS2_PS5_THINLTO=ON` | ThinLTO across the whole program (slower link, faster game code). Off by default. |
| `-DDS2_OPT=-O3` | Optimisation level of the recompiled code (`-O2` is the PC build's tested default) |
| `PS5_WHOLE_ARCHIVE=0 ps5/tools/ps5_package.sh ...` | Link the runtime and GPU backend as plain archives instead of whole |
| third argument of `ps5_package.sh` | Another title id |

In a Claude Code cloud session the toolchain's build scripts are refused as code from
outside the repository. `.claude/settings.json` allows this one command, so a session can
build and package the port.

## On the PS5

Copy `PPSA94108` to `/data/homebrew/` (to update, copy it over the old one: your files,
saves and settings stay). Everything is in that folder:

- `install/`: your Disc 1 image (and Disc 2), Title Update #2, optionally Severed
  (`install/README.txt` says exactly what). The first start checks them and prepares the
  game, with a screen that says how far it is: a few seconds, or a few minutes when Severed
  is unpacked (about 1 GB). It then offers to delete the packages the game no longer needs
  (Cross deletes, Circle keeps); the disc images stay. Later starts go straight to the game.
- `game/`: `default.xex` and the Title Update #2 patch per disc, taken from your files.
- `UserData/`: saves, installed DLC, the shader and pipeline caches.
- `ds2.toml`: optional settings (copy `ds2.example.toml`, which explains each one).
- `logs/`: one log per start. `ps5_perf_log = false` keeps it short.

If something goes wrong, the newest log is what to send: its first line names the build,
`ps5 stack:` lines appear if no frame was drawn for 30 s, and `[ps5 crash]` is the last line
if a thread crashed. `ps5_package.sh` keeps the program with its symbols
(`out/ps5/ps5-link/build/llvm-pie.elf`) to turn those addresses into function names.

## Where things are

| | |
|---|---|
| `src_tu2/` | The recompiled game (Title Update #2), from [Dead Space 2 Recomp](https://github.com/bifftannen1/Dead-Space-2-Recomp); `src/app/ds2_disc_swap.*` is shared with its PC app |
| `sdk/` | The pinned ReXGlue commit and the patches applied to it |
| `ps5/CMakeLists.txt`, `ps5/cmake/` | The build: SDK, recompiled game and app as one static program |
| `ps5/src/ds2_ps5_app.h`, `main.cpp` | The app: start-up settings, the first-start screens, the FPS counter, disc mounting |
| `ps5/src/ds2_ps5_install.*`, `ds2_ps5_io.h` | Finding, checking and preparing the game from the player's files |
| `ps5/src/ds2_ps5_framerate.cpp` | 60 frames a second, 30 in cutscenes |
| `ps5/src/ds2_ps5_gpu_wait.cpp` | The game's waits for the GPU made gentle on the processor |
| `ps5/src/ds2_ps5_lowres.cpp` | The game's 4x MSAA sections kept at 1x (far fewer copies on the GPU) |
| `ps5/src/ds2_ps5_intro.cpp` | Skipping the logo videos |
| `ps5/src/ds2_ps5_disc_swap.cpp` | Automatic disc switching |
| `ps5/src/ds2_ps5_d3d_trace.cpp`, `ds2_ps5_capture.h`, `ds2_ps5_script.cpp` | For tests: the game's Direct3D calls, screenshots, scripted button presses |
| `ps5/src/ds2_ps5_pch.h` | The recompiled code's precompiled header with the PS5's memory offset |
| `ps5/tools/` | The build script, SDK fetch, link and packaging, and the small C files the link needs |
| `ps5/package/` | `sce_sys/` (icon, `pic0.dds`/`pic1.dds` background: 3840x2160 BC7), `install/README.txt`, the settings template |

## Licence

The PS5 layer is derived from the GTA IV PS5 port (GPL-3.0) and uses the PS5 payload SDK's
platform library (GPL-3.0): the PS5 files here (`ps5/`, `sdk/rexglue_ps5.patch`) are under
the GPL-3.0. ReXGlue keeps its own BSD-style licence. None of these cover the game.
