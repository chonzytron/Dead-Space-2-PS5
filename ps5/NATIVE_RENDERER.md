# A native renderer for Dead Space 2 on the PS5

## Why

The PS5 build draws the game through ReXGlue's emulation of the Xbox 360's
GPU (Xenia's command processor, the `xenos` GPU plugin). Profiles on a console
(ps5_profile, native 4K, a heavy scene at about 20 FPS) put the GPU's 44 ms a
frame at:

| Work | ms a frame |
|---|---|
| EDRAM transfers between host render targets | 25.8 |
| The game's own draws | 9.5 |
| Texture loads (untiling) | 4.9 |
| Resolves | 3.5 |

Most of it is the emulation, not the game. 70% of the transfers are the main
depth buffer being reinterpreted between 1x and 4x MSAA over the same EDRAM,
about 25 times each way a frame: free on the Xbox 360, a full 4K copy here.
The fragment shader interlock path (no transfers) draws three times slower and
comes out even. Skipping the transfers draws next to nothing: the game needs
them.

The GTA IV PS5 port holds 60 at 4K with a renderer written for GTA IV
(`gta4-native` in its ReXGlue tree): it takes over the game's Direct3D calls and
draws them with Vulkan directly, with native render targets and resolves and
no EDRAM. Its code is GTA IV's (its hooks are GTA IV functions at GTA IV
addresses, its shaders GTA IV's), but the idea is not, and Dante's Inferno, on
Dead Space 2's engine, was given a renderer the same way (docs/TODO.md).

## The way in

Dead Space 2 links the Xbox 360 SDK's Direct3D, and its symbols name the
functions (`tools/modapi/symbols_tu2.tsv`: `D3DDevice_SetRenderTarget`,
`D3DDevice_DrawIndexedVertices`, `D3DDevice_Resolve`, `D3DDevice_BeginTiling`,
...). Each can be taken over at link time the way `ps5/src/ds2_ps5_framerate.cpp`
takes over two of the game's functions. Render states are inline code that
writes the device structure in guest memory, so a draw reads the state from
there.

## Where the transfers come from

The game's renderer is EA's `llrender` (`src_tu2/EARS/ears_rt_llrender/src/xenon/`),
the only code that calls Direct3D, with its real names from the PDB. The main
picture is drawn at 1x, without MSAA. `Displ_StartLowRes_XenonSpecific` and
`Displ_EndLowRes_XenonSpecific` switch the colour target (render target 0) and the
depth target (4) to 4x MSAA over the same EDRAM and back: half the width and
height, each 4x pixel's samples being a 2x2 block of the 1x pixels. On the Xbox
360 that is a quarter of the pixel work for free; here each switch is a copy of
the whole target, both ways. Their callers:

- `EARS::Framework::DeferredLitHiStencilCullingClass` (`StartCullingType` and the
  rest): for each kind of light in the deferred lighting, stencil-only full-screen
  passes (`DS_vSetStencilCull`, colour writes off) mark the pixels the lights can
  reach. 21 to 25 sections a frame in a lit room: this is the depth buffer's 1x/4x
  switching the console's profile showed.
- `SceneManagerInternal::RenderDeferredBuffer_CreateShadowAttenuation`.
- `EARS::Alchemy::LowResolution` (low resolution effects), with
  `Displ_ResolveInHalf_XenonSpecific`.

`ps5/src/ds2_ps5_lowres.cpp` counts the sections per caller in the log
(`ps5 lowres:`), and `ps5_lowres_culling = "full"` keeps the culling at 1x: the same
pixels are marked, with no copies. On the Linux test build (below), in the
save-station room of a save (720p, Radeon 760M), 30 s each:

| | `game` (as the Xbox 360) | `full` |
|---|---|---|
| frames a second | 34.4 (35.2 on a second run) | 58.7 |
| EDRAM transfers | 17.6 ms, 58 a frame | 4.1 ms, 8 a frame |
| the game's draws | 6.7 ms | 6.3 ms |

The pictures differ from each other no more than two runs of `game` do (on the
animated character and the fog). Walking out of the room into the corridor (a
script of stick pushes), the same: no light missing, cut off or spilling, 54.3
frames a second with `game` (EDRAM transfers 8.8 ms, 25 a frame) and 60 (the
cap) with `full` (4.0 ms, 8).

On a PS5 at native 4K (`resolution_scale = 3`, profiling on), the room with the
hacking panel after the first save, the worst place in the stretch played, 30 s
windows:

| | `game` (ds2_014) | `full` (ds2_015) |
|---|---|---|
| frames a second | 21.5-21.9 | 33.1 (40-46 in the windows around it) |
| culling sections a frame | 42.5 | 48.3, all at 1x |
| EDRAM transfers | 24.5 ms, 103 a frame | 7.4 ms, 22 a frame |
| the game's draws | 9.2 ms (1665) | 9.3 ms (2036) |
| texture loads + resolves | 7.3 ms | 8.2 ms |

In `game`, 85 of the 103 transfers were the main depth buffer between 1x and 4x
at tile 0 (43.6 and 41.8 a frame): the culling. With `full` the lighting looked
right in the room, the corridors and the fights after it, and the quieter parts
held 60 for minutes. What is left in the heavy room is about 25 ms of GPU work:
draws 9.3, transfers 7.4 (the other 4x sections: shadow attenuation and low
resolution effects, about 1-2 each a frame, each a copy both ways), texture
loads 4.8, resolves 3.4.

The transfers left are Direct3D's own: with the clears left out
(`ps5_debug_skip_clears`, a wrong picture) the Linux test build has none.
`D3D::ClearRect` clears the tile-aligned middle of a rectangle as 4x MSAA at half
the size over the same memory, and only the edges as the surface is.
`ps5_lowres_clears = "full"` sends the whole rectangle down Direct3D's own path
for small rectangles (one `D3D::ClearRects` at the surface's MSAA). On Linux, in
the save room, culling at 1x: transfers 4.1 ms (8 a frame) -> 1.9 ms (5, none of
them 1x/4x any more), draws 6.3 -> 6.5 ms, the pictures as close as two runs of
the same setting. The 5 left are the depth buffer and a colour target over the
same memory at 1x.

The same room on the PS5 at native 4K, both on (ds2_016), against the culling
alone (ds2_015), 30 s windows:

| | culling at 1x | and the clears |
|---|---|---|
| frames a second | 33.1 | 37.9 (2077 draws a frame); 41.1 (1671) |
| EDRAM transfers | 7.4 ms, 22 a frame | 3.9 ms, 14 a frame; 3.7 ms, 12 |
| the game's draws | 9.3 ms | 9.1 ms; 8.4 ms |

No picture showed leftovers or flashes. On by default since. In the 37.9 window
the GPU was busy 82% of the time; the game's render thread used 95% of a core,
but almost all of it waiting for the GPU (`D3D::CDevice::BlockOnFence`,
`D3D::CBlocker::Check`), and the command processor 60%. What is left is the
GPU's: draws 9.1, texture loads 4.8, transfers 3.9, resolves 3.2 ms.

The next console run (ds2_017) used run 3's program with the ds2.toml written
for the next one: the two new settings were not in that program, so it ignored
them, and the clears fix was off (a default only in the newer program, so the
ds2.toml no longer set it). Transfers were back to 7.1 ms, 20 a frame, and the
room at 34.6-38.4 FPS: the same as the culling alone. Not a result for the new
settings. Since then the log's first line says which commit a program was built
from (`Dead Space 2 for PS5 0.1 (build 520d08d)`), and a `ps5 settings:` line what
it has for `resolution_scale`, `ps5_lowres_*`, `ps5_resolve_forward` and
`ps5_draw_extent_from_viewport` ("(not in this program)" for one it doesn't know).

## What a native renderer would save

Texture loads are timed in two parts since (`ps5 gpu:` "of them resolved pictures
read back"). On the Linux test build at `resolution_scale = 2`, all of a frame's
texture loads were resolved pictures (7.0 of 7.1 ms, and 7.4 of 7.4): the game's
own textures load once; what loads every frame is the emulated EDRAM's output,
written to guest memory by a resolve, then untiled back into a texture. So in the
heavy room on the PS5 at 4K (ds2_016, 21.6 ms of GPU work in the 37.9 window):

| | ms | in a native renderer |
|---|---|---|
| EDRAM transfers | 3.9 | none: no EDRAM |
| resolves | 3.2 | an image copy, or none where the target is read as it is |
| resolved pictures read back | 4.8 (all of the texture loads) | none |
| the game's draws | 9.1 | the same: the same shaders |

About 12 of the 21 ms is the EDRAM's emulation. What a native renderer has to
do differently is in render targets, resolves and the textures made from them;
the draws, shaders, vertex and constant data and the game's own textures can be
the emulated renderer's as they are.

## Step 1: native resolves (resolve forwarding)

Chosen over taking over every Direct3D call: the savings are all in render
targets, resolves and their read-back, so those are made native inside the
emulated renderer, and the draws stay as they are.

The profiler's resolve catalogue (`ps5 gpu resolve` / `ps5 gpu resolved load`
lines) shows what the game resolves: in the rooms seen so far, whole 1280x720
(and 640x360) pictures at 1x, colour from a k_8_8_8_8 target to a k_8_8_8_8
texture with red and blue swapped, depth from D24S8 to a k_24_8 texture, each
read back once by a texture of the same address, size and format.

`ps5_resolve_forward` (SDK: `VulkanTextureCache::ForwardResolve`,
`VulkanRenderTargetCache::FindWholeForwardSource`):

- **1**: at a whole colour resolve, the render target is also blitted into an
  image of its own (an RGBA to BGRA blit does the swap); a texture that is
  exactly that picture is filled from it by an image copy instead of being
  untiled from memory. Memory is still written. A later write to the range
  (another resolve, the processor) drops the image.
- **2**: the same, and once a picture has been read only through forwarding
  for 30 loads, its resolve is no longer written to memory at all (no EDRAM
  dump, no resolve pass). If anything then reads that memory another way, the
  log says so and it is written to memory again from then on.

`ps5_resolve_forward_check = true` checks it: every 5 s one forwarded picture
is loaded the usual way as well and both are read back and compared. On the
Linux test build every check, of every picture including the frame sent to the
screen, found 0 of 3,686,400 pixels different.

Linux test build, `resolution_scale = 2`, the in-game window of the save-room
script (Radeon 760M):

| | off | 1 | 2 |
|---|---|---|---|
| frames a second | 23.0 | 23.7 | 26.8 |
| resolves | 6.8 ms | 8.9 ms | 3.9 ms |
| resolved pictures read back | 7.4 ms | 4.5 ms | 4.5 ms |

Depth (D24S8 to a k_24_8 texture, a float texture on the host) is forwarded by
a compute shader (`src/graphics/shaders/ps5/resolve_forward_depth_unorm24.cs.glsl`
in the SDK) that does what the two passes it replaces do: the depth quantized to
24 bits as the EDRAM dump does (round to nearest even), then back to a float as
the k_24_8 texture load does (`(n + (n >> 23)) * 2^-24`). The check found it
byte-identical too. With it, level 2: 28.2 frames a second, resolves 2.8 ms,
read-backs 3.6 ms (14.2 ms of the two at off).

At `resolution_scale = 3` (the PS5's native 4K) the check found every picture
identical as well (3840x2160 and 1920x1080, depth included).

What was left of those was the two copies themselves (into the forwarding
image at the resolve, into the texture at the read-back).

### One pass

Once a texture has read a colour picture through forwarding, the next resolve
of it goes straight into that texture (`ps5 resolve forwarding: ... now goes
straight into its texture`), and the read-back is nothing. The texture is
created as B8G8R8A8 with `MUTABLE_FORMAT` and viewed as its own format, the
same bits, so the blit that swaps red and blue can write it directly; copies
into it, the usual loads included, are of bits as before. If the texture is
gone or another texture reads the picture, the two copies are used again (and
the log says so). The check covers it: the texture's contents are read back
before the usual load overwrites them. Depth now takes one pass too (below).

Linux test build, `resolution_scale = 2`, same script, the same 30 s windows
before and after:

| | two copies | one pass |
|---|---|---|
| resolved pictures read back | 3.3 / 5.2 / 3.5 / 3.6 ms | 1.1 / 1.5 / 0.8 / 1.0 ms |
| frames a second | 54.1 / 40.0 / 44.7 / 33.7 | 55.8 / 45.7 / 50.6 / 37.8 |

The check: every picture, one pass included, 0 pixels different.

**Depth in one pass.** A depth picture (D24S8 to a k_24_8 texture) went
through a compute shader into a buffer of floats, then a copy into the texture.
Now the texture that read it last time is created with storage use as well
(R32_SFLOAT, which every Vulkan device can store to), and the same shader
(`resolve_forward_depth_unorm24_image.cs.glsl`, writing an image instead of a
buffer) writes straight into it: `ps5 resolve forwarding: depth ... now goes
straight into its texture`. If the texture is gone, or the shader couldn't be
made, it is the buffer and the copy as before. Linux test build, the same
script and windows as above:

| | depth in two passes (f787c22) | one pass (4325898) |
|---|---|---|
| resolved pictures read back | 1.1 / 1.5 / 0.8 / 1.0 ms | 0.0 / 0.0 / 0.0 / 0.0 ms |
| frames a second | 55.8 / 45.7 / 50.6 / 37.8 | 56.4 / 48.4 / 52.4 / 38.2 |

The check (`ps5_resolve_forward_check = true`): the depth picture, after it went
one pass, 0 of 3,686,400 pixels different both times; every colour picture 0.
The console's heavy room has three depth pictures a frame (the scene's, and two
960x960 shadow maps), about 0.5 ms of its 1.0 ms of read-backs.

### The profiler's own cost

On the console the command processor thread spent 12% of its time in the
system's clock (`c23_timespec_get`, ds2_018): the PS5's RADV reads the clock
once for every query `vkGetQueryPoolResults` returns, even without waiting, and
the profiler read one per draw (about 4,000 a frame). The submission now copies
its results into a buffer (`vkCmdCopyQueryPoolResults`) that is read directly.
The console runs with `ps5_profile = true` measured a slower program than the
game is without it; the next one will say by how much.

## Render targets the size they are (ps5_draw_extent_from_viewport)

The EDRAM transfers left after the low-res fixes (6.5 ms a frame at
`resolution_scale = 2` on the Linux test build, 3.7-3.9 ms in the heavy room at
4K on the PS5) were the depth buffer at tile 0 and the colour target at tile 720
trading ownership of each other's EDRAM, 37% of their tiles each time. They
don't overlap: a 1280x720 depth buffer is exactly 720 tiles. The emulator decides
how far down a draw reaches from its scissor, its viewport, or by running its
vertex shader on the CPU; the game's full-screen quads have no scissor (8192),
clipping and the viewport transform off (positions already in pixels), and a
vertex setup it can't run, so they are taken to reach 2048 rows and the depth
buffer claims the colour target's EDRAM. Direct3D still sets the viewport
registers to the surface (0 to 720) for them. `ps5_draw_extent_from_viewport`
bounds such draws by it (SDK `draw_extent_estimator.cpp`).

Linux test build, `resolution_scale = 2`, with `ps5_resolve_forward = 2`:
transfers 6.5 ms (5 a frame) -> 0.0 (none), 28.2 -> 33.9 frames a second in the
corridor, 35.8 -> 46.1 in the save room. The pictures (menus, inventory, pause
menu, save station, corridor) unchanged. On by default since the console run
below (ds2_018), as is `ps5_resolve_forward = 2`.

## Both on the PS5 (ds2_018)

Native 4K (`resolution_scale = 3`), build 520d08d, the same stretch played by
hand to the hacking room, 30 s windows. Against ds2_016 (the low-res fixes alone):

| | ds2_016 | ds2_018 |
|---|---|---|
| heaviest window | 37.9 frames a second (2077 draws a frame) | 54.2 (1859 draws) |
| the game's draws | 9.1 ms | 9.0 ms |
| EDRAM transfers | 3.9 ms, 14 a frame | 0.6 ms, 4 a frame |
| resolves | 3.2 ms, 13 a frame | 1.4 ms, 12 a frame |
| resolved pictures read back | 4.8 ms | 1.9 ms (image copies now) |
| GPU work a frame | 21.6 ms | 13.5 ms |
| other windows | 41.1-59.2 | 56.3-59.1 |

The worst 10 s stretch, 47 frames a second, holds a 6 s cutscene the game runs
at 30; the player saw high 40s in the hacking room. The captures show nothing
wrong. Forwarding found the pictures it can't take whole and wrote them to memory
as before: small 640x360 / 352x288 / 320x184 pictures read back at another size
or format (`... it doesn't match ...: always written to memory from now on`).

What limits it now, in the 54.2 window:

- the GPU, busy 73%: draws 9.0 ms, the read-back copies 1.9, resolves 1.4,
  transfers 0.6, uploads 0.4;
- the emulator's command processor thread, 78% of a core: in the driver
  (`c23_timespec_get` 12%, thread-local storage `__emutls_get_address` 8.5%),
  `VulkanSharedMemory::UploadRanges` 7.7%, hashing (xxhash) about 6%,
  descriptor updates;
- the game's render thread, 90% of a core, but mostly waiting for the GPU
  (`D3D::CDevice::BlockOnFence`, `D3D::CBlocker::Check`, sleeping): the game is
  not what is slow.

So the last frames are shared between the GPU (13.5 ms is under 16.7, but with
the command processor feeding it in turn they don't overlap fully) and the
command processor's own time. Next, in that order: one-pass forwarding (the
read-back copies, 1.9 ms), then the command processor's time (part of which
was the profiler's own: see "The profiler's own cost").

## One pass on the PS5 (ds2_019)

Native 4K, build d672f63 (one pass, and the profiler's clock reads gone), the
same stretch played by hand. 10 s windows, ds2_018 against ds2_019:

| | ds2_018 | ds2_019 |
|---|---|---|
| windows under 59 frames a second, outside the hack | 53.6, 54.9, 55.4, 57.2, 57.5, 58.5 | 58.4 (the first seconds of play) |
| the window with the hack in it | 47.0 | 50.1 |
| heaviest 30 s: GPU work a frame | 13.5 ms | 12.4 ms |
| of it, resolved pictures read back | 1.9 ms | 1.0 ms |
| of it, draws / resolves / transfers / uploads | 9.0 / 1.4 / 0.6 / 0.4 | 8.9 / 1.3 / 0.6 / 0.4 |

Every colour picture forwarded went straight into its texture; none went into
another one. What is still read back is depth (two passes) and the small
640x360 / 320x184 pictures read at another size, as before.

**The drop in the hacking room was the 60 frames a second code, not the
renderer.** In both runs the slow window holds `a cutscene has the player: 30`
for 5.5 s, and the picture then (capture 018) is the hacking minigame: the
game holds the player in a scripted sequence while he hacks, which is what
`ps5_cutscenes_30` takes for a cutscene. The on-screen counter, over half a
second, shows the switch as the mid 40s. A sequence is now taken for a hack, and
kept at 60, when a `HackingStationMiniGame` event (`PlayerCame`, `PlayerWent`,
`OnEnterMessage`, `OnExitMessage`, `OnSuccessfulSelection`, `OnGameSuccess`,
`OnTimerRunOut`) comes during it or up to 5 s before it. The log says
`the player is hacking: 60`, and the first 40 events by name, so the next
console run shows which the game calls and when. (The Linux test build can't
get to a hacking station by itself; the change was built and run there.)

**Short drops, in the log.** A 10 s average hides a drop of a few frames. The
`ps5 diag` line now ends with the frames that took over 20 ms and over 40 ms
in those 10 s, and the longest (frames of a cutscene held to 30 on purpose
aren't counted). On the Linux test build: 0 in the corridor, one 481 ms frame at
the loading screen.

**What the profile says, read again.** The game's render thread is 99% busy in
every window, even at 60 frames a second with the GPU 28% busy: it is
`D3D::CDevice::BlockOnFence` and `D3D::CBlocker::Check` waiting, not work. The
command processor thread (56-72% of a core) spends part of that in its own
waiting: when the game has given it nothing it calls `sched_yield` 500 times
(each a system call), then waits 5 ms at a time. Two of the profiler's "~"
places on it, `~os_time_get_nano` (10.7%) and `~InterruptCallback` (3.8%), are
the first address of a function, which can't be where a call returns to: the
profiler's scan up the stack found a function's address left there, so those
samples (and probably the ones in waits) are misattributed. The thread-local
storage lookups (`__emutls_get_address` calling the system's
`pthread_getspecific`, 17%) are real calls. So the heaviest gameplay, at 84%
GPU busy and 59 frames a second, is close to the GPU's limit, and the GPU's work
is still what to cut first.

## Depth in one pass on the PS5 (ds2_020)

Native 4K, build 821d322, the same stretch by hand. Every 10 s window outside
two is at 59.5-60.1 frames a second; the heaviest 30 s: GPU work 12.0 ms a frame
(draws 9.1, transfers 0.6, resolves 1.3, read-backs 0.4 from 1.0, uploads 0.4).
All three depth pictures (the scene's and two 960x960 shadow maps) went one pass,
no "went into another texture".

- **The hack** stayed at 60: the game held the player in a sequence only for
  the last moment of it, and that was taken for a hack (`the player is playing
  it (hacking)`).
- **The elevator** (Nicole's hallucination, 20 s): the 55 frames a second the
  player saw there is the game's own 30 as it runs on the PS5. The vblank the
  game waits for is ours; with `vsync_adaptive` a frame waiting for its second
  vblank is taken for a late one and let go, so "30" makes about 55 frames a
  second. Its timing is still the Xbox 360's (measured against `vsync_adaptive
  = false`, a steady 30: see `ds2_ps5_framerate.cpp`), and at 60 a cutscene's
  events do come early, so cutscenes keep it.
- **The charge in the hacking room**: 1 to 11 frames in each 10 s took 22-30
  ms. The command processor never waited for the GPU over a whole window, so
  those are the game's thread or the command processor's own time, frame by
  frame; the profile's averages can't say which (its "~" places for a thread
  that sleeps are mostly wrong: idle time lands on stale return addresses).
  Each slow frame now gets a line in the log: how long the command processor
  worked, waited for the game's commands and waited for the GPU, and its draws,
  new pipelines, texture loads, bytes uploaded and write watches
  (`ps5 slow frame: ...`). On the Linux test build: a loading frame waits on
  the game; one 41 ms frame was the command processor uploading 14 MB.
- The on-screen counter and the log's `the game's own` count the game's frames
  (its frame lock), not the pictures it hands over.

## The action scenes (ds2_021)

Build b2d12ae, the same stretch and further. Two action scenes dropped (the worst
10 s 52.3 frames a second, the player saw low 40s); every other window 59.5-60.
The slow-frame lines say what it was: in each of the 112 slow frames of the
worst 10 s, the command processor **worked the whole frame** (22-32 ms), never
waiting for the game or the GPU, with about 2,000 draws and **13-17 MB of guest
memory uploaded** a frame. The profile, read without its idle samples, has no
single hot place: hashing (6%), register writes, descriptor updates, copies and
memory protection, a little each.

Those uploads were the SDK's `clear_memory_page_state` (true as it came; Xenia's
own default is false, "for Team Ninja games"): at every frame's end it drops
what the CPU uploaded, so each frame uploads every vertex and index buffer it
draws with again, changed or not. The game's writes are caught without it (its
pages are write-watched, the often-written ones hashed: "volatile pages"). With
false, on the Linux test build (2x, the usual script), a frame's uploads
3.9 MB -> 0.6 MB and the command processor's time with guest memory 6.0 ms ->
3.0 ms; the pictures the same (the differences are Isaac's idle animation and
the camera, where the two runs' timing differs). It is an experiment in
ds2.toml until a console agrees.

The log now splits the command processor's time: each slow frame's line and a
`ps5 cp:` line every 10 s give a frame's draws (of it guest memory, textures,
pipelines, bindings, primitives, render targets, state), resolves, the waits for
the game and the GPU, and the uploads. On the Linux test build the GPU is the
limit (14 ms a frame waited for it); the command processor's own work was 8.3 ms
for 756 draws, guest memory the largest part. The profiler leaves out the
samples of a thread that used less than half of the 10 ms since its last one
(waiting: its stack says little), and says how many.

## clear_memory_page_state = false on the PS5 (ds2_022)

Build 546158e with it false, 15 minutes played past the earlier stretch. The
action scenes' uploads 13-17 MB -> 1.6-3 MB a frame; the command processor's
draws ~4 us each (2,000 draws in 8-10 ms); the worst 10 s 54.7 frames a second
(52.3 before), most windows 59-60; nothing wrong reported. False is the default
since.

What is left in the slow frames (22-23 ms): draws 8-10 ms, and "the rest" 13-14
ms, which on the Linux test build is 2.5 ms. The busy samples still find the
command processor outside the program a quarter of the time (as
`~__emutls_get_address` and `~os_time_get_nano`, the latter a function's first
address: the stack scan's "caller" there is not one). Likely the waits the
game's own commands ask for (WAIT_REG_MEM: for a vblank, or spinning until the
game writes a value), which were counted as the command processor's work. They
are now timed apart (`waited ... for the game (between its commands, in them for
a vblank and for a value, the longest on <register or memory>)`, and in `ps5
cp:`), so the next run says whether the slow frames are the command processor's
work or the game's thread.

## The waits ruled out (ds2_023)

Build dcda385, 21 minutes played. Every 10 s window but one at 56.8 or better
(the worst 53.7, an action scene). The slow frames' waits inside the game's
commands (WAIT_REG_MEM) were 0: the command processor worked the whole 20-25 ms,
draws 7-9 ms of it (2,000-3,000 draws) and 13-16 ms "the rest", outside any draw
or resolve. The GPU was 75-80% busy over those windows.

To say what the rest is, each packet of the game's commands is timed by kind,
its own time only (an indirect buffer's packets count as theirs): each slow
frame gets a second line with the kinds that took the most, and the GPU's work
finished since the last frame; every 600 frames a summary line (`ps5 cp
packets:`). If the rest is the swap (handing the frame to the driver, which
waits there when the GPU is behind, uncounted as waiting for the GPU) and the
GPU's time near 16.7 ms, the heaviest frames are the GPU's; if it is register
writes or interrupts, the command processor's.

## Two kinds of slow frame (ds2_024)

Build d85a38a, 15 minutes further into the game: a few stretches at 49-55
frames a second. The command packets say what each slow frame was:

- **The GPU's**: in the stretch at 760-790 s (GPU 81-84% busy), the swap packet
  takes 11-14 ms of the slow frames (handing the frame on waits there for the
  GPU) and the GPU's work finished in them is 16-22 ms. The 30 s average there:
  draws 10.1 ms, resolves 2.8 (16 a frame), transfers 0.8, read-backs 0.8 -
  14.9 ms a frame. One lighting shader (VS 1269CF227D8B6A3D, PS
  151AAD501828735B: 41 draws a frame of 72 us at 4K) is 3 ms of it.
- **The command processor's**: at 510-520 s (GPU 43-56% busy) frames of
  4,500-5,200 draws, the draws alone 15-16 ms (bindings 4.1, guest memory 4.1,
  primitives 2.0, render targets 1.2).

`ps5_reuse_texture_bindings` (an experiment, off by default) takes on the
upstream TODO in UpdateBindings: every draw wrote a new texture descriptor set
for each stage, even with the textures and samplers of the draw before. A draw
whose views and samplers are those of the set last written for the stage, this
frame (a frame's transient sets stay valid until it is done), binds that set
again (and not even that if it is still bound). Linux test build: bindings
1.6 -> 1.3 ms a frame for 756 draws, the pictures the same.

Next for the GPU: the transfers (the shadow map's depth and the scene's colour
over the same EDRAM, 43% of the tiles each way, while the pass that follows
clears them) and the resolves.

## Draw-heavy frames (ds2_025)

Build 1836f28 with `ps5_reuse_texture_bindings = true`, 12 minutes: one 10 s
window under 58 frames a second (54.6), frames of about 4,000 draws, the GPU
9-11 ms: the command processor's draws 15 ms (guest memory 5.4, bindings 3.2,
primitives 2.4), register packets 2 ms. Reuse now the default (nothing wrong
seen).

Guest memory: about 1,670 volatile page hashes a frame on average (6.7 MB). Not
cut: a page hashed for one draw can't be taken as unchanged for the next, since
the game may rewrite it once the first draw has gone through (its fence).
Cut instead, both on the command processor's thread for every call:

- every register write looked its register up in the register table
  (`GetRegisterInfo`, a switch of all of them) only for a debug line that is
  off: now only when debug logging is on. Linux test build: register packets
  1.6-1.8 -> 1.0 ms a frame for 4,060.
- `SharedMemory::RequestRanges` made a new vector for each request (several a
  draw): one is kept. Guest memory 3.2 -> 2.9-3.1 ms a frame.

`ps5_profile = true` costs the command processor about 6% of its draws' time
(Linux test build: 8.1 -> 8.6 ms), the GPU nothing measurable; the slow frame
and `ps5 cp` lines don't need it.

## A long run without the profiler (ds2_029)

Build c47e79b, 19 minutes, `ps5_profile = false`: every 10 s window of play at
58.1 frames a second or better (the three under it a cutscene at the game's own
30). Slow frames (over 20 ms): 115, against 255 in ds2_025 (12 min) and 1,929 in
ds2_024 (15 min); the longest 33 ms. What is left are frames of about 3,000
draws: the command processor's draws 11 ms, and in some the GPU's work 20 ms or
more (the swap packet then waits for it).

The game's clock keeps true time in slow frames: on the Linux test build at 39
frames a second (26 ms frames), 5.01 s of game time in every 5.00 s, the frame
time 1 or 2 vblanks in turn (TimeManager carries the remainder). What slows in a
dip is what the game counts in frames, not in time (as with the cutscene events
at 60): fewer dips is the remedy, not the clock.

## The swap, and a submission thread (after ds2_029)

In ds2_029's slow frames (3,000-3,500 draws, 20-23 ms) the command processor
was busy the whole time: draws 10-12 ms, and the swap packet 6-10 ms. The swap's
time was not waiting for the GPU: it is the end of the frame's submission, where
the deferred commands (each draw recorded in a buffer of our own) are replayed
into a Vulkan command buffer, one driver call each, and submitted. On the Linux
test build 0.7 ms a frame for 630 draws (1.1 us a draw); on the PS5 about 2 us.

`ps5_submit_thread = true` (off until a console run confirms it) does the replay
and the submit on a thread of its own. The command processor decides everything
about a submission (command buffer, fence, semaphores, timestamps) and hands it
over with its commands; the thread replays and submits them in order. A fence is
only waited for or looked at once the thread has submitted it. A swap's own
submission is still made by the command processor, after the thread has made the
ones before it (the presenter shows the image right after). A frame with many
draws is cut into submissions of `ps5_submit_chunk_draws` (300) draws, so the
replay goes on while the frame is being made, and the GPU starts on it sooner;
not while an occlusion query is open.

Linux test build, 630-750 draws a frame: the swap 0.8 -> 0.2 ms; the draws
0.2-0.4 ms slower (the two threads share the processor); one cut a frame at 300
(cuts at 150 cost more than they save). The picture the same.

Also: the volatile pages' 30 s log line read the clock at every request; on the
Linux build that took longer than hashing the pages (0.57 against 0.40 ms a
frame). It is now read every 1,024 requests. And `ps5_volatile_look_once` (off,
being tested) looks at a page once for each batch of commands the game hands
over instead of each draw: what a draw needs the game wrote before it moved the
ring buffer's write pointer, or before a value the GPU waits for (a new batch
starts at each, and after the game's interrupt code). On the Linux build it
skipped 4 looks in 10 but saved little there; the slow-frame lines now say what
the looks cost on the console.

## The console run with the submission thread (ds2_030)

Build 0aa5ba0 with `ps5_submit_thread = true`, 16 minutes: the swap packet in
slow frames 0.2-0.6 ms (6-10 ms before); the replay, now on its own thread,
1.7-2.1 ms a frame of 2,500-3,000 draws; nothing wrong seen. The thread is now
on by default. Five 10 s windows of play under 58 (55.4-57.9), slow frames
(over 20 ms) 216, most 20-22 ms: the command processor's draws 10-14.5 ms
(guest memory 3.2-5, of it the volatile page looks 1.6-2.0; textures 1.5-2.8;
bindings 2.1-3.2), and in many the command processor waiting 3-20 ms for the
game's commands. Those 27-34 ms frames are the game's own threads.

One of them never rests: the thread in D3D's `CDevice::BlockOnFence` /
`CBlocker::Check`, a flat-out loop that uses a whole core (99% in every 30 s
window of ds2_025's profile, at 60 frames a second too; 98% on the Linux test
build). On the PS5 that core is shared with another thread.
`ps5_gentle_gpu_waits` (off until a console run confirms it,
`ps5/src/ds2_ps5_gpu_wait.cpp`) puts the processor's spin-wait pause (16 of
them, about 0.3 us) before each check and a yield before every 16th.

`ps5_perf_log = false` leaves the detailed performance lines out of the log
(the 10 s and slow-frame reports, pipelines made, the memory-protection
notices, resolve forwarding): an 80 s Linux run, 203 lines, nearly all from
the start-up. `ps5_capture = 0` (the default) takes no screenshots.

## The gentler waits on the console (ds2_031)

Build ac20ea3 with `ps5_gentle_gpu_waits = true`, 49 minutes of which about
20 a pause (the game's menu: 1,032 draws a frame, no slow frame). In play, 288
of 292 10 s windows at 59.5 frames a second or more; the four under it a
start-up or a single hitch. In the windows averaging 2,000 draws a frame or
more (21 of them, against 28 in ds2_030): 11 slow frames against 124, a mean of
60.0 frames a second against 59.4, the command processor's draws 7.5 ms a
frame against 8.7 (the core the spinning thread shared is free). Now on by
default.

Left: single hitches of 200-250 ms where the game moves on to a new area (a
door, a room just entered; about 270 draws a frame, the command processor
waiting for the game the whole time): the game's own threads, which the slow
frame lines can't see into.

## A frame, as recorded

`ps5/tools/d3d_frame_summary.py logs/d3d_frame_<n>.txt` turns a recorded frame
into its passes. The save-station room on the Linux test build (frame 5917, 4208
calls, 602 draws, 51 passes):

| Calls | Colour / depth | What |
|---|---|---|
| 96-1030 | 1280x720 / 1280x720 D24S8 | the scene: 245 draws; colour and depth resolved to textures |
| 1050-2129 | alternately 640x360 4x over the same memory, and 1280x720 | the deferred lighting: 23 times one stencil draw at 4x (the culling) and one light at 1x |
| 2149-2156 | 80 wide, no depth | a small pass (luminance?), resolved |
| 2194-4184 | the second 1280x720 colour / the same depth | 306 draws (forward, effects, post), resolved for the screen |

The game keeps a few surface structures and rewrites them in place: the 4x view
is the 1x surface's structure with a new size word (`0A020280` for `14000500`).
With `ps5_lowres_culling = "full"` the lighting loop is all at 1x. The 8
transfers a frame left are other 1x/4x switches (once or twice a frame each) and a
small float target aliasing the depth.

## Testing on a PC

`ps5/tools/linux_test.sh` (ps5/BUILD.md, "On Linux") builds the PS5 program for
Linux, with the same app, SDK, log and profiler. `ps5_input_script` plays the menus
by itself (`"6:start 10:start ... 38:start 42:a 50:a"` gets from a cold start into
the last save in about 55 s), and `ps5_capture` saves the picture every few
seconds, so two settings can be run the same way and their pictures and profiles
compared.

## Steps

Every step is tested on the Linux test build first and then on a console. Each
adds what the log needs to say what went wrong, and the emulated renderer
stays the default until the native one is better everywhere.

0. **Record** (`ps5/src/ds2_ps5_d3d_trace.cpp`, `ps5_d3d_trace = 1`): the game's
   Direct3D calls, frame by frame, and the shaders it creates. From these: the
   argument lists and structure layouts as they really are, and how a frame is
   built (passes, render targets, resolves, tiling, the depth MSAA trick).
   Done on the Linux test build for one room ("A frame, as recorded"); more
   scenes, and the console's, to come.
0b. **The depth MSAA trick as what it means**, inside the emulated renderer
   first, since it is most of the cost and needs no new renderer: the lighting's
   culling at 1x (`ps5_lowres_culling = "full"`, measured above, the default
   since a console agreed), and Direct3D's clears without 4x
   (`ps5_lowres_clears = "full"`, the default too). The worst room measured,
   at native 4K: 21.5 -> 38-41 frames a second.
1. **A native frame beside the emulated one**: the same calls drawn natively
   into an image of their own, shown instead of the emulated picture by a
   setting, with Xenia's shader translator and texture code reused for
   shaders and textures. The menus first, then a quiet corridor.
2. **The whole game**: every pass the records show, the depth MSAA trick and
   tiling as what they mean rather than as EDRAM, post-processing, video.
3. **Speed and resolution**: native 4K with nothing emulated in the way.
