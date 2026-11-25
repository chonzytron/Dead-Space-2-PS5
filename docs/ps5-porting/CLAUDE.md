# Porting a recompiled Xbox 360 game to the PS5

Copy this file to the root of a new porting repository as `CLAUDE.md`, then fill in the
**Project** section. It describes how two Xbox 360 static recompilations were made into
native, jailbroken-PS5 homebrew apps, what the PS5 needs that a PC build doesn't, and what
was learnt about performance on the console.

## Project (fill in)

- Game, region, title id (hex, from the XEX), disc count, title update used:
- Recompiled source tree (path) and how it was generated:
- ReXGlue SDK version the PC build pins (commit) and its game patch:
- GPU path: the SDK's Xenos emulation (Vulkan) or a game-specific native renderer:
- PS5 title id for the package (`PPSA9xxxx`, unique per port):
- Files the player supplies, and their SHA-256 pins:

## The two reference ports

| Repo | What it is | Read it for |
|---|---|---|
| `chonzytron/gta4-ps5` (fork of `elripalda/gta4-ps5`) | GTA IV. ReXGlue 0.9 fork, its own native renderer, a hub app, installer payload, FSR 3. Runs on consoles. | Everything proven on hardware: the platform layer, frame pacing, memory limits, the profile-driven fixes |
| `chonzytron/Dead-Space-2-PS5`, branch `ccr-29702053-rt2nyp` | Dead Space 2. The GTA IV layer ported to upstream ReXGlue 0.10.0, using the SDK's Xenos emulation. Compiles for the PS5, but has **not yet** been linked with the real toolchain or run on a console. | How to carry the layer over to a different SDK version as one patch, the install/disc logic, and checks that need no console |

Upstream and toolchain:

