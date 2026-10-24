# PS Vita, Phase 2: drawing on the Vita's GPU

Phase 1 (`docs/VITA.md`) ran the game and Aurora's renderer against a null WebGPU and
measured the CPU: about 15.6 FPS in the hangar's heaviest seconds. Phase 2 puts the
picture on the screen.

## Why not WebGPU on the Vita

Aurora draws GX through WebGPU, and its GX shaders fetch their own vertices: every
indexed GX array sits in a storage buffer (`vbuf`, `abuf`) and the vertex shader decodes
the GameCube vertex formats by index (`lib/gx/shader.cpp`). The Vita's GPU (SGX543 behind
GXM) has no storage buffers; it takes ordinary vertex streams. So a WebGPU implementation
on GXM could not run Aurora's shaders as they are, whatever else it did. Phase 2 therefore
adds a Vita path beside the WebGPU one, cut in where Aurora turns GX state into draws:

- **Vertices:** GX vertices decoded on the CPU into interleaved streams the GPU reads
  directly. Resident display lists (all retail world and model geometry) are decoded once,
  when they are retained; immediate primitives each frame.
- **Shaders:** Cg generated from the same configuration Aurora keys its WGSL on (TEV
  stages, texgens, lighting channels, fog, alpha test), compiled on the Vita and cached on
  the memory card. Compiling takes 0.1 to 6 s a shader there, so the cache matters; a
  warm-up pass can fill it once.
- **Textures:** GX formats converted to ones the GPU samples: CMPR to DXT1 (a lossless
  re-pack), the rest to RGBA8, RGB565, L8 or LA8.
- **State:** blend, depth, cull, scissor and viewport from the same GX registers.

## vitaGL rather than raw GXM, to start

`vitaGL` (installed with VitaSDK) is a GL layer over GXM that already handles display
buffers, GPU memory, Cg compilation through libshacccg (`vitashark`), a shader cache
(`vglSetShaderCachePath`, `glProgramBinary`), DXT textures, framebuffers and swaps. On
raw GXM all of that has to be written first. vitaGL costs some CPU per draw that raw GXM
would not; the Vita path keeps its own draw list, so it can move to raw GXM later where the
profile says so. Separate raw-GXM prototypes on the same Vita measured about 5 µs a draw.

SDL3 on the Vita draws the settings overlay through its own GXM renderer, which would
clash with vitaGL's. The overlay moves to Dear ImGui's OpenGL 2 backend on vitaGL, and
SDL keeps only the window, events and pads.

## Milestones

Each ends with a run on the Vita.

1. **Bring-up.** vitaGL initialised at 960x544; each frame cleared and swapped by the
   render worker; the overlay drawn with ImGui's OpenGL 2 backend; SDL's renderer off.
   Done when the overlay shows over a coloured screen and the game runs as before.
2. **First geometry.** CPU vertex decode for the formats the world uses; one fixed Cg
   program (position, colour, texture 0); the world's resident display lists drawn with
   GX's matrices. Done when the hangar's shapes are on screen.
3. **Textures.** GX texture formats converted and uploaded once, with their mips; GX
   texture objects mapped to GL textures. Done when rooms look textured.
4. **Materials.** Cg generated per TEV configuration with the shader cache; lighting
   channels, fog, alpha test, blending, depth and culling. Done when the hangar looks like
   the game.
5. **Copies and effects.** EFB copies and render targets (visors, heat haze, the map),
   the depth reads the game makes.
6. **Speed.** Shader cache warm-up, render resolution scaling, batching; raw GXM where
   vitaGL's per-draw cost shows.

Phase 1's profile stays in the log throughout, so each step shows its cost.

## Status

**Milestone 1 (built, not yet run on the Vita).** `extern/aurora/lib/vita_gl.cpp` starts
vitaGL at 960x544 in place of SDL's renderer (`aurora.cpp`), with its fixed-function
shader cache in `ux0:data/MetroidPrime/vitagl_shader_cache` and 32 MiB of RAM left to the
system. The render worker clears every frame to dark blue, draws the overlay with ImGui's
OpenGL 2 backend (its `GL/gl.h` is vitaGL's, `platforms/vita/vitagl_shim`) and swaps; the
font atlas is built on the game thread and uploaded on the render worker. ImGui images
(`aurora_imgui_add_texture`) are made there too. The game's GX draws still go to the null
WebGPU, so the screen shows only the blue and the overlay.

First run on the Vita: a crash at startup. `vglInitExtended` had started GXM (its garbage
collector and splash screen threads were running) but returned false, which the code took
for a failure: it created SDL's renderer, whose own GXM init failed ("already initialized")
and crashed in its cleanup. This vitaGL's init returns true only when the requested size was
beyond the display's and it clamped it; false is the normal case. The return is now only
logged, and SDL's renderer is never created once vitaGL has run.

Second run: vitaGL up at 960x544, 4 minutes without errors, the hangar's heavy seconds at
16.0 FPS (15.6 before); begin-wait rose from about 4 to 11 ms (the swap's wait for vblank
holding a frame slot, most likely).

**Milestone 2 (built, not yet run on the Vita).** `lib/gx/vita_draw.cpp`, on the FIFO
thread: each call of a retained display list (`call_resident_dl`) decodes the list once
(positions and colour 0 from the arrays or the list, any primitive type made into
triangles, a face normal per triangle), caches it by the list and the arrays and vertex
formats it was decoded with, and records a draw with GX's current position matrix and its
projection (row 2 as `2 * row 2 + row 3` for GL's depth range). The frame's draws go to the
render worker at the frame end (`vita_gl::end_frame_draws`), which draws them with one Cg
program (the face normal shaded towards the camera, times the vertex colour), depth tested,
before the overlay. Not drawn yet: lists with per-vertex matrices, positions made at run
time (CPU skinning), immediate primitives, lines and points. The WebGPU path still runs into
the null device beside it.

First run of milestone 2: nothing on screen, no line from the program (never used), the
render worker as idle as before, while the game made about 1,680 retained list calls a
frame; so no draw reached the render worker. The log could not say why, so now it does:
`aurora::gx::vita` reports every 300 frames what became of the calls (recorded, positions
not retained, decoded, not drawable and why), `aurora::vita_gl` every 300 presents how many
frames had game draws and whether the program is ready, and an orange triangle in the
screen's top-left corner is drawn every frame with the same program and identity matrices.
The test that skipped run-time positions is now whether the position array is retained
(the retail models' arrays are, whatever their byte order), not its byte order.

That build crashed at its first frame inside `glLinkProgram` (a null pointer in SceGxm), when
the test triangle first needed the Cg program; vitaGL had reported both shaders compiled. So
milestone 2 now draws with vitaGL's fixed-function pipeline, which the overlay already uses:
GX's matrices through `glLoadMatrixf` (as column-major 4x4s), vertex and colour pointers into
each mesh's buffer, and the face shading baked into the vertex colours when a list is decoded
(up-facing brightest, as the game's world is Z-up). Generated Cg returns with milestone 4.

The bigger binary first failed to package: `vita-elf-create` appends the module's import and
export tables (4,660 bytes) to the end of the code segment, and only 1,656 bytes were left
before the data segment. `-Wl,--defsym,__sce_headroom=0x10000` makes VitaSDK's linker
script keep room there.
