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

Its first run (a30f548): the programs work. Seventeen built, none failed, about 350 ms each
(6 s in all), and every draw in those seconds drew with its program, none waiting. Then the
render worker crashed inside SceShaccCg compiling the eighteenth: its stack is 32 KiB (the dump's
thread record: base 0x820e9000, size 0x8000, what VitaSDK gives every `std::thread`), and the
crash's stack pointer, 0x820e8ad0, was below its base. The compiler overflowed it. A program now
builds on a thread of its own with a 2 MiB stack while the render worker waits for it.

Its run (9f6d5cf, 20 minutes to the Frigate's reactor core, no errors, no crash): 61 programs
built, none failed, about 375 ms each (23 s in all, spread as rooms come into view); nearly every
draw drew with its program (a handful a window waited a frame for theirs). Three things to do:
- The compiles are not kept: the first seventeen took the same 6 s as the run before, so
  vitaGL's shader cache does not keep these Cg programs, and every session pays the 23 s again
  in hitches. The programs' GXP binaries need a cache of our own on the memory card.
- The draws cost the render worker more: about 35 ms a frame in the heavy seconds (25 before),
  the game thread waiting 17 ms (10), 11.9 FPS there (13.8); four uniform uploads a draw, the
  13 constants among them, against the fixed-function draw's matrix loads.
- Memory: VRAM 39 MiB free of 96 at the end (about 60 before), with 32 MiB of textures in it, and
  the meshes' vertices are 56 bytes now; the game's heap at 160 MiB in use.

**Programs kept on the memory card (built, not yet run on the Vita).** A program's two GXP
binaries now go to `ux0:data/MetroidPrime/vita_programs/<key>.gxp` when it is compiled, the key
being the XXH3 of its sources with a version seed (raised when what a source compiles to could
change); a later session loads them with `vglShaderGxpBinary` (which registers a GXP with the
shader patcher as a compile does) and links, without the compiler. The compile itself now calls
vitashark (`shark_compile_shader`) directly for the GXP, as vitaGL's `glCompileShader` does for
the `GL_CG_*` types; `vglGetShaderBinary` was no use for this: it writes vitaGL's own format
(a uniform list ahead of the GXP) and cannot say its size without writing. A file holds a header
(magic, both sources' lengths, which a hash collision would also have to match, both binaries'
sizes) and is written aside and renamed, so a file cut short is never read; binaries that no
longer link are removed for a compile next time. Loads have their own budget (eight a frame) apart
from compiles (one), and the log gives the programs loaded from the card and the time they took.

Its run (685daf0): every compile failed at once, "the Cg compiler failed; fatal internal error",
so nothing was saved and every draw went fixed-function. vitaGL starts the compiler
(`start_shader_compiler`: `shark_set_allocators`, `shark_init`) only inside `glCompileShader`,
at the first compile it is needed for; this session's fixed-function shaders all came from its
shader cache, so nothing had started it when vitashark was called directly. The first compile now
goes through `glCompileShader` once, with a throwaway shader, so vitaGL starts the compiler its
own way.

Its two runs (a48830e; the build stamp reads 685daf0-dirty): the same, 44 and 46 programs
failed, none built or loaded, "fatal internal error" every time, though the first compile took
about 120 ms, so the throwaway compile did run. Both reached the Emergency Evacuation Area all
fixed-function, 11.2 FPS in the heavy seconds, VRAM 49 MiB free. Why vitashark fails when called
from outside vitaGL is not known, and the direct calls are dropped.

**Programs kept as GL program binaries.** Compiles are back on
vitaGL's `glCompileShader` with the `GL_CG_*` types and a link, the path that built 61 programs
in 9f6d5cf. A linked program is then saved with `glGetProgramBinary` (its size from
`glGetProgramiv(GL_PROGRAM_BINARY_LENGTH)`; vitaGL writes the whole binary whatever the buffer
says, so the buffer has room to spare). vitaGL's format holds the program's attribute bindings
and both shaders' GXPs with their parameter tables; `glProgramBinary` rebuilds the two shaders
from them, registers them with the shader patcher and links, with no compiler. The file's header
is now magic `VTP2`, both sources' lengths, the binary's format and size; the rest (key, writing
aside and renaming, removal of a binary that does not link, the two budgets) is as before.

Its two runs (6c5f443; the build stamp still reads a48830e-dirty), both about five minutes to the
Map Facility, no crash:
- First, empty card: 53 programs compiled, none failed, 22.5 s of compiling in all (about 420 ms
  each, as in 9f6d5cf), and each saved.
- Second: 53 loaded from the card in 414 ms in all, about 8 ms each, none failed to link; one new
  program (a state the first run did not reach) compiled in 368 ms and was saved. At most one draw
  a window went fixed-function waiting for its program, against 256 in the first run's worst.
- Speed unchanged, as expected: 11.3 FPS in the heavy seconds both runs, the render worker about
  35 ms a frame there. VRAM 42 MiB free at the end.

The card cache is done. What a program draw costs the render worker is next.

**Cheaper program draws.** vitaGL keeps each program's
uniforms and, at a draw, reserves and fills a side's uniform buffer only when a `glUniform` call
or `glUseProgram` has marked that side since (`dirty` flags in `_glDrawArrays_CustomShadersIMPL`).
Every program draw sent four arrays, so both sides were filled every draw. Now:
- The vertex program reads one array, `uVert`: the projection times the position matrix (worked
  out on the FIFO thread, which waits on the render worker), then only the rows each coordinate
  set uses (three for a projective one, else two), against seven matrix rows and all twelve
  texture rows before.
- The fragment program declares only the constants its source reads, renumbered in order
  (`pack_constants`); a TEV register's constant is read only where a stage reads the register or
  it holds the result.
- Each program remembers what its two arrays hold, and the render worker sends one only when the
  draw's differs, so the fragment side, whose colours seldom change between draws, is mostly left
  alone, and the vertex side too between draws under one matrix.
- A texture already on its unit with the same sampler state is not bound again (both paths).
The sources change, so the first session compiles every program again (about 22 s of hitches in
all), saved for later. The log's present line gives uniform arrays sent and kept a side, and
textures bound and kept.

Its run (f25cc22; stamp 9c985e4-dirty): one session of 12.7 minutes, a first one for these
sources (54 programs compiled, none failed, 25 s in all), to the Map Facility, no crash. Photos:
the HUD, scan icons and the map draw right, so the packing holds.
- The skips work: of program draws' arrays, 84% of the vertex ones and 87% of the fragment ones
  were left alone, and half the texture binds.
- They were not the cost: the render worker went from about 34.7 to 32.7 ms a frame in the heavy
  seconds, 11.5 FPS there (11.3); the game thread still waits on it (begin-wait 24 ms). With about
  2000 draws in such a frame that is some 16 microseconds a draw, mostly inside vitaGL's draw
  (`_glDrawArrays_CustomShadersIMPL` patches and sets the vertex program, sets the fragment
  program, the textures and the streams every draw). Where those microseconds go needs measuring
  before more is cut.
- In the morph ball something covers the view until Samus unmorphs (not new). The ball's shadow
  is drawn into a texture by an EFB copy and projected onto the ground, so this is most likely
  milestone 5's copies.

**The render worker's present, timed.** One present in eight is
timed section by section with `sceKernelGetProcessTimeWide` (microseconds); each mark ends a
section where the last ended, so the sections add up to the present. Once a second the log gives
a `[draw path]` line, per timed present: the draws (and those with programs); for program draws
the program switches, the arrays (buffer and attribute pointers), uniforms, textures, state and
`glDrawArrays`; for fixed-function draws the switches, arrays, texture units, matrices, state and
`glDrawArrays`; and for the frame the setup, program builds, the reset after the draws, the test
triangle and stats, ImGui and `vglSwapBuffers`. The timer's own cost is measured once at the first
present and given with the number of calls a timed present makes, so it can be taken off: each
section holds about one call a mark. Timed presents are slower by that much, one in eight.

Its run (1789720; stamp ef886cd-dirty): 8 minutes, no crash, programs all from the card. The timer
costs 0.68 us a call. The 40 timed presents of 1500 draws or more averaged 1715 draws, 1693 with
programs, and 31.1 ms once the timer's calls are taken off (one a section a draw, about 1.15 ms
a program-draw section):

| Section | ms | a draw |
|---|---|---|
| program draws' `glDrawArrays` (vitaGL) | 8.4 | 4.9 us |
| program draws' uniforms (compare, send) | 5.8 | 3.4 us |
| fixed-function draws' arrays, about 22 draws | 4.4 | about 200 us |
| finish: the frame's draws destroyed | 4.3 | 2.5 us |
| program draws' arrays (buffer, made at first use, and pointers) | 4.1 | 2.4 us |
| textures, state, switches, builds | 1.3, 1.1, 0.4, 0.6 | |
| setup, swap (the GPU waited on) | 0.2, 0.2 | |

- `vglSwapBuffers` hardly waits: the GPU keeps up, the CPU side is the cost.
- `draw_world(take_frame())` destroys the frame's vector after `draw_world` returns, so "finish"
  is mostly the 1715 draws' destructors: some eight `shared_ptr` releases a draw (mesh, textures,
  program) and the meshes made for that frame alone (skinned and immediate draws), whose
  destructors queue their buffers' deletes. That work need not be on the render worker.
- The fixed-function draws' arrays cost about 200 us a draw: their meshes are large and made for
  the frame (the buffer is made and filled at the first bind), most likely. Which draws these are
  and how many bytes they upload needs counting.
- Uniforms cost 3.4 us a draw though most arrays are not sent: what is the compare and what the
  `glUniform4fv` calls is not split yet.
- A draw holds about 870 bytes (both paths' inputs), some 1.5 MB a heavy frame written by the FIFO
  thread and read here, which the caches feel in every section.
- `glDrawArrays` itself, 4.9 us a draw, is vitaGL's own work (patching and setting the programs,
  textures, streams, uniform buffers); fewer draws is the only lever on it from here.

**Frames freed off the render worker, and the unclear sections split.** A drawn frame goes to a reaper thread of its own, which destroys its draws; the render
worker's `enqueue_work` runs work at once when called on the render worker itself, so the
deletes of the meshes made for that frame alone (`glDeleteBuffers`) were part of "finish" too.
From the reaper they are queued and run on the render worker between presents (render worker
busy time still, but not the present's). The timing now splits:
- arrays into "upload" (the mesh's buffer bound, made and filled at its first draw) and
  "pointers", on both paths;
- uniforms into "compare" (against what the program holds) and "send" (`glUniform4fv`).
A second line, `[draw uploads]`, gives per timed present for each path and mesh kind (`lists`
decoded once and kept, `skinned` list calls, `immediate` and `indexed` draws, `other`: draws,
buffers made, their KiB, and the upload section's time; for the fixed-function draws, how many
wait for their program and how many have none (a material a program cannot draw yet), and the
largest mesh made for one since the last line.

Its run (7073285; stamp 61027ac-dirty): 5.6 minutes, no crash. The 43 timed presents of 1500
draws or more: 1708 draws (1686 with programs), 27.3 ms with the timer's calls taken off (31.1
before). With them taken off section by section:
- finish: 0.03 ms (4.3). But the render worker's busy time hardly fell (32.4 to 31.5 ms a frame,
  [split]): the deletes queued from the reaper run between presents now, about 2.5 ms of them, so
  `glDeleteBuffers` on a buffer drawn that frame is itself dear in vitaGL.
- upload: the kept lists' 1700 draws about 2 ms (a `glBindBuffer` each); the meshes made for the
  frame cost the rest, about 3.5 ms for some 36: 28 immediate (175 KiB), 7 skinned (121 KiB) with
  programs, and one skinned mesh without a program, 325 to 450 KiB (5934 to 8202 vertices) alone
  taking 1.5 ms.
- The fixed-function draws are 16 a heavy frame, none waiting for a program: materials a program
  cannot draw yet (texture coordinates from normals or positions, most likely; the arm cannon's
  reflection is one), and the big skinned mesh is among them.
- compare: about 5.5 ms, 3.2 us a draw, though it is two `memcmp`s of a few hundred bytes. It is
  the first read of the draw's program inputs (464 bytes, written by the FIFO thread on another
  core), so it is cache misses, not the compare; send (`glUniform4fv`) is only 0.5 ms.
- vitaGL's `glDrawArrays` 8.4 ms, pointers 1.1, textures 1.3, state 1.1, builds 0.8, switches 0.4.
- 10.9 FPS in the heavy seconds (11.1 the run before; another mix of rooms).

Two things follow. The meshes made for one frame should not get a buffer of their own:
drawn from client memory, vitaGL copies them into its per-frame pool, with no buffer to make,
fill and delete (about 3.5 ms of uploads and 2.5 ms of deletes for some 36 meshes). And a draw's
program inputs should be smaller and read less: only the rows a program declares, in a per-frame
arena, with a draw whose values repeat the last ones of its program in that frame pointing at the
same ones, so that the render worker skips them without reading them.

**Frame-only meshes from client memory, program constants in the frame's rows.**
- A mesh made for one frame (skinned list calls, immediate and indexed draws) whose material has a
  program has no buffer now: its arrays point at its vertices in client memory, and vitaGL copies
  them into its per-frame pool at the draw. For a program's attributes that all lie within one
  vertex of the first (aPos at offset 0, the others after it), vitaGL counts them "packed" and
  makes one copy of the draw's vertices (`_glDrawArrays_CustomShadersIMPL`). Its fixed-function
  draw copies each attribute apart (position, colour, both coordinate sets: four copies), so a
  frame-only mesh without a program keeps its buffer (the big skinned one among them).
- A draw's constants are rows in its frame now: the frame (draws, rows, a serial) is what the FIFO
  thread hands over. record_draw works them out on its stack and adds them to the rows unless they
  repeat its program's last ones in that frame, when the draw points at those. The render worker
  sends a program's array unless it is the rows it sent last (same frame, same place), reading
  nothing to tell. This holds because a program ready at its first draw of a frame stays ready the
  whole frame (a build is tried there and the budget only shrinks), so its draws are sent in order;
  the first in each frame is always sent. A draw is about 460 bytes smaller (about 410 now).
- The log's material line gives constant arrays recorded and those repeating the last.

Its run (8f7e350; stamp 8d54e6c-dirty): 8 minutes, as far as the Reactor Core Entrance, no crash,
nothing reported wrong on screen.
- 86% of the draws' constant arrays repeated their program's last ones and were not stored again;
  85% (vertex) and 86% (fragment) of program draws sent nothing.
- The 48 timed presents of 1500 draws or more: 1676 draws, 19.6 ms with the timer's calls taken
  off (27.3 the run before). The frame-only meshes with programs upload in 0.05 ms in all (about
  2 ms before); compare is about 0.7 ms (5.5); vitaGL's draw went from 8.4 to 9 ms, its copy of
  those vertices into its pool included. The one big skinned fixed-function mesh is still 1.6 ms.
- The render worker's busy time in the heavy seconds: 24.0 ms a frame (31.5), with the deletes
  of the frame-only buffers gone from between presents too.
- But 11.5 FPS in the heavy seconds (10.9): the game thread now waits for the FIFO thread. Its
  zones in those seconds: `begin_frame` 22.1 ms a frame, of it `finish_frame_end` (the FIFO
  thread ending the last frame, AURORA_DEFERRED_FRAME_END) 21.0 ms; the render worker's frame
  slots are free. A heavy frame of the game thread: input 3, tick 31 (particles' update 12, CPU
  skinning 4 of it), begin-wait 24, draw 33 ms (DrawWorld 27), about 91 ms. The FIFO thread,
  which works through the frame's GX commands as the draw sends them and ends the frame during
  the next tick, needs some 85 ms a frame: it is the bottleneck now, and what it spends them on
  (Aurora's command processing, the Vita's materials, decoding and recording, Aurora's own work
  for its WebGPU frame) is not measured yet. Without its wait the game thread would be at about
  67 ms, some 15 FPS.
- VRAM 33 MiB free at the end (735 BC1 textures, 10 MiB, in the reactor rooms).

**The FIFO thread's frame, timed.** As the render worker's
present: one frame in eight, marks ending sections where the last ended
(`lib/vita_fifo_clock.hpp`), from the end of the last frame's end task to the end of this one's;
a `[fifo path]` line once a second gives them per timed frame, with the timer calls a frame:
- idle: the worker waiting for the game to publish commands (`fifo::worker_main`);
- state: register writes and the stream's reading, everything not below;
- copies: BP 0x52 (EFB copies and clears);
- material, decode, record: the Vita path's draws, marked after `current_material`, at
  `record_draw`'s start (the triangles and the mesh made before it) and at its end;
- other draws: what is left of a draw command (draws the Vita path did not take, to Aurora's
  WebGPU path);
- aurora: Aurora's other extension commands (texture object and array loads, retains...);
- the frame end task (`submit_frame`): vita (end_frame_draws, the stats logs), textures
  (`texture::end_frame`), finish (`gfx::finish`), frame (`gfx::end_frame` and the rest).
Busy is everything but idle.

Its run (ca83dc5; stamp 60164c5-dirty): no crash. The 34 timed frames in seconds of 1500 list
calls a frame or more: 107 ms busy, of it about 21 ms the timer's own (31,500 calls a frame at
0.68 us), so some 86 ms of work, as the game thread's wait had it. With each section's calls
taken off (one a draw for the Vita sections; about 12,000 a frame for state and aurora, two for
each extension command that is not a draw):

| Section | ms a heavy frame |
|---|---|
| decode (from the material to `record_draw`) | 32.5 |
| material (`current_material`) | 23 |
| record (`record_draw`) | 13 |
| state (register writes, the stream) | 8.5 |
| aurora (some 10,000 extension commands a frame besides the draws) | 5.5 |
| other draws | 2.8 |
| copies, the frame end task | 0.4 |

- Material: the stats (per 300 frames) in a heavy stretch give about 210 materials worked out a
  frame and 910 found by hash, of 1400 list calls; 991 cached of the cache's 1024 direct-mapped
  slots. With about as many materials alive as slots, they evict each other: the cache is too
  small for these rooms.
- Decode: no list was decoded again (0 redecodes), so for most calls it is the lookups alone
  (`resident::find` of the position array, the list's variants in `sCache`, `matches_current`:
  the descriptor and each used vertex format, a `VtxFmt` of some hundreds of bytes, in cold
  memory), plus some 15 skinned calls from their templates (a copy of each template's vertices,
  450 KiB in all a frame, and the positions read again) and the immediate draws' decodes. Which
  of these the 32 ms is needs the section split.
- Record, 7.5 us a draw: the matrices, the texgen rows and constants worked out and compared, and
  the 410-byte draw with its `shared_ptr` copies pushed into the frame.

**The material cache set-associative, decode split.**
- 4096 materials in 1024 sets of four (1.1 MiB, 288 bytes each), the least recently used of a
  set replaced (a use stamp on each, set at every use, the last draw's kept one included), an
  empty entry first. The materials line now gives, of those worked out, how many sample an EFB
  copy (their hash moves with each copy, so they are worked out again after one) and how many
  replaced a material still in use that window, and the cache's size.
- The FIFO timing's decode is four sections now: lookup (a cached list's triangles found, decoded
  first when no variant matches), skinned (a skinned call's triangles from its template, or
  decoded whole), immediate (an immediate or indexed draw's vertices decoded and triangulated),
  mesh (the mesh made of the triangles, or a cached list's taken, up to `record_draw`).

Its run (a935426; stamp bdd2dcf-dirty): 8 minutes, to the Cargo Freight Lift to Deck Gamma, no
crash.
- Materials worked out: about 2000 per 300 frames (64,000 before in the heavy stretch), most of
  them sampling an EFB copy (whose hash moves with every copy); 1100 to 2800 cached of 4096.
- The FIFO thread's heavy frames (36 timed): 98 ms busy, 21 of it the timer's, so about 77 ms of
  work (86 before). With each section's calls taken off:

| Section | ms a heavy frame |
|---|---|
| material | 17 |
| lookup | 13.3 |
| record | 11.6 |
| skinned (some 15 calls) | 10.6 |
| state | 8 |
| immediate (immediate and indexed decodes) | 7 |
| aurora | 5 |
| other draws | 2.7 |
| mesh | 0.8 |

- Material is still 10 us a draw with few worked out: the hash (XXH3 over some 1.2 KiB built from
  the stages, generators and textures) for each draw whose registers were written, and, as no
  mark comes between, the list call's own `resident::find` of its key in `call_resident_dl` and
  the command's dispatch before `current_material`.
- Lookup, 8 us a list call: `resident::find` of the position array (is it skinned?), the list's
  entry in `sCache`, and `matches_current` against variants whose `DecodeState` holds all eight
  vertex formats (312 bytes each, some 2.6 KiB a variant, 10 KiB a list's entry): cold memory,
  read for every call. Comparing a small hash of the current descriptor and used formats (hot,
  the FIFO thread just wrote them) and remembering the last position array's answer would leave
  little of it.
- Skinned: some 0.7 ms a call (template copied, positions read again), worth a look of its own.
- 11.7 FPS in the heavy seconds; the game thread still waits 22 ms a frame for the FIFO thread.

**Cheap list lookups.**
- `DecodeState` holds a hash of the vertex descriptor and the formats the list names
  (`layout_hash`), the format mask, the texture coordinates and three fields of each array: about
  90 bytes (2.6 KiB before; a list's four variants some 420 bytes, 10 KiB before).
  `layout_hash` remembers its hash for each format mask until a descriptor or format write moves
  `GXState::vtxLayoutVersion` (bumped in `clearVtxSizeCache`, which every VCD and VAT write
  calls), so a list call compares one number with the current layout's, worked out once a change.
- Whether the position array is retained (a list over one that is not is a skinned call) is
  remembered for the last array, and forgotten when a list is retained or released.
- The list stats count re-decodes for a changed layout (descriptor or a named format) together.
- A new FIFO section, dispatch, ends at the Vita path's entry (`draw_resident`, `draw_immediate`,
  `draw_indexed`): a draw command's reading and a list call's own `resident::find` of its list,
  which the material section held before.

Its run (57083c0; stamp 53787cc-dirty): 11 minutes, to Biohazard Containment, no crash, nothing
reported wrong. No list was decoded again for a changed layout (some for changed textures in new
rooms, as before); skinned templates made only in new rooms.
- 13.1 FPS in the heavy seconds (11.7), the game thread's wait for the FIFO thread 16.0 ms a
  frame (22.4), tick 29.8, draw 30.8 ms; the render worker 22.8 ms.
- The FIFO thread's heavy frames (40 timed): 97 ms busy, 24 of it the timer's, about 73 ms of
  work (77). With each section's calls taken off: material 14, record 13.4, skinned 9.6, state
  9.5, immediate 6.9, aurora 6.6, lookup 5.5 (13.3), dispatch 4.5, other draws 3.2, mesh 1 ms.
- The extension commands ([ops], a heavy frame): about 11,000 besides the frame end: 1540
  resident list calls, 1520 draw tags (`GX_AURORA_SET_DRAW_TAG`, for the PC port's draw-id and
  PBR features, nothing the Vita path reads), 1420 texture object loads, and about 6000 array base
  loads (the game sets most attribute arrays for each model, those the descriptor does not index
  too). The game thread writes each and the FIFO thread reads each.
- Left in lookup: the `sCache` map's find of the list (the list's own entry was found already in
  `call_resident_dl`, and could hold it) and the variants' first read.

**The material key in parts, the list's cache in its entry.**
- The key is two hashes kept between draws: the pipeline part (stages, blending, depth, culling,
  masks, alpha compare, swap tables, channels, the generators the stages name; with the maps and
  the TEXn sources those name) worked out again only on DirtyPipeline, which every register it
  reads sets (regs.cpp: gen mode, z mode, blend modes, TEV, ksel, alpha compare, channel control,
  texgens, post matrices, matrix index B); the texture part (the named maps' texture objects and
  palettes, and whether one is an EFB copy) only on DirtyTextures, a new pipeline part or a moved
  bind generation. A draw's key mixes the two with the descriptor's entries for the TEXn sources
  (four bits each; a descriptor write sets no dirty bit) and, when a copy is sampled, the
  generation. The materials line counts the parts worked out.
- `resident::Entry` holds a pointer to the list's `sCache` entry, set at its first call (an
  unordered_map's elements stay where they are; both go at the list's release), so a call no
  longer finds its list in `sCache`.

Its run (32660ef; stamp 000a964-dirty): 6.6 minutes, to Biohazard Containment, no crash, nothing
reported wrong.
- 13.5 FPS in the heavy seconds (13.1), the game thread's wait 11.7 ms a frame (16.0); tick 29.5,
  draw 31.4 ms: the game thread's own work (some 64 ms with input) is now about the FIFO
  thread's.
- The FIFO thread's heavy frames (38 timed): 89 ms busy, 23.6 of it the timer's, about 65 ms of
  work (73). Material about 8 ms (14), lookup 4.4 (5.5); the rest as before: record 13.4, skinned
  9.6, state about 9, immediate 7.5, aurora about 6.5, dispatch 4, other draws 3, mesh 1.
- Per 300 frames of the heavier stretches: of some 130,000 draws not keeping the last draw's
  material, the pipeline part was worked out 30,000 to 43,000 times, the texture part about once
  each (every texture object load sets DirtyTextures). The texture part still hashes all eight
  slots' 48 bytes, those no stage names included.

**Fewer extension commands at the source.**
- Draw tags: `draw_tag::flush` sends nothing on the Vita. Only Aurora's WebGPU draws read a tag
  (to name a draw in a warning); some 1500 a heavy frame were written by the game thread and
  read by the FIFO thread. A tag set inside a display list is still recorded into it.
- Array loads: `GXSetArray` remembers, by CP index, the array last sent outside display lists
  (pointer, size, stride, byte order) and leaves out a set that repeats it: the processor treats
  such a set as no change anyway (it keeps its cached range). A display list records every set.
  A display list sent as its bytes (`GXCallDisplayList` of one not retained) may set arrays itself,
  so what was sent is forgotten after one; a retained list's bytes are draws alone (the Vita
  decode fails on anything else, and none has).