| Repo | Pin used | Role |
|---|---|---|
| `rexglue/rexglue-sdk` | `c94f5ebd` (v0.10.0) for DS2 | Xbox 360 recompilation runtime (derived from Xenia). Already has a Switch (`REX_PLATFORM_NX`) layer and 16 KB host-page support (written for Apple Silicon): both are templates for the PS5. |
| `Nazky/ps5-plume-triangle` | `d54d9adb` | Builds the whole PS5 toolchain with `tools/build.sh`, and provides the link recipe (`tools/radv-link.sh`, `radv_link_recipe`), app CRT (`build/obj/app_crt.o`, `app_cpp_runtime.o`), libSceAgc stubs, the `ps5-native-tool` signer and `runtime/libc.prx` |
| `mihawk-99/PS5_PayloadSDK` | `95c08f27` (pinned by plume-triangle) | clang wrappers, FreeBSD/PS5 headers, libc++ 18, kernel stubs (`sce_stubs/*.c` = the console's exported symbols), `libps5platform` (`ps5platform/shm.h`: `ps5_shm_*`, `ps5_vrange_reserve`) |
| `mihawk-99/PS5_Mesa` | `0b2d6d1a` (pinned by plume-triangle) | RADV for the PS5 as a static `libvulkan_radeon.ps5.a`. Reports Vulkan 1.4, runs geometry shaders as compute, presents through `VK_KHR_display`. |
| `ps5-payload-dev/pacbrew-repo` | v0.40.2 | Prebuilt PS5 libraries (plume-triangle fetches them) |

## Files to open first for context

GTA IV (`gta4-ps5`):
- `ps5/BUILD.md`: the build command, the on-console layout, where things are
- `patches/liberty-ps5.patch`: every SDK change, each marked `[ps5 port]` with the reason. **This is the most valuable file.** Grep it for `[ps5 port]`.
- `ps5/CMakeLists.txt`, `ps5/cmake/ps5-toolchain.cmake`, `ps5/tools/ps5_package.sh`
- `glue/rexglue-sdk-main/gta4-recomp/src/ps5/gta4_ps5_app.cpp`: the app class (start-up, paths, settings)
- `glue/rexglue-sdk-main/patches/ffmpeg-ps5.patch`
- `ps5/payload/installer/`: the fast-install payload (port 9021)

Dead Space 2 (`Dead-Space-2-PS5`, branch above):
- `ps5/BUILD.md`: steps, design notes, the checks
- `sdk/rexglue_ps5.patch`: the platform layer for ReXGlue 0.10.0 as one patch (67 files). Start here when the target game uses upstream ReXGlue.
- `ps5/CMakeLists.txt`: the SDK built in-tree, the recompiled code as an OBJECT library, CMake's own link skipped
- `ps5/src/ds2_ps5_app.h`: `ReXApp` subclass: setting defaults, paths, missing-files screen
- `ps5/src/ds2_ps5_install.cpp`: finds the player's ISOs/title update/DLC, identifies them by XEX execution info, checks SHA-256 pins, stages `default.xex` + `default.xexp`
- `ps5/src/ds2_ps5_pch.h`: overriding the generated code's `REX_PHYS_HOST_OFFSET` without touching the generated tree
- `ps5/tools/`: `fetch_sdk.sh`, `build_ps5.sh`, `ps5_package.sh`, `ps5_syntax_check.sh`, `ps5_symbol_check.sh`, `ps5_symbolize.py`, `ps5_emutls.c`, `ps5_stat.c`, `ps5_relro_pad.c`, `ps5_skip_link.sh`
- `.claude/settings.json`: the permission rule that lets a cloud session run the toolchain build

In the new game's repo: its PC app class (`src/app/*_app.h`), its CMake recipe for the generated code, the generated PCH (`*_pch.h`, which has the memory-access macros), and anything the PC launcher passes on the command line (those become setting defaults on the PS5).

## How a port fits together

```
recompiled PPC → C++ (generated, as checked in)  ─┐
ReXGlue SDK + game patch + rexglue_ps5.patch      ├─ one static PIE ─ RADV + app CRT ─ signed eboot.bin
app: ReXApp subclass, installer, PS5 glue          ─┘
```

- No shared libraries: the runtime and the GPU backend become static libraries, linked with
  `--whole-archive`. Their kernel exports and settings register themselves from static
  initialisers, and the linker would otherwise drop them without any error. The GPU backend
  is created directly (`rex_gpu_create`) instead of loaded as a plugin.
- CMake compiles. `ps5_package.sh` links: it reads CMake's link line
  (`ninja -t commands <target> | tail -1`), uses each input **once** (CMake repeats static
  libraries, and taking one whole twice duplicates every symbol), and runs plume-triangle's
  recipe. Then it signs with `ps5-native-tool` and writes `sce_sys/param.json`.
- The app folder goes to `/data/homebrew/<TITLE ID>/`, which the app sees as `/app0`.
  The player's files go in `install/`; the app checks them and stages them on first start.
  The package contains no game data.

## Workflow for a new game

1. **The PC build first.** The recompiled code must already run correctly on the PC with the
   same SDK commit. The PS5 adds platform risk; it can't be where game bugs get found.
2. **The SDK.** If the game uses upstream ReXGlue, apply DS2's `sdk/rexglue_ps5.patch` on top
   of the game's own patch and fix the conflicts. If it uses a fork, carry the `[ps5 port]`
   hunks over one file at a time. Keep a clean SDK work tree with the base and game patch
   committed, make the PS5 changes there, and regenerate the patch with
   `git diff --cached --binary HEAD`. Never edit the fetched copy by hand.
3. **The app.** Copy DS2's `ps5/` folder and rename. Port the PC app's defaults (launcher
   flags, language) into `OnPostInitLogging`. Write the installer for this game's
   files: XEX execution-info title id and disc number, SHA-256 pins for every XEX/XEXP the
   code was recompiled from, and the title update and DLC formats (STFS content types).
4. **Generated code stays byte-identical** to the verified PC tree. Platform differences go in
   a PCH that includes the generated one and then redefines macros
   (see `ds2_ps5_pch.h`).
5. **Check without a console** (below): syntax, then object code, then symbols, then a full
   build with a stand-in toolchain.
6. **Real toolchain build, link and package**, then run it on a console. Expect the first runs
   to need fixes. Read the log in the app folder after each run.
7. **Profile on the console before optimising anything** (see Performance).

## PS5 platform facts the layer handles

Memory:
- 16 KB pages and 64 KB direct-memory units. The guest's 0xE0000000 physical view needs a
  `+0x1000` host offset (`REX_PHYS_HOST_OFFSET`). The SDK's `PhysicalHostOffset` and the
  generated code's macro **must agree**, or kernel and game see different bytes.
- Guest memory lives in direct memory with mirrored fixed views (`Memory::MapViewsPs5`).
  Reserve the address space with `ps5_vrange_reserve` near 64 GiB (`0x1000000000`).
  Shared memory comes from a small flexible pool, so don't put the guest there. Use
  `shm_open(SHM_ANON)` for anything else that needs it.
- No `MAP_FIXED_NOREPLACE`: inside the guest arena, change protection only and never `mmap`.
  No `/proc`, so `QueryProtect` has to answer from the page table.
- Files can't be memory-mapped: read them with `pread`. Use O_DIRECT for disc images
  (`direct_io.h`), and `Ps5FileCopy` instead of mapped copies.
- `statvfs` always answers 16 GiB, so a free-space check is meaningless.
- **Each kernel allocation counts.** GTA IV stopped getting memory (even 16 KB) at around
  10,000 texture allocations while the driver still showed half its budget free. It now
  treats more than 6,000 images as memory pressure.

Threads, signals, time:
- `ucontext` doesn't switch stacks: fibers use a hand-written `rex_fiber_switch`. Thread
  stacks come from direct memory.
- No realtime signals: suspend/resume use SIGUSR1/2. The signal context's mcontext is at
  offset +64 (`Ps5MContext`). Bad guest accesses arrive as **SIGBUS** as well as SIGSEGV.
- No thread affinity, priority or robust mutexes. Name threads with `pthread_set_name_np`.
- The clock is the TSC (`sceKernelReadTsc`). Its tick counts overflow 64-bit
  multiply-divide, so use 128-bit math, including in the `clock_no_scaling` path.
- `std::recursive_mutex` is slow: the global critical region is a spin lock
  (`rex::thread::global_lock_t`; DS2 replaced 27 sites).
- Thread wake-ups are slow: spin briefly on the signal count before sleeping
  (GTA IV's `ps5_wait_spin_us`).
- **`thread_local` costs a call into the system on each access** (emulated TLS,
  `-femulated-tls`). Keep it out of hot paths.

Toolchain and C library:
- Target `x86_64-sie-ps5`, `-march=znver2 -femulated-tls -fno-stack-protector -fno-plt
  -fdenormal-fp-math=ieee -fvisibility-nodllstorageclass=default`. The target has
  **exceptions and RTTI off by default**: add `-frtti -fexceptions`. libc++ 18 needs
  `-fexperimental-library` (jthread/stop_token). Use the default (small) code model.
  Define `FMT_USE_CONSTEVAL=0`.
- Force-include a compat header (`rex/ps5_compat.h`) that maps Linux names to FreeBSD ones.
  Platform macros: `REX_PLATFORM_PS5` **and** `REX_PLATFORM_LINUX` (to share the POSIX paths).
- FreeBSD ships its own `<sha256.h>`, which shadows a library's: include `"crypto/sha256.h"`.
- Missing from the console's libraries although headers declare them: `fwrite_unlocked`
  (set spdlog's `SPDLOG_FWRITE_UNLOCKED` OFF), `wcslcat`/`wcslcpy`/`wcsnlen` (used by SDL),
  `__cxa_thread_atexit` (provided by `ps5_emutls.c`). `stat`/`lstat` are wrapped
  (`ps5_stat.c`, `--wrap=stat --wrap=lstat`).
- If signing fails with "no room for PS5 process parameters", relink with `ps5_relro_pad.c`.
  The package script already retries this automatically.
- A strong symbol in the app against an SDK stub (for example `__imp__XamSwapDisc`): make the
  SDK's weak on the PS5. A DLL build never had this clash.

Graphics, input, audio, system:
- Vulkan: RADV linked statically, entry point `vk_icdGetInstanceProcAddr`. Some functions
  are only found under their KHR names, so fall back to the alias. The surface is
  `VK_KHR_display` (the first display that reports modes).
- SDL is used with the dummy video and audio drivers only. Turn off X11, Wayland, ALSA,
  HIDAPI, and so on.
- Input through `scePad` (DualSense), audio through `sceAudioOut`. SDL has no PS5 backend
  for either.
- Message boxes become PS5 notifications. The language comes from
  `sceSystemServiceParamGetInt(1, …)`.
- Paths: the executable is `/app0/eboot.bin` and user data is under `/app0/…`.

## Performance: what mattered on the console (GTA IV, from profiles)

- Measure first. GTA IV's biggest wins came from profiles of real runs (a SIGPROF sampler
  across all threads, frame-time captures, per-caller counters), not from guesses. One
  profile had a game thread spending 60% of its time in `memset`.
- Avoid `thread_local` on hot paths (above). Use fixed tables with atomics.
- Don't look up settings by name while frames are built: the cvar registry locks and copies
  strings. Cache the values (GTA IV re-reads at most every 250 ms per call site).
- Spin before sleeping on hand-offs between threads.
- Move work off the game's render thread: GTA IV captures commands on a core of its own,
  through a lock-free byte ring.
- Cache per-draw lookups, since consecutive draws use the same targets.
- Evict unused textures early (40 s) and count the number of images, not only bytes, as
  memory pressure.
- Run expensive passes every other frame where it can't be seen (reflections), and keep
  cutscenes and loading screens at their original 30 fps.
- Frame pacing at 120 Hz output with a 60 fps limit needs care in the presenter.
- Seed a shader/pipeline cache in the package (`ps5/package/shader_cache_seed`; DS2 sets
  `p4_vk_pipeline_cache_path`). Compiling pipelines on the console causes stutter.
- Use exact floating point (`-ffp-model=strict`, `-fno-strict-aliasing`), as the PC builds
  do. x86 FMA contraction changes results: GTA IV fixed fused multiply-subtract cases one by one.
- ThinLTO across the whole program is an option (DS2: `-DDS2_PS5_THINLTO=ON`). The
  recompiled functions span many files, and calls between them can't be inlined without it.
  Get a plain build working first.
- FSR 3 (FidelityFX) runs on the PS5's RADV. Building it needs Wine for AMD's shader
  compiler, plus spirv-tools and glslang.
- For Xenos-emulation ports (DS2): sparse shared memory is disabled as a precaution,
  `readback_resolve=none` and `clock_no_scaling=true` are set as on the PC, and
  `ps5_display_height` defaults to 2160.

## Checks that need no console (DS2's tools)

```
ps5/tools/fetch_sdk.sh
ps5/tools/ps5_syntax_check.sh <ps5 headers> [jobs]                       # every SDK + app source, syntax only
PS5_CHECK_OBJ_DIR=<objs> ps5/tools/ps5_syntax_check.sh <ps5 headers>      # to PS5 object code
ps5/tools/ps5_symbol_check.sh <objs> <PS5_PayloadSDK source tree>         # duplicates, missing, libc coverage
```

- `<ps5 headers>` can be assembled from the payload SDK's **source tree** (no build needed):
  `include/freebsd`, `include/khronos`, `include/ps5`, `platform/include/ps5platform`,
  copied under `target/include`. Pair them with the host's clang 18 and libc++ 18 headers.
- A whole-build dry run uses a stand-in toolchain file: host clang with
  `CMAKE_*_COMPILER_TARGET=x86_64-sie-ps5`, `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`,
  the sysroot from those headers, and no linker. Then run `ninja <target>` and a symbol
  check over every library on the link line.
- These checks found real link errors before any console run: SHA-256 compiled into both the
  runtime and the app, the runtime library repeated on the link line, and spdlog using
  `fwrite_unlocked`.
- They prove the code compiles for the PS5 and that its parts fit together. They do not
  prove that it links with the real toolchain or that it runs. Say so plainly when
  reporting.

## On the console: logs and crashes

- The log goes in the app folder (flushed every 2 s), with a crash hook and a diagnostics
  monitor (`ps5_diag.h`: counters, thread samples, the guest arena range, the time of the
  crash).
- Crash and stack lines print addresses relative to an anchor (`Anchor()` = `&NowMs`).
  Run `ps5_symbolize.py` against the unstripped `ps5-link/build/llvm-pie.elf` to turn them
  into functions. Compile the recompiled code with `-gline-tables-only` so guest functions
  resolve.

## Working in Claude Code cloud sessions

- The auto-mode classifier refuses to run the toolchain's build scripts (plume-triangle's
  `tools/build.sh` and what it clones) as "code from outside". Don't try to work around it.
  Commit a narrow allow rule for one in-repo entry script instead:

  ```json
  { "permissions": { "allow": ["Bash(ps5/tools/build_ps5.sh)", "Bash(ps5/tools/build_ps5.sh *)"] } }
  ```

  Have that script pin plume-triangle to a commit. The rule is read only by a session that
  has this repository alone, started from a branch that contains the file.
- The toolchain build downloads from github.com and zlib.net, and the environment's
  network policy must allow both.
- Run long builds in the background and write logs to files. The full DS2 build is 852 steps.

## Rules

- Never commit or package game data (XEX, ISOs, assets). Check the player's own files against
  SHA-256 pins and stage them on the console.
- The PS5 layer derives from GPL-3.0 code (the GTA IV port, `libps5platform`), so PS5 files are
  GPL-3.0. ReXGlue keeps its own licence. Credit elripalda, mihawk-99, Nazky and
  ps5-payload-dev.
- State test status honestly in the README: "compiles", "links", "boots" and "playable" are
  different claims.
