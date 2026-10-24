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

**Milestone 2 (done; on the Vita).** `lib/gx/vita_draw.cpp`, on the FIFO
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

Third run (c3783cf): the hangar on screen. Walls, tunnels, doors, the ship's surroundings and
the arm cannon and visor frame, grey-shaded, in perspective and depth tested, with the test
triangle in the corner. Up to about 1,750 retained list calls a frame, nearly all drawn (the
rest have run-time positions); 1,749 lists cached. But the hangar's heavy seconds fell to
8.9 FPS (15.6 in Phase 1), the game thread waiting about 52 ms a frame for the FIFO thread,
which spent about 55 ms a frame in `vita::draw_resident`: in those seconds about 84 lists a
frame were decoded again. A list's cache entry compared all eight GX vertex formats, and the
HUD and particles change the ones a world list never uses. The render worker drew about
1,250 draws in about 14 ms a frame.

So a list's decode now remembers only what it read (the vertex descriptor, the formats its
primitives name, and the position and colour arrays when indexed), keeps up to four such
variants, and the log counts any decode of a list already cached by what changed. A list the
Vita path has drawn no longer also goes through the WebGPU path into the null device (about
45 ms a frame of the FIFO thread's time in the same seconds); lists it cannot draw yet still
do.

Fourth run (8583a7c): no list decoded again all run, and the seconds with 1,500 or more list
calls a frame at 20.5 FPS (15.6 in Phase 1, 8.8 the run before). The game thread no longer
waits (begin-wait 0.9 ms); the FIFO thread spends about 22 ms a frame on the lists (15 ms of
it in `draw_resident`, about 9 µs a call) and the render worker about 11 ms. The game's own
tick and draw (about 19 and 28 ms) are now the frame.

Controls: SDL calls the Vita's pad a standard one, whose default layout put Z on R and L and R
on analog triggers the Vita does not have, so L never locked on. On the Vita the default is now
the NSO GameCube pad's layout: L and R on the shoulders, Z on Select. Select + Start still opens
the overlay, and no longer opens the map as well.

Fifth run (f2a9715): 15 minutes, to the Frigate's reactor core, without an error; the heavy
seconds at 18.4 FPS (in other rooms than the run before). The Vita's own pad layout followed it:
cross fires (A), circle jumps (B), square morphs (X), triangle fires missiles (Y).

**Milestone 3 (done; on the Vita).** Each static GX texture gets a copy for the
Vita's GPU where Aurora creates it (`new_static_texture_2d`): its first level, as BC1 (DXT1)
for an opaque CMPR texture, a lossless re-pack, and RGBA8 for every other format (decoded by
Aurora's converters, palettes applied). The WebGPU texture beside it stays as the handle
Aurora's caches hold, on the null device, with nothing uploaded. The copy goes to the GPU on
the render worker at its first draw.

A draw takes up to two textures (vitaGL's fixed-function pipeline has two texture coordinate
sets): those of the TEV stages whose colour reads their texture, in stage order, with
coordinates a matrix makes from a vertex attribute. They are resolved through Aurora's texture
cache as the WebGPU draw would, each list is decoded with those attributes' coordinates (a
variant of its cache entry), and each unit gets GX's texture matrix (rows 0 and 1, folded for
GL's (s, t, 0, 1) input), wrap modes and filters. The units modulate: vertex colour * unit 0 *
unit 1, which is right for a lightmap and a diffuse pass and wrong for GX's other combinations
until milestone 4. Not yet: mipmaps (first level only), EFB copies and dynamic textures as
textures, texture coordinates made from positions or normals (reflections), the post-transform
matrices. The log adds a line of draws by how many textures they took.

First run (2ade6f8): six minutes, Frigate exterior and interior, without an error. The rooms are
textured: the hangar's floor tiles and walls, the sky's stars and nebula, the corridors' panels,
the doors, the arm cannon, and the visor frame and energy bar of the HUD. Most draws take two
textures (lightmap and diffuse), and all but a handful found a Vita copy. Still wrong, as
expected before milestone 4: surfaces GX blends (glass, glow, decals) are drawn opaque, as flat
grey or white patches; the radar and map panels of the HUD are white (render targets). The
heavy seconds at 17.9 FPS (18.4 untextured), the render worker at 17.6 ms a frame (11 before).

The game's heap settled about 25 MiB higher in the same hangar than the untextured run (181
MiB in use, of the 224 the heap may take), though the textures' CPU copies are freed once
uploaded. The likeliest cause is vitaGL placing textures in the game's heap once its own pools
are full; to be measured (vitaGL's free memory by pool, in the log) before larger areas.

**Milestone 4, first part (run on the Vita): GX's fixed-function state.** Each
draw now carries GX's blending, alpha test, depth and culling, mapped to GL on the FIFO thread
(`render_state` in `vita_draw.cpp`) and set on the render worker only where it changes:
- blending: GX's blend mode and factors as Aurora's WebGPU path maps them (a source factor of
  SRCCLR is the destination's colour), subtract as reverse subtract, the clear and no-op logic
  ops; the rest copy;
- alpha test: GL takes one comparison and GX two joined by an operator, so the one that is not
  ALWAYS under AND (the usual cut-out), none when either passes under OR, else the first;
- depth: the test, its function and writes; GX writes depth with its test off when asked to,
  which GL only does with the test on, so that case tests with ALWAYS;
- colour and alpha writes, and culling, with GX's clockwise front faces.

The fragment's alpha is still the vertex colour's times the textures', not TEV's, so a draw
whose alpha GX computes otherwise blends and tests with the wrong value until the generated
shaders. The log counts blended, alpha-tested and culled draws, and every 300 presents gives
vitaGL's free memory by pool (VRAM, RAM, phycont, budget), the game heap's free memory, and the
textures on the GPU by format.

Its run (b0fff8b, seven minutes, to the Frigate's map facility, no errors): blending works where
the game blends for real, Tallon IV now shows as the orange textured planet behind the hangar,
the nebula and the radar draw, and the heavy seconds held at 18.0 FPS. But nearly every draw
counted as blended, and solid hulls and towers outside the hangar turned see-through, leaving
their glowing trim floating. The game draws most opaque surfaces blended, with an alpha of one
that TEV takes from a constant, while the fixed-function draw blended with the textures' alpha,
which on those textures holds something else. Culling is not the cause: vitaGL turns `GL_BACK`
with `GL_CW` into the matching GXM cull mode, minding whether it draws to the display or to a
framebuffer (read from its `glCullFace` and `glFrontFace`).

vitaGL's pools are not what grew the game's heap: VRAM had 75 to 83 MiB free of 96 all run, and
the game's textures took about 5 MiB of it. The heap grew because every static texture got a Vita
copy where Aurora made it, the HUD's, the particles' and the fonts' too, which the WebGPU path
still draws: those copies were never uploaded, so their CPU bytes (RGBA8, four times a 4-bit
source) stayed for as long as Aurora cached the texture.

So, second part:
- TEV's final alpha is worked out on the CPU where it does not come from a texture (`tev_alpha`):
  the stages' alpha passes over the TEV registers, the konst alphas, and an unlit channel's
  register material alpha; compare ops and lit or vertex alpha are not. With it a constant, the
  alpha test passes or fails for the whole draw (a draw that fails is not drawn), and blending by
  a source alpha of one or zero becomes a fixed blend (an opaque one is not blended at all).
  vitaGL has no constant blend colour, so a constant in between still blends with the draw's
  own alpha.
- A Vita copy is made only for the textures the Vita path resolves (`VitaCopyScope`), from their
  first level alone; a texture Aurora cached before the Vita path drew with it gets its copy
  then, from the source the game is drawing with. The rest are not converted at all.

Its run (fa769be, five minutes, no errors): the hulls by the hangar are solid again, the HUD's
frame, radar and energy bar, Tallon IV and the asteroids draw as they should. Blended draws fell
from nearly all to about one in eight; TEV's alpha is a worked-out constant for about a fifth of
the draws and reads a texture for the rest; no draw failed its alpha test outright. The game's
heap is back where it was before textures (about 156 MiB in the hangar, 181 the run before),
and the heavy seconds ran at 17.5 FPS. Still missing, and the likeliest reason for trim floating
with no body outside the hangar: what the Vita path does not draw yet, the lists whose positions
the CPU makes each frame (skinned models, about 30 calls a frame there), and the immediate draws
(particles, about 28 a frame, and the rest of `push_gx_draw`).

**The rest of the geometry (built, not yet run on the Vita).** Two kinds of draw went only to the
null WebGPU, and now come to the Vita path too, decoded each time into a mesh for that frame
alone (a vertex buffer freed once the frame is drawn):
- a call of a retained list whose positions the CPU makes each frame (skinned models): the list
  is decoded again with the positions as they are now;
- the immediate draws: a primitive's vertices as the FIFO carries them (`GXBegin`, and display
  lists called without being retained) in `draw_prim`, and `GXBeginIndexed`'s vertices with
  its host-order indices (`GX_AURORA_DRAW_INDEXED`). Inline vertex data is big-endian, as in
  display lists, so the same decoder reads it.

They share the resident draws' textures, matrices and state. Lines and points still go to the
WebGPU path. A draw whose texture has no Vita copy (an EFB copy, a dynamic texture) is now left
out rather than drawn without it, since the full-screen passes over a copy of the screen would
otherwise cover it in a flat colour. The log adds a line of immediate draws (drawn, lines or
points, not decodable) and list calls decoded each frame.

Its run (a56f00a, no crash; its log stayed on the memory card): the opening cutscene draws,
Samus walking out in the third person, the gunship, the engine glow and other particles, and
the HUD's numbers (energy 99, missiles 15). But parts of Samus's suit and the gunship were flat
black with only their green lights showing: a glow or incandescence pass, whose texture is black
but for its lights, was multiplied over the diffuse texture where GX adds it. So a unit now takes
its stage's combine: a stage that adds its texture to a register (d a register, the operation
an add) becomes a `GL_ADD` unit; the first unit, which takes the vertex colour, always
modulates. The log counts the second units added.

Its run (f7efa12, four and a half minutes through the opening, no errors): Samus's suit and the
gunship are whole, their glow added on top; every immediate draw decoded (none failed) and every
skinned call drew (about 12 a frame in the cutscene). Lines and points are still left to the
WebGPU path (up to 16,500 per 300 frames in play: the HUD's and visors' lines). Samus's
textures are mostly RGBA8: 72 RGBA8 textures took 16.4 MiB of VRAM, 206 BC1 ones 2.9 MiB, with
58 of 96 MiB free.

The cost shows: the seconds with 1,500 or more list calls a frame (here the cutscene's) ran at
11.5 FPS, the game thread waiting 24 ms a frame for the FIFO thread, which spent about 45 ms a
frame in `draw_resident` (about 26 µs a call, a skinned call's decode about 1 ms of it) and the
render worker 26 ms. What a call does each time it is drawn (resolving its textures, choosing
units, working out TEV's alpha and state, building the draw) is the next thing to make cheaper.

**Materials cached (built, not yet run on the Vita).** What the GX state decides for a draw apart
from its matrices (its texture units with their textures, samplers and combine, the attributes
their coordinates come from, the fixed-function state with TEV's constant alpha folded in) is now
a `Material`, kept in a 256-entry cache by an XXH3 hash of the state it reads: the TEV stages,
texture coordinate generation, the vertex descriptor, the loaded textures and their palettes by
what identifies them, Aurora's texture bind generation, blending, depth, culling, the colour mask,
the alpha compare, and the constants TEV's alpha may read. The game draws run after run of
calls under one material, so most calls hash about a kilobyte of state and find theirs; a draw
still reads its position, projection and texture matrices (texture matrices animate). A material
no draw used for a stats window (300 frames) is dropped with the textures it holds. On the
render worker, the projection and texture matrices are loaded only when they differ from the
last draw's. The log adds the cache's hits and misses, and zones for the hash, the working out,
the list lookup and the recording.

Its run (f869dbc, the opening again, no errors, no change on screen) made it slower: about half
the draws found their material, the hash cost about 10 µs a call (with its zone's own timing), and
the seconds with 1,500 calls a frame ran at 10.2 FPS (11.5 before). The colour constants in the
hash (konst and TEV registers, which the game sets per model) split the cache, and the skinned
calls stood out on their own: about 12 a frame at about 2 ms each, decoded whole every call.

So, second try:
- A material is the last draw's while none of the registers it reads has been written since.
  Aurora already marks those writes (`DirtyPipeline` for TEV, texgens, blend, depth, cull, alpha
  compare and the vertex descriptor; `DirtyTextures` for texture slots); its dirty bits are now
  a `DirtyBits` that sets every bit in a second set the Vita path reads and clears, so the WebGPU
  path keeps its own. Only when they are set, or the texture bind generation moved, is the state
  hashed, and the hash leaves out the colour constants.
- TEV's constant alpha is worked out per draw, and only for materials whose alpha test or
  blending reads it.
- A skinned list is decoded once into a template, with each triangle vertex's position index;
  a call copies it and reads only the positions, from the array as it is now. The faces keep the
  shading of the pose the template was made in.
- The zones inside a call are gone again, but for working out a material and a skinned call.

Its run (53a2e2c, the opening, no errors): the seconds with 1,500 calls a frame at 13.5 FPS
(11.5 before the cache, 10.2 with it), the game thread's wait for the FIFO thread down from 24 to
13 ms a frame, `draw_resident` from 63 to 46 ms a frame. Every skinned call drew from its
template (a dozen a frame, 5 ms in all where they took 22). But materials are still worked out for
about 44% of the calls in those seconds (about 750 a frame, 10 µs each): the hash takes in all
eight texture slots and texgens, and the game leaves slots no stage reads loaded with whatever it
bound last, so the same material hashes differently from draw to draw.

Also seen: the upside-down Samus in front of the camera is `CCubeRenderer::CacheReflection`'s
pass, which draws the player from a reflection camera (its up is -Z) into a 256x256 corner of the
EFB and copies it to a texture for the helmet and ball reflections. The Vita path ignores the
viewport, the copy and its clear, so the pass lands on the screen. Left for milestone 5.

So the material hash now takes in only what the stages name: the texture slots and coordinate
generators they read, and of the vertex descriptor only those generators' source attributes
(the rest of it differs from model to model and the material does not read it). The cache grew
to 1,024 entries.

Its run (a2814e5, 16 minutes, no errors): materials worked out in the heavy seconds fell from
about 750 to 590 a frame, `draw_resident` from 46 to 42 ms, the game thread's wait from 13 to 10
ms; 13.3 FPS there, the game thread's own tick higher this run. But the cache was full (1,024)
in every window: Aurora's texture bind generation, in the hash, moves at every EFB copy
(`GXCopyTex` calls `invalidate_bindings`), several a frame, and each move left every cached
material unreachable. A static texture is named in the hash by its identity already, so the
generation now goes into it only when a slot the stages read holds an EFB copy's destination.

Its run (b4ef5c5, 18 minutes to the Frigate's reactor core, no errors): the cache holds. Once a
room's materials are in it, about 3 a frame are worked out (900 per 300 frames, from tens of
thousands), the cache holding 300 to 900 of its 1,024; in the opening's heaviest seconds, where
new materials keep coming, 270 a frame (590 the run before, 750 before that). `draw_resident`
is 40 ms a frame there (63 before the material work), the game thread's wait for the FIFO thread
10 ms (24), 13.8 FPS. The frame is now the game thread's own: about 31 ms of tick and 30 of
draw. The FIFO thread's remaining cost per call (about 19 µs with the material reused: the list
lookup, recording the draw) is the next of its costs, but no longer what sets the frame rate.

**Milestone 4, generated programs (built, not yet run on the Vita).** First, why milestone 2's
Cg program crashed in `glLinkProgram`: vitaGL takes `GL_VERTEX_SHADER` and `GL_FRAGMENT_SHADER`
source for GLSL, and by default (`VGL_MODE_POSTPONED`) only translates and compiles it at the
link; `glCompileShader` said yes to a source it had not compiled, the translation of Cg as GLSL
failed at the link, and the link then read the invalid GXM program (the dump's return address is
just after `sceGxmProgramGetParameterCount` on the vertex shader's program). Cg source takes
`GL_CG_VERTEX_SHADER_EXT` and `GL_CG_FRAGMENT_SHADER_EXT`, and then compiles at
`glCompileShader`, with a status that holds. `glLinkProgram` also looks up every
`glBindAttribLocation` name in the compiled program and uses the result unchecked, so a name the
compiler dropped would crash it too: the generated programs bind no names, and ask for the
locations after the link (`glGetAttribLocation` and `glGetUniformLocation` check theirs). vitaGL's
uniform locations are its records' addresses negated: anything but -1 is one.

`gx/vita_tev.cpp` generates a Cg vertex and fragment program from the TEV state when a material
is worked out:
- the stages' colour and alpha combiners as GX evaluates them: d +/- lerp(a, b, c) with bias,
  scale and clamp (to [0, 1], or GX's 11-bit range unclamped), and the compare modes (R8, GR16,
  BGR24, RGB8/A8, greater and equal); a, b and c clamped to [0, 1] as GX's 8-bit inputs are;
- every input: the four registers (PREV first), konst colours and their component selections,
  texture and raster colours through the swap tables, one and a half;
- up to four textures and four coordinate generators (2x4, or 3x4 sampled projectively), each
  with its matrix for GX's (s, t, 1, 1) input;
- the raster colour from the channel setup: the vertex or register material colour, and for a
  lit channel the ambient colour plus the face's shading (there are no normals in the meshes
  yet, so no real lights);
- the alpha compare with both comparisons and the operator, as a `discard`.
A program is cached by a hash of the state it is made from, then by its source. Each draw carries
the konst colours, registers, channel colours and alpha references (13 vec4s), its coordinate
matrices and the program's textures; the fixed-function draw's units and state stay beside them.

On the render worker, a program compiles at its first draw, one a frame at most (vitaGL's shader
cache keeps them on the memory card for the next run); until it is ready, or if it fails, the draw
goes the fixed-function way, and the first three failures log their source. Indirect textures,
coordinates made from positions or normals (reflections), more than four textures or coordinate
sets, and fog leave a material fixed-function. `MP_VITA_TEV=0` turns the programs off.

The meshes now carry four coordinate sets, the vertex colour as GX has it and the shading apart
(56 bytes a vertex, 32 before). The log adds the draws recorded with a program and the programs
generated (the materials line), and the draws drawn with theirs, waiting, and compiled, failed
and the time spent compiling (the presents line).