- The [ops] line shows what is left.

Its run (3255423; stamp 43538b2-dirty): no crash. No draw tags; about 9,500 extension commands
a heavy frame (11,000), the array base loads hardly fewer: they name another model's arrays,
not the same again. 14.1 FPS in the heavy seconds (13.5), the game thread's wait 10.9 ms, tick
27.1, draw 30.6 ms; the FIFO thread about 64 ms of work.

Reported on screen (not new): 3D models show through the HUD; the planet outside covers most
models; the frigate seems cut off at a distance. The Vita path ignored the viewport's depth range
(GXSetViewport's near and far: `GXState::renderViewport` znear, zfar), with which the game layers
its passes: the HUD drawn into the nearest depths, the sky and the planet into the farthest. All
depth went to [0, 1], so the HUD and the planet were sorted with the world (the planet covering
the far frigate too, the "render distance").

**The viewport's depth range (built, not yet run on the Vita).** record_draw folds it into the
projection, as glDepthRange would apply it to GL's depth d: n + (f - n) d. Clip z, already
2 z + w for GX's -w..0, becomes (f - n) z' + (f + n - 1) w, so [0, 1] lands in [n, f]; the clip
volume stays inside the GL one, so nothing visible is clipped. The program draws' MVP and the
fixed-function projection both take it. The textures line counts draws with a range other than
[0, 1].

Also reported: with Twin Stick Aim on, L and R stop working in the pause menu, so it cannot be
turned off there (`twin_stick=0` in `port_settings.ini` turns it off). The cause is not twin stick
itself: "Separate menu buttons" (`menu_standard_buttons`, on by default; touch is never active on
the Vita) makes menus and the map read their own button rows, whose defaults come from
`PADGetDefaultMapping` for the pad's type. That had no Vita case, so the Vita's pad (SDL's
"standard") got the standard layout: L and R on analog triggers it lacks, dropped, so no L or R
in menus at all, while gameplay (`__PADSetDefaultMapping`) has the Vita layout. Now
`PADGetDefaultMapping` gives the Vita layout on the Vita too (L and R on the shoulders, Z on
Select). Under twin stick the left shoulder is also the beam shift with the D-pad
(port_input_devices.cpp); that chord is bound in menus too, which needs checking on the device.

**Viewports, scissors and EFB copies' clears (a first piece of milestone 5).** Photos of the depth range build: an upside-down Samus over the view in the
intro and in play, and a white background (gone in the scan visor). The upside-down Samus is
`CCubeRenderer::CacheReflection`: the reflected view drawn into a 256x256 corner (viewport and
scissor set to it), copied into the reflection texture and cleared (`GXCopyTex(..., clear)`). The
shadows (`CMorphBallShadow`, `CProjectedShadow`, `CWorldShadow`) work the same way. The Vita path
ignored the viewport and scissor rectangles and the copies' clears, so such a pass was drawn over
the whole screen and stayed; the white is most likely a shadow pass's (none in the scan visor).
- A draw carries GX's viewport and scissor (`logicalViewport`, `logicalScissor`, in the game's
  render mode's framebuffer, `vi::configured_fb_size`, 846x480 for its widescreen one) scaled to
  the 960x544 screen; the render worker sets `glViewport` and `glScissor` where they change, the
  scissor test on through the frame's draws.
- A texture copy that clears (BP 0x52, `bp_efb_copy`) records a clear among the frame's draws: its
  source rectangle (`texCopySrc`), GX's clear colour and depth, with the colour, alpha and depth
  updates as masks (as Dolphin clears). The copy itself is still not made, so reflections and
  shadows sample nothing yet; their passes no longer show.
- The textures line counts draws with a smaller viewport and the copies' clears.

Its run (aa68617; stamp 434fe33-dirty): 13 minutes, no crash; the upside-down Samus not seen
again. Thousands of draws a window with a smaller viewport and some hundreds of copies' clears.
14.3 FPS in the heavy seconds. Reported: Samus's high-detail model and the gunship's colours are
off (bright, flat orange; asked for before), and the morph ball's light seems to turn the
surfaces round it grey.

**GX lighting in the generated programs (the rest of milestone 4's lighting).** A lit channel was the material colour times the ambient colour plus a shading
made up from the face's normal: none of the game's lights, their colours or the vertex normals,
so lit models (Samus, the gunship, the creatures) came out flat, and a light like the morph
ball's did nothing. Now, as Aurora's shaders do (shader.cpp, lighting_func):
- A vertex carries its normal (the first three components of a normal, or of NBT; unit length,
  as three signed bytes in the bytes the padding held: still 56 bytes). A skinned template reads
  the normals again with the positions at each call, CPU skinning writing both.
- The vertex program has the view-space position and normal (GX's current position and normal
  matrices; a zero normal stays zero) and sums each lit channel part's lights: GX's diffuse
  functions (none, sign, clamp) and attenuations (none, spot: angle and distance, specular), GX's
  lights being in view space. The sums go to the fragment program (vLit0, vLit1: rgb the colour
  part's, a the alpha part's), whose raster colour is material times clamp(ambient + lights).
- The lights are rows of the draw's vertex constants after the coordinate rows: the position and
  normal matrices, then for each lit part its mask's lights (five rows each). The program has the
  count of each part's lights (in its key, and in the material's pipeline key), the rows the
  lights the mask names at the draw.
- Texture coordinates from the position or the normal (GX_TG_POS, GX_TG_NRM: environment
  mapping), normalisation, and post-transform matrices (dual texture matrices: three more rows,
  projective sampling after one), so such materials have a program instead of the fixed-function
  draw. kProgramVertexConstants is 194 rows now.
- The sources changed (kBinaryVersion 3): the first session compiles every program again.

Its run (76cab02; stamp aa68617-dirty): 17.6 minutes, no crash; 192 programs built, none
failed (the lit ones compile: Cg as generated is right), but about a second each (205 s of
compiling in the session, against 54 programs and 25 s before: the light counts make more
programs, and lit ones are longer). No fixed-function draws left. The present's heavy frames:
22.2 ms (19.6), vitaGL's draw 15.3 ms (10.1), the lit draws' uniform buffers being larger; the
render worker's busy time and the 11.1 FPS were inflated by the compiles, one a frame at a second
each. Photos and reports, in the morph ball: the surfaces round the ball pale and untextured; the
shell disappearing at times; the ball's trails gone (they were there before this build); the
map's outlines missing (always).
- The pale surfaces: `CMorphBallShadow::Render` draws the surfaces in the shadow's volume again,
  blended, black with the shadow in alpha, sampling the shadow's EFB copy (texture map 7). Without
  a Vita copy of it the material had no program and drew fixed-function with the other textures,
  a pale wash. A material whose program would sample an EFB copy's texture is left out now, until
  milestone 5 makes the copies.
- The shell and the trails: a 2x4 generator with a post-transform matrix was sampled projectively,
  divided by what the post matrix makes of z; Aurora keeps its x and y. Environment maps take
  that path (newly given programs by this build). Fixed.
- The map's outlines are GX lines, which the Vita path never drew. Immediate lines and line
  strips are drawn now as GL lines (meshes have a primitive), one pixel wide (GX's line width is
  not applied); points are still not drawn.
- The dark blue bands above and below in the morph ball are its HUD's strips, drawn opaque; to
  look at.

Its run (1b0ff54; stamp 76cab02-dirty): the trails back, the map's outlines drawn; but the shell
mostly missing and pieces of the gun and some pipes coming and going; and a crash on the death
screen (staying on its "continue" prompt): `std::bad_alloc` in `make_vita_texture`
(`new_static_texture_2d`), the heap having grown from about 128 to 185 MiB in use (220 taken) in
some ten seconds. 211 programs, 180 loaded from the card.
- Leaving out a material that samples an EFB copy was wrong: the game's reflective materials (the
  shell, the gun, pipes) sample the reflection, `CacheReflection`'s copy, and showed only when the
  game swapped its black texture in. And the death screen draws over a copy of the last frame
  with a palette-animated texture: a new conversion each frame (about 0.5 MiB, with its Vita copy)
  for a draw then left out, so the copies' pixels were never uploaded and freed, and Aurora's
  content cache (a 128 MiB desktop budget) kept them until the heap ran out.
- Now an EFB copy's texture has a stand-in, 1x1 transparent black: a reflection adds nothing, the
  morph ball's shadow fails its alpha test and draws nothing, and the material draws. The
  textures line counts such materials.
- The content cache's budget is 32 MiB on the Vita (`texture::ContentCacheBudgetBytes`).
- The orange test triangle is gone.

Its run (1022711; 437 s): no crash, the heap steady at about 141 MiB in use (158 taken), the
death screen survived. 172 programs, 146 loaded from the card. Reports: the borders round the
picture (the boot logos' sides, the morph ball's strips) dark blue, not black; the pause screen's
and the map's blurred backdrop missing.
- The dark blue was present's clear colour, chosen at bring-up to tell this path's screen from a
  black one; where nothing draws, it showed. Black now, as the GameCube's frame starts.
- EFB copies are made (the start of milestone 5): an EFB copy into a texture is a draw among the
  frame's draws (`record_copy`, before its clear), and the render worker reads that rectangle of
  the screen back there (vitaGL's `glReadPixels`, which ends the scene and waits for the GPU) into
  the destination's own Vita texture, flipped to GX's top-first rows, halved where the destination
  is half the source (the blur's copies), and converted as the copy format reads (opaque colour,
  intensity in all four channels, one channel, alpha only with an alpha pixel format). A material
  sampling the destination takes that texture; a depth copy is not made and keeps the transparent
  black stand-in. The copy's order among the draws keeps each draw sampling the copy made before it,
  the same texture filled again at each copy. This should give the pause screen's and the map's
  blur, the reflections (`CacheReflection`), the morph ball's shadow and the other copies their
  pictures. Each copy costs a scene break and a GPU wait: the presents line gives the copies, their
  pixels and their time, and the draw path line a `copies` section.

Its run (a303747): the borders black; the pause screen's and the map's backdrop flat grey, not the
blurred scene; and a crash, `std::bad_alloc` after the heap went from 129 to 178 MiB in use in four
seconds, on a screen of 27 immediate draws making 1.7 new palette textures a frame (as the death
screen two builds before).
- The copies: about one a frame in play, 7 to 22 ms each (a few thousand pixels: the cost is the
  scene's end and the wait for the GPU, not the read). Off till milestone 5 (`kScreenCopies` in
  vita_draw.cpp): the black stand-in again. A copy made on the GPU (a blit into a texture's
  framebuffer, no wait) is the next try, and the grey backdrop to look into.
- The leak: Aurora's content cache counted a texture by the null device's texture (a palette
  texture: a byte a texel), while the Vita's copy is RGBA8, four times that: the 32 MiB budget held
  over 100 MiB of the Vita's. And a cached material keeps its textures until its window ends (300
  frames) or its set replaces it: about 20 s of a new texture each frame. The GPU's memory full,
  vitaGL took the heap's. Now the content cache counts the Vita copy's bytes
  (`texture_handle_size`), and every 16 frames the materials holding a texture nothing else holds
  (no cache, no frame in flight) are freed (`drop_lone_textures`; the materials line counts them).

Its run (fd79801): the crash again on the continue screen (after "Game Over", the screen black till
yes or no): `std::bad_alloc` in `make_vita_texture`, the GPU holding 157 RGBA8 textures (93 MiB)
with 11 MiB of VRAM free, the heap 131 to 181 MiB in use. The sweep freed 90 materials, so the
textures were held elsewhere: Aurora's texture object cache keeps each object's texture 600 frames
after its last use, and that screen inits new texture objects with a new palette texture each
frame (some 1 MiB a frame of Vita copies). Now on the Vita an object whose texture nothing else
holds (the content cache let it go, no slot binds it) goes after four idle frames
(`sweep_object_caches`).

Its run (62f77ec): the crash again on the continue screen ("An error has occurred", C2-12828-1):
a data abort in `convert_texture` (an allocation that failed), 146 RGBA8 textures on the GPU
(84 MiB) against 22.7 MiB before the death, 12 MiB of VRAM free and the heap filling after it. The
material sweep freed 111 materials. Not found by reading: what holds them.
- One holder fixed blind: the four-idle-frames rule asked that nothing but the object hold the
  texture, but objects inited with the same content share one, each counting against the others.
  Now only references from outside the object cache keep it (every fourth frame).
- For the next run: a `caches:` line (each 300 frames, with the materials line) gives the content
  cache's and the object cache's entries, distinct textures and their Vita copies' MiB, and the
  palette caches; the vitaGL memory line adds the textures made and not yet uploaded (pixels on
  the heap).

