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

What is left of those is the two copies themselves (into the forwarding image
at the resolve, into the texture at the read-back). Next: one pass, straight
into the texture.

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
menu, save station, corridor) unchanged. Off by default until a console has run
it.

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
