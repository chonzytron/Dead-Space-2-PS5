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
cap) with `full` (4.0 ms, 8). Still to see: a console at 4K, and the rest of the
game.

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
   culling at 1x (`ps5_lowres_culling = "full"`, measured above). Next: the
   remaining 4x sections, then the default once a console agrees.
1. **A native frame beside the emulated one**: the same calls drawn natively
   into an image of their own, shown instead of the emulated picture by a
   setting, with Xenia's shader translator and texture code reused for
   shaders and textures. The menus first, then a quiet corridor.
2. **The whole game**: every pass the records show, the depth MSAA trick and
   tiling as what they mean rather than as EDRAM, post-processing, video.
3. **Speed and resolution**: native 4K with nothing emulated in the way.