Its run (349fe7e): the same crash. The caches line found it: on the continue screen Aurora's
caches held 55 textures (32 MiB, the budget), the object cache none, while the GPU held 214 RGBA8
(125 MiB). The rest were the materials': the sweep freed a material only when it alone held its
texture, but one palette texture is in several materials (one picture with several blends and
fades), each counting against the others. Now the count is over all the materials (a freed
material's references leave it). The materials line gives the textures the materials hold, and
of them those only materials hold.

Its run (d993a29; 193 s): no crash. On the continue screen the GPU held 57 RGBA8 textures (32 MiB,
the content cache's budget), the materials 3 (none held by them alone), the heap steady at 126 MiB.
Once its palette textures were cached the continue screen ran at about 50 FPS; the slowness the
user saw was the death itself and play before it (10 to 20 FPS, a second at 3), the general speed.

- The VPK has a LiveArea, in `platform/vita/sce_sys`: the user's own (from a VPK they made), style
  `ad0` with no gate, the logo a transparent live item in the bottom left over Samus, the emblem
  for the icon. (An installed app keeps the LiveArea it was installed with: a new one shows after
  the bubble is deleted and the VPK installed again; the saves are in `ux0:data/MetroidPrime`.)

## The climb from the save point

1cc1616 did not work for the user, while 4445f24 did: the code went back to it (c4e947a, tag
`savepoint`, with the user's LiveArea of 62f77ec), everything after it kept on the branch
`before-rollback`, to bring back a step at a time with the map screen tried at each.
- Step 1 (521e748): the memory fixes (349fe7e, d993a29). The user: it works. Its run was a new game
  on the frigate, though: the save point and step 1 boot fast (`MP_FAST_BOOT`), into a new game.
- Step 2 (dc2e47c, b6d597c), asked for ahead of the order: the front end again, to select a save
  (05d9f7c), no vitaGL splash screen (its wrap of `invoke_splashscreen`), the app named Metroid
  Prime (4126db4's name), the menu movie's decode with NEON (3433fc7). The user, on their own save:
  the pause menu has the old problem. On this base the screen copies are off (`kScreenCopies`): the
  blur samples the transparent stand-in and draws nothing, the backdrop is whatever the frame was
  cleared to, the light quarters multiplied over it.
- Step 3 (95a8ebb, adc4d48): the pause screens' backdrop an opaque black quad on the Vita (1cc1616's
  `CCameraBlurPass::Draw` change alone). The user: still a problem.
- Back to step 1 as the base (the user's call): steps 2 and 3 reverted, kept on the branch
  `climb-step3`. Open: what the pause menu's problem is on the user's save (the backdrop, or the map
  screen crashing), and whether step 1 has it there too; booting fast, it has only run new games.
- From step 1, the newer fixes as never done (the user), the menu asked for again and done afresh
  (5085bbc, 4a55340): the front end (no `MP_FAST_BOOT`), vitaGL's splash screen not started
  (`vglInit` calls an empty `__wrap_invoke_splashscreen`), the VPK named Metroid Prime (param.sfo's
  TITLE and STITLE), and the menu movie's decode with NEON (`inverse_dct_neon`, flat blocks written
  whole), I8 tile rows converted eight texels at once. Tested again against this base's own
  decoder: the PC path the same byte for byte; the NEON path, its intrinsics emulated, all but 3 of
  2.3 million bytes, those by 1; the I8 conversion's own text, both paths, 452,952 texels over whole
  and partial tiles and mipmaps, none wrong. Compiled for the Vita, the per-block decode has no
  double-precision operation. These are step 2's four again, which on the user's save showed the
  pause menu's problem: this build's logs on that save are to tell what it is.

## Tallon Overworld's frame rate (from step 1)

The menu build's run (2dc9d4e's code; the user's save, then a new game): the title at 20 to 26
frames a second, the file select 12 to 18 (its movie's decode 12 to 28 ms a frame); no load failed,
no memory ran short, no crash; the pause and map screens opened (about 20 and 24 frames a second).
The user: Tallon Overworld seems too taxing, things perhaps not loading in time; the frigate normal.
Tallon's Landing Site ran at 7 to 10 frames a second, a frame 100 to 130 ms:
- The FIFO thread was the limit: busy 107 ms of a 113 ms frame, the main thread waiting 38 ms a
  frame for it (`begin-wait`). Of its 107: skinned list calls 50 (40 a frame, each its template's
  1074 vertices or so, 2.4 MiB a frame), immediate draws 25 (about 590 a frame, 13,000 vertices:
  the HUD's 446 small ones and 60 of the world's with 10,000 vertices), recording 9, state 6, and
  the rest under 4 each.
- The main thread: the ticks 34 ms (5.5 a frame, catching up), drawing 22, input 3 to 4 (the pad
  read each tick, 0.76 ms).
- Effects that read the screen are left out on this base (screen copies off): 112 to 276 draws a
  window in Tallon, 96 on the frigate. Programs read from the card at their first draw (650 ms for
  40) draw fixed-function meanwhile.
- A skinned call copied its template and read each of its triangles' vertices again, a component
  at a time (1.2 us a vertex): a position or normal index is in several triangles. Now a template
  keeps its distinct position and normal index pairs and each vertex's among them; a call reads each
  pair once, checks the arrays once against the largest indices, and copies the posed pairs to the
  vertices. Components are read a vector at a time (`read_components`, the type and the 1 / 2^frac
  scale worked out once), and `pack_normal` rounds as `std::lround` without its library call.
  Against the code they replace, on the PC: 960,000 components over every type, fraction and byte
  order, 2 million normals and the rounding's edges, all the same.
- An immediate draw allocated its vertices, then its triangles or lines a push at a time (a line
  list of hundreds doubling over and over). Now the vertices go in the FIFO thread's room for them
  and the triangles and lines are sized at once (a list's triangles grow by doubling).

## Metroid Prime Demo (from 41226a3)

Asked for alongside the Tallon work: a separate app that stops with the frigate, so it never
replaces the build being worked on. One build makes both packages (docs/VITA.md, "Metroid Prime
Demo"): the same `eboot.bin` as Metroid Prime Demo, `MPPD00001`, which `vita_platform.cpp` tells
apart by the title ID it runs under.
- The menu skip again, for the demo only (`MP_FAST_BOOT`): a new game on the frigate, so no save
  can be loaded (and the frigate has no save station).
- The frigate's way out (`CScriptWorldTeleporter`, `kSM_SetToZero` to another world) quits as the
  pause screen's Quit Game does, and the reset and the front end start a new game on the frigate.
  Dying does the same.
- No port menu: Select + Start is Start, and the hint about the menu is not shown.
- Saves, settings, logs and the shader cache in `ux0:data/MetroidPrimeDemo`; the disc read from
  `ux0:data/MetroidPrime`. A DEMO banner on its icon and LiveArea logo.
- First test (db063f1): the menu skip worked, but the demo still went on to Tallon Overworld,
  with no `[demo]` line in its log. Its check asked whether the target was a world before
  `SetCurrentWorldId` had readied the target's PAK: as on the GameCube, every world PAK but the
  current one's is stashed (`CMain::EnsureWorldPakReady`), so the answer was no, and the game's
  own check, made after that call, went ahead. The demo now decides inside the game's check and
  makes the frigate the current world again before it quits.
- Second test (8b00110): the demo loops. The user: a micro stutter every second, in the demo
  and the main build alike.

## The stutter every second

Once a second the log takes the report of `MP_TRACE_TIMING` and `MP_PROFILE`, some 70 lines
(about 8 KiB): `[timing]`, `[split]`, 30 or more `[zones]`, `[gxsrc]`, `[heap]`, `[ops]`, and
vitaGL's `[fifo path]` and `[draw path]`. The log's streams were line-buffered onto the file and
`PortLog::Write` flushed each message, so each line was a write of its own to the card, made on
the thread that logged it: the game's main thread for most, the GX FIFO thread for the rest. A
card access takes milliseconds (programs read from the card took about 10 ms each), so dozens of
them held up a frame every second. The streams now hand each line to a 128 KiB ring
(`QueueWrite` in place of newlib's `FILE::_write`), and a thread of its own writes the ring out
as it fills; a logging thread copies the line and goes on. On the PC, the ring's own text: 4
threads writing 80,000 lines of up to 2 KB as newlib would, 82 MB through it (625 wraps) to a
slow "card", and one write three times its size: every line whole, each thread's in order, none
missing; ThreadSanitizer found no race. Its mutex and condition variables are never destroyed:
destroyed at exit with the writer still waiting, glibc's condition variable waited forever. The
streamed music was not it: disc reads go through Aurora's DVD worker thread. Not changed: the quick-resume snapshot, taken on
the main thread 2 seconds into a room and every 30 seconds in it (`kResumeStableSeconds`,
`kResumeRefreshSeconds`) and written to the card at once: 10 to 50 ms to take
(`[resume] snapshot`), then the write.

## The escape: the water and the crowds (from fc76049)

The user: the stutter gone in both apps; in the frigate's escape the flooded rooms' water black
(a photo: the floor of a long hall flat black), and slowdowns with many enemies and explosions, the
parasites' swarms in the escape above all. Not wanted: a lower resolution.

The demo's run through the escape (917 seconds of log), by room: most of the frigate 20 to 40
frames a second; the escape 10 to 26, the low points three kinds of limit:
- The FIFO thread, in Subventilation Shafts A and B (about 10 frames a second, its frame 100 of
  105 ms busy) and the Exterior Docking Hangar: its `skinned` section 57 to 75 ms a frame. Each
  parasite's surface is a skinned list call: 118 to 137 a frame of about 1100 triangle corners,
  130,000 to 150,000 corners, 7 to 8 MiB of vertices a frame (`[draw uploads]`), at about 0.45 us
  a corner: copying its template's corners, posing their positions and normals, and the render
  worker's copy of them all again. Templates were made rarely (about 100 in 300 frames): the cost
  is the corners.
- The GPU, in Main Ventilation Shaft F (8 to 10 frames a second): 240 to 330 particle systems, 6000
  particles; the render worker's swap waits 78 ms of its 99 (`[draw path]`), the game 57 ms a frame
  at `begin-wait`; 3.6 ticks a frame catching up, nearly all particle updates (38 ms a frame).
- The main thread, in Biotech Research Area 1's explosions: a tick about 25 ms (550 particle
  system steps and 200 new particles a frame).

The water was black because no program could draw it. `CFluidPlaneCPU::RenderSetup` sets six or
seven textures (lightmaps, two patterns, the colour layer, the environment map and its bump map),
coordinates made from the position and the normal (the bump map's normalised through a post
matrix), and an indirect stage: the bump map offsets the colour layer's coordinates, or the
environment map's. `tev_layout` refused any indirect stage and more than four textures or
coordinate sets, so the water drew fixed-function: its textures times the vertex colour, which
channel 0 makes black (lit, no lights, a black ambient). The rolled-back branch had drawn it
(c5af261: 33 indirect programs, none failed on hardware); now on this base:
- Indirect stages in the generated programs as c5af261 has them (and Aurora's shaders): each
  indirect stage sampled once (its coordinates in texels by GX's SU scale, scaled by `GX_ITS_n`,
  the texel's a, b and g as S, T and U), a stage's TEV coordinate (the format's bits, the bias,
  the static or dynamic matrix to a texel offset; wrapped, or added to the last stage's), the bump
  alpha as a raster colour; constants for the coordinate slots' texel scales, the textures' sizes
  and the three matrices. Without fog here (c5af261 came after fog), the constants follow the
  alpha reference (slot 13 on).
- 8 textures and 7 coordinate slots a program (`kSamplers`, `kProgramCoords`): a set from a vertex
  attribute stays in the mesh's first four slots, those past them are made from the position or
  normal. The lit channels' varyings go past the slots, at 5 and 6 still with four slots or fewer,
  so the programs made before keep their sources and their binaries on the card.
- GXM's ring buffers larger before `vglInit` (cd75c62's: fragment 4 times, VDM 4, vertex 2,
  fragment USSE 4): c5af261's water put 1100 to 4300 program draws of up to seven textures in a
  scene, and a run ended in a GPU hang until they grew.
- A log line each 300 frames: the materials worked out that read an indirect stage, those left
  fixed-function and why, the indirect programs; the first two indirect programs' sources.

The crowds: a skinned call with a program now draws by index. A template keeps its vertices made
unique for a program (the same position and normal pair, coordinates and vertex colour: a program
reads no face's shading) and its corners as 16-bit indices into them, made once and shared by
every call's mesh; a call poses and copies its unique vertices only, and the render worker draws
them with `glDrawElements` (vitaGL copies client arrays up to the largest index). A call without a
program (its program still compiling, or none) lays out every corner as before. On the PC, the
function's own text over random templates: 644,202 corners as 238,438 vertices, each corner's
vertex its own, alike corners sharing one, the indexed and per-corner paths the same in position,
normal, coordinates and colour; past 65,536 vertices no order. The `skinned calls:` line adds the
calls drawn by index and their corners against their vertices: the ratio on the parasites is the
gain. Left for a later look: the particles' update (the GPU and the main thread in the swarm).

Its runs (b154f27; both apps at the frigate's start): textures broken in both, differently. The
main app's helmet frame (its top band and bottom corners) was coloured noise in 4x4 blocks; the
demo's gunship and Samus's cutscene model had black textures; a square of noise around the arm
cannon (an effect), and smaller patches elsewhere. Not the programs: the demo's logs match
fc76049's window by window (1, 5, 17, 18, 19 and 23 programs, every draw with its program, the
same draws by unit count), and no indirect material was worked out in either app. The difference
was the skinned calls, drawn by index (`300 drawn by index` a window in the cutscene).
- vitaGL copies a draw's client-memory arrays and indices into its per-frame pool
  (`vgl_reserve_data_pool`), which moves its pointer on by each size, unaligned. Before b154f27
  everything put there during a frame's draws was 56-byte vertices (the legacy pool 256 KiB), so
  it stayed 8-byte aligned. `glDrawElements` added the indices, 6 bytes a triangle: off by 2, 4 or 6
  bytes after any call whose triangles were not a multiple of 4.
- A BC1 texture's upload (`gpu_alloc_compressed_texture`, at its first draw) is staged in that
  pool and read by `sceGxmTransferCopy` as 8-byte blocks into the swizzled texture: from an
  address off by 2 its colours and indices mix (noise), or nothing lands (black). RGBA8 textures
  are written by the CPU, so only the BC1 ones broke, and only those first drawn after such a
  call that frame: which ones is a matter of timing, hence the two apps' different damage. The
  textures stay so for the session.
- Now the indices a template shares are padded to a multiple of 12 with triangles of one vertex
  (`vita_gl::pad_triangle_indices`), so every copy is whole 8-byte blocks; and the draw is
  `glDrawRangeElements` with the vertex count, so vitaGL no longer looks through the indices for
  the largest at each draw.

Its runs (56ec84c): the demo confirmed right; the main app's run through Tallon Overworld (about
13,000 lines of log) with no graphical fault. By window, three costs:
- Freezes: windows at 0.6 to 2 frames a second, the render worker 0.5 to 1.6 s a frame. They were
  compiles: 16 new programs, 26.8 s in all, 1.2 to 2.2 s each (`8925 ms` for 4 at the frigate's
  start before), the render worker waiting each out on its compile stack.
- The FIFO thread, 50 to 85 ms a frame in the heavy rooms (12 to 17 frames a second): immediate
  draws 22 to 34 ms (about 600 a frame, 20,000 to 24,000 vertices, most of them `DrawWorld`'s:
  the water), their recording 7 to 15 ms.
- The main thread: the game's ticks 25 to 42 ms a frame (3 to 4 ticks, each `SM: Think (all
  entities)` 4 to 6 ms and the particles' update 3 to 4), drawing 13 to 22 ms, input about 4 ms.
  The map screen ran at 6 frames a second: 5,800 draws a frame, nearly all immediate.

Now:
- Compiles left the render worker. A program not on the card goes to a compiler thread at a low
  priority (`compile_jobs`, started with the first job and ended when none is left, with the
  2 MiB stack SceShaccCg needs), which calls vitashark for its two GXPs; the render worker links
  them (`glShaderBinary` with vitaGL's own form of a shader, the matrix uniforms' count, none,
  ahead of the GXP) once done, from the same budget as the card's loads (8 a frame), and saves
  the program to the card as before. Its draws are fixed-function meanwhile. vitashark keeps one
  compile in its statics: the linker wraps `shark_compile_shader_extended` and
  `shark_clear_output` with a lock, so the compiler thread's compiles and vitaGL's own (a
  fixed-function shader its cache lacks, on the render worker) never overlap. The direct calls of
  685daf0 and a48830e failed because they passed a source length of 0: vitashark reads the length
  from `size` (`shark_input.size = *size`). The presents line counts the programs sent to the
  compiler thread, and the compile time is that thread's.
- An immediate draw with a program draws its vertices as decoded, by index: the indices of each
  primitive and vertex count (`triangle_indices`, padded as above) shared by every such draw, so
  no vertex is copied to each triangle's corners and no face is shaded (a program reads neither).
  Without a program it is triangulated and shaded as before.
- A primitive's vertices decode by a plan made once for them (`plan_vertices`: the descriptor's
  attributes present, their sizes, sources and coordinate sets) instead of going through all of
  GX's 21 attributes for each vertex. On the PC, the old decode (git's HEAD) against the new over
  200,000 random descriptors, formats, arrays and streams: the same vertices to the byte (115,383
  of them), the same indices and the same failures.

Its run (7dfaad5; Tallon Overworld, about 25,000 lines of log): 40 programs went to the compiler
thread, none failed, 68.6 s of compiling there, and no window fell below 3.5 frames a second (the
0.6 to 2 of the compiles before). In the room held longest (about a minute at 18.5 frames a
second, 1,070 draws a frame) the FIFO thread's immediate draws took 12 ms a frame where a like room
took 20 before; the FIFO thread was 62 ms busy (skinned calls 11, immediate 12, recording 10, GX's
state 7, Aurora's commands 6.5), the main thread as much (ticks 22, the game's draw 21, input 3).
- Left: about 40 stalls of 250 to 850 ms, at room changes. The timed presents put them in the
  render worker's builds (40 to 844 ms), before any compile was sent too: the program cache's card
  I/O, a file's existence asked at each program's first draw, its binary read (up to 8 a frame),
  a compiled one's written, each waiting behind the game streaming the next room from the disc
  image on the same card.
- Now the card's programs are listed once at the start (`index_card`: 200 or so names), and a
  program's first draw asks that list. Reads, writes and removals go to a thread of their own
  (`card_thread`, a `JobThread` as the compiler's now is: started with the first job, ended with
  the last); the render worker links what they read (`link_loaded`, `glProgramBinary`) or hands
  them a compiled program's binary (`program_binary`). A binary that does not link is removed and
  compiled at once rather than next session. Links are 4 a frame at most. The presents line adds
  the binaries compiled again.

Its run (cdeac54; Tallon Overworld, about 16,000 lines of log): 1,103 programs on the card at the
start, one stall left (the game's load, 1 s), none at room changes. 148 programs loaded from the
card (about 50 ms each, on the card thread), 4 sent to the compiler thread, 2 of them linked (21.9 s
of compiling, 11 s each). The heavy rooms ran at 15 to 20 frames a second, the lighter 25 to 42,
the map screen at 6 to 7.
- The slowest windows, 8 to 12 frames a second, were the compiles: about 37 windows in four
  stretches, each as long as a compile. In them the FIFO thread's `mesh` section (one `make_shared` a draw)
  went from about 2 to 17-39 ms a frame and `immediate` (a vector a draw) from about 10 to 24-47;
  GX's state, the lookups, the recording and the rest held. Input on the main thread doubled as
  well (its controls profile, rebuilt each frame with vectors). vitaGL hands vitashark `vglMalloc`
  (`start_shader_compiler`), which tries newlib's `malloc` first: SceShaccCg's allocations took the
  heap lock every thread's take, from the compiler thread at a low priority, often preempted
  while holding it.
- Now SceShaccCg allocates from a heap of its own (`use_compiler_heap`: 8 MiB carved once from
  newlib's heap, a `sceClibMspace` over it, given to `sceShaccCgSetDefaultAllocator` after
  `vglInit`); one compile runs at a time (the vitashark lock). What does not fit goes to
  `vglMalloc` as before, and a free goes back by its address. The presents line adds the heap's
  most in use at once and the allocations past it.
- The frame's own meshes: an immediate draw with a program (triangles by index, and GX's lines:
  the rain, 64 draws and about 10,000 vertices a frame), a program's line strip and a skinned call
  with a program decode or pose their vertices into the recording frame's own array
  (`vita_gl::frame_vertices`) and copy their indices to its own (`frame_indices`), and the draw
  carries a `FrameMesh` (where they are) instead of a `Mesh`. No `make_shared` and vector a draw
  on the FIFO thread, nor their frees on the reaper's thread (500 to 1,100 draws a frame, all
  through newlib's lock); the arrays grow by doubling, are not cleared first, and start each frame
  at the last one's size. The render worker points the arrays into the frame (vitaGL copies them
  into its pool at the draw, as it did a client-memory mesh) and draws by the frame's indices or in
  order. The rest (lists, draws without a program) keep their `Mesh`.

Milestone 4's parts the rollback to step 1 took, asked for again and done afresh on this base
(after the designs of 9ee73b0, 4126db4 and ef82f19):
- Fog. The generated programs fog as GX does and Aurora's shaders have it (shader.cpp): the vertex
  program passes clip z and w (`vFog`, in the TEXCOORD varying the lit sums leave free past the
  coordinate slots: 4 with four slots or fewer, else the one after the last slot), and the fragment
  program takes GL's window depth from them (the viewport's depth range is in the projection, so it
  is the EFB's z): perspective A / (B - z) or orthographic A * z, less C, clamped, through the
  type's curve, the colour (clamped, as GX's pixel engine takes it) mixed toward the fog's. Two
  more constants, last among the slots (the fog colour; A, B, C). The fog type is in the program's
  key and the material's pipeline key (its register, 0xF1, sets `DirtyPipeline`). A program
  without fog keeps its source, and its binary on the card; a fogged material needs one of its own,
  compiled in the background at its first draw and unfogged till then. Not done: GX's range
  adjustment (the game leaves it off), and fog on the fixed-function path.
- Lit programs by light counts rounded up to 1, 2, 4 or 8 (`program_lights`); exact counts made 17
  combinations and 257 lit programs before the rollback. The lights past the mask's are dark:
  black, far off, both attenuations (1, 0, 0), finite for every attenuation function (the all-zero
  padding, which the mask changing without the program used, divided zero by zero). A part with 3,
  5, 6 or 7 lights changes its program's source: compiled again once.
- Lines GX's line width wide. A line or point draw carries its width (`Draw::width`: GX's sixths
  of a framebuffer pixel scaled to the screen and rounded, 1 to 16 as GXM takes them), and the
  render worker sets `glLineWidth` and `glPointSize` where a line or point draw changes it (GXM's
  context keeps them, for the programs and the fixed-function path alike). GX points are drawn as
  GL points, fixed-function (vitaGL's point size reaches only its own programs); the game draws
  none. The immediate-draws line counts lines and points apart. Not done: GX's line and point
  texture offsets.

The programs' warm-up, and a thread log (asked for while 748e22a was tried):
- Each material state a program is made for is saved on the card (`vita_programs/materials.bin`)
  as the generator reads it (`MaterialRecord`: the TEV stages, swap tables, channels and their
  light masks, the alpha compare's operators, the indirect stages, the coordinate generators the
  stages name and their descriptor entries, the fog's type). What no stage reads is cleared, so
  materials alike save alike, once each; the card thread appends them a batch a job.
- The next session reads them at the start, and at each frame's end the FIFO thread makes their
  programs again with its own generator (a record swapped in for GX's state, then back), up to 2 ms
  a frame. Each one the card lacks goes to the render worker, which compiles it in the background,
  links it, saves its binary and drops it; the draw that needs it reads it from the card. One at a
  time: a draw's own compile waits behind one at most, and a draw whose program the warm-up is
  compiling waits for it (`g_compilingKeys`, shared by both, so neither compiles a program twice).
- So after a change to the generator (fog, the light counts), the programs of materials seen
  before compile from the title screen on rather than at their first draw; places never seen still
  compile at their first draw. A session's first run with it only saves. The log adds the records,
  the programs made again, those the card had and those sent, and the warm-up's builds.
- The thread log, each 300 presents (`[threads]`): each named thread's core (the one it is on, the
  one it last ran on, its affinity mask), priority, the share of the time it ran, and its moves
  between cores (`SceKernelThreadInfo`), to see where the compiler's time goes before placing it.

Precompiled programs (asked for next: compiled on the PC, not the Vita). The compiler is Sony's
own, the module vitashark loads (`ur0:data/libshacccg.suprx`, the user's), run on the PC by
open-shacccg's oracle (MIT; Unicorn's ARM emulation, the module's 25 imports answered by the host),
so the GXPs are the ones the Vita would make. `tools/vita_precompile`:
- `build.sh` builds `precompile_sources`: the generator itself (`vita_tev.cpp`) compiled for the PC
  from the Vita build's dependency trees. It reads a session's `materials.bin` and makes each
  record's program as the Vita would (`tev_record_program`, shared with the warm-up: the record
  swapped in for GX's state, the same sources and keys), writing the sources and a manifest.
  `--self-test` makes a textured, vertex-coloured, fogged material's record and prints its program.
- `precompile.py prepare` turns the module into the oracle's ELF and import maps (once);
  `precompile.py compile` compiles every program in the manifest as vitashark does for
  `compile_gxp` (the source named `<built-in>`, `main`, useFx, optimisation 3, fast math and
  integers, full precision, warnings off), a process a program across the PC's cores, into
  `precompiled.bin`: "VPK1", the count, a table sorted by key (key, each GXP's offset and size),
  then the GXPs.
- The Vita reads the pack's table from `vita_programs/precompiled.bin` at the start. A program the
  card lacks and the pack has takes its GXPs from there, read on the card thread, instead of the
  compiler (`submit_compile`); they are linked and the binary saved to the card as a compiled
  program's are. GXPs that do not read back or link are compiled after all. The warm-up leaves the
  pack's programs alone (linked at their first draw, in a frame or two). The presents line counts
  the programs taken from the pack; the start, the pack's size.
- It covers the materials a session has seen (its records): it pays after a change to the
  generator (every program the records make, compiled on the PC in minutes rather than on the Vita
  over hours) and on a card without the programs; a material never seen still compiles on the Vita.

The run that sent the module (873832b, the warm-up's build: tallon5 was installed, not tallon6):
three sessions, each ending in a crash about a minute in.
- Two, the last two, at the title screen: the render worker ran 0x78 bytes past its 32 KiB stack
  inside SceShaccCg. vitaGL compiles a fixed-function shader its own cache (`vitagl_shader_cache`)
  lacks inside the draw: `glDrawRangeElements`, `_glDrawElements_FixedFunctionIMPL`,
  `reload_ffp_shaders` (a frame of some 17 KiB), `shark_compile_shader_extended`. Now the vitashark
  wrapper runs a compile called with less than half the compiler thread's stack left on a thread
  with that stack (2 MiB), its caller waiting, and the log says each one and its time.
- One, the first, in play (the main thread in the particles' update): the FIFO thread aborted on an
  uncaught `std::bad_alloc`. The dump holds no stacks and none of the heap's state, and the next two
  sessions overwrote that session's log (the last held at 134-137 MiB in use of 224), so what
  failed is not known. Now a failed `operator new` writes its size, the thread, the heap (in use,
  free, taken from the system, of 224 MiB) and the code addresses on the stack to the log before
  it throws (each, less the `operator new` address the line gives plus its address in the ELF, is a
  return address in the ELF), and an uncaught exception's type and message reach the log
  (`std::set_terminate`): libstdc++'s own report went to stderr, whose lines a thread writes to
  the log, and the abort stopped that thread first. A crash report's lines now wait for it (a
  second at most). The `[threads]` line gives each thread's ID, as the dumps name them.

The pipeline with the user's module (`libshacccg.suprx`, SDK 3.0.0, shacc r13225) and card:
- The module is a fake-signed SELF (ShaRKF00D's); `prepare` unwraps it itself (VitaSDK has no
  `vita-unmake-fself`); its segments come relocated for the addresses their headers give. The
  oracle, made with a 1.6.5 module, lacked three of this one's imports: SceLibKernel's
  `__stack_chk_guard`, a variable (3,916 MOVW/MOVT references; the oracle resolves functions only),
  now pointed at a word of zeros in the oracle's trampoline page, and `__stack_chk_fail` and
  `sceKernelGetTLSAddr` (a slot of the oracle's TLS block), answered from `precompile.py`.
- The card's `materials.bin`: 678 records, 552 programs, whose 552 keys are the names of the card's
  552 binaries: the PC's generator makes the Vita's sources. Eight programs compiled and compared:
  all 16 GXPs byte for byte the card's (SceShaccCg's output runs 1 to 3 bytes of padding past a
  GXP's own size, to 4). A program takes 5 to 45 s under the emulation; each one's GXPs are kept
  (`<work>/gxp`), so a run cut short goes on, and a pack made again compiles only new programs.
- The pack of all 552: 27 minutes on 12 cores (11 processes), none failed, 1,119 KiB; every one
  of its 1,104 GXPs byte for byte the card's. The card has every one of these programs already, so
  on that card it changes nothing; it is ready for the next change to the generator, and a card
  without the binaries links them from it.

The port menu on the Vita (asked for after tallon7's run, b07dbe8: six minutes, no crash):
- Remastered. The page imports Metroid Prime Remastered's models from a Switch dump, with its
  gallery; nothing of it runs without the import, and the Vita could not draw the models anyway.
  It is gone there, with the Debug page's Remastered section and the search's rows for it. The
  crosshair's Remastered style stays (Video). Remastered's look on the PC, per-pixel lighting, the
  sun shadow and its projected actor shadows, bloom, volumetric fog and the reflection probe, never
  reached the Vita (its programs light per vertex; `MP_SHADOWS`, `ProjectedShadows` and
  vita_platform.cpp's settings keep the rest off), so there was no frame time to win; Video's
  checkboxes for the first three are gone there too.
- The search box (any text field). With no hardware keyboard SDL opens the system keyboard
  (SceImeDialog) when a field takes text, but a common dialog draws only when the swap asks for it
  (`vglSwapBuffers(GL_TRUE)`), and ours never did: the dialog stayed up unseen, the field waited for
  typing, and the menu could not be left. Now SDL's is off (`SDL_ENABLE_SCREEN_KEYBOARD=0`) and the
  overlay opens the keyboard itself. vita_gl opens, draws and closes it on the render worker, so
  all of SceImeDialog's calls are on the thread that swaps; the overlay (ui_common.cpp) asks for it
  when a field becomes active, with the field's text, keeps the pad and the touch screen from the
  menu while it is up and until its last button is let go, and on confirm types the text over the
  field's and presses Enter. Cancelled, the field closes as it was; a keyboard that cannot open
  closes the field at once.
- What the Vita has no use for, asked to go next. Key hints: the overlay's hints list the bindings
  of their actions (`BindingsText`), and the PC's keys are bound on the Vita too, so the header
  read "F1 / Back + Start: hide   F10: frame limit   F12: screenshot" and the buttons "Screenshot
  (F12)" and "Photo mode (F8)". There the hints list the pad's bindings alone, under the Vita's
  names (Cross, Circle, Square, Triangle, Select, L, R; the Remap page too): "Select + Start:
  hide". Video's Fullscreen (F11) goes, and its launch hint is "Show the menu hint at launch".
  Sections: System's Memory card (imports and exports through a file browser), Disc image (its new
  image picked the same way; its check went with it) and Updates (a web page); Video's
  Compatibility tab (GPU drivers and renderers); Game's Flaahgra boss theme and PAL languages
  (both import a disc image picked with a file browser); and the Archipelago page. The search
  leaves their rows out (`HiddenOnVita`).
- Then (tallon9's run, 6f2a5a3: 31 minutes, no crash): the Screenshot button (the Vita takes its
  own), Open mods folder (it opened the Vita's web browser), the settings search, and the keyboard
  and mouse: Remap lists the controller's bindings alone (the keyboard's stay in the profile,
  unseen) and its Reset leaves out the keyboard presets; Controls' Mouse settings page is gone,
  its crosshair and size (which twin stick uses) moved to Controller settings.

The music repeating and overlapping (the same run: the frigate's music, a stream). The music
streams (CDSPStream: each channel its own, `int_introstageL`/`R`) play from a 4 s ring buffer read
2 s at a time; each half must be read again within 2 s of its refill being asked for. The
GameCube's drive serves its queue by priority, and the refills ask for 1 where every other read the
game makes asks for 2, so they never wait behind the room's loads. Aurora's DVD worker took no
notice of priority (`(void)prio`) and read in arrival order: from a disc image on the Vita's memory
card, a refill queued behind a room's loads could miss its 2 s, and the channel played its old half
again (the music repeating), then went on 2 s behind the other channel (the two overlapping). The
worker now queues by priority as the drive does (`DvdWorker::enqueue`): a refill waits for the
one read under way at most.

Still there in tallon10's run (3d02fff, 9 minutes; "likely due to low frame rate"). The frame rate
fell to 8-17 FPS while rooms loaded, with the CPU far from full (the main thread 65% busy, waiting
on reads). A refill could still miss its 2 s two ways: the read under way when it came may be a
long one (the queue's order only decides what goes next), and the worker ran at the lowest
priority (191, below the game's 160), so with the game's threads busy it waited for a core, and
its completions take the MusyX lock the mixer (112) waits on. Now a read goes 256 KB at a time
(`kReadPiece`), and a more urgent command waiting runs between two pieces, the rest of the read
put back ahead of its own priority's others; on the Vita the worker runs at High (112). The log
says when a refill still comes late (`mpstream: refill late`, once a refill). Not done: bigger
stream buffers, which the game's emulated ARAM (its samples' room) would have to make room for.

Cutscenes with Skippable cutscenes off (the same run): some were skipped all the same. The game's
own rule lets Start skip a cinematic the memory card has seen played before, and the port added A,
so the fire button skipped cutscenes seen in earlier sessions. Now off means none can be skipped
(`CMFGame`); on, Start or A skips any; Original experience keeps the game's rule, Start alone.

The shader compilation toast (Video > Quality, "Show shader compilation") never showed on the
Vita: it counts the PC's startup pipeline queue, which stays empty there. The Vita compiles a
program at a material's first draw, all through play: vita_gl counts the programs sent to the
compiler thread and those it has finished (`aurora_get_program_compiles`), and the toast shows
while any are waiting or compiling, half a second on.

The settings lost between two runs (port_settings.ini was in the folder at tallon9's start, gone at
tallon10's, its choices back to their defaults). A save writes port_settings.ini.tmp and renames it
over the file, and VitaSDK's rename removes the target before it renames (`_rename_r`:
`sceIoRemove`, then `sceIoRename`, which will not replace a file). A rename that failed after the
removal went on to delete the temporary file too, and nothing was left. Now a failed rename keeps
the temporary file, and the next start takes it back. Aurora's own atomic writer (the GCI saves,
the pad configuration) had the same end on the Vita: there the temporary file is kept, and logged,
when the target is already gone.

tallon11's run (0ee747a, 29 minutes): "some scenes don't render properly, and the pause menu".
- The scenes and the pause menu are the screen copies, still off (`kScreenCopies`): 1,683 draws a
  window took the black stand-in for an EFB copy's texture, 195 to 214 were left out for a texture
  without a Vita copy, and the 37 or 38 materials with an indirect stage stayed fixed-function for
  the same reason. That is milestone 5's: copies made on the GPU.
- Three refills came late in the 29 minutes (`mpstream: refill late`, the frigate's music), where
  before the music repeated often.
- The crash: `std::bad_alloc` on the FIFO thread again, now with its report. `operator new` of
  5,251,008 bytes from `frame_vertices` (the frame's own vertices, for `draw_immediate`), the heap at
  165 MiB in use and 55 MiB free in pieces, with 3.7 MiB left to take from the system's 224: no free
  piece of 5 MB. The frame arrays grew by doubling, each frame's allocated afresh at the last one's
  size and freed a frame later on the reaper's thread, and such pieces coming and going broke the
  heap up. Now an array's storage, when its frame lets go of it, is kept for the frames after
  (`SpareStorage`: the two largest pieces kept, the smallest that fits taken), so the frames reuse
  the same few blocks.

The scenes and the pause menu were the Phazon Suit (found by the tester after tallon12), not the
copies being off as said above. The suit's effect (`CCubeRenderer::DrawPhazonSuitIndirectEffect`:
for the pause screen's Samus model, `CSamusDoll`; the cutscenes' Samus, `CScriptPlayerActor`; and
the Phazon Beam) draws Samus's shape into the EFB's alpha, blurs it there and copies it with
`CopyTex`, which masks the colour off (`GXSetColorUpdate(false)`) for each copy and its clear: the
first copy takes the whole viewport, and its clear is of the alpha alone. vitaGL's `glClear` writes
all four channels whatever `glColorMask` says (its clear program is patched once at start, with no
blend state and so no mask), so that clear blacked out everything drawn before it: the world
behind a cutscene's Samus, the pause screen's backdrop and model. Only CopyTex clears so; the
others turn the colour on first. Now a clear of the colour alone or the alpha alone is a quad
drawn over the rectangle with the mask, at the clear's depth where depth is cleared too
(`record_masked_clear`); the log counts them among the clears. The suit's glow itself still waits:
it distorts a copy of the screen through its blurred alpha mask, so it needs the copies
(milestone 5) and `GXSetDstAlpha`'s constant alpha (Samus written as 255, the rest 0), which the
Vita path does not do yet.

## Milestone 5: the copies on the GPU (tallon14)

The EFB copies are made again, now on the GPU with nothing waiting for it. Before (a303747 and
on) each was a `glReadPixels` (7 to 22 ms: it ends vitaGL's scene and waits for the GPU) and a
CPU conversion, and they were turned off (`kScreenCopies`): the pause screen's backdrop, the
particles' heat haze and the other effects that sample the screen drew black, or were left out.

- A copy is a draw (`copy_on_gpu`). The destination's texture is attached to a framebuffer of its
  own, and a quad is drawn into it through the copy program, which samples the display's back
  buffer: a GL texture whose GXM texture is set, at each copy, to that buffer's own `backgroundTex`
  (the texture GXM loads the buffer with at a scene's start; vitaGL's blit samples the display the
  same way). The program puts the texel through a colour matrix by format (`copy_rows`): the colour
  (alpha one for RGB565), RGBA, GX's YUV luma for the intensity formats (16 to 235, in all four
  channels as Aurora samples I8), or one channel in all four. Where the EFB has no alpha (the
  game's pixel format but for the Phazon Suit's and the thermal visor's frames) the alpha reads as
  one, as on the GameCube. A halved copy samples each 2x2 block's middle with linear filtering:
  GX's box filter. The first row of the texture is the rectangle's top, as GX copies.
- The copy program is a generated program like the materials' (compiled on the compiler thread,
  then read from the card: `vita_programs`), built from the first frames on, so it is ready by the
  first copy that matters: the pause screen copies the world once, when the game pauses. Copies
  before it is built are left unmade (the log counts them).
- A destination keeps a texture for each of up to three sizes (`Texture::target`; the spare
  buffer takes the particles' copy of the whole viewport and the beam change's quarter, frame
  after frame), each with its framebuffer, to which vitaGL gives a depth buffer and a render
  target at its first scene.
- Each copy ends the display's scene, and the next draw starts another. GXM starts a scene with
  the depth at its background value unless told to load it, and vitaGL tells it neither to store
  nor load (only its STORE_DEPTH_STENCIL build does, for every scene): the draws after a copy (the
  particles behind a wall) would have tested against nothing. A frame with copies now stores the
  depth and stencil at the end of each scene a copy ends, and the next scene loads them
  (`keep_depth`, on vitaGL's `gxm_depth_stencil_surface`); a frame without copies does neither. The
  scissor's mask bit lives in that surface and is loaded with it; this vitaGL (VitaSDK's, before
  upstream's 1199cca) clears it again at a scene's first scissor update, so it does not leak. A
  vitaGL that skips that clear would need the mask cleared after the load.
- Render targets: GXM sizes a target's memory by the scenes it expects a frame, one by default,
  past which a scene waits for the GPU to finish one. The display's is now made for 8 (a frame's
  copies and one), a copy framebuffer's for 4 (`vglSetupRenderTargetScenesNum`).
- Not made (their materials keep the transparent black stand-in, as before): depth copies, and
  the alpha copies (`GX_CTF_A8`: the morph ball shadow's IDs, the Phazon Suit's mask, the fog
  volumes), whose alpha the game writes with `GXSetDstAlpha`'s constant, which the Vita path does
  not do yet. A copy repeating the last one into its destination in a frame (the same rectangle
  and format: the particles copy the viewport for every system they draw) is left to that one,
  and a frame makes seven at most: each costs a scene's end and start, with the depth stored and
  loaded.
- The fixed-function path took no texture without a Vita copy, a copy's destination among them:
  a draw sampling a copy was left out unless its program was ready. It now takes the copy's
  texture as a program does.

What to look for: the pause screen's blurred backdrop; the heat haze and the other particles that
distort the screen behind them (their indirect programs are built for the first time, so expect
compiles); the beam change's wipe, the scan visor's zoom, reflections, the X-ray visor's blur. The
thermal visor reads a copy through a palette (C4), which the Vita path does not do: its hot pass
shows the copy's intensity instead. The log's `EFB copies made on the GPU` (presents line) and
`EFB copies` (textures line) count them, and a frame rate drop in rooms with many copies would show
their cost.

tallon14's run (415b9eb): the pause screen with the Phazon Suit drew whole, its blurred backdrop
included (the tester's photo). The copies held: 300 to 870 a 300-frame window (three a frame at
most), 0.2 to 0.3 ms each on the render worker where a readback took 7 to 22 ms; none repeated or
past the limit; the Phazon Suit's frames' alpha copies left unmade, as meant; 20 indirect programs
built, none failed. The frame rate over the last 40 s averaged 24 FPS (15 to 32).

It ended in `std::bad_alloc` on the FIFO thread when shaders began compiling as the tester walked
and shot enemies (new effects, new programs): a frame's vertices (`FrameArray::reserve`,
2,577,680 bytes, for `draw_immediate`) with the heap at 197 MiB in use, 25 MiB free in pieces and
2 MiB left to take from the system. The heap had held at 162 to 167 MiB all run; it took 31 MiB
more in its last second. The frame reaper ran at the lowest priority (191, a thread's default), as
the program compiler does, and a busy thread does not give its core to another of its priority:
while a compile ran (a second or so each), the reaper waited, the frames it had not destroyed kept
their arrays, none came back to the spares (`SpareStorage`), and each new frame took its vertices
(megabytes) from the heap afresh. Now the reaper runs at Normal (160), above the compiler, and a
frame it is two behind on is destroyed by the render worker instead (`FrameReaper::reap`; the
presents line counts them). tallon11's crash (0ee747a, the same allocation) was likely this too.

tallon15's run (c478a51, about 18 minutes): no crash; the heap between 130 and 178 MiB; the
reaper never two frames behind (the priority alone did it); 19,713 copies, 24 FPS on average, 205
indirect programs built and none failed. The alpha copies left unmade came to 8,771, in 37 of the
89 log windows: the main gap left. No depth copy was asked for in any run.

## GXSetDstAlpha and the alpha copies (tallon16)

The game draws with an alpha channel in some frames (GX's RGBA6 pixel format: the Phazon Suit's
effect, the thermal visor, the morph ball's shadow, the fog volumes, the elevator transitions) and
there sets the alpha it writes with `GXSetDstAlpha`: zero for the whole frame (`CCubeRenderer::
BeginScene`), 255 for Samus in the Phazon Suit, each nearby object's ID (`id * 4`) for the morph
ball's shadow, which then copies that alpha (`GX_CTF_A8`) and draws the shadow where a TEV compare
finds the object's ID. The Vita path wrote TEV's alpha instead, and left those copies unmade.

- `dst_alpha_mode` (gx/vita_draw.cpp) decides as Aurora's WebGPU path does without dual-source
  blending. Zero: the alpha is blended to zero by factors of its own (`glBlendFuncSeparate`: ZERO,
  ZERO), the colour as GX blends it; no program needed. Another constant, where the colour's
  blending reads no source alpha (or the colour is not written): the program returns it as its
  alpha after the alpha test (`tev_program`'s `constantAlpha` variant; the constant rides in the
  alpha references' row, `uConst[12].z`), written as it is (ONE, ZERO). A constant the colour's
  blending would read goes unwritten, as on the PC; so does one under subtractive blending. The
  fixed-function draw (a program not built yet) writes its own alpha.
- An EFB without alpha (every other frame) reads its alpha as one: a destination-alpha blend factor
  is ONE (or ZERO for its inverse) there, where it read whatever the display's alpha held.
- The display is cleared to alpha 0 each frame, the game's clear colour's, which an EFB with alpha
  starts from (the display shows no alpha).
- The material key takes the pixel format's alpha and the constant's mode (the constant itself is a
  draw's, in its program constants), and the warm-up makes no `constantAlpha` programs (the card
  keeps them once compiled; they compile the first time each effect is drawn).
- The alpha copies are made: the copy program reads the screen's alpha where the EFB has it.

What to look for: the morph ball's shadow on the floor and on nearby objects; the Phazon Suit's
glow around Samus on the pause screen and in cutscenes; fog volumes. The log's textures line counts
the alpha copies ("of alpha").

tallon16's run (1d2f5c6, 3.6 minutes, quit normally): "looks really promising". The alpha copies
were made, four a frame in stretches (the Phazon Suit effect's chain: three blurring copies and
the mask) and one a frame in others (the morph ball shadow's IDs); no program failed to build;
32 FPS on average.

## The thermal visor's palette (tallon17)

The thermal visor's hot pass (`CCubeRenderer::DoThermalBlendHot`) copies the screen's intensity
(I4) and reads that copy as a palette texture (C4, nearest) through the thermal palette (TLUT0):
each texel's four bits pick one of 16 colours, the orange-to-yellow ramp. The Vita path sampled the
copy's intensity as it was: grey where the ramp should be.

- A copy read as a palette texture (C4 or C8) is now converted on the GPU into a texture of its own
  (`copy_texture_for` in gx/vita_draw.cpp records it ahead of the draw sampling it; vita_gl's
  `palette_on_gpu` draws it), as Aurora's WebGPU path converts it
  (`resolve_dynamic_palette_texture`): each texel's 8-bit value as an index (C4: its top four bits,
  as GX's I4 copy keeps them; C8: the value), that palette entry its colour. The draw then samples
  it as any texture (the palette before the filter, as GX applies it). A palette is a texture of one
  row (`palette_texture`, from `convert_tlut`), made once per palette and version; a conversion is
  made again only after another copy into its source or another palette.
- The conversion is a framebuffer's draw like a copy's: it counts among the frame's copies for the
  depth kept across scenes.
- A copy repeating the last one into its destination in a frame is now left to it only with
  nothing drawn since. Before, any repeat of the rectangle and format was, and the hot pass's copy
  repeats the cold pass's with the hot objects drawn between: it took the cold pass's copy.

What to look for: the thermal visor's colours on warm creatures and objects, its cold
surroundings as before. The log's textures line counts the conversions ("converted through a
palette").

tallon17's run (c500f7c, 2.2 minutes, quit normally): "looks good". 1,486 palette conversions (the
thermal visor in use), 3,289 copies, no program failed, 30 FPS on average. Milestone 5 is done:
depth copies, never asked for in any run, are left out.

## Milestone 6: speed. The input poll (tallon18)

Where a frame went in tallon17's run (the [split] and [zones] lines, averaged): the game's ticks
24 ms (2.1 a frame: the game runs 60 a second, two at 30 FPS), its draw 6.6 ms, input 4.3 ms; the
main thread 93% busy, the FIFO thread 79%, the render worker 36%. The CPU sets the frame rate, the
main thread first; the GPU waits.

- The input poll's 2.7 ms a frame: the controls profile built once a frame (1.2 ms: the default
  mappings, the user's bindings, the menu rows, built and compared), PADRead (0.65 ms), the
  devices read and the bindings engine (0.2 ms each). On the Vita the profile is now built only
  when what it is built from moved: the user's bindings (their version), port 0's pad and its
  type, whether there is a pad at all; while the Port menu is up (where the settings change) and
  for a second after; and once a second besides. Elsewhere it is built once a frame as before.
- The audio poll (`ARQPoll + AIPortPoll`) ran the game's audio callback: 6.3 ms a frame while a
  cutscene video played (its sound decoded; the video's own decode took 20 ms), about 1 ms in
  play. That is the work itself, left as it is.

What to look for: the frame rate in play (the log's [timing] line), and the input zones; bindings
and pad changes still taking effect (the Port menu's Remap page, a pad's type).

tallon18's run (daaa433, 5.7 minutes, quit normally): the profile was built every frame in 149
seconds, 144 of them with the Port menu on screen (the overlay's draw time) and the other 5 the
second after it closed; in the 167 seconds without it, once a second. 18 FPS on average, in heavier
rooms than tallon17's (3.6 ticks a frame, the draw 20 ms).

## The Vita's controller presets (tallon19)

The Remap page's reset to Remastered or Modern mapped the fire to RT, and kept the lock-on on LT
and missile or jump on RB and LB, with free look on the right stick's click: the Vita has no
triggers and no stick clicks. On the Vita the two presets now map onto what it has:

- Remastered: fire on R, lock on with L (the Vita default's), jump on cross, morph on square,
  missile on circle, triangle held with the D-pad picks beams (and springs the ball), map on
  Start, pause on Select; scan and X-ray swapped, twin-stick on, as Remastered's.
- Modern: fire on R, lock on with L, jump on cross, morph on circle, missile on square, beams on
  triangle with the D-pad, map on Select (the Vita default's), pause on Start; twin-stick on.
- In both, GameCube R (free look) has no button: the right stick aims under twin-stick. In morph
  ball it is the spider ball, so there L, with nothing to lock on to, holds R
  (`BindController`, whenever R is unmapped on the Vita).

## The map's GPU crash (tallon20)

tallon19's run (b5032f7): "controls look good, opening the map caused a GPU crash"
(psp2core-1793784484-GPUCRASH). The log's last second is the map: 1,729 draws a frame (the run's
most), 849 of them lines, nearly all with programs and blended, and fragment constants sent 750
times a frame (each map element its own colour; about 20 in play); no copies; 30 FPS. vitaGL's
memory steady (VRAM 26 MiB free). The dump's render worker waits in `vglSwapBuffers`: the GPU
failed on a frame already sent. Its GPU registers (GPU_INFO) name no cause readable here.

The likely one: vitaGL's shader patcher. A generated program is patched once for each blend state
it draws with (the factors and the colour mask are in the patch), and vitaGL releases none of
them (`patch_fragment_program`), so the variants pile up for the session in the patcher's heaps,
1 MiB each by default; vitaGL does not check the patch, so a full heap leaves a draw a broken
program. The pause screen and the map draw with an alpha channel (`CInGameGuiManager::Draw`
requests RGBA6 whenever the game is not drawn), and since tallon16 GXSetDstAlpha's zero gives
every draw there a blend state of its own (`dst_alpha_mode`: the alpha's factors ZERO and ZERO):
another variant of every program the map draws with.

- The heaps are larger (`vglSetupShaderPatcher`, before `vglInit`): fragment USSE 4 MiB, vertex
  USSE 2 MiB, the patcher's buffer 2 MiB. The USSE memory comes from vitaGL's RAM pool, and past it
  from the phycont pool, which no run has used (26 MiB free).
- The log's memory line (every 300 presents) gives each heap's use, to tell whether this was it
  and how much room is left.

It was not: tallon20's run used 83 KiB of the fragment heap and crashed the same way on the map
(next section).

## The map's GPU crash: wide lines (tallon21)

tallon20's run (60ec454) drew the map for about 25 seconds, the area map (1,700 draws and 36
resident calls a frame) and a heavier view (520 resident calls), then the GPU crashed again
(psp2core-1793785334-GPUCRASH). Its last memory line: fragment USSE 83 KiB in use of 4,096, vertex
USSE 79 of 2,048, buffer 0. Not the patcher's heaps.

**The dumps' GPU registers.** Imagination's register header for the SGX544 (`sgx544defs.h`,
TI's DDK; the Vita's SGX543MP4+ is close) reads most of GPU_INFO: a header (the time of the
crash), the master bank as (register, value) pairs from 0x4000, then each core's registers as
(register, core 0, 1, 2, 3). All five GPU crashes since Oct 25 (two in Tallon Overworld that day,
one on Nov 2, tallon19's and tallon20's map) show the same state:

- EVENT_STATUS (0x012C) 0x24142600 on core 0, 0x04142600 on the others: the TA finished, the
  pixel back end ended a render, core 0's event timer pending. No parameter buffer out of memory,
  no TA fault.
- BIF_INT_STAT (0x0C04) only "flush complete", BIF_FAULT (0x0C08) 0: no bad address. A hang.
- MEM_BLK_INFO (72-byte records: uid, name, type, address, size) puts vitaGL's VRAM pool at
  0x62000000, and vitaGL makes the rings there first: VDM (512 KiB), vertex (4 MiB), fragment
  (2 MiB). 0x0238 and 0x0334 point into the VDM ring, 0x0A5C into the fragment ring, in every dump
  (the Oct 25 morning's too, with the default rings' places).

**What the five share: wide lines.** GX's line widths came in with 4126db4 (Oct 25, 03:57); until
then every line was a pixel wide.

- Both Oct 25 hangs followed a burst of lines: run65 (c5af261) went from 300 lines a log window to
  34,081 in its last; run67 (cd75c62, after the larger rings, which did not stop it) drew 95,000 a
  window to the end.
- The rollback's builds drew lines a pixel wide again. From then until 748e22a brought the widths
  back (Oct 29), no GPU crash reached the PC, with the rain at the Landing Site among the runs.
- Then Nov 2's, and the map twice: tallon20's last 300 presents drew 167,201 lines among 257,163
  immediate draws, about 560 a frame, the outlines GX widens.

The likely way (not confirmed): a line wider than a pixel is drawn as a quad whose sides come from
the line's direction on screen, and a line seen end on has none: a room's vertical edge with the
map seen from above, rain falling toward the camera. That fits a hang when the map opened
(tallon19) or after 25 seconds of turning it (tallon20), and after minutes of play in the rain.

**The change.** Lines are drawn a pixel wide again (`line_width` in `gx/vita_draw.cpp`); points
keep GX's size (the game draws none). The immediate draws' line gives how many line draws GX
asked wider. The patcher's heaps stay as tallon20 made them (their use under 200 KiB); the larger
rings stay too.

## The warm-up as one batch (tallon22)

The user: the game "only compiles bits of it at a time". The log sent with that (tallon21, 52
seconds of the title screen and file select; the longer runs before it were left on the card)
shows why. The card's 1,592 material records were made again 2 ms a frame, and the FIFO thread
went on only once the one program it had found missing was compiled. So the 14 the card lacked
were found one after another (5 sent by 600 presents, 10 by 900, 14 by 1,200), and the toast,
which counted only what the compiler thread had been sent, grew by one at a time.

- The warm-up makes records again for up to 8 ms a frame, and goes on past a missing program while
  fewer than 64 wait (`kWarmUpWaiting`; about 9 KiB of sources each): all the records within the
  first seconds, every missing program waiting at once.
- The render worker still sends them to the compiler thread one at a time, so a draw's own compile
  still waits behind one at most.
- The toast counts the warm-up's waiting programs as sent (`program_compiles`), so it shows the
  batch whole from its start (0 / 14 up to 14 / 14). The log's warm-up line gives how long making
  all the records took.

What this cannot do: compile programs for materials the Vita has never drawn. Those still compile
at their first draw, one by one as they appear. Compiling the whole game's at once needs the whole
game's materials first: the PC capture run designed on Nov 2 (a tour of every room recording each
draw's material, the programs precompiled on the PC into `precompiled.bin`).

## The map's GPU crash: stale shaders (tallon23)

The user, after tallon22, had found days before that the map's GPU crash came from "the old
shaders from any of the previous build after a new build": with the card's old shaders deleted,
the map held. So the lines drawn a pixel wide since tallon21 were probably not the cure (the wide
lines' case rested on timing alone).

What could make a shader cached by an earlier build break a later one:

- The generated programs' cache (`vita_programs/<key>.gxp`) looks sound. The key is XXH3 of both
  Cg sources (`kBinaryVersion` 3), so a binary is read back only for the very same sources. It is
  written aside and renamed, its header is checked against the sources' lengths, and one that
  does not link is compiled again. vitaGL's program binary holds the two GXPs (each after a
  matrix-uniform count and a sampler count) and the attribute table, which the GXP decides; and
  nothing of a session's.
- Two compiles never overlap: vitashark's entry points have been wrapped with one lock since the
  compiler thread came in (4abc21d, then 7dfaad5).
- vitaGL's fixed-function shaders are the weak spot. vitaGL caches them in
  `ux0:data/shader_cache/v31/v` and `/f`, a folder hardcoded into it and shared by every vitaGL
  app (`vglSetShaderCachePath` sets only `glCompileShader`'s cache, which nothing here calls, so
  the game folder's `vitagl_shader_cache` stays empty). It writes each shader in place right after
  compiling it, without checking the compile worked, and hands the shader patcher whatever bytes a
  file holds. A write cut short by a crash or a quit, or a failed compile (vitaGL's compiles ran
  past the render worker's 32 KiB stack until 7e9aa23), leaves a broken shader that every later
  session loads. And after a build that changes the generated programs, the map's 1,700 draws a
  frame draw fixed-function until their programs compile again: those are the draws that load it.

**The change.** At the start, before vitaGL reads any, each file in
`ux0:data/shader_cache/v*/v` and `/f` that is not one whole GXP is deleted
(`check_fixed_function_cache`): magic `GXP\0`, version 1, and the header's size (at byte 8)
the file's own, which a GXP's size field always is (it counts every byte, unpadded, as the
GXPs in the card's own programs show). vitaGL compiles a deleted one again when a draw needs it.
The log gives how many were on the card and which were deleted.

Not it, as it turned out: the user deleted `ux0:data/MetroidPrime/vita_programs`, and
`ux0:data/shader_cache` had been empty all along (tallon25, below). The check stays; it costs a
look at an empty folder.

## Fixes from upstream 0.42.0 (tallon24)

The user sent upstream's 0.42.0 source ("Metroid Prime Unbound", `~/upstream/0.42.0` beside
0.32.0 from e3f2903) and asked for its fixes alone. It is mostly PC and phone features: the
Remastered and Metroid Dread imports, the Wii Trilogy disc, a third-person camera, an iOS build.
Three of its fixes apply to this build's code as it stands (the three files are 0.32.0's here):

- **Room streaming freeze** (`CScriptDoor`). A door opening a third room while two stay loaded can
  fill the 1,024-entry object list, and `AllocateUniqueId` then hangs. 0.32.0 released the rooms
  behind first in Processing Center Access only; now at any door once 512 objects are loaded
  (Central Dynamo with Quarantine Access A and Metroid Quarantine A hung so on a tablet).
- **The empty lock-on's marker** (`CTargetReticles`, issue #250). With twin-stick or mouse aim, a
  lock-on with no target keeps the camera's pitch, and the orbit point's marker sat above or below
  the screen centre by that pitch; it is lifted onto the view ray.
- **Held buttons across a controls rebuild** (`port_input_engine`, issue #235). A rebuild of the
  controls profile (the Port menu opening or closing swaps its buttons in and out) cleared what
  was held, so a button still down read as a fresh press: the menu could flip while its button
  was held.

Left out: #243 (switching targets while locked on belongs to 0.42.0's new Next/Previous target
inputs), #255 (inside its new crosshair styles), #253 (mouse aim with a pad, on input code 0.32.0
lacks), #201 (doors after loading a save state, which changes the save states' format), and its
2,048 object slots (11 ID bits instead of 10, through every object list).

## The map's GPU crash: another build's program binaries (tallon25)

The user's answer: the folder they deleted was `ux0:data/MetroidPrime/vita_programs`, and
`ux0:data/shader_cache` "has been empty". So it was the generated programs' own cache: their
binaries (`<key>.gxp`) and the warm-up's records (`materials.bin`) went together.

How an old binary could differ from a new compile is still not found:

- Its key is XXH3 of both sources, so it is read back only for the very same sources; the
  compiler's settings have not changed since Oct 25 (and vitaGL's `glCompileShader` path before
  used the same); on Nov 2 every binary on the card (552) was byte-identical to a fresh compile.
- What vitaGL stores besides the two GXPs is the attribute table the GXP decides. A loaded
  program has it twice (`glProgramBinary` restores it, then attaching the vertex shader appends
  the defaults again), but `glGetAttribLocation` finds the first and the draws read only the
  first `attr_num`, so it draws as a fresh one does.
- Our `finish` treats both alike (attributes, uniforms and samplers by name); program slots are
  reset when reused, deletes run between frames on the render worker, and vitaGL's vertex
  program cache never releases what it evicts.

The binaries are the likelier half of the folder: the warm-up reads every record in every session,
build change or not, and draws nothing; in tallon19's and tallon20's sessions most of the map's
programs were read from the card (115 of 126), hardly any compiled. Worth keeping in mind too:
the folder was probably deleted around tallon21 ("a few days" before Nov 7), which also drew
lines a pixel wide again.

**The change.** `vita_programs/build` names the build that wrote the binaries. When it names
another (or none, as on the user's card now), the binaries are not indexed, so nothing reads
one, and the card thread's first job deletes them (and any `.gxp.tmp` cut short), then names this
build. That is the user's fix, made at each new build. `materials.bin` stays: the warm-up finds
every program missing and compiles the batch from the start (about 1,200 on the user's card,
roughly a second each: around twenty minutes of compiling in the background after each new build,
the toast counting them). If the map still crashes after a new build with this, the records are
next (they would go too), and the PC precompile check could compare the card's binaries with
fresh compiles before they are deleted.

## Upstream 0.42.0 (0.42.0 test 1)

The user asked for the whole port on upstream's 0.42.0 ("Metroid Prime Unbound"), not 0.32.0 with
its fixes. Branch `port-042` (worktree `~/mpport-042`): `upstream-042` is 0.32.0 (e3f2903) with
0.42.0's tree over it (50b149f, the user's zip, `.vscode` left out); master's Vita work merged into
that (4592513, from 6623d2d), then master again for tallon25 (e8306b9). Master stays the 0.32.0
line until this one runs on the Vita.

**The merge.** 63 files had changed on both sides; 21 conflicted:
- Upstream's own: CScriptDoor, CTargetReticles, port_input_engine (tallon24 had backported their
  fixes).
- Aurora: the VFX scene copy stays a FIFO command, so the game thread does not drain the FIFO,
  now with 0.42.0's `again` as a u8 and its copy (the level-0 view, the pause screen's second copy)
  run on the FIFO thread. GX_AURORA_PORT_VFX_CAPTURE_SCENE and GX_AURORA_PORT_FIFO_TASK move to
  0x0075 and 0x0076, past 0.42.0's new commands. Its array bounds checks, and its skip of draws
  that make no whole triangle, run before the Vita's draw hooks. Lazy draw tags stay beside its
  performance flags.
- Input: the Vita's controls profile still builds only when its sources change (tallon18), with
  0.42.0's new steps inside (the saved pad preset, the Morph Ball, map and target rows, twin
  stick's free aim); whether the player is in the Morph Ball joins the sources, as twin stick frees
  R outside it.
- Presets: the Vita's Remastered and Modern layouts move into `ApplyPresetMapping`; on the Vita no
  alternate fire and jump buttons (its face buttons are all taken), no LT Spider Ball row (it has
  no LT; L holds R in the ball, BindController), no keyboard rows.
- `main.cpp`: 0.42.0 always sets aside room geometry's 256 MB arena and larger frame buffers, free
  on a PC until touched; the Vita takes its arena whole from newlib's heap, so there the reserve
  stays as before, only with room geometry installed (never, on the Vita).
- CStateManager, CElementGen: 0.42.0's additions and the Vita's profile zones both. The rest:
  both sides' includes.

**For the Vita.** The Vita's nod gains `nod_partition_find_file` (0.42.0's lookup by path, for the
Echoes import: the FST walked from the root, ASCII case ignored as the DVD's lookup does); the log's
instance lock is always free (newlib has no flock); the Mods page's built-in imports (Prime
Remastered, Metroid Dread, Echoes) are hidden, as the Remastered tab was; the VPKs' APP_VER comes
from versionName (00.42), as the game's version string does. The GX structs the material records
hold are unchanged, so the card's records still warm up.

**To watch in its first runs.** The ELF is 29 MB against 25 (code and data the heap no longer has;
about 11 MiB of the heap was free in play). The first start compiles every program the records name
(tallon25: another build's binaries go). 0.42.0's new options (Morph Ball rows, Dodge, Aim Lock-On,
flick stick, toggles) have not run on a Vita; GX state 0.42.0 adds for the Remastered imports and
the beam colours (clip planes, colour matrices, the shell) is not drawn by the Vita path.

## 0.42.0 test 2: the minimap tap and the Echoes suits

**Test 1** (6cdeb57, the user's log of Nov 7) ran 21 minutes without a crash or GPU hang, and the
map opened many times (31 log windows draw its outlines). The card's `vita_programs` was empty, so
every program compiled at its first draw (276 s on the compiler thread); presented FPS had a
median of 26 over the first third of the run and 32 over the last. The heap peaked at 177 MiB in
use of 207 MiB taken. vitaGL's cache check deleted 4 broken fixed-function shaders.

The user asked for two things:

- **Tap minimap for map.** The 0.32.0 Vita builds never had it: the setting and the tap (the touch
  overlay's) were Android's, and the Vita's settings search only listed the name. Now the front
  screen takes it. `PortDebug::MinimapTapEvent`, on SDL's event watch beside `PhotoTouchEvent`,
  takes a finger put down on the front panel (`SDL_TOUCH_DEVICE_DIRECT`; the back panel is
  indirect) and lifted within 0.4 s, less than 3% of the screen from where it landed, on the
  minimap's rect as the HUD publishes it (`PublishMinimapRect`, framebuffer fractions, as the
  overlay's own test reads them) while it is drawn and the Port menu is closed. That asks
  `RequestMapTap`: one poll of Z, through the stock `kTouchMapTap` binding. Its setting is
  Controls > Touch & gyro > Touch screen, "Tap minimap for map" (`touch_map_tap`, on by default).
- **Echoes suits.** On the Vita the Mods page's built-in mods are Metroid Prime 2: Echoes' alone
  (Prime Remastered's and Dread's imports stay out). The disc image is the first `.iso` or `.gcm` by
  name in `ux0:data/MetroidPrime2`, looked for again every 2 s while the page is open; Import
  converts the Varia, Dark and Light Suit bodies into `mods/echoes-suits`, as elsewhere, and the
  image is not needed after. The Vita's disc reader takes plain GameCube images only (no `.rvz`,
  `.ciso`, `.wbfs` or NKit, and no Trilogy). The import runs on a thread of its own with a 512 KiB
  stack, its std::thread waiting for it: a std::thread gets `PTHREAD_STACK_MIN`, 32 KiB, there.

Not known yet: the import's time and memory on the Vita, and how the Echoes suit models draw
through the Vita path.

## 0.42.0 test 3: the Vita's own button prompts

**Test 2** (bff03e4, the user's log of Nov 8) ran 4 minutes and quit cleanly. The Echoes import
read `ux0:data/MetroidPrime2/Metroid Prime 2 - Echoes.iso` and converted 15 bodies and left arms,
6 balls, 3 right arms, 5 beam guns and 113 textures in about 18 s. After the mods reload
`echoes-suits` served 10 Samus body models and the arm cannon of all 5 beams (181 resources);
VRAM stayed at 42-47 MiB free of 96, and presented FPS had a median of 45 over the 3 minutes
after.

The user then sent PS Vita button art for the prompts. Three things kept any prompt icon off the
Vita:

- **No icons in the VPK.** PortPrompts reads `<base>/textures/bindings`, and the VPK carried no
  `textures` folder (`MP_EMBED_RESOURCES` is off on the Vita), so it never started. The VPKs now
  carry the Vita's set alone, `textures/bindings/vita_*.dds` (46 files, 1.1 MiB), read from
  `app0:/textures/bindings` (SDL's base path there is `app0:/`). PortTextures takes
  `app0:/textures` as a device-agnostic pack, as the PC build does, and finds no pack textures
  there.
- **No Vita set.** The Vita's pad is an SDL standard pad, which the port gives Xbox letters.
  `PortTextures::DeviceDirForType` now names every pad "vita" on the Vita, and
  `tools/make_prompt_glyphs.py` has a `vita` device: the face buttons, Start and Select, L and R,
  both sticks and the D-pad, whole and per direction. It has no triggers or stick clicks, so those
  keep the game's art. Its icons are the user's art, framed by `tools/vita_prompt_icons.py`: each
  picture cropped to its art and centred on 128x128, filling as much of the side as Kenney's icons
  do. `make_prompt_glyphs.py textures vita` writes one device's icons without rewriting the
  others'.
- **No Vita copy of a file's replacement.** Aurora made a file's replacement (a texture pack's DDS
  or PNG, a prompt's icon) a WebGPU texture only. The Vita path draws with its own copy
  (`vitaTexture`), and `add_missing_copy` makes none when the replacement's size is not the
  game texture's, so the draw found no texture. `create_converted_texture_handle` now makes the
  Vita copy from level 0: RGBA8 (a BGRA DDS swapped back) or BC1. A user texture pack needs the
  same on the Vita.

F1 > Controls > Options > On-screen prompts > Button glyphs offers PS Vita (auto) and GameCube (the
game's own art) on the Vita. Prompt resolving, four times a second on the game thread, has a
profile zone ("prompts: resolve the bindings"), and the log names each prompt's icon as it changes
("metroid_prime_port: prompt ...").

Not known yet: how the art reads at the HUD's size on the Vita's screen, and what the resolve costs.

## 0.42.0 test 4: prompts that stay, a map in one piece

**Test 3** (e55a927, the user's log and photos of Nov 8). The Vita icons showed for some prompts
(Cross, Circle, the map's Select) and not for others (L, R, Y, Start, the sticks, the C-stick, the
main menu's Z, the map's A), and read small. Resolving the prompts cost 0.01 ms. After ten minutes
the game ended with `std::bad_alloc` on the FIFO processor: `operator new(7,319,552)` in
`vita_gl::record`, from `record_draw`, with 177 MiB of the heap in use and 51 MiB free, but not in
one piece, and 228.7 of 229.4 MiB taken from the system. The frames before it drew Chozo Ruins'
map: 5,771 draws a frame, 5,733 of them immediate and nearly all untextured lines and faces, at
9 FPS. Each draw is recorded at 1 KiB, so the frame's vector of draws doubled to 7 MiB.

- **Merged draws.** `vita_gl::record` joins a draw to the frame's last one when both draw the
  frame's own vertices as lists (lines or triangles), the second's right after the first's, and
  nothing else differs: matrices, units, state, program, its textures and rows, viewport, scissor
  and width. The second's indices are offset past the first's vertices. GXM rasterises the joined
  primitives in the same order. The log's `presents` line counts the draws "joined to the draw
  before them".
- **Prompts that kept the game's art.** Aurora loads replacements in the background
  (`AsyncTextureReplacements`). A draw that resolved before an icon was in took the game's texture
  (or a thumbnail). Aurora then dropped that object's cached texture and moved the bind generation,
  but the Vita path's material cache had the generation in its key only for EFB copy slots, and kept
  the material. Aurora now counts, per texture object, how many times its cached texture was dropped
  for its replacement (`texture::replacement_version`), and the Vita material key includes the
  count. Only those materials are worked out again, once the icon is in.
- **A race.** `texture::invalidate_replacement` edited Aurora's texture caches directly.
  `unregister_replacement` calls it on the game thread (PortPrompts, when a prompt texture is freed
  or its icon changes), while the Vita's FIFO processor reads those caches. It is now queued, and
  applied with the pending invalidations on the thread that resolves textures.
- **Larger icons.** The art fills the inset the game's own art fills, where it took 75% of it: a
  third larger. L and R also serve the trigger prompts (`vita_lt`, `vita_rt`), the names the port
  uses when the bindings drive GameCube's L and R as triggers, or bind nothing to them. The VPKs'
  glyph list is globbed with `CONFIGURE_DEPENDS`, so new icon files reach them.
- **The log.** Each prompt's icon at the start and whenever one changes ("prompt icons for vita: A
  vita_south, ..."), and once per prompt texture, the icon it took or why it kept the game's art
  ("prompt texture 6D22966B (Start, 32x32): ...").

Not known yet: how far the map's draws merge (its rooms differ in colour), and why Y, Start and the
sticks asked for the game's art in test 3, which the new log lines answer.

## 0.42.0 test 5: CPU round 2 begins

**Test 4** (82b3f9b, the user's log of Nov 8) ran 14 minutes and quit cleanly. Every prompt texture
took its Vita icon: the log's per-texture lines name one for A, B, X, Y, Start, L, R, Z, both sticks
in every direction and the D-pad. The first table, written before the bindings existed, named only L
and R (as triggers); the full set came a second later, before any prompt texture was bound. Test 3's
misses were the stale materials.

The map's draws merged by 42% (Chozo Ruins: 5,763 to 3,325 a frame), and it stayed near 10 FPS.
`CMapWorld::DrawAreas` sorts every surface of every room back to front and shades each surface's fill
by its normal, so no two fills share a colour, and fills alternate with outlines: at least two draws
a surface are left. The FIFO thread took 133 ms a frame (recording the draws 47.6 ms, decoding the
immediate vertices 33.4) and the render worker 72.7 ms. The map code is 0.32.0's: Chozo Ruins has
some 1,650 surfaces, against the 1,700 draws a frame of the maps tested before.

Normal play (the 354 windows over 28 FPS) averaged, per frame on the main thread: input 4.1 ms,
tick 10.0 ms (1.34 ticks), draw 5.7 ms, begin-wait 1.1 ms.

**Round 2, first step.** `PortInputDevices::PollPortActions` (the port's own actions: the Port menu,
save states, screenshots, custom actions), which `UpdateControllerNav` calls every frame, built the
whole controls profile every frame: the build tallon18 took out of `Poll`. On the Vita it now builds
as `Poll`'s does (when its sources move, while the Port menu is up and a second after, and once a
second besides). The frame's start before the ticks, [split]'s "input", is now zoned part by part:
"frame: aurora_update (the event pump)", "frame: the events", "frame: mouse capture and cursor",
"frame: aspect", "frame: UpdateControllerNav (Port menu, Port actions)" (with "frame: UI scale and
theme" and "frame: Port actions (PollPortActions)" inside it), "frame: PollGyro",
"frame: PortTextures::Poll", and "input: build the Port actions' profile" when it builds.

Next: whatever the zones name in those 4.1 ms; then the game's own logic (SM: Think 2.8 ms a frame,
CrossTouchActors 0.9 ms) and the Vita draw path's cost per draw, which sets the map's speed (some
22 us a draw on the FIFO thread and as much on the render worker).

## 0.42.0 test 6: the sticks' diagonals; the rear touch pad off

The user saw the sticks' icons in four directions where the map's Rotate and Move prompts turn
through eight, and sent photos of the GameCube art doing so. The game has nine frames for each
stick: the GC disc's GGuiSys.pak names them (read from the user's own image) LStickN at rest,
LstickU, LStickD, LStickL, LStickR, and the diagonals LStickUL `99F6C3B4`, LStickUR `217C015A`,
LStickDL `4A414F04`, LStickDR `13A7E65D`; and CStickN... with CStickUL `5400AEEF`, CStickUR
`63E1497C`, CStickDL `9C8A7FD1`, CStickDR `AB6B9842`. Upstream's table took the diagonals for the
whole stick, its icon sets (Kenney's) having no diagonal art.

They are prompts of their own now (`PROMPT_STICK_UP_LEFT`... and the C-stick's). A diagonal's icon
is the two ways' at once, `<device>_stick_l_up_left`, where its vertical and horizontal ways go to
the one stick and the set has that icon (looked for once per name); otherwise the whole stick, as
before. The Vita's set has them, from the user's eight diagonal pictures. The other stick frames on
the table (`2B97F6A0`, `3A446C61` and the rest without a name on the GC disc) keep their kinds.

The rear touch pad is off: `SDL_HINT_VITA_ENABLE_BACK_TOUCH` is "0" before SDL's video starts, so
SDL no longer reads it. Its touches (the hands holding the Vita) reached the menus' pointer
gestures, which take every touch device, and the Port menu's mouse. The front screen stays.

Test 6 also carries test 5's change, which the user had not run yet.

## 0.42.0 test 7: the pad read once a pump

**Test 6** (b9b0124, the user's log of Nov 8) ran 31.6 minutes and quit cleanly. The diagonal
prompts took the Vita's diagonal icons (`prompt texture 99F6C3B4 (stick up-left, 64x32):
vita_stick_l_up_left_64x32.dds` and the rest), and the rear touch pad was off.

Normal play (the 766 windows over 28 FPS) averaged, per frame on the main thread: input 3.1 ms (4.1
in test 4: test 5's change), tick 6.2 ms (1.25 ticks), begin-wait 4.4 ms, draw 5.1 ms. The new zones
split the input: "frame: UpdateControllerNav" 2.65 ms, of which "frame: Port actions
(PollPortActions)" 1.42 ms (its profile builds now 0.12 ms); then each tick's "input: controller
Poll" (1.35 ms a frame), of which "input: PADRead" 0.54 ms, 0.44 ms a call.

The slow part of the run was another kind of frame: 769 windows (42% of them) ran 12 to 18 FPS, and
there the main thread was busy 57 ms a frame and waited for a frame slot only 2.5 ms. A frame that
long has 3.5 of the game's 60 Hz ticks to catch up (`SM::Update` 24 ms a frame, `SM: Think` 10.9,
`CrossTouchActors` 4.7, the HUD 3.4), and each tick reads the pad again, so a tick's cost counts
three and a half times there, and every millisecond cut from a tick shortens the next frame's catch-up
as well.

**The pad.** Each `SDL_GetGamepadButton` or `SDL_GetGamepadAxis` takes SDL's joystick lock and looks
the pad and its joystick up in SDL's object table, each look-up behind a lock of its own (on the Vita
SDL's generic read-write lock: a mutex and a condition, whose `sceKernelLockLwMutex` calls add up).
From the zones, about 20 us a call: PADRead's 0.44 ms is its 22 or so calls (each button mapping,
eight axis halves, the triggers). The readers added up to about 100 calls a frame in play and nearly
twice that in the slow windows: PADRead every tick; `PADGetRawState`'s 32, the first read after
each pump (PollPortActions pays them); `UpdateControllerNav`'s 40 or so for the Port menu's
navigation; and `get_controller_for_player`, which asks SDL for each pad's player index
(`SDL_GetGamepadPlayerIndex`, as dear) every time a PAD call looks for a port's pad, four times a
PADRead alone.

SDL's pad state changes only when events are pumped, so on the Vita:

- `pad_snapshot` (Aurora's pad.cpp) reads a pad's buttons and axes once a pump, only those its
  mapping has (SDL's "PSVita Controller" mapping has 14 buttons and 6 axes, which it reads as the 21
  calls with the pad's id), and every reader takes them from there: PADRead's mappings and ext
  buttons, `PADGetRawState`, and through the new `PADGetGamepadButton` / `PADGetGamepadAxis` the Port
  menu's navigation (`UpdateControllerNav`) and `CDolphinController`'s Start+Back check. Off the
  Vita those are SDL's own calls, as before.
- `player_index` keeps each pad's index for the pump. Setting an index can take it from another
  pad, so every set Aurora makes goes through `set_gamepad_player_index`, which forgets them all, as
  adding or removing a pad does.

What test 7's log should show: "frame: UpdateControllerNav" well under 1 ms, "input: PADRead" near
0.05 ms a call, "frame: Port actions (PollPortActions)" about 0.5 ms (the pump's one read of the pad).
The Port menu's buttons, the remap page's capture (which still asks SDL itself) and the controls in
play should all work as before.

This helps where the main thread sets the pace, as in most of the play (at 28 to 31 FPS the FIFO
thread was busy 22.8 ms a frame and idle 10.4). The slow room is the FIFO thread's: there it was
busy 63.5 ms a frame for 1,176 draws, some 54 us a draw (recording 14.1 ms, skinned lists 13.3,
state 8.4, materials 6.5, Aurora's commands 5.2, immediate draws 4.3, dispatch 3.7), and the main
thread only filled the wait with catch-up ticks; the room held 16.7 FPS for its 13 minutes. Cutting
the main thread's ticks does not move that frame rate: the FIFO thread's cost per draw does.

Next: the FIFO thread's cost per draw. `record_draw` builds each draw's `vita_gl::Draw` (about
1 KiB) zeroed, and `vita_gl::record` takes it by value and copies it into the frame: built in
place, those copies go. Then the skinned lists' path, and the game's per-tick logic (`SM: Think`,
`CrossTouchActors`, the HUD's update), which the catch-up multiplies where the main thread is the
slower one.

## 0.42.0 test 8: a core each, and draws that hold nothing

**Test 7** (ac8c2bd, the user's log of Nov 8) ran 68.8 minutes (125,529 frames) and quit cleanly.
The pad's reads came down as planned. In play (the 2,008 windows over 28 FPS) [split]'s input fell
from 3.1 to 1.7 ms a frame: "frame: UpdateControllerNav" 2.65 to 1.25 ms, its own part 1.24 to 0.07
ms, "frame: Port actions (PollPortActions)" 1.42 to 1.19 ms (about half of it the pump's one read of
the pad, the rest its bindings engine); "input: PADRead" 0.44 to 0.007 ms a call; the ticks'
"input: controller Poll" 1.35 to 0.50 ms a frame. The heavy stretch after loading (some 17 minutes,
the same numbers every second, as in test 6) went from 16.7 to 17.7 FPS. Over the run, the [timing]
lines' presented FPS: median 28.1; 45% of the seconds at 31 or more, 26% at 12 to 18.

The plan, as the user settled it after test 7: there is no Phase 3. Milestone 6 is Phase 2's last;
after it the port is done, and what is left is optimisation. `docs/VITA.md`'s target after Phase 2
stands: 20 to 25 FPS in the heaviest rooms, 30 in the lighter ones.

**The heavy room is the FIFO thread's.** There, per frame: 1,155 draws, and the FIFO thread busy
62.4 ms by its own clock in the frames it times. Its marks are kernel calls (some 0.4 us each,
16,500 in a timed frame), so the frames it does not time take about 55.7 ms; the main thread waits
4.6 ms a frame for it, and fills the rest with 3.4 catch-up ticks. By section, for the timed frames:
recording 14.1 ms, skinned calls 13.2, state 8.0, materials 6.4, Aurora's commands 4.8, immediate
draws 4.5, dispatch 3.5, other draws 2.8, lookup 2.1, mesh 1.5. And the [threads] line for that
stretch: the FIFO processor ran 86% of the time while its clock says it was at work about 97% of
it. The frame reaper (6%, at its priority), the DVD worker (4%, above it) and a texture worker had
last run on its core; the render worker had core 2 at 36%. The Vita's scheduler runs a thread till
it waits or a higher one is ready, whatever else of its priority waits: a frame's destruction ran
through while the FIFO thread waited.

**Cores** (Aurora's `thread::Options` gains `vitaCore` and `vitaPriority`, set in `set_current`):
the main thread keeps to core 1, the FIFO processor to core 0, the render worker to core 2. The
helpers at their priority or above share core 2: the frame reaper (now priority 170, just below the
render worker and above the program compiler), the DVD worker (high, as it was: the music stream's
refills) and the program cache thread. The compiler and the texture workers (low) still go wherever
a core is free: they cannot take one from the busy threads.

**Draws hold nothing.** A `vita_gl::Draw` held its mesh, its units' and its program's textures, its
program and a copy's destination as shared pointers: up to 13, each copy an atomic count change
behind two barriers on the FIFO thread, and another as the reaper let it go. Now the frame being
recorded holds each of those once (`vita_gl::keep`, by the frame's serial in the object), and a
draw points at them: a `Draw` is trivially copyable and destroyed with its frame's vector at once
(`DrawUnit` is a draw's unit; a material's `TexUnit` keeps its texture as before). `record` takes
the draw without a copy, `record_draw` takes its mesh by reference, the viewport and scissor
rectangles are worked out only when they change (four `std::lround` calls each, library calls
here), a program's constant rows go into the frame as one block, and the skinned calls' posing
buffer only grows.

Two more FIFO sections split the largest ones: "rows" (a program draw's vertex rows and constants,
until now part of "record") and "skinned copy" (a skinned call's posed vertices into the frame,
until now part of "skinned").

What to look for in test 8's log: the [threads] lines (main 1, FIFO 0, render worker 2, and the
reaper, DVD worker and program cache on 2; no "could not keep to core" warnings), the heavy room's
frame rate, and the [fifo path] sections ("record", "rows", "skinned", "skinned copy"). On screen,
nothing should change: textures, the EFB copies (the thermal visor's palette pass, the pause
screen's blurred backdrop), the Port menu.

## 0.42.0 test 9: the FIFO thread off core 0; no mouse and keyboard set-up

**Test 8** (7cdd6d2, the user's log of Nov 8) ran 32.4 minutes (45,440 frames) and quit cleanly.
Every thread kept to its core (no warnings, no moves). The heavy stretch came at the end this time:
16.4 FPS for 1,209 draws a frame (test 7's: 17.7 FPS for 1,155). Per draw the FIFO thread took
what it took in test 7, about 50 us: by section for its timed frames, skinned calls 11.3 ms and
their copy 3.6, state 8.2, recording 7.9 and the program rows 6.1 (14.1 for both in test 7, with a
mark a draw fewer), materials 6.4, Aurora's commands 4.9, immediate draws 4.6, dispatch 3.7, other
draws 3.3, lookup 1.9, mesh 1.4. So the shared pointers' count changes were no large cost, and the
split shows where recording goes: the program rows about 5 us a draw (a lit program's lights and
matrices), the rest 6.5; a skinned call's posing about 155 us, its copy 49.

The cores told something else. Alone on core 0, the FIFO processor ran no more than 81% of the time
all run (78 to 81% in the heavy room it held back), while its own clock had it busy 93% of each
frame: with no thread of its priority or above left on its core, core 0 seems to give some of its
time elsewhere (the system's interrupts or threads), or the FIFO thread waits on something inside
its sections. In test 7, moving between cores, it ran 86%.

So test 9 swaps them: the FIFO processor keeps to core 2, the render worker (busy about a third of
the time) to core 0 with the helpers (the frame reaper, the DVD worker, the program cache thread,
which now runs just below the render worker too, as the reaper does), the game to core 1. And the
[threads] lines count, for each thread since the last line, its preemptions by interrupts and by
other threads, and its waits: core 0's interrupts, if that is where the time goes, show there.

The Controls page's Quick setup no longer offers "Use mouse & keyboard" on the Vita (the user's
ask: it has neither); the Port menu's search no longer lists it there either.

What to look for in test 9's log: the heavy room's frame rate and the FIFO processor's busy share
there (over 81%?), and the preemption counts of the threads on core 0 against core 2's.

## 0.42.0 test 10: Phendrana's snow, and every level of a CMPR texture

**Test 9** (989aaac, the user's log of Nov 8) ran 27.5 minutes (24,964 frames) and quit cleanly.
Its first ten minutes were the shader warm-up a new build brings (tallon25 deletes another build's
program binaries): 437 programs compiled in 591 s, the front end at 2 to 20 FPS meanwhile. The
counters answered test 8's question: core 0 takes the interrupts. The render worker there was
preempted by interrupts 10,577 times in 20.7 s (about 510 a second), against 75 for the FIFO
processor on core 2 and 813 for the game on core 1; and on core 2 the FIFO processor ran 87% of
the time in its heaviest stretch (on core 0 in test 8: 81% at most). The play was in Phendrana
Drifts, heavier than tests 7 and 8's room (the world's draw 24 ms a frame, 4.1 ticks, about 14.5
FPS), so the frame rates do not compare.

**No snow in Phendrana** (the user's report). `CEnvFxManager` draws each flake as a quad sampling
`TXTR_SnowFlake` (CMPR, 64x64, four levels), times a gradient by height, added to the screen. Before
the first snow draw the game zeroes the flake's first level (`BlankFirstSnowflakeMip`): a flake
near the camera is large on screen and samples that level, so it vanishes, while the far ones
sample levels 1 to 3. The Vita's texture copies had their first level alone, so every flake sampled
the blanked one: black, added to the screen, nothing. Now a CMPR texture's Vita copy carries its
levels down to 4x4 (as BC1, as far as levels without transparent texels go:
`gfx::convert_cmpr_to_bc1`), uploaded level by level (vitaGL grows the mip chain for each), and
the samplers take the game's own filters (`GX_LIN_MIP_LIN` as `GL_LINEAR_MIPMAP_LINEAR`); a copy
with one level samples it as before. The rest of the world's CMPR textures get their levels too:
distant surfaces sample the game's smaller levels, as on the GameCube, rather than shimmering on
the first, for about a third more BC1 memory (2 to 7 MiB of BC1 in play).

**Controls texts for the Vita** (the user's ask): Quick setup's "Use twin-stick controller" help
names the Vita's Remastered layout (R fire, L lock on, Cross jump, Square morph ball, Circle
missile, Start map, Select pause, Triangle with the D-pad for beams); Remap's Presets... tooltip
names the Vita's four layouts (its keyboard presets were already left out); and the Remastered and
Modern presets' own descriptions name the Vita's buttons, not Xbox ones.

What to look for: snow falling in Phendrana Drifts (the nearest flakes fading out, as on the
GameCube); distant floors and walls steadier; the presents lines' BC1 megabytes; nothing drawn
wrong on far surfaces (noise there would mean a level uploaded wrong).

## 0.42.0 test 11: the mip levels uploaded one transfer at a time

**Test 10** (6b6386f, the user's log and dump of Nov 8) crashed at the first room it loaded, under
a minute in (39 profile windows): the log stops at a new pipeline just as the game's own ticks and
draws begin, and the front end before it had drawn no BC1 texture at all (its textures are RGBA8). The dump (`psp2core-1794190372-…`, cut short:
no stacks) has one stopped thread, the render worker (`0x400102fd`), on a data abort inside
SceLibKernel, with a register at `0x62000010`: the start of vitaGL's VRAM heap. Its other threads
were where they wait (the game in `C_MTXConcat` under `CGraphics::SetViewMatrix`, simply running).

What test 10 added there: every mipmapped CMPR texture's levels went up one `glCompressedTexImage2D`
each. vitaGL grows the texture's chain for each level with `vgl_realloc`, which on VRAM is
`sceClibMspaceRealloc`: where the chain cannot grow in place it moves, and the old block is freed at
once. And a power-of-two level is copied in by `sceGxmTransferCopy`, a GPU transfer nothing waits
for. So a chain moved while its last level's transfer was still writing, and the write landed in a
freed block: the heap keeps its bookkeeping in its free blocks, and the next allocation or free
that read it broke. The first room with mipmapped textures did that dozens of times.

Now each level after the first waits for the transfers before it (`sceGxmTransferFinish`: small
copies all, a few microseconds a level), so nothing is writing when the chain moves; and the levels
under 16x16 are left out (one or two 4x4 blocks wide, a transfer of their own each, nothing seen of
them). The snowflake keeps three levels: 64 (blanked), 32 and 16. The first four chains uploaded
log a line ("BC1 texture WxH uploaded with N levels").

What to look for in test 11: no crash at the first room; those four lines in the log; then test
10's list (snow in Phendrana, steadier distant surfaces, nothing drawn wrong).

**Test 11** (6e99d09, the user's log of Nov 8: "crash has been fixed") ran 24 minutes (34,365
frames) and quit cleanly. The four chains logged went up whole (256x128 with 4 levels, 64x32 with
2, two 128x64 with 3). With the levels the BC1 textures took 8.9 MiB at most (535 of them) beside
24 to 32 MiB of RGBA8, and VRAM kept 36 to 45 MiB free of its 96. vitaGL's fixed-function cache
had 4 GXPs that were not whole (test 10's crash left them half written); the start-up check
deleted them. In the slow stretch (523 windows at 12 to 18 FPS) the game thread set the pace, the
FIFO thread waiting on it (67 ms a frame against 50): the ticks 35.9 ms (3.6 of them: `SM: Think`
14.6, particles 6.8, the HUD 3.9) and the draw 24 ms (`DrawWorld` 18.2).

And the snow falls in Phendrana Drifts again (the user: "there were snow"): the flakes take the
snowflake texture's levels 1 and 2, the near ones its blanked first, as on the GameCube.

## 0.42.0 test 12: the game's tick, measured part by part

The next round is the game thread's tick, which the catch-up multiplies: in test 11's slow stretch
a tick took 8.3 ms (`SM::Update`) and a frame ran 3.6 of them. By zone, per tick: `SM: Think (all
entities)` 4.06 ms (particle systems about half of it: `CElementGen::Update` 88 us a call, some 21
a tick), the HUD (`CInGameGuiManager::Update`) 1.08, the environment effects (the snow) 0.63, the
camera manager 0.61, the player's collision move 0.60, MovePlatforms and MoveDoors 0.54, the
player's input 0.53, CrossTouchActors 0.52, PreThinkObjects 0.51, PostUpdatePlayer 0.41,
UpdateSortedLists 0.35, and the input poll 0.49. No one of them is most of it, and the largest,
Think, is one zone over every entity.

So test 12 measures before cutting:

- `PortThinkProfile` (`platform/vita/vita_think_profile.cpp`) times each entity's Think and
  PreThink by its class (`typeid`), sampled as the zones are (a class's first 32 calls in a report
  window, then one in eight, weighted): the report's `[think]` lines give the 14 classes that took
  the most, ms and calls a frame (`~/tools/think.py <log> <lo> <hi>` averages them by FPS).
- Zones inside the player's input (scanning, grapple, movement or the ball's, free look, the orbit
  targets, the gun and visor state, the gun's own input), the HUD (faceplate and reflection, the
  player visor, `CSamusHud::Update`, the auto mapper), the camera manager (its hints, its cameras)
  and the two input polls (the direct actions, the pad status; the Port actions' devices and their
  engine). The report lists 48 of the game thread's zones, not 32, to hold them.

The game's own simulation rate (Performance > "Simulation rate (very experimental)", `sim_rate`)
would halve the ticks at 30 Hz, but upstream calls 60 Hz "console-accurate and the only supported
rate": other rates "can break physics, enemy behaviour, cutscenes and scripted events". It stays at
60 unless the user chooses otherwise.

**Test 12** (4df215d, the user's log of Nov 8) ran into Magmoor Caverns. In its 300 windows at 12 to
18 FPS the `[think]` lines put first: `CScriptPickup` 2.15 ms a frame (44.6 calls, 48 us each, for a
Think that is little code: next to read), `CEnergyProjectile` 1.76 (192 us each), `CScriptEffect`
1.55 (23 us each), `CExplosion` 1.06 (271 us each), `CAtomicAlpha` 0.97 (1.5 ms a call), then
`CBabygoth`, `CPlayer`, `CParasite`, `CScriptActor` (110 calls) and `CFlickerBat`, 0.5 to 0.75 each.
And the user found Magmoor's lava black: test 13.

## 0.42.0 test 13: Magmoor's lava (emboss coordinates)

The lava with a bump map (`CFluidPlaneCPU::RenderSetup`, `kFT_Lava`) is the game's one user of
GX's emboss coordinates: a second set for the bump map, `GX_TG_BUMP3`, made from set 3 moved along
the bump light's direction, and two stages taking the difference of the two samples (plus a half,
unclamped, into register 0) to scale the lava's colour by twice it. The Vita's program generator had
no emboss coordinates (`vita_tev.cpp`: "a bump map as the source: not yet"), so the whole six-stage
material had no program and went to the fixed-function draw, which has no TEV: the vertex colour
(the wavecaps' intensity, near zero on lava) times the first texture. Black.

Emboss coordinates are made now, as GX makes them and Aurora's own shaders do (`shader.cpp`): the
source set's coordinates plus the light's direction (from the view-space position to the light's
position, a row of its own) dotted with the vertex's tangent and binormal, both through the normal
matrix and not normalised (the lava scales its normal matrix by its bump scale for this). The
Vita's vertices hold the normal alone, so the tangent and binormal are made from it as the fluids
make theirs (`CFluidPlaneRender`: the normal (x, y, z), then (x, z, -y), then (z, y, -x)); no other
draw in the game uses `GX_TG_BUMP`. The layout takes the source's slot first; the material key and
the warm-up's record include the source; a generator's `src` (`GX_TG_TEX0` for an emboss one, which
`GXSetTexCoordGen2` writes and nothing reads) no longer asks the mesh for a coordinate set.

The precompile tool's self-test makes the lava's program (`precompile_sources --self-test lava
<dir>`), and the Vita's compiler compiles it on the PC (`precompile.py compile`: vertex 496 bytes,
fragment 860). Every other program's source is as before, so the card's binaries still serve.

The log names what is still left fixed-function: the "indirect textures" line counts every material
worked out whose layout no program can draw, and the first six of them get a line each ("a material
no program can draw", each stage's coordinate generator and texture).

What to look for in test 13: the lava lit and moving in Magmoor (its colour and bump), nothing
black where it was; in the log the new count, and any "no program can draw" lines.

**Test 13** (947caae, the user's log of Nov 9): the log names two materials still left
fixed-function. One has five stages, each sampling a coordinate set of its own from a vertex
attribute (3x4 matrices, `GX_TG_TEX0` to `TEX4`): more sets from vertex attributes than the
mesh's four. The other is a seven-stage filter, every stage sampling texture 7 through a set of
its own made from `GX_TG_TEX0` (each its own matrix: a blur's taps), which needs seven mesh sets
for one attribute. The count of such materials worked out reached 300 a window in 32 windows,
about once a frame: likely the filter, if texture 7 is an EFB copy (a material reading one is
worked out again as the copy changes). Both are for later: a layout that lets sets from one
attribute share the mesh's set would take the second.

## 0.42.0 test 14: resolution scale and dynamic resolution

The user asked for the two settings that did nothing on the Vita: "Resolution scale" and
"Dynamic resolution" (F1 > Video > Quality). Aurora's frame buffer scale never reached the Vita's
renderer, which drew every frame into the display's back buffer at 960x544.

Now a scale other than 1x draws the frame into a framebuffer of its own at that size
(`vita_gl.cpp`, `WorldTarget`), and draws it to the display at the frame's end
(`draw_world_to_display`): linear, through the copy program as an opaque copy, or sharpened (CAS's
cross of five texels) where it is drawn up from fewer pixels and "Sharpen when upscaled" is on.
On the Vita a scale is a multiple of its screen (Aurora's `window.cpp` gives the Vita's sizes:
1x and Auto are 960x544; widths a multiple of 8, heights of 2; 0.25x to 2x, the slider to 2x),
so the settings saved so far (`render_scale=1`) draw exactly as before, on the display, with no
pass added. The draws' viewports and scissors, given in the screen's pixels, are scaled as they are
set; points are as wide in the frame's pixels; lines stay one pixel (GXM's wide lines hang the
GPU). The EFB copies read the frame's texture (vitaGL draws a framebuffer upside down: its first
row is the frame's bottom) and copy at the frame's scale; the depth kept across a frame's copies
is its framebuffer's (vitaGL's `framebuffer` object, mirrored at 104 bytes, gives its depth
surface). Its render target is sized for the display's 8 scenes a frame. A new size is made at the
next present (a stutter, and on that first frame the depth after its first copy is not kept),
and back at 1x the framebuffer goes. A size whose texture cannot be had (no video memory) is not
tried again till another is asked for: the frame stays at the screen's size, and the log says so.

Dynamic resolution needs the GPU's time a frame, and the Vita's GPU has no timestamp queries. But
every scene vitaGL sends ends with its fence (`query_fence`: the number of the last scene sent, and
the last the GPU has finished, written as its fragments are done), so the GPU is busy while the
two differ (`GpuClock`): the render worker looks every fourth draw, after each copy and around the
swap, and times the moments that changes; a frame finished while the render worker waited for the
next one is timed by the display callback (`vglSetDisplayCallback`, as GXM's display thread sees
the frame drawn). `GXPortGetGpuTimes` gives that busy time a frame over each half second. A scene's
vertex work while it is still being recorded is not seen, so it is mostly the pixels' time: what
the scale changes. And the controller (`port_dynamic_res.cpp`) counts the Vita's GPU as the limit
only while it is busy for most of each frame's own time too (`Sample::busyShare`): its CPU sets the
pace well below 60 fps while the GPU may be busy for more than 80% of a 60 fps frame's 16.7 ms.

The log's "presents:" line adds the GPU's busy time a frame and its share of the time, and the size
frames are drawn at; each new size logs a line and vitaGL's memory.

How to use it on the Vita (the settings as they were saved keep 960x544):
- Sharper: "Resolution scale" above 1x (dynamic resolution off). Each step costs GPU time.
- Steady: "Dynamic resolution" User, "Maximum resolution scale" 1.5x, "Minimum resolution scale"
  1x (never below the screen), "Frame rate target" 30: it supersamples where the GPU has time to
  spare at 30 fps and comes back down where it has not.
- Faster where the GPU is the limit: the same with the minimum below 1x (0.75x, say).

What to look for in test 14: at the saved settings, nothing changed (and the "presents:" lines
give the GPU's busy time: how much room there is); with a scale above or below 1x, the frame drawn
right (the HUD, the visor effects, the map, the pause screen, reflections, the morph ball's
shadow, the thermal and X-ray visors, all of which use copies); with dynamic resolution, its
`port: dynamic resolution` lines and the Video page's "GPU ... of ... ms a frame".

**Test 14** (4d1ac59, the user's log of Nov 9) played at the screen's size throughout (no scale
was set). The GPU's busy time came out at 5 to 19 ms a frame in most stretches and 34 to 42 ms in
the heaviest, 16% to 59% of the time: the GPU set the pace nowhere. Magmoor Caverns, the user's
"slow" (and not the lava fix's doing), ran 12 to 18 FPS in 188 of its 265 windows, bound by the
game thread: 67 ms a frame, the main thread busy 65.8 of them and the FIFO thread 38. The ticks
took 37 ms (2.5 a frame), and in them `SM: Think` 21 ms, of which `CPuffer` 11.5 (19 Thinks a
frame at 601 us each: some 7.6 Puffers a tick). Their cost is particles:
`CAnimData::AdvanceParticles` 13.8 ms (33 calls), `CElementGen::Update` 14.7 ms (276 updates),
and the `[particles]` line's 311 system steps a frame for only 1,316 particles, some 45 us a step
whatever its particles. A Puffer has fourteen gas jets (`GasJet01`-`14`, effects on its model),
and turns on only those facing away from where it moves (all of them off when it stands), but
`CParticleDatabase::UpdateParticleGenDB` updates every effect of a model, on or off, and a jet's
generator stepped in full every tick with nothing in it.

## 0.42.0 test 15: idle particle systems, and the system keyboard's text

**Idle systems.** `CElementGen::PortIsIdle` finds a system with nothing to step: no particle and
none to come this update (no emission, or past its lifetime, or spawned by code alone), no child
or per-particle system, no light, no shutdown countdown, no translation element to evaluate, no
motion blur, no Remastered frame; and a description that spawns no child at a frame (ICTS, IITS,
KSSM, IDTS, SSWH, SELC, the extra swooshes, Remastered's spawns), varies no system value, scale or
time scale, moves no emitter and has no fixed bounds. Such a system's steps change nothing but its
time and draw no random number, so `PortIdleUpdate` does what `InternalUpdate` would leave: the
same frame and time arithmetic step by step, the bounds and particle count reset, the frame the
children's update notes (`x84_prevFrame`), the port's step bookkeeping and the bounds built; then
`Update` goes on as before (KILL). The `[particles]` line counts those steps ("of systems with
nothing to step").

**The system keyboard.** The user: typing in the Port menu's fields (a suit colour's hex, the
teleport section's room filter) "doesn't register". The bridge (`UpdateSystemKeyboard` in
`ui_common.cpp`) put the keyboard's text in as key presses (Ctrl+A, the characters, Enter), and
took it only when the keyboard closed with its Enter key. Now its text stands however the keyboard
closed (Enter or its close button, as SDL's own Vita keyboard takes it), and goes straight into the
field's edit buffer (`ImGuiInputTextState`), filtered as the field's flags filter typing (decimal,
hexadecimal, upper case, no blanks), before an Enter that closes the field: ImGui copies the edit
buffer to the field's own before it lets the field go. The log has a line for each keyboard ("the
system keyboard closed (Enter or its close button)", "port: system keyboard closed with its text
(N bytes)", or that the field had closed meanwhile). And the room list no longer opens the keyboard
the moment it opens (as on Android): select its filter to type.

What to look for in test 15: Magmoor's frame rate (and the `[particles]` lines' share of idle
steps; `CPuffer`'s Think in `[think]`); typing in the menu's fields taking.

## 0.42.0 test 16: the catch-up, raised on the Vita

The user saw Magmoor in slow motion, as the Metroid Prime 2 port runs on their Galaxy S7. Both
ports slow the game when it cannot keep 60 ticks a second, rather than drop frames without end.
The MP2 port keeps Echoes' own rule (`src/MetroidPrime/main.cpp`: a frame over 35 ms runs one tick
and drops the rest of the time), so a phone short of about 28 FPS runs slow at once (a third of
the speed at 20 FPS). This port catches up with whole ticks but stops a frame's ticks once they
have taken two periods (33 ms) of it, upstream's guard against the spiral of issue #54
(`CGameArchitectureSupport::UpdateTicks`). In test 14's Magmoor a tick took some 15 ms, so 2 or 3
fitted in a frame where real time wanted 4 or more: 2.5 a frame at 15 FPS, the game at 62% speed.

"Catch-up" (`catch_up`, F1 > Video > Frame rate, 2 to 8 frames' worth) now sets that bound, 4 by
default on the Vita (67 ms), 2 elsewhere (upstream's) and under Original experience. At 4, a frame
of Magmoor's 15 ms ticks takes up to 5 of them: nearer the game's own speed, and fewer frames a
second (about 10 at test 14's costs); as test 15 makes the ticks cheaper, more of them fit in both
the bound and real time. Where ticks keep up (most rooms) nothing changes.

What to look for in test 16: Magmoor's speed (the `[split]` lines' ticks a frame against its
frame rate: 60 ticks a second is full speed; F1 > Video > Frame rate shows "Simulation N ticks/s")
and whether the lower frame rate there is worth it; "Catch-up" back at 2 gives test 15's behaviour.

**Test 16** (48bdb8b, the user's log of Nov 10: Phendrana, then Magmoor): at 4, Magmoor ran 5
ticks a frame (2.5 before) at 13.6 ms each, frames of 109 ms in 162 of its 264 windows (about 9
FPS), the game near 77% of its speed. Test 15's idle systems were 23 of 718 system steps a frame:
the Puffers' gas generators are not idle (their particles, or emission), so it gained little
there. `CPuffer`'s Think: 615 us a call (601 in test 14). The particle zones (the particles'
update, creation, bounds, the port's mover) held 9 of `CElementGen::Update`'s 32.6 ms a frame;
some 35 us an update was outside them. The user: "you don't need to add catch-up in the settings,
let's still optimize the game though".

## 0.42.0 test 17: the clock without locks, the particles without stopwatches

**The setting goes**; the Vita's catch-up stays at 4 (`PortDebug::kCatchUpFrames`), 2 elsewhere
and under Original experience; a saved `catch_up` is read and dropped.

**OSGetTime.** Every `CStopwatch` reads `OSGetTime`, which read Aurora's game clock
(`aurora::time::game_clock::now`) under a `std::shared_mutex` read lock: libstdc++'s
`pthread_rwlock`, and vitasdk's pthreads are kernel mutexes and semaphores (`sceKernelLockMutex`),
for every call, a frame's thousands. And `OSGetTime` passed through `std::call_once` each time
(its thread-local callable, `pthread_once`). The clock's readers now take no lock: its writers (a
scale or pause change, a reset) still hold a mutex and step a sequence number around their change,
and a reader reads the fields between two looks at the sequence, again if a writer came between,
or waits on the writers' mutex while one is under way. `OSGetTime` checks an atomic flag before
`std::call_once`.

**The particles' stopwatches.** `CElementGen::InternalUpdate` and `Render` each timed themselves
into `x2cc` and `x2d0`, which nothing reads: two `OSGetTime` calls an update and a render (some
1,400 a frame in Magmoor). Left out on the port (kept for the retail build). The particle scopes'
depth and fog state (`PortFx::UpdateScope`, `RenderScope`) were `thread_local`, which is emulated
TLS on the Vita (`__emutls_get_address`, a lookup an access, two an update); they are plain statics
now, as CElementGen's own update depth is (particles update and render on the game thread alone).

The log's start has a line of what these cost on the Vita ("clock costs (us a call...)": OSGetTime,
steady_clock, sceKernelGetProcessTimeWide, a shared_mutex read lock, a mutex lock, a thread_local
access), so the old cost is on record beside the new.

What to look for in test 17: that line; Magmoor's `CElementGen::Update` and `CPuffer` Think against
test 16's (32.6 ms a frame, 615 us); its frame rate and ticks a frame.

**Test 17** (fc4f92b, the user's log of Nov 14: Magmoor, then Phendrana). The clock costs, in us a
call: OSGetTime 7.47, steady_clock::now 5.66, sceKernelGetProcessTimeWide 0.70, a shared_mutex read
lock and unlock 0.73, a mutex 0.28, a thread_local access 0.48. Magmoor: 32% of its windows under 12
FPS (61% in test 16); in those at 0 to 18 FPS a frame ran 4.65 ticks, `CElementGen::Update` took
13.0 ms a frame for 572 updates (23 us each; 32.6 ms in test 16) and `CPuffer`'s Think 304 us (615).
The game thread is still the limit (at 0 to 12 FPS it was busy 97 of a frame's 98 ms, the FIFO
processor 33). The thread log: no thread ever on core 3; the texture worker and the program
compiler, free to float, moved between cores 0 to 2 thousands of times a minute. The user: "the
Vita's 4th core barely goes above 20%, which means we're not taking full advantage of the
CapUnlocker plugin that unlocks it".

## 0.42.0 test 18: the fourth core

The kernel keeps core 3 for the system: it refuses a game thread that core's mask, and a thread
left to float (mask 0) is placed on cores 0 to 2 alone. CapUnlocker patches that check
(`isIllegalAffinity` in SceKernelThreadMgr) to pass, so a thread may ask for core 3; none of the
port's did, so the core ran the system's own work (the ~20% the user saw) and nothing else.

**What can go there.** The game is one thread, as on the GameCube: a tick's entities, physics and
particles, then the frame's draw, each step reading what the one before wrote. Only work whose
results that thread does not read itself can run beside it. In test 17's heaviest windows:

- CPU skinning, 2.4 to 2.9 ms a frame (5 to 8 models, ~350 us each). The posed vertices are read by
  the FIFO processor alone: the game hands GX their address, and the processor reads them when it
  draws. This test moves it.
- The particles' updates, 13 ms a frame in Magmoor. They share the particle globals
  (`CParticleGlobals`), the global random number generator, the token counts and the allocator with
  the rest of the game thread, so that state must be made per thread before they can move: a later
  step, not this test.
- The audio mix (~1 ms a frame in play): the mixer is not thread-safe, and the port's
  `OSDisableInterrupts` (its critical sections on the GameCube) does nothing.
- The input polls (~4 ms): on a thread of their own they would read the pad a frame late.

**Test 18.**

- `thread::Options::vitaCore` takes 3 (`thread::kVitaSystemCore`); without CapUnlocker a thread asking
  for it takes `vitaCoreFallback` instead (-1: it floats, as before). Whether core 3 is open is asked
  once, by moving the first thread that wants it there and back, and logged ("core 3 (the system's)
  is open to the game (CapUnlocker)..." or "...is closed to the game...").
- The program compiler and the texture workers keep to core 3: a new build's warm-up compiles on a
  core of its own, not in the game's and the FIFO processor's spare time, and the texture worker no
  longer takes the FIFO processor's core while it waits.
- The side worker (`extern/aurora/lib/vita_side_jobs.cpp`; `GXPortSideJob*` in `GXExtra.h`): one
  thread on core 3 (priority 128, above the compiler's and the texture workers' 191) that runs the
  jobs the game hands it, one at a time, in order. CPU skinning is its first: `CSkinnedModel::
  Calculate` builds the bones' transforms as before, copies them (every instance of a model shares
  its skin rules' bones, and the next instance rewrites them), and queues the points and normals
  (`CSkinRules::PortSubmitPointsAndNormals`: the same arithmetic, so the same bits). The model's
  draws write `GX_AURORA_PORT_SIDE_JOB_FENCE` before themselves, and the FIFO processor waits there
  until the job has run; the game thread waits itself only where it must (a display list being
  recorded, the workspace written again or freed). On the game thread still: models a morph, a
  point generator or the verify run reads at once, the swarms' shared workspaces, models under 64
  points, and those the helper threads split (`MP_SKIN_THREAD_MIN`). Without CapUnlocker there is
  no side worker, and every model skins as before.
- Aurora's native clock, behind every OSGetTime, reads `sceKernelGetProcessTimeWide` on the Vita
  (0.70 us a call, against steady_clock's 5.66).

The log has a `[side jobs]` line beside `[threads]`: the jobs a second, the worker's share of core 3,
and how often and how long the FIFO processor or the game thread waited for one. A job that never
ran (none should) is logged as an error after half a second, and its waiter goes on.

What to look for in test 18: the "core 3" line at the start; `[threads]` showing the program
compiler, the texture worker and the side worker on core 3 (mask 0x80000); the `[side jobs]` line's
FIFO waits (few and short: the vertices were ready before their draws); the game thread's
`CSkinnedModel::Calculate (CPU skinning)` zone (2.4 to 2.9 ms a frame in test 17) beside
`CSkinnedModel: skinning (side worker, core 3)` in `[zones] other`; how long a new build's warm-up
takes; and skinned models drawn whole (a torn or exploded one would be a draw that read its
vertices before its job ran).

**Test 18** (bdb5ef1, the user's log of Nov 15: Magmoor, an elevator ride). A clean 30-minute run.
Core 3 was open; the program compiler, the texture worker and the side worker kept to it (mask
0x80000, no moves). The side worker ran 20 to 124 jobs a second, none hung; the FIFO processor
waited for 5 to 20% of them, under 2% of its time, except in the elevator (a big model's jobs of
~3 ms: 17%). The game thread's skinning went from 2.7 to 0.2 ms a frame; Magmoor's windows under 18
FPS from 58% to 48% of the time. The warm-up did not shorten (some 890 programs at ~1.3 s each,
~23 minutes): the compiler had had an idle core during it before too; every new build deletes the
last one's binaries (tallon25), and that is the time. The user: "4th core is around 30% and when I
was at the elevator, it went up to 40%". Every helper thread woke some 1,400 times a second, the
side worker too with 25 jobs a second, as in test 17: not looked into yet.

## 0.42.0 test 19: the actors' particles on the fourth core

With the catch-up, a millisecond off a tick is about five off a Magmoor frame (4.8 ticks a frame).
A tick there was ~11 ms, ~2 of it the actors' particle systems (`CAnimData::AdvanceParticles`:
`CParticleDatabase::Update`, 8.5 ms a frame; the Puffers' fourteen gas jets each).

**The update in three parts.** Within the window CStateManager::Update opens around its entities'
Think, `CParticleDatabase::Update` runs its systems' transforms and lifetimes on the game thread as
before, hands the systems' own steps (`CParticleGenInfo::PortUpdateSystem`: the system and its
children, nothing else) to the side worker, and leaves the rest (`PortUpdateRest`: the lights the
systems feed; then the finished systems taken out and the times kept) for the game thread: before
any other method of that database runs (each finishes it first), or by the drain at the end of the
tick (`PortFinishAll`, in the order the updates began). The parts keep the whole's order system by
system; what moves is only that a system's lights and removal now wait for all of its database's
steps. The destructor waits for a running step. Without the side worker every update runs whole.

**What the steps share, made safe for two threads.** The particle code keeps its state between
calls in statics, which the game thread's own particle code (effects, projectiles, the draw) uses
meanwhile. Each now holds a value per thread (`platform/include/port_thread_slot.h`: a thread's stack
pointer tells the side worker apart, a compare; the two values in cache lines of their own), read
and written as the statics were, so the decompiled code is unchanged: `CParticleGlobals` (the
particle, system and lifetime tweens being evaluated), `CRandom16`/`CGlobalRandom` (the current
generator), `CPortVarGlobals`/`CPortKeepGlobals`, the seeds new generators, swooshes and electrics
take, the update and scope depths. The live counts (the 0xa00 particle cap among them), the
per-particle seed counter and the stats are atomic, and so are the token counts (`CObjectReference`):
a step that spawns a child copies and locks its description's tokens (never the last reference: the
parent's description keeps every child's loaded, so nothing loads or unloads off the game thread).
The allocator has its lock and `PortFx`'s generator list its mutex. Left shared: a few elements'
own caches (sample-and-hold, a cone's basis), whose race could only misplace a particle.

The log's `[particles]` line adds the actors' updates stepped on the side worker a frame and those
the game thread waited for (and how long); `[zones] other` has "CParticleDatabase: systems stepped
(side worker, core 3)", the main list "SM: particle updates finished (side worker drain)".

What to look for in test 19: Magmoor's frame rate and `CAnimData::AdvanceParticles` (8.5 ms a frame
in test 18's heavy windows) against the side worker's zone; the `[side jobs]` busy share; the
waits in `[particles]` (few and short: the steps were done before anything read them); particle
effects that look as before (the Puffers' gas, lava, sparks; a misplaced or missing one would be a
step that read something the game thread changed); and no crash.

## 0.42.0 test 20: the shaders kept from build to build

Test 20 is test 19 with this alone. Since tallon25 a new build deleted every program binary the
last one left on the card ("The map's GPU crash: stale shaders"), so every build began with the
warm-up compiling some 890 programs again, ~23 minutes (test 18). The user chose to keep them. A
binary's name is its key, a hash of both its sources seeded with the binaries' format
(`kProgramBinaryVersion`, 3 since before tallon25): a build whose generated sources are the same
reads the same files, and one that generates others asks for other keys and leaves the old files
unread. `vita_programs/build` now names that format ("binaries 3"); binaries of another format, or
on a card with no such file (from before tallon25), are still deleted. The first run of test 20
finds a build's revision there (tallon25 to test 19 wrote theirs), keeps the binaries and rewrites
the file: "program binaries on the card from build ...: N kept".

What to look for in test 20: that line at the start, and no warm-up (the "Compiling shaders" toast
for a moment at most); the map and every room drawn as before (the stale-shader GPU crash came back
would be a crash on the map with this build's binaries kept); and everything test 19 asks.

**Tests 19 and 20** (ee966e7, then 5f60013; the user's log of Nov 16, test 20, 4.75 minutes: the
title and file select, Magmoor, the map). Test 20 kept test 19's 935 binaries; its first frame came
5.5 s after the main loop began (100 s in test 18) and nothing compiled. The user: "the map doesn't
crash". The side worker ran up to 787 jobs a second, busy up to 21% of core 3, none hung. In
Magmoor's heaviest windows (12 to 14 FPS) 52 to 83 actors' updates a frame stepped there
(`CParticleDatabase: systems stepped`, 15.8 ms a frame on core 3 for 815 system updates);
`CAnimData::AdvanceParticles` on the game thread fell to 3.2 ms a frame (8.5 in test 18), but the
drain at each tick's end waited 4.8 of them (1 ms a tick): the side worker had only the rest of the
tick after the last Think to finish. The user: "the main menu save select screen lags, the map too
the bigger it gets. Our goal is still trying to [get] the game to run at 30+FPS".

- The title and file select: the THP video's decode on the game thread, 8 to 52 ms a frame.
- The map: the FIFO processor's, busy 98 to 141 ms a frame for some 4,300 draws (1,300 surfaces,
  each its fills and its outlines with a colour of its own, sorted back to front; the outlines drawn
  twice when thick, both passes a pixel wide on the Vita), the render worker ~50 ms for the 2,700
  left after joins, the GPU 12 to 15. The game thread waited for them (begin_frame 34 ms). No speed-up
  of a draw reaches 30 FPS there: it takes far fewer of them (a later step).

## 0.42.0 test 21: the movies on the fourth core; the particle drain once a frame

**THP video.** `CMoviePlayer::DecodeFromRead` copies a frame's video component (with the 32 spare
bytes its bit reader reads past the scan) and decodes it on the side worker into the texture slot
(`PortDecodeJob`; the decoder keeps its state on its own stack). The audio component decodes on the
game thread as before. `Update` moves the picture to a slot only once its decode has run
(`PortSlotDecoded`), else the frame shown stays and its time stays owed, as when a decode fell
behind before; a slot's next decode waits for its last, and `Rewind` and the destructor wait for
them all before the planes go. Without the side worker every decode runs here, as before.

**The particle drain.** The actors' particle steps a frame's ticks left on the side worker are
finished once, as `CMFGame::Draw` begins ("frame: particle updates finished (side worker drain)"),
no longer at the end of every tick: a database the next tick (or the draw) reads first finishes its
own first, as every method does. The side worker gets the whole frame's ticks for its steps.

What to look for in test 21: the title and file select screens' frame rate (the video itself plays
no faster than core 3 decodes it, ~25 to 30 frames a second); `movie: THP video decode (side
worker, core 3)` in `[zones] other`; Magmoor's frame rate and the `[particles]` waits against test
20's (4.8 ms a frame); the movies' pictures in step with their sound.

**Test 21** (53e62eb; the user's log of Nov 16, ~12 minutes: the title and file select, Magmoor,
Phendrana, fights). The user: "The main menu running near full speed is a huge improvement", and
"There are slowdowns during combat though".

- The title and file select ran 39 to 123 FPS (11 to 50 in test 20).
- The drain waited at most 1.5 to 2 times a frame, 3 ms at most (4.8 ms a frame in test 20).
- Magmoor ran mostly 12 to 22 FPS, 3.7 ticks a frame at ~10.5 ms each.
- Phendrana stayed under 12 FPS, held back by the FIFO processor (98 to 113 ms a frame; its
  immediate draws 32 to 39 of them, the snow's ~27,000 vertices a frame in 64 draws). The game thread
  took 82 ms a frame.
- The worst fight, against flying pirates, ran ~2 minutes at 8.6 to 11 FPS, the game itself at 43% to
  85% of its speed:
  - each projectile's Think took ~750 µs (13 alive, ~10 ms a tick);
  - the physics moves (`MovePlatforms + MoveDoors`) took 5 ms a tick;
  - the game thread ran only 61% of the time while it held the frame rate back, and waited ~450 times
    a second (71% to 78% busy in calm Magmoor).
- Magmoor's fights against its calm stretches cost 1.5 ms more a tick. Part of it is the particles
  the game thread still updates itself (explosions, effects, projectiles): 1.16 ms a tick against
  0.30.

The waits have a cause in MusyX's PC mixer (`extern/musyx/src/musyx/runtime/hw_pc.c`):

- `snd_handle_irq` mixes every voice (ADPCM decode, resample, nine volume ramps) holding MusyX's lock
  (`hwIRQEnterCritical`, an SDL mutex): ~2 ms of each 5 ms frame at 30 voices.
- Every sound call of the game's takes that lock (`hwDisableIrq`): emitter updates, pitch bends, starts.
  A fight has the most voices, so the longest mix, and the most calls.
- `CSfxManager::PitchBend` runs the whole `CSfxManager::Update(0)` for an emitter that is not playing,
  and every projectile with a sound calls it every tick.
- SDL's audio thread floats on cores 0 to 2 at priority 159, just above the game's threads, and polls
  the device every millisecond.

## 0.42.0 test 22: the sound's mix off the game thread's way, on the fourth core

**The mix in three parts** (`salCtrlDspControl`, `salCtrlDspRender`, `salCtrlDspFinish`):

- **The control pass**, under the lock as before, settles in the whole mix's order what the game and
  the synth change: the break cleanup, a new voice's start, each subframe's control step and ADSR,
  the volume ramps. It copies what the render reads (the sample, its ADPCM coefficients and loop
  state, each subframe's pitch and envelope, the filter). The `changed` bits it read are cleared with
  it, before the lock opens: one a sound call writes meanwhile is kept for the next frame, as when the
  call waited.
- **The render** decodes, resamples and mixes the voices with the lock open, as the GameCube's DSP
  mixed the parameter blocks the CPU had built. It reads samples from aurora's ARAM buffer and writes
  the studio buffers MusyX made at its start, both kept for the run.
- **The finish**, under the lock again, writes the play positions back and retires a voice whose
  sample ran out. A voice the game stopped or started again meanwhile keeps nothing but its sound in
  this frame. The finish then runs the studio inputs, the aux effects (reverb, whose settings the
  game changes under the lock) and the downmix.

The output is the whole mix's. `MP_AUDIO_LOCKED_MIX=1` keeps the lock through the render, as before.

**The mixer on the fourth core.** On its first frame the mixer's thread (SDL's audio device thread)
moves to core 3 when CapUnlocker opens it (`MusyxPortOnMixThread`, `GXPortKeepThreadToFourthCore`)
at priority 127, above the side worker's 128: a long side job, a movie frame's decode, must not hold
the mix past the device's buffer. It is named in `[threads]` ("MusyX mixer (SDL audio)").

**The `[audio]` line**, each second after `[particles]`:

- the frames mixed and the voices in each;
- the mixer's time holding the lock and rendering;
- the other threads' lock takes, and their waits for it (count, ms in all, the longest);
- the mixer's core.

**The fights, measured.**

- `[zones]` time a projectile's Think in parts (`projectile: movement`, `collision check`, `near
  list`, `the near list's actors`, `hit`, `particle effects`, `light`, `nested lights`, `sound`) and
  `sound: CSfxManager::Update`.
- The `[think]` lines add `Move` (each physics actor's `CGameCollision::Move`) and `Touch`
  (`CrossTouchActors` per actor) by class, 20 rows.

What to look for in test 22:

- The `[audio]` line: the other threads' waits near 0 ms a second, the mixer on core 3.
- The sound as before: no crackles, no sound cut short or left playing (the charge beam's hum, rapid
  fire, the music, the movies).
- The fights' frame rate against test 21's, and the game thread's busy share in `[threads]`.
- `[side jobs]`: the FIFO processor's waits, since the mixer now runs above the side worker on its core.
- Where a projectile's 750 µs go.

**Test 22** (bc13bf5; the user's log of Nov 16, 6.5 minutes, 11,281 frames, a clean exit: the
menus, Magmoor, a flying-pirate fight). The user: "It's good to see that the 4th core is being
utilized".

- The mixer kept to core 3 (`[audio]`: "mixer on core 3"), busy 26% to 50% of it: renders of 166 to
  367 ms a second with the lock open, the lock held 67 to 103 ms a second (its control pass, the
  reverb, the downmix, the synth's own passes).
- The other threads took the lock 150 to 600 times a second and waited for it 0.5 to 5 ms a second
  in all. The game thread ran 78% to 92% of the time in play (61% to 78% in test 21).
- The flying-pirate fight kept the game's speed: 55 to 61 ticks a second (26 to 51 in test 21's),
  9 to 20 FPS, mostly 11 to 15 (8.6 to 11). A projectile's Think took ~100 µs (~750 in test 21):
  `UpdateParticleFX` 80 of them, the collision check 13, the movement 6.
- But the mixer at 127, above the side worker, made its jobs wait:
  - Magmoor's `[particles]` waits grew to 8.0 ms a frame (0.73 in test 21), the drain to 2.8 (0.29);
  - in the fight they reached 6 to 32 ms a frame;
  - the FIFO processor waited for the side worker 27 to 100 ms a second (~4 in test 21).

## 0.42.0 test 23: the fourth core's order; the mixer's ramps in 32 bits

**The fourth core, in order.**

- The side worker first (128): the FIFO processor and the game thread wait for its short jobs.
- MusyX's mixer next (129).
- Then the side worker's background lane (130), a second thread for long jobs nobody waits for soon:
  the movies' video decode, 15 to 40 ms a frame (`GXPortBackgroundJobSubmit`, `Done`, `Wait`; its job
  numbers are its own). A decode can no longer hold the mix past the audio device's buffer, and the
  mixer no longer holds the side worker's jobs back.
- Then the helpers (the program compiler, the texture workers).

`[side jobs]` gets a second line for the background lane.

**The mixer's ramps.** A voice's nine volume ramps of a subframe (`salMixRamps`) take
`mixRampChannel`'s arithmetic, the envelope worked out once for all nine. When every decoded sample is
within [-32768, 32768] and the ramp within ±65535 over the subframe, `vol * env` and `src * mixVol`
stay below 2^31, so the 64-bit steps run in 32 bits with the same results; otherwise the ramp takes
`mixRampChannel` itself. Checked on the PC against the old code, bit for bit: 400,000 subframes with
edge cases, 2.4 million ramps on the 32-bit path, no difference.

What to look for in test 23:

- `[particles]`: waits back near test 21's (under 1 ms a frame).
- `[side jobs]`: the FIFO processor's waits back to a few ms a second.
- `[threads]`: the mixer's busy share against test 22's 26% to 50%.
- The sound clean everywhere, the movies' screens above all (the title, the file select).
- The fights' frame rate against test 22's.

**Test 23** (1cf1033; the user's log of Nov 16, 4.5 minutes, 8,429 frames, a clean exit: the menus,
Magmoor, a flying-pirate fight).

- The fourth core's order held:
  - the game thread's `[particles]` waits were 0.30 ms a frame (0.39 in test 21, 6.27 in test 22);
  - the drain took 0.30 ms a frame;
  - the FIFO processor waited for side jobs a few ms a second;
  - the background lane decoded the movies at 24 to 29 frames a second.
- The mixer ran 23% to 43% of core 3, little less than in test 22 with as many voices (12.7). Its
  ramps are not where its time goes: the decode and the resample are (per sample, `int64` steps
  through memory the compiler cannot keep in registers).
- Median FPS by kind of room (tests 21, 22, 23):
  - Magmoor with Puffers: 17.4, 17.2, 20.2;
  - Magmoor with Triclops: 22.0, 29.1, 27.9;
  - Metaree rooms: 22.3, 28.4, 25.7;
  - the flying-pirate fight: 9.7 (the game at 75% of its speed), 14.3, 16.7 (full speed; 7 windows).
- The game thread ran 88% to 96% of the time in play. In 12 to 30 FPS windows, per frame:
  - its ticks took 24.8 ms (2.85 ticks: Think 8.5, physics moves 2.3, touches 1.9, PreThink 1.8,
    the HUD's update 3.0);
  - its draw took 13.2 ms (the world 7.5, particles 2.2, the HUD 1.9);
  - the particle updates it still does itself took 2.6 ms;
  - `begin_frame` waited 7.2 ms for the FIFO processor to end the last frame.
- The FIFO processor was busy ~33 ms a frame (39 timed) for ~710 draws. Its sections: state 5.2,
  record 5.1, immediate 4.4, material 4.0, rows 3.8, skinned 3.3, aurora 2.7, dispatch 2.2, other
  draws 1.9, skinned copy 1.4, lookup 1.3, mesh 0.9.
- Of the ~560 immediate primitives a frame, ~460 were sent outside every zoned stage of
  `CMFGame::Draw` (~5 vertices each): the minimap's surfaces.

The user chose the graphics thread before the effects for the next step ("Graphics thread first").

## 0.42.0 test 24: the map's surfaces in fewer draws; the immediate vertices decoded faster

**The map's surfaces** (`CMapArea::CMapAreaSurface::Draw`) sent each fill primitive (strip, fan,
triangles) and each outline strip as a GX primitive of its own, the outlines twice when thick. That
made some 460 draws a frame of the minimap, in every room, and the map screen's ~4,300.

- A surface's fills now go as one `GX_TRIANGLES` primitive and its outlines as one `GX_LINES` (each
  pass, when thick).
- The triangles are the ones Aurora makes of each primitive, in its order (`triangle_order`: a
  strip's odd ones turned, a fan's about its first vertex, a quad's two), and the lines are a
  strip's segments, which it draws a strip as.
- What is drawn is the same. Checked on the PC by running both through Aurora's triangulation:
  100,000 surfaces, 1.2 million triangles and 780,000 segments, no difference.
- A surface with fills of another kind is sent as it was.

**The immediate vertices** (`decode_vertices`).

- A primitive whose attributes all sit in the list (the particles, the snow, the HUD) is decoded by
  operations worked out once for it (`plan_direct`, `decode_direct`): each vertex starts from one
  made once, and the common layouts are read with straight code (positions f32 or s16 x3, colour
  RGBA8, coordinates f32, s16, u8 or s8 x2). The others go through `read_components` and
  `read_color`, as before.
- The bounds come to one check, the last vertex ending by the list's end.
- Checked on the PC against `decode_vertex`, value for value: 200,000 random layouts, 4.1 million
  vertices, under the address and undefined-behaviour sanitizers too.

A zone `HUD: the map (CAutoMapper::Draw)` times the minimap and the map screen, and counts their GX
primitives in `[gxsrc]`.

What to look for in test 24:

- `[gxsrc]`: the map's primitives against the ~460 `CMFGame::Draw` had (counted under the new zone).
- The FIFO processor's `immediate` and per-draw sections, and `draws a frame`.
- Magmoor's and Phendrana's frame rates against test 23's.
- The map screen's frame rate (~10 FPS before).
- The minimap and the map drawn as before: the rooms' fills, their outlines, the thick ones.

**Test 24** (34f3537; the user's log of Nov 16, 12 minutes, 20,223 frames, a clean exit:
Magmoor, a flying-pirate fight, a snowy area, the map screen).

- Median FPS by kind of room (tests 23, 24):
  - Magmoor with Puffers: 20.2, 21.4;
  - Magmoor with Triclops: 27.9, 29.2;
  - Metaree rooms: 25.7, 28.9;
  - the flying-pirate fight: 16.7, 16.9 (the FIFO processor busy 52 ms a frame, 62 in test 23).
- The minimap's primitives (now `HUD: the map`) fell only 9% to 26% in the same kinds of rooms
  (Triclops 393 to 319 a frame, Metarees 377 to 277), and its vertices grew by a third to three
  fifths (a strip's corners as a list's). Most surfaces had one fill and one outline already: the
  minimap is still a fill and an outline a surface, 230 to 320 draws a frame, each surface its own
  colour.
- The map screen: 2,600 to 5,400 primitives a frame (22,000 to 66,000 vertices), 10 to 22 FPS.
- The immediate decode, per vertex with its draws' share: 0.63 µs in play (0.99 in test 23). In the
  snowy windows it was 0.54 µs, against 0.78 in test 21's Phendrana; those windows ran 20 FPS, the
  FIFO processor busy 50 ms a frame, but not in test 21's rooms.
- From 18 to 31 FPS the game thread and the FIFO processor were each busy about the frame's length
  (18 to 22 FPS: 47 and 51 ms).

Fewer draws for the map need its surfaces merged across surfaces, each vertex carrying its surface's
colour. To keep fills and outlines in one draw, the outlines must either become thin triangles in
the original order, or be drawn after the fills: a look question for the user.

## 0.42.0 test 25: the map in one draw

The user chose the exact look: every fill and outline kept in its original order, the outlines
drawn as thin triangles.

**The batch** (`GXPortDrawBatch`, `dolphin/gx/GXPortBatch.h`; the Vita path only).

- `CMapWorld::DrawAreas` sorts all the surfaces ahead of the doors and objects. That whole run now
  goes as one batch (`CMapWorld::PortDrawSurfaceBatch`): each surface's fill primitives, then its
  outline strips once for each pass, in the colours the loop gives them (`PortSurfaceColor`, one
  function for both, and `CMapAreaSurface::PortBatch`).
- The positions are the areas' own arrays, kept as `GXSetArray` keeps them. Each area's transform
  rides with its array; the plane's projection is the draw's model matrix.
- The FIFO processor (`vita_draw.cpp`, `draw_batch`; geometry in `vita_batch.hpp`) makes one draw of
  it, more only past 65,535 vertices. The map screen's ~64,000 come to one or two.
- The triangles are `triangle_order`'s. A run of one area's primitives in one colour shares its
  vertices.
- The colour is the vertices'. The batch's own material is one TEV stage passing the unlit vertex
  colour and alpha on (the map's own passed a TEV constant on, set before each draw). Blending, depth,
  culling, alpha test and fog stay the game's. The batch puts GX's TEV and channels back for the
  draws after.

**The outlines** are quads one pixel of the frame wide, half a pixel to each side across the line's
major axis. That is the parallelogram GL draws a one-pixel line as, and the Vita's lines are all a
pixel since tallon21.

- Each corner is the end's position plus a step in model space that moves the clip position by half
  a pixel, its w kept. The depth stays the line's, under a perspective or an orthographic view.
- Where a strip keeps its axis, the corners are shared and the quads meet edge to edge, as GL's strip
  pixels do. Where it turns, they overlap or leave a gap of a pixel at most, and a line's last pixel
  follows the quad's edge rule, not GL's.
- Each quad goes in both windings under GX's front or back culling (the culling keeps one), in one
  with none. Under `GX_CULL_ALL` the lines go alone, unculled, since GX culls no line.
- The width follows the frame's pixels at the resolution scale asked for (`vita_gl::frame_rect`). A
  scale changed while a frame is recorded could leave that one frame's lines a hair off.

**Checked on the PC** (`~/scratch/batchtest`; the header's own code, under the address and
undefined-behaviour sanitizers too).

- The fills against `triangle_order`, value for value: 6,944 primitives, 89,348 triangles.
- Chunks past 65,535 vertices, order kept: 257,758 vertices in 4 draws.
- The outline quads carried through random views as the GPU would, against the reference
  parallelogram made in window space. The views were perspective and orthographic, and the
  minimap's kind of model matrix (a camera's inverse squashed 0.001 along its depth).
- Over 430,000 segments and 128 million pixels, the quads missed 1 to 5 pixels in 100,000 and added
  as many, each a pixel centre within a thousandth of a pixel of an edge. No pixel was drawn twice
  by one quad.
- The corners sit within about a thousandth of a pixel of where they should (1.06e-3 at most), w
  within 3e-6 of the end's.

`MP_MAP_BATCH=0` sends the surfaces as test 24 did (the PC's environment; the Vita sets its own).
The universe view's worlds (`CMapUniverse`) are still a draw a surface.

What to look for in test 25:

- `[gxsrc]`: `HUD: the map` should fall from ~300 primitives a frame (test 24's minimap) to the doors'
  and icons', and the map screen's from 2,600 to 5,400 likewise.
- A new `batches:` line: primitives, triangles, lines, vertices and draws.
- The map screen's frame rate (10 to 22 FPS in test 24), and the rooms' against test 24's.
- The map and minimap drawn as before: the rooms' fills, their outlines, the thick ones on the map
  screen, the colours, the selected and flashing rooms. A line's end or corner may differ by a pixel.

**Test 25** (c647ad0; the user's log of Nov 15).

- The map "looked like it was glitching", and the user asked for it back as it was. The batch is
  reverted; the map is drawn as test 24 drew it. c647ad0 keeps the batch, should it come back.
- The log shows the batch working as built: every frame's map in one draw (`HUD: the map` 50
  primitives a frame against test 24's ~300), no primitive left out, none failed. What looked wrong
  is not in the log, and there is no photo.
- A suspect for a later look: the minimap is drawn with a GEQUAL depth test against its frame's mask,
  0.02 units apart, squashed into the HUD's 1/512 of the depth range. The batch's positions are
  rounded differently from the GPU's (each area's transform applied on the CPU), and its outline
  corners are stepped far along the squashed axis. Either could flip that test for some pixels.
- The speed it gave, against test 24's medians:
  - the map screen: 28 to 33 FPS (10 to 22);
  - Metaree rooms: 36.4 (28.9);
  - Magmoor with Puffers: 24.4 (21.4);
  - Triclops and the pirate fight: about the same.

## Upstream 0.46.0 (0.46.0 test 1)

The user sent `MetroidPrimePort-port_source_code.zip`, the original repository being gone, and asked
to move to 0.46.0 if that is what it holds. It does, by way of a fork:

- It is GitHub's archive of a fork's branch `port` (commit d23b2c4, Nov 13). Its release notes run
  through Odrannnn/MetroidPrimePort's 0.43.0 to 0.46.0 (Nov 8 to 12, each linking upstream's
  release); versionName 0.46.0, versionCode 54 (0.42.0: 50).
- The fork adds French translations of the port's menus, shown when the system's language is
  French (`translations/fr.json`, `platform/port_translate.cpp`, `cmake/PortTranslations.cmake`; its
  README's note), and its own CI workflow. A copy of the zip is kept in
  `~/upstream/zips/MetroidPrimePort-0.46.0-d23b2c4.zip`.

Branch `port-046` (worktree `~/mpport-046`, build dir `build/vita`): `upstream-046` is 0.42.0's
commit (50b149f) with the zip's tree over it (5e3ddd1, `.vscode` left out, as 0.42.0's was), and
port-042 (b527787) merged into that (264af01). `~/mpport-042` stays as it was, for a fallback.

**The merge.** 0.42.0 to 0.46.0 changed 291 files (113 added, 4 removed). 47 of them the Vita work
had changed too; 10 conflicted:
- `input.cpp`/`hpp`: 0.46.0's first-port moves (choose_first_port's give and take,
  take_first_port) set pad indices through the Vita's `set_gamepad_player_index`, so the player
  index copies go with them (0.42.0 test 7).
- `window.hpp`: 0.46.0's `joystick_update_ns` and the Vita's `pump_generation`, both. 0.46.0 now
  calls `SDL_UpdateJoysticks` itself in `poll_events`, timed, where SDL's pump called it.
- `port_echoes_suits_import.cpp`, `settings.cpp`: the Vita's large-stack import thread and its
  settings taken back from `.tmp`, with 0.46.0's `PortUtf8::FromUtf8`.
- `port_textures.cpp`, `make_prompt_glyphs.py`: the Steam Deck's icon set and the Vita's, both.
- `ui_common.cpp`: the Vita's timed zones around 0.46.0's new per-frame calls.
- `video.cpp`: the Vita's resolution scale stays at most 2x; 0.46.0's 8x elsewhere.
- `search.cpp`: 0.46.0's translated labels and places, after the Vita's hidden rows and pages.

**For the Vita.**
- 0.46.0's Steam Controller and Deck reader (`PortSteamTriton`) runs on Linux alone: nothing on the
  Vita. Its back-button setting is hidden there, and now its search row too.
- 0.45.0's "the pad used last takes port 0" is left out: the Vita has its one pad, on port 0
  already, and the check is an SDL call (some 20 us) each frame a stick moves.
- The VPKs still carry the Vita's prompt icons alone (0.46.0 adds the Steam Deck's).
- The overlay's font: 0.46.0 merges in a system font with Cyrillic where it finds one. The Vita has
  none, so the default font stays, with a log line saying so.
- 0.44.0 logs a frame over 250 ms as an `MP stall` line (the event pump's, input's, the tick's and
  the draw's shares): loads and shader builds on the Vita will make some.
- APP_VER is 00.46. The GX state the material records hold is unchanged, so the card's records still
  warm up.
- It built for the Vita at the first try; nothing of 0.46.0 needed changing to compile there.

**New in 0.43.0 to 0.46.0** (their release notes, what reaches the Vita):
- Settings: precise stick deadzones (the sliders the only deadzone, the game's ~12% gone), the
  crosshair's colour on an enemy the beam hurts, the morph ball camera following the FOV setting,
  menus stretched to the screen.
- Prompts switch to the right icon as soon as the input context changes.
- Fixes: doors with the FPS cap above 60, Central Dynamo's maze in save states, twin-stick and touch
  aim after an elevator, the left hand looking straight up or down, Fast Morph's zoom, savestates
  under water, the map counting collected pickups as seen; savestates restore animations and script
  fields more reliably.
- Performance: fewer texture and vertex copies.

What to look for in 0.46.0 test 1:
- That it starts, loads a save and plays as test 24 did, with test 24's map.
- The new settings above, the prompts switching between contexts, the stick deadzones.
- The log's `MP stall` lines: where they come and how long.
- Frame rates against test 24's.

## 0.46.0 test 2: the side worker's own sleep

**Test 1** (9e1754e; the user's log of Nov 15, three and a half minutes, ended without an exit).
- It started, loaded the save and played; the front end ran as before (57 FPS).
- In play (a Metaree room) it fell to 2.4 FPS (median; 7.5 mean): 60 `MP stall` lines, nearly every
  frame 520 to 590 ms, the tick's.
- None of the side worker's jobs hung in 0.42.0's tests 24 and 25; here 205 did, each for 500 ms (the
  limit), all of them the game thread's (the particle steps it finishes, `pwait` 339 ms a frame). The
  FIFO processor's waits stayed short (3.6 ms at most).
- The worker was not busy (1 to 3%) and core 3 was mostly free (the mixer 23%): it slept with jobs
  queued. It woke 17 to 40 times a job, against 1.5 in 0.42.0's tests.

**The cause, as far as the log shows.** The worker slept in `std::atomic::wait` on its job count.
Without a futex (newlib), libstdc++ parks every atomic's waiters on one of 16 condition variables
picked by address, which the FIFO processor's own atomic waits use too. Its wakes per job say 0.46.0's
new layout put the worker's beside a busy one, and it then slept through wakes meant for it (which
step loses them, libstdc++'s or pthread-embedded's, the log does not say).

**The fix** (`vita_side_jobs.cpp`): the worker sleeps on a condition variable of its own. A
submitter wakes it through its mutex when it is asleep (seq_cst flags on both sides, so neither
misses the other), and the sleep is timed at 4 ms, so a wake-up lost anyhow costs that at most.

What to look for in test 2: no `has not run after 500 ms` errors, `hung` 0 in the `[side jobs]`
lines, `pwait` back to a millisecond or so, and the frame rates against test 24's.

**Test 2** (80ea3a6; the user's log of Nov 15, 8.7 minutes, a clean exit; a fight with two flying
pirates, to test combat).
- The side worker: no job hung, no timeout; 200 or so jobs a second at 6% busy, waits of a few ms,
  as in 0.42.0. Seven `MP stall` lines: the start, loads, and three draws of 260 to 330 ms.
- Median FPS by kind of room (test 24 on 0.42.0, then this):
  - the flying-pirate fight: 16.9, 19.1 (195 windows);
  - Magmoor with Puffers: 21.4, 21.7;
  - Magmoor with Triclops: 29.2, 26.9 (means 29.2 and 28.7);
  - Metaree rooms: 28.9, 28.4.
- In the fight the FIFO processor is the limit: busy 53.2 ms a frame (the whole frame at 19 FPS;
  52.3 in test 24), for ~970 draws a frame, of which the minimap's are 321. Skinned models take
  11 ms of it (7.7 and 3.3 to copy), the per-draw sections (state, record, material, rows,
  dispatch) about 27.
- The game thread is about 44.5 ms a frame (3.1 ticks at 9.0 ms, draw 16.5), its largest Think the
  energy projectiles (4.1 ms), then the flying pirates (2.1, and 0.8 moving) and the script effects
  (1.9).

## 0.46.0 test 3: the projectiles' particle effects on the fourth core

The user chose the projectiles first. In test 2's fight the energy projectiles' Think was 4.1 ms a
frame of the game thread, 2.9 of it their particle effects (`UpdateParticleFX`: each shot's
systems stepped a sixtieth of a second, about 34 a frame), the rest movement, collision and sound.
(The FIFO processor limits that fight, so the frame rate there gains only once it is lighter too.)

**The step on the side worker.** Within the window CStateManager::Update opens around its
entities' Think (as for the actors' particles, test 19), `CEnergyProjectile::Think` hands its
`UpdateParticleFX` to the side worker (`CProjectileWeapon::PortStepOnSideWorker`), and the rest of
its Think waits:
- At the pass's end, all of them in the order their steps began (`CEnergyProjectile::PortFinishAll`,
  zone "SM: projectiles' Think finished"), or before, when a message reaches the projectile, or a
  `Set3d0_26` (CScriptSpecialFunction's).
- A projectile that explodes, is dead or reaches its 45 s this tick runs whole, as before: those
  are known before the step, which changes none of them. What moves is only the light, the sound,
  and the deletion of a projectile whose effects have just run out, all within the tick.
- Every `CProjectileWeapon` method that reads or changes its systems, or what the step reads (the
  transforms' setters, the movement), waits for a pending step first, on the game thread (the
  step's own calls are the side worker's). The systems the step finds finished are deleted on the
  game thread once it is done, their pointers cleared at once as before.
- The particle code's shared state was made per-thread in test 19 (its generators, random numbers,
  seeds, depths): each system steps with its own random generator, so the game's own stays as it
  was. Without the side worker every Think runs whole.

The `[particles]` line adds the projectiles' effects stepped on the side worker a frame and those
waited for; `[zones] other` has "projectile: particle effects (side worker, core 3)".

What to look for in test 3: the fight's `SM: Think (all entities)` and projectile zones against test
2's; few and short waits in `[particles]`; the shots' trails and impacts as before (a misplaced or
missing one would be a step that read what the game thread changed); no crash.

**Test 3** (0f2d821; the user's log of Nov 15, 8.3 minutes, a clean exit, the flying-pirate fight
again).
- The fight's projectiles: 33 particle steps a frame on the side worker (3.9 ms there), the waits
  for them 0.1 ms at most; the game thread's `SM: Think (all entities)` 10.5 ms a frame against
  12.7 in test 2, the rest of the projectiles' Think 1.0 at the pass's end. No side job hung.
- Median FPS (test 2, then test 3): the fight 19.1, 20.1 (203 windows; its ticks 8.4 ms against
  9.0); Magmoor with Triclops 26.9, 30.5; Metarees 28.4, 28.1; Magmoor with Puffers 21.7, 19.8 (its
  ticks and draws a little heavier, another spot: no projectiles there).
- The cores, from the `[threads]` reports in the fight: core 0 at 42 to 44% (the render worker 35 to
  37%, the DVD worker 4, the reaper 3), core 1 at 90 to 92% (the game thread), core 2 at 85 to 89%
  (the FIFO processor), core 3 at 62 to 65% (the mixer 36 to 39%, the side worker 24 to 25). The
  user noticed core 0 resting at about half. Tool: `~/tools/cores.py <log> [reports]`.
- Which thread limits: the fight the FIFO processor (busy 53 ms a frame); Magmoor's and the Metaree
  rooms the game thread (the FIFO processor idle 13 to 16 ms a frame there). Most particle steps run
  on the side worker already (the Puffers' gas 9 ms a frame there).
- What core 0 could take: the FIFO processor's work that each draw does on its own, the skinned
  models' vertices (10.6 ms a frame in the fight: `skinned` and `skinned copy`) and the immediate
  draws' decode (5.8).

## 0.46.0 test 4: the core 0 helper

The user chose a helper on core 0 for the FIFO processor, which limits the fight.

**The helper.** A third worker of the side jobs' kind (`vita_side_jobs.cpp`, `submit_helper`), on core
0 at priority 158: above the render worker there (160, a third of the core in the fight), below the
DVD worker (112). Its waits do not give up (a job writes into the frame being recorded).

**What it takes: a skinned call's vertices.** `skinned_from_template` still checks and builds the
template, reserves the frame's vertices and copies the indices; the posing (each distinct
position and normal read from the arrays the CPU skinned) and the writing (the template's vertices
copied, each given its pose) go to the helper (`pose_skinned`, `write_skinned`), and the draw is
recorded at once. The fixed-function path, rare, poses as before.

**What waits for it.**
- The frame's end (`finish_helper_jobs`, before `end_frame_draws`), so the render worker gets the
  frame whole. The game thread draws its next frame only after that end (the deferred frame end's
  begin-wait), so the arrays the helper reads stay as they were, as they did for the FIFO processor.
- A reservation that would move the frame's vertices (they grow by doubling; each frame starts at
  the last one's size, so seldom): the helper's writes land first (`frame_vertices_safe`, for every
  reservation of the frame's vertices).
- A template rebuilt or released while the helper may read it (`wait_for_template`).

The log adds a `core 0 helper:` line (calls posed there, the waits at frame ends and for room) and a
`[side jobs] core 0 helper` line (its jobs and busy share); `[threads]` has it as "core 0 helper".

What to look for in test 4: the fight's FIFO processor (`skinned` and `skinned copy`, 10.6 ms a frame in
test 3) and frame rate; core 0's load in `[threads]`; few and short waits on the `core 0 helper:` line;
the skinned models (Samus's arm and suits, the pirates, the other enemies) drawn as before.

### Test 4 result

`~/logs/t046_4.log` (e0e9353, 245 windows). The user: "there's a flickering problem, first thing I
noticed were the doors. There's also this white shape that appears briefly and disappears" (a photo
in Magmoor: a large white rectangle across the middle of the screen).

The speed was there:

| | test 3 | test 4 |
|---|---|---|
| Magmoor, Puffers | 19.8 FPS, FIFO busy 36.2 ms a frame | 27.4 FPS, FIFO busy 24.9 ms |
| Metarees | 28.1 FPS | 29.0 FPS |
| Magmoor, Triclops | 30.5 FPS (81 windows) | 26.8 FPS (16 windows) |
| the pirate fight | 20.1 FPS, FIFO busy 53.3 ms, idle 5.3 | 17.4 FPS (11 windows), FIFO busy 41.1 ms, idle 21.5 |

- In the fight the FIFO processor's skinned work (`skinned` 7.4 and `skinned copy` 3.2 ms a frame in
  test 3) was down to 0.3 ms.
- What limited the fight's 11 windows was the game thread: 9.46 ms a tick against 8.39, a different
  stretch of the fight.
- Cores: core 0 at 54% (helper 19%, render worker 29%), core 2 (FIFO) at 59% (85% in test 3).
- 69,062 skinned calls posed on the helper; the waits at the frames' ends came to some 3 ms a second.

**The flicker: the game's ring of skinned vertices.**
- An owned skinned model (`CSkinnedModel`, `x38_owned`: the doors, the enemies, Samus's arm) takes the
  vertices it is skinned into from a ring, `sSkinningBuffer` (`AllocateStorage`).
- After its draw, `PostDrawFunc` sets a GX draw sync token. Once `GXReadDrawSync` shows that token
  processed, the next allocation lets that space go (`TickAllocations`).
- `CCircularBuffer` starts again at its first byte when everything in it has been let go
  (`CCircularBuffer::Free`). The FIFO processor keeps up with the tokens (`GXSetDrawSync` publishes
  the FIFO), so the next model is skinned into the same memory at once, on the side worker or the
  game thread.
- Until test 4 the FIFO processor read the positions before it came to the token. In test 4 it only
  handed them to the helper and went on to the token, so the helper could read the next model's
  positions through the first one's template: a door for a frame in another model's shape, or a
  stretched one in a white or glowing material.

## 0.46.0 test 5: the draw sync tokens after the helper

**The tokens.** The command processor's `GX_AURORA_SET_DRAW_SYNC` asks `vita::defer_draw_sync`
first.
- While the helper has calls not yet done, the token goes to it as a job of its own
  (`publish_draw_sync`).
- The helper runs its jobs in order, so the game sees the token only once the helper has read every
  array handed to it before. The FIFO processor does not wait; the game's ring just lets go a little
  later (its 32 MiB has room).
- With nothing pending, the token is published at once, as before.

**The draw done.** `GXDrawDone`'s callers free what the draws before it read
(`CFrameDelayedKiller::StallAndFlushAllAllocations`, the debug teleport). The BP draw-done register
now waits for the helper (`finish_helper_jobs_for_draw_done`) before the drain returns. It is rare
(loads, teleports, the credits).

**Unchanged.** The other guards (the frame's end, the frame's vertices growing, a template rebuilt
or released) stay. The non-owned workspaces and the swarms' shared ones are skinned again only in
the next frame's draw, after its begin-wait, which waits for this frame's end.

The `core 0 helper:` line adds the tokens the helper published and the waits at a draw done.

What to look for in test 5: no flicker on the doors or elsewhere, and no white shapes. Frame rates as
in test 4 (Magmoor's Puffers 27.4 FPS). Tokens on the `core 0 helper:` line, and no new waits.

