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

## Steps

Every step is tested on a console: none of this can run where it is written.
Each adds what the log needs to say what went wrong, and the emulated renderer
stays the default until the native one is better everywhere.

0. **Record** (`ps5/src/ds2_ps5_d3d_trace.cpp`, `ps5_d3d_trace = 1`): the game's
   Direct3D calls, frame by frame, and the shaders it creates. From these: the
   argument lists and structure layouts as they really are, and how a frame is
   built (passes, render targets, resolves, tiling, the depth MSAA trick).
1. **A native frame beside the emulated one**: the same calls drawn natively
   into an image of their own, shown instead of the emulated picture by a
   setting, with Xenia's shader translator and texture code reused for
   shaders and textures. The menus first, then a quiet corridor.
2. **The whole game**: every pass the records show, the depth MSAA trick and
   tiling as what they mean rather than as EDRAM, post-processing, video.
3. **Speed and resolution**: native 4K with nothing emulated in the way.
