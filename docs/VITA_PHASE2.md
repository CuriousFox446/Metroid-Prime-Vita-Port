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

### Test 5 result

`~/logs/t046_5.log` (d1d0e2d, 186 windows). The user: "the white box still appears briefly and
disappears"; the doors' flicker was not mentioned again.
- The helper posed 35,489 skinned calls and published 9,881 draw sync tokens. It waited at no draw
  done, and no job hung.
- Magmoor's Puffers: 25.7 FPS (27.4 in test 4, 19.8 in test 3). Metarees: 30.3 FPS.
- The warm-up took 27.9 s to make its 1,534 materials again (2.5 s in tests 2 to 4).

**The white box was not the helper's.** The test 4 photo shows a rectangle, axis-aligned and evenly
white, behind the arm cannon and the reticle and over the walls, with no depth test. That is a fog
volume's last pass (`CCubeRenderer::ReallyRenderFogVolume`): a quad over the volume's rectangle on
screen, without the depth test, blended adding. Magmoor's lava has fog volumes (the logs: about 600
depth copies a window, two a frame).
- With its program the pass adds nothing on the Vita: its alpha is the ramp read through the two
  depth copies, and those are the black stand-ins.
- A program is built on the render worker at its first draw, one a frame (from the card: 60 to 90 ms
  each; tests 3 to 5 loaded 46 to 119 during play). Meanwhile its draws are fixed-function: about
  3,300 to 3,800 draws in each of tests 2 to 5.
- Fixed-function, the pass has no unit: `choose_units` takes the stages whose colour reads their
  texture, and this one's colour is a konst. Its vertices have no colour, so the decoder gives them
  white (`grey_for`: 255 for a textured format) at full alpha. Added over the rectangle: white,
  until the program is ready, which takes a few frames to a second or two when an area brings many
  new programs.
- The same happened in tests 2 and 3; it was noticed in test 4.

## 0.46.0 test 6: no flat shapes while a program loads

The render worker leaves out a draw whose program is still pending (`!settled()`), when its
fixed-function form would have none of its textures (`left_out_while_pending`):
- no unit (no stage's colour reads its texture),
- a program that samples a texture,
- and no depth write, so what draws after is unchanged.

The fog volume's last pass is one. So is any draw whose texture gives only its alpha, a glow over a
vertex colour for instance, which fixed-function was a solid quad. A draw whose program failed is
drawn fixed-function as before.

The `presents:` line adds "N left out waiting for theirs" after the fixed-function ones.

What to look for in test 6: no white box over the lava (or anywhere) on entering Magmoor's rooms;
the "left out" count above 0 where programs load; the rest as in test 5.

### Test 6 result

`~/logs/t046_6.log` (203c3ae, 722 windows). The user: "there's no white box in Magmoor Caverns", and
"Phendrana Drifts could use some optimization, running at sub 20fps".

The log names no areas. Phendrana's windows are told by their enemies: Flickerbats, Sheegoths
(`CBabygoth`), the fish and the parasites, windows 600 to 725. The tools (`area_fps.py`,
`kind_*.py`) now have the kinds "Phendrana" and "Phendrana, Sheegoths", and `~/tools/marks.py` maps
a run's windows by the classes that think in them.

| | FPS | FIFO busy, ms a frame | its immediate draws | game thread: ticks + draw |
|---|---|---|---|---|
| Phendrana | 18.2 | 51.4 | 18.3 | 3.01 x 7.74 + 20.5 ms |
| Phendrana, Sheegoths | 15.6 | 67.2 | 23.7 | 4.09 x 9.06 + 25.7 ms |
| Magmoor, Puffers | 23.8 | 25.5 | 4.5 | 2.65 x 9.88 + 12.0 ms |

- Phendrana is the FIFO processor's. Its immediate draws a frame with the Sheegoths:
  - the snow (counted under `CStateManager::DrawWorld`): 63 draws, 27,504 vertices
  - the map: 530 draws, 5,896 vertices
  - `CStateManager::RendererDrawCallback`: 409 draws, 4,678 vertices
  - the particles: 14 draws, 633 vertices
- Taken from the Puffers' room and Phendrana together, the immediate section costs about 0.48 us a
  vertex and 4.9 us a draw. The snow alone is about 13 ms a frame.
- With the Sheegoths the game thread is near its limit too: 60 ticks a second at 9.06 ms, plus a
  25.7 ms draw a frame, is about 95% at 15.6 FPS. That caps those rooms near 17.7 FPS whatever the
  FIFO processor does.

## 0.46.0 test 7: the big immediate draws decoded on the core 0 helper

**What it takes.** A program's immediate draw of 32 vertices or more whose attributes all sit in the
list (`decode_on_helper`). That is the snow's draws and the bigger particle ones; the map's and the
water's small draws stay where they were. The FIFO processor plans the decode, checks the list holds
the vertices, reserves the frame's vertices, copies the indices and records the draw. The helper runs
`decode_direct` into them, the function the FIFO processor ran, so the vertices are the same.

**The bytes are copied first.** The FIFO processor copies the draw's bytes into a room of its own,
1 MiB to start, grown when one draw needs more. The FIFO's buffer is the game's to write again once
the FIFO processor is past it, and a drain empties it, so the helper never reads the buffer itself.
- The room is taken in order and is all free again whenever the helper has run every job
  (`finish_helper_jobs`, at each frame's end among others).
- When it runs out within a frame, the FIFO processor waits for the helper first (counted).

**Unchanged.** The other guards stay: the frame's end, the frame's vertices growing, the tokens, the
draw done.

The `core 0 helper:` line adds the immediate draws and vertices decoded there and the waits for the
copies' room.

What to look for in test 7: Phendrana's frame rate in the open (18.2 FPS in test 6; up to about 24
expected, where the game thread takes over), and the FIFO processor's `immediate` there (18 to 24 ms
a frame in test 6). The snow should look exactly as before.

### Test 7 result

`~/logs/t046_7.log` (c908dab, 248 windows, 204 of them "Phendrana, Sheegoths"). The user: "could be
that we need to optimize the game thread."
- The FIFO processor went from 67.2 to 49.5 ms a frame there. The helper decoded about 21,000
  immediate draws (8.9 million vertices) a report and was 30 to 33% busy. The FIFO processor waited
  for it at frame ends for 2 to 4% of the time.
- The frame rate stayed at 15.5 FPS (15.6). The game thread is the limit now: 94% busy, 3.84 ticks
  of 9.70 ms plus a 22.2 ms draw a frame.
- Cores: 0 at 77% (helper 32%, render worker 39%), 1 at 94% (game), 2 at 59% (FIFO), 3 at 43%.

The game thread's frame there, in ms:
- `SM::Update` 31.2 (3.84 ticks):
  - `SM: Think` 13.4: the two Sheegoths 4.8 (Think 3.1, Move 1.5), the parasites 2.2, the
    Flickerbats 1.4, the collision actors' Move 1.0
  - `SM: environment effects` (the snow's update) 3.3
  - platforms and doors 2.9; PreThink 2.7; the touches 2.6
- the HUD's update 4.4
- input about 3
- `CMFGame::Draw` 22.1: `CStateManager::DrawWorld` 15.1 (7.1 in Magmoor's Puffers' room); the map
  2.4; the HUD 2.2

The snow's draw is most of that difference. `CEnvFxManagerGrid::RenderSnowParticles` writes each
flake as four vertices of `GXPosition3s16` and `GXTexCoord2u8`: ~55,000 calls a frame. Each call is a
function of its own (GXVert.cpp), and each component written checks the FIFO's room again. That is
about 110 cycles a vertex, some 7 ms a frame for ~27,500 vertices.

## 0.46.0 test 8: the snow's vertices in one write

**The writes.** `GXPortWriteVertexBytes` (new header `dolphin/gx/GXPortVertexBytes.h`, in
GXVert.cpp) writes a run of vertex bytes into the FIFO at once.
- The snow's, the rain's and the underwater flakes' loops (`CEnvFxManagerGrid::Render*Particles`)
  put each vertex together themselves, as those calls wrote it: three s16 big-endian, then the two
  u8 coordinates (the rain: an s16). Then each grid is sent in one write.
- The arithmetic is the loops' own, the presentation step back included.
- A harness (`~/tools/harness/snowbytes.cpp`) ran both ways over 40,000 random grids, cameras and
  steps: 54 MB of bytes, none different.

**Zones.** `CEnvFxManager::Render (snow, rain)` (a gxsrc stage too, so the snow's draws are counted
under it rather than under `DrawWorld`), and in the update `envfx: visor splash`,
`envfx: blocked grids (ray casts)` and `envfx: particles (new and moved)`.

What to look for in test 8: Phendrana's frame rate (15.5 FPS with the Sheegoths). The game thread's
`DrawWorld` there (15.1 ms a frame in test 7), and the new snow zones. The snow, the rain and the
underwater flakes should look exactly as before.

### Test 8 result

`~/logs/t046_8.log` (d3f1482, 182 windows). The user: "there was a crash I couldn't replicate". The
dump was only the cut-short `.tmp`.

**The speed.**

| | test 7 | test 8 |
|---|---|---|
| Phendrana, Sheegoths | 15.5 FPS, draw 22.2, `DrawWorld` 15.1 ms | 17.3 FPS, draw 18.2, `DrawWorld` 11.0 ms |
| Phendrana (open) | 18.2 FPS (test 6) | 19.9 FPS, draw 16.5, `DrawWorld` 9.4 ms |

- The snow's draw is 1.75 to 2.0 ms a frame now, about 7 ms before.
- Its update: `envfx: particles (new and moved)` 0.55 ms a tick, `envfx: blocked grids (ray casts)`
  0.25 ms where grids move.
- With the Sheegoths the game thread (3.55 ticks of 9.82 ms, plus 18.2) and the FIFO processor
  (53.0 ms) are level, about 53 ms each.
- Cores: 0 at 86% (helper 36%, render worker 45%), 1 at 88%, 2 at 64%, 3 at 36%.

**The crash.** The log's last lines:
- `vita: out of memory: operator new(4956896) failed on thread 0x4002025B`, the FIFO processor.
- The heap at the time: 191,179 KiB in use, 33,768 KiB free (in pieces), 224,948 of 229,376 KiB
  taken from the system.
- Then `std::terminate`, an uncaught `std::bad_alloc`.

Its stack, through the ELF (runtime minus 0x1B000): `draw_immediate` → `frame_vertices` →
`FrameArray<Vertex>::reserve` → `operator new[]`. 4,956,896 bytes is 88,516 vertices: the frame's
vertices doubling from 44,258.
- Phendrana's frames record ~45,000 to 50,000 vertices: the snow ~28,700, the map ~6,200, the rest
  ~10,000. That is ~2.5 MiB in one piece.
- A frame that outgrew the spares (`SpareStorage`, the two largest kept) asked for twice that in one
  piece, from a heap near its ceiling.
- Test 8's spike, 190 MiB in use against a mean of 165, came while the reaper was behind: the render
  worker destroyed 555 frames itself in 17 reports, against 51 in test 7. The helper takes more of
  core 0 now.
- Each frame also allocated its draws (~1 KiB each, a megabyte a heavy frame) anew, freed a frame or
  two later.

## 0.46.0 test 9: the frame's vertices in chunks

**The vertices.** A frame's own vertices are kept in chunks of 8,192 (448 KiB), not one array
(`FrameChunks`, vita_gl.cpp).
- The chunks come from and go back to a pool (`ChunkPool`, 16 kept). No piece is larger than a
  chunk, and the same pieces are used again and again.
- A run that does not fit in the last chunk starts the next. One larger than a chunk gets one of its
  own size; the immediate draws' are at most 65,535.
- A place is the chunk's number (bits 20 and up) and the place in it. A mesh's vertices are together
  in one chunk. Two meshes are next to each other (`merge_into_last`: place plus count) only in the
  same chunk, so draws are joined as before within a chunk and not across one.
- What was handed out never moves (`room()` is unbounded), so `frame_vertices_safe` no longer waits
  for the core 0 helper when the frame grows.

**The draws.** A frame's draws keep their storage for the frames after (`RecycledVector`,
`VectorPool`: the two largest kept). A new recording takes the smallest that fits instead of a new
megabyte.

The indices stay as they were: a heavy frame's are ~150 KiB.

What to look for in test 9: no out-of-memory crash in Phendrana. The `[heap]` lines' in-use and taken
figures, against test 8's 165 MiB mean and 190 peak. Frame rates as in test 8.

### Test 9 result

`~/logs/t046_9.log` (e89a8ad, 1,830 windows: a long run in several areas). It ended cleanly: no out
of memory, no error, no hung job. The heap was at 169.6 MiB in use on average, 186.5 at most.
- Phendrana: 20.5 FPS in the open (19.9 in test 8), 18.3 with the Sheegoths (17.3).
- The user: "I've noticed the third person option hurts the performance because it uses Samus'
  higher poly model for third person."

**Third person.** The log has each change of the setting (`port: setting third_person: 0 -> 1`),
three times. `~/tools/span.py` sums the windows between two lines.

| windows | third person | FPS | the game thread's draw | `CStateManager::PreRender` |
|---|---|---|---|---|
| before the first | off | 26.6 | 11.6 ms | 0.9 ms |
| the first | on | 10.1 | 54.8 ms | 37.2 ms |
| the second | on | 15.3 | 34.6 ms | 28.2 ms |

The model is not what costs:
- Its skinning on the fourth core goes from 2.2 to 4.6 ms a frame.
- The FIFO processor's and the render worker's loads stay about where they were.

What costs is the body's pose (`PortThirdPersonBody::PreRender`, under `CPlayer::PreRender`):
- `PoseBody` samples some ten clips a frame: the idle or the turn, the walks, the ready pose's
  upper body, the jumps, and the aim grid's clips at their fixed phase.
- Each `Sample` called `CAnimData::SetAnimation`, which builds a new tree for the clip. A new tree's
  compressed readers then decode the clip from its start to the phase
  (`CFBStreamedPairOfTotals::SetTime`).
- The pose itself is cheap: `CAnimData::PreRender`, through which every sample goes, was 0.4 ms a
  frame.

## 0.46.0 test 10: the third-person body's clips keep their trees

Each clip's tree is built at its first `Sample` and kept (`PortThirdPersonBody::Tree`).
`CAnimData::PortSetAnimationTree` installs a kept tree: it is `SetAnimation(..., true)` line for line,
the kept tree in place of a new one.
- A kept tree seeks from where it is. The aim clips at their fixed phase decode nothing; the walks
  only the frames since the last one.
- A reader that goes back (a loop wrapping) decodes from its start as before.
- The decoded values come from the same increments in the same order, so the poses are the same.
- The trees are dropped with the body (`Drop`), and the body's own when it is built again.
- Zones: `third person: the body's pose`, `third person: a clip sampled`,
  `third person: a clip's tree built`.

CAnimData.hpp changed, so most of the game is built again.

What to look for in test 10: third person's frame rate against first person in the same place (10.1
against 26.6 FPS in test 9), the body moving and aiming exactly as before, and the new zones (the
pose ~30 ms a frame in test 9).

### Test 10 result

`~/logs/t046_10.log` (8526e5f, 418 windows, third person on from the start, most of it Phendrana
with the Sheegoths). Clean.
- The body's pose: 1.3 ms a frame. 6.2 clips sampled a frame, 0.15 ms each; each tree built once.
- `CStateManager::PreRender` 2.9 ms (28 to 37 in test 9).
- Phendrana with the Sheegoths in third person: 16.1 FPS, against 18.3 in first person (test 9).
- What is left of third person is its vertices:
  - The body's skinning on the fourth core: 6.3 ms a frame, against 2.7.
  - The core 0 helper is 45% busy and core 0 89%. The FIFO processor waited for the helper at 298
    frame ends of 300, ~9.4 ms a frame.
  - The game thread is at 96%: 3.78 ticks of 9.57 ms, plus 20 ms.

The user chose the game thread next. Its zones over test 9's 1,570 play windows (`~/tools/pertick.py`):
a tick 7.5 ms. `SM::Update` is 5.7 ms of it:
- `SM: Think` 2.3
- PreThink 0.6, the touches 0.53
- the player's collision move 0.37
- platforms and doors 0.35
- PostUpdatePlayer 0.25, the player's input 0.23
- the sorted lists 0.33
- the camera 0.15

The HUD's update is 1.0 ms a tick (`CSamusHud::Update` 0.71, almost none of it under a named zone).
The input generator is 0.37 a tick, and the Port actions 1.7 ms a frame (~1 ms of it unnamed). The
largest Think class is `CScriptEffect`: 0.54 ms a tick, 25 effects. The think profiler itself costs
little: it times each class's first 32 calls a report, then one in 8.

## 0.46.0 test 11: the effects' particles on the fourth core

**The effects.** `CScriptEffect::Think` steps its `CElementGen` and `CParticleElectric` on the side
worker while the Think pass goes on, as the projectiles' (0.46.0 test 3).
- Its rest (`PortThinkTail`) waits for the pass's end (`CScriptEffect::PortFinishAll` in
  `CStateManager::Update`, after the projectiles'). That rest is the particle counts, the light fed
  from the system, the nested lights, the die-when-done checks and the modulation colour.
- It runs sooner when anything reaches the systems first (`PortFinishStep`): a message,
  `SetActive`, `PreRender`, `AddToRenderer`, `Render`, `CalculateRenderBounds`, the external
  variables, the barrier, `PortParticleSystem`.
- The destructor waits for a step still running.
- What moves: the light's `SetLight` and the deletion request, both to the pass's end.
  `CGameLight::Think` only counts its lifetime down, so the light the frame draws is the same.
- The `[particles]` line adds the effects' steps and waits.

**Zones** for the next round:
- `HUD update:` radar, lag and dynamic lights, targeting (reticles), damage and static; energy,
  threat, missiles, menus; the frames' widgets; the interfaces and menus.
- `input: the Port actions' sources check` and `the Port actions' game contexts`.

What to look for in test 11: `SM: Think` against test 9's 2.3 ms a tick where effects are many, the
`[particles]` effects count and its waits (few, short), and effects looking as before (steam, sparks,
lava bubbles, waterfalls). And the new HUD and input zones, which name the next targets.


### Test 11 result

`~/logs/t046_11.log` (3d754c5, 703 windows: a save loaded in Phendrana, the Scatter Bombus
(`CAtomicAlpha`), Quarantine Cave and the Thardus fight, the Spider Ball, then the elevator) ended
in a crash after the fight. The user also named slow moments: the save station animation, picking up
health and missiles, and enemies with electric attacks. They asked for frame pacing to be checked,
and for the animation after choosing a save file, which is cut before its end.

**The crash** (`~/crash/dumps/t046_11.psp2dmp`): a data abort in SceLibKernel's heap code on the
render worker, in `glBufferData` (`gpu_alloc_mapped_aligned_for_gpu`). The cause is in vitaGL
(e119b3e).
- `gpu_free_texture_data` frees a texture's memory at once when no draw has sampled it for 4 frames
  (`FRAME_PURGE_FREQ`), or ever since it was made (`last_frame` is `OBJ_NOT_USED` after each
  allocation). Otherwise it marks it for the garbage collector, 4 frames on.
- A draw *into* a texture through its framebuffer does not count as a use.
- A copy surface (`Texture::target`) was made again in place at another size (`glTexImage2D`), or
  deleted with its copy texture, soon after a copy into it and before any draw sampled it. Its memory
  went back to the heap while that copy's scene, still to run on the GPU, wrote into it. The heap's
  bookkeeping lives in its free blocks, so it broke.
- 0.42.0 test 10's crash was the same kind (a GPU transfer into a moved mip chain).
- The fight's copies were the heaviest of the run: the thermal visor, 300 to 450 Mpix a report.

**The slow moments.**
- The save station (Samus appearing after the load) is the session's warm-up: programs still
  loading, vitaGL compiling fixed-function shaders of its own (145 ms each, 8 in the run), then
  GPU-bound (the swap 51 ms a present).
- Pickups and the Bombus' blasts: GPU-bound at their worst (the swap 79 ms, 316 immediate draws and
  1.4 MiB of their vertices a present). Explosions (`CExplosion`, a pickup's flash among them) were
  3 to 6 ms a frame of the Think pass.
- The core 0 helper was the FIFO side's limit in the heavy rooms. In the fight it was 44 to 56% busy
  (7.4 M immediate vertices in 13.5 s, the snow's and the particles'). Core 0 was full (the render
  worker 36%, the DVD worker above both). The FIFO processor waited for the helper 4 to 6 s in each
  12 s report, ~20 ms a frame at its end.
- The GPU was busy 22 to 52 ms a frame throughout Phendrana and the fight.

**Frame pacing.** 18 stalls over 250 ms (`MP stall`). Ten of them were the frame's input part, 161
to 444 ms. That part was `PollPortActions`' sources check: `[zones] input: the Port actions' sources
check` reached 43 ms a frame over a window, and `[split] input` was 10 to 15 ms. Inside it,
`UserBindingsVersion` looks at controls.toml's time once a second: a stat on the memory card, on the
main thread, which waits behind the disc image's streamed music and the area loads. The rest were
area loads (`TravelToArea`, 0.3 to 1 s) and the warm-up's draws.

**The save-select cut.** The file-select->game movie ends its transition at its last frame, and a
timer of the movie's length (in ticks) ends it too. The port decodes the movie's frames on the
background lane and holds the shown frame while the next is not ready. On the Vita the movie ran
behind its timer, and the timer won.

## 0.46.0 test 12: the copy surfaces kept, the card's stat, the helper's backlog shared

**The copy surfaces** (`vita_gl.cpp`).
- A copy surface is never made again in place. One the LRU lets go, and every surface of a copy
  texture that goes, is kept for `kSurfaceKeptFrames` (8) presents, then deleted after the swap
  (`delete_kept_surfaces`).
- Meanwhile a copy at its size takes it again (`take_kept_surface`) instead of making one: this
  also saves the render-target remakes the fight's churn cost.
- The `presents` line adds the surfaces made, taken again, let go, deleted and kept.

**The card's stat** (`RefreshLocked`, `port_input_devices.cpp`). On the Vita, controls.toml is looked
at again after its first load only while the Port menu is up. Only this game writes it there, and
`SetUserBindings` publishes what it writes.

**The helper's backlog shared** (`vita_side_jobs.cpp`).
- The core 0 helper's jobs are claimed one by one. While the FIFO processor waits for one
  (`wait_helper`: the frame's end, a full ring slot, a template), it runs the jobs up to it that the
  helper has not taken.
- A job's end is counted per slot. `finished` moves over the jobs that have run in a row, so "done"
  still means every job up to it.
- The draw sync token's publish job is `ordered`: it runs only after every job before it, wherever
  they ran.
- Two fills may run at once, so the helper's posed pairs are two (`sHelperPosed`).
- The `[side jobs] core 0 helper` line adds the jobs the FIFO processor ran itself and their time.

**The explosions** (`CExplosion`): as test 11's effects.
- Think sends the system's step to the side worker while the pass goes on. The rest (the light, the
  nested lights, the lifetime and deletion checks) runs at the pass's end (`PortFinishAll`, after
  the effects') or sooner (`PortFinishStep`: a message, `SetActive`, `PreRender`, `AddToRenderer`,
  `Render`, `CalculateRenderBounds`).
- The fixed 1/60 s step of a Remastered flag-0x10 explosion stays a double, as before.
- `[particles]` adds the explosions' steps and waits.

**The save-select cut** (`CFrontEndUI::Update`). When the timer runs out while the transition's
movie still plays (not looping, its last frame not shown), the transition waits for that last frame,
up to 5 s more.

**vitaGL's own shaders kept** (`check_fixed_function_cache`). Since the check began, the same 8 of 31
files in ux0:data/shader_cache failed it at every start (292 to 480 bytes) and were deleted. vitaGL
then compiled those 8 again in each session (~145 ms each; two came in the save station's warm-up and
one in play) and wrote them back. The check wanted the GXP header's size to equal the file's. It now
keeps a file longer than its header says (counted as "longer than their header says"). Magic,
version and a file cut short (shorter than its header) still fail. Each deleted file's reason is
logged.

**Frame pacing.**
- The `presents` line counts the presents by the vblanks between them: 1 or less, 2, 3, 4, 5 or
  more.
- `[timing]` counts the presented frames by the ticks each ran.
- A frame-rate cap that divides 60 sets vitaGL's swap interval (`GXPortSetSwapInterval`, applied
  before the swap). The Port menu's lowest is 30 (the 60 FPS cap off, the FPS cap 30): every frame
  then stays up two vblanks and runs two ticks.
- The default is unchanged (60, interval 1).

What to look for in test 12:
- No crash after heavy copies.
- No `MP stall` with a large input part.
- `[side jobs] core 0 helper`: the FIFO processor's waits down, and the jobs it ran itself.
- `SM: Think` down where explosions are many.
- The save-select movie playing to its end.
- The fixed-function shaders line at the start: none deleted, and no "vitaGL compiled" lines after
  the first session.
- The pacing counts, at the default and, if the user wants to try it, at the 30 cap.

### Test 12 result

`~/logs/t046_12.log` (58f89bc, 1,088 windows, 18.5 minutes): a clean exit, no crash.
- Long stalls: 5 (test 11: 18), all area loads or one draw. None is the input part (the card's stat).
- vitaGL's own shaders: 31 on the card, all kept (8 longer than their header says); not one compiled
  in the session (8 before).
- The core 0 helper: the FIFO processor waited for it 20 to 75 ms in each 6 to 16 s report (test 11:
  4 to 6 s in 12 s) and ran thousands of its jobs itself (up to 10,640 jobs, 4.0 s, in 11 s).
- Explosions: 2 to 6 systems a frame stepped on the side worker, no waits.
- Copy surfaces: made, taken again and deleted 8 presents on, as meant.
- FPS by median: the Thardus fight 22.7 (21.6 in test 11), windows with explosions 22.6 (20.8), the
  Scatter Bombus 31.8 (32.2), Phendrana 27.6 (28.8; another route).
- Pacing over the run (`~/tools/pacing.py`): presents 1 vblank apart or less 63%, 2 22%, 3 9%, 4 4%,
  5 or more 2%. Frames by ticks: 1 53%, 2 28%, 3 10%, 4+ 5%, 0 4%.
- The heavy scenes are now the GPU's: busy 22 to 52 ms a frame where test 11 measured it. That is the
  next round's subject.

The user then asked to try the Metroid Dread mod (its image auto-detected in ux0:data/MetroidDread),
and for the port to rebuild all its shaders at each new build again (test 13, rolled back).

## 0.46.0 test 13: rolled back to test 12

The first test 13 (410d8db, 78a1058) tried the Dread Samus import on the Vita and went back to
rebuilding the shaders at each new build:
- The image and keys were found in ux0:data/MetroidDread (keys also in ux0:data/MetroidPrime).
- A software AES (ECB, CTR, XTS) stood in for the missing OpenSSL. It matched OpenSSL 3.5 on 100 of
  100 cases.
- The image was read with sceIoPread, for offsets past 2 GB.
- The textures were TXTR only, and PBR was off on the Vita.

The user tried it, and it did not work. They chose a full rollback: the code is test 12's (58f89bc)
again, reverted commit by commit, so the per-build shader rebuild went too. The card's program
binaries carry the first test 13's "binaries 3 build <revision>" marker. Test 12's code reads that as
another format, so it deletes them once: the first start rebuilds every program, and from then on
builds keep them.

The new test 13 is that rollback, staged under the same name.

## 0.46.0 test 14: the textures' levels, and a GPU cost test

Test 12's heavy scenes were the GPU's: busy 40 to 52 ms a frame in Phendrana and the Thardus fight.
Its reports put no single thing behind it. The heaviest frames had tiny EFB copies (7 kpixels a frame)
and 290 to 460 blended and 110 to 190 immediate draws. In light frames the clock reads high: a frame
of one draw read 8 to 10 ms, the display callback's vblank wait among it.

**The textures' levels** (`gfx/texture.cpp`, `make_vita_texture`; `vita_gl.cpp`, `Texture`).
- Since 0.42.0 test 11 a CMPR texture's Vita copy has had its mip levels (down to 16x16). Every other
  GX format (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, the palette formats) had its first level alone.
- The game's mipmapped filters then sampled that one level as their plain ones. A far surface read
  its full-size texels, shimmering where the GameCube's levels smoothed it, and the GPU fetched texels
  from across the whole texture for each pixel.
- Their levels now come too, down to 16x16 as CMPR's, uploaded one by one, each after the GPU's
  transfers before it, as BC1 chains are (0.42.0 test 10).
- The cost is a third more memory for those textures. Test 12 had at least 23 MiB of VRAM free, and
  its RGBA8 textures, the copy surfaces among them, were about 30 MiB.

**The GPU cost test** (`vita_gl.cpp`, `gpu_test`; F1 > Performance > "Run the GPU cost test").
- Stand still where the frame rate is low, press it, and close the menu. About a second later the
  test leaves one kind of draw out at a time for 1.6 s each, 0.4 s of it for the GPU to catch up:
  EFB copies, immediate draws, blended draws, alpha-tested draws, skinned draws, display lists, and
  every draw (the clears and copies alone).
- It runs everything before and after, to see the scene did not change.
- One log line, "GPU cost test:", gives each phase's GPU busy time a frame (and its change), its
  frame time, and the first phase's draws a frame by kind.
- The picture is wrong for those 15 s.

What to look for in test 14:
- Far surfaces less shimmery, the same up close.
- "RGBA8 texture ... uploaded with N levels" lines.
- VRAM in the presents lines.
- The frame rate where test 12 was GPU-bound.
- The GPU cost test's line from a heavy scene or two: the Thardus room with the thermal visor,
  Phendrana with snow and Sheegoths, the save station.

### Test 14 result

`~/logs/t046_14.log` (4ba0d9f, 5.3 minutes in Magmoor: the lava rooms, the Triclops, a flying-pirate
fight, a save station, an elevator): a clean exit, 3 long stalls (room loads).
- RGBA8 textures uploaded with their levels ("RGBA8 texture 64x64 uploaded with 3 levels").
- The GPU cost test ran three times, all in lighter places than test 12's heavy ones (17 ms of GPU a
  frame):
  - One run was at 60 FPS, where the clock reads high (6 ms with no draws at all).
  - One run's scene changed mid-test: the "everything" phases read 17.3 and 28.6 ms.
  - The clearest (622 draws, 23.7 ms frames, so not GPU-bound) gave display lists -10.2 ms of 17.4,
    skinned -3.8, blended -3.0, immediate -0.6, alpha-tested and EFB copies about 0, no draws 1.4 ms.
- The heaviest GPU reports (27 to 38 ms, up to 100% busy) came with the visor's billboard effects and
  the effects' particles: 240 to 270 blended draws a frame of 255 to 320.

## 0.46.0 test 15: intensity textures in their own bytes, the cost test's pixels and vertices

**Intensity textures** (`gfx/texture.cpp`, `vita_gl.cpp`). GX's intensity formats (I4, I8) now go to the
GPU a byte a texel: `GL_INTENSITY`, GXM's U8_RRRR, sampled (I, I, I, I). Those with alpha (IA4, IA8) go
two bytes a texel: `GL_LUMINANCE_ALPHA`, GXM's A8L8, sampled (I, I, I, A).
- These are the particles', the effects' and most HUD pieces' textures, drawn large and blended in
  test 14's heaviest frames.
- They were RGBA8, four bytes a texel. The values sampled are the same, from a quarter and a half of
  the bytes, levels and all (test 14).
- The presents' memory line counts I8 and IA8 apart.

**The GPU cost test** (F1 > Performance) has three more phases. Each draws a kind of draw into one
pixel (a 1x1 scissor), so its vertices are shaded and binned as before and its pixels are not:
- every draw
- the display lists
- the blended draws

"Everything" less such a phase is what those draws' pixels cost; that phase less "no ..." is what
their vertices cost. When the two "everything" phases are more than a sixth apart, the line says the
scene changed. It runs some 20 s.

What to look for in test 15:
- Particles, effects and the HUD looking exactly as before.
- I8 and IA8 in the memory line.
- The GPU in Magmoor's effect-heavy places.
- The cost test where the frame rate is lowest.

### Test 15 result

`~/logs/t046_15.log` (25d8eb9, 3.6 minutes, Magmoor's Triclops room with flying pirates): a clean
exit.
- I8 and IA8 textures uploaded.
- Texture memory: the front end holds 234 I8 textures (34 MiB), its movies' Y/U/V planes, where test 14
  held 55 RGBA8 (32 MiB). Every format but CMPR decoded to RGBA8 before, so those planes had copies
  then too. The cache keeps about the same bytes, now four times as many old movie frames, some 25 MiB
  of them still there in play (in test 14 too). A memory saving for later.
- The GPU cost test ran three times there (850 to 940 draws, GPU 33 to 44 ms a frame):
  - Drawing kinds into one pixel made the GPU slower (+7 to +17 ms). A scissor changed at every draw
    cost more than the pixels saved (test 16 changes the phase).
  - The "no ..." phases were all large and noisy (the scene moved; one run drifted 40%): blended
    10 to 23 ms, immediate 10 to 18, alpha-tested 5 to 18 (23 to 27 draws), skinned 5 to 22, display
    lists 8 to 26, the one small EFB copy 0 to 12. No draws: 3.4 ms.
  - Even with no draws the frames took 53 ms. **That room is the game thread's**, not the GPU's:
    3.5 ticks a frame of 10.9 ms, SM: Think 18.4 ms a frame (the flying pirates 4.7, the player
    1.8, the projectiles 1.5), drawing 17.
- The game thread waited for the side worker's particle steps 2.0 s in 16.9 s (7 ms a frame). The
  fourth core ran them one after another beside the mixer (44% of the core); cores 0 and 2 were half
  idle (render worker 25-31% with the helper 18-21%; the FIFO processor 50-55%).
- Off the Vita, on the PC: a dozen generated fragment programs compiled with the Vita's compiler
  (`~/harness/halfexp`). With `fastprecision` on they lose 5 to 13% of their instructions. With the TEV
  colour maths written in half, about 10% (and most variants need their mixed calls cleaned up first).
  Too little for a rebuild of every program, so it is set aside.

## 0.46.0 test 16: a second side worker for the particle steps

**A second side worker** (`vita_side_jobs.cpp`), on the first core: priority 159, below the core 0
helper and above the render worker.
- `GXPortSideJobSubmitAny` gives a job to whichever side worker has fewer queued. Its number carries
  bit 31 for the second, so `GXPortSideJobDone` and `GXPortSideJobWait` find the right one.
- These are particle steps, each object's own and in no order: the actors' (`CParticleDatabase`),
  the projectiles', the effects' and the explosions'.
- Skinning stays on the fourth core's worker, with the FIFO processor's fences.
- The particle code's per-thread statics (`port_thread_slot.h`, `CParticleGlobals`) have a third
  slot, the second worker's, told apart by its stack as the first's is.
- The log's `[side jobs] side worker 2` line counts its jobs.

**The GPU cost test** has "everything" after every two phases, and gives each phase's change
against the mean of the two around it. The one-pixel phase is every draw's, the scissor set once a
frame.

What to look for in test 16:
- The `[side jobs]` lines: the game thread's waits (the "other threads" figure), and side worker 2's
  jobs.
- The Triclops room and other particle-heavy fights against test 15.
- Effects, particles and projectiles looking as before.

### Test 16 result

`~/logs/t046_16.log` (950d0e5, 11.5 minutes in Phendrana, mostly the Sheegoth rooms): a clean exit.
Side worker 2 started on core 0 at priority 159.
- **Slower, not faster.** The Sheegoth rooms ran at 12.0 FPS (median of 263 windows), against 15.5 to
  18.3 in tests 7 to 10. That was 4.7 ticks a frame of 11.6 ms each, where tests 7 to 10 had 3.2 to 3.8
  ticks of 9.4 to 9.8 ms. SM: Think took 5.6 ms a tick (3.2 to 3.5 before).
- The difference is waiting. The game thread waited for the particle steps 10.9 ms a frame there
  (tests 7 to 10: 0.25 to 0.47 ms; `~/tools/pwaits.py`). It waited about once a frame, but 5 to 7 ms
  each time: the effects' steps 7.5 ms a frame over 1.1 waits, the projectiles' 2.5 ms over 0.5.
- The `[side jobs]` lines: side worker 2 was busy only 4 to 10% of core 0. Even so, the game thread
  waited for it 1.4 to 2.4 s in each 13 to 21 s report, about 3 ms a wait. For the fourth core's
  worker it waited 0.2 s. Below the core 0 helper (158, 28% busy in bursts for the FIFO processor),
  the second worker waited for its core while the game thread waited for it.
- The GPU cost test ran three times in a snowy room: 1130 to 1160 draws a frame, about 670 of them
  immediate, 690 blended, 10 to 29 alpha-tested, 48 skinned and 420 display lists.
  - Every phase took 80 to 96 ms a frame, even with no draws (81 ms). The room is the game thread's.
  - The GPU took 27 to 44 ms a frame. With every draw kept to one pixel it took 9 to 10 ms, and with no
    draws 3.5 to 4.8. So about two thirds of its time is pixels.
  - No blended draws saved 15 to 26 ms. No alpha-tested ones saved 8 to 17 ms, from only 10 to 29 draws,
    as in test 15. No immediate ones saved 8 to 19 ms. The first run's scene drifted (33%).

## 0.46.0 test 17: the game thread runs the particle steps it waits for

**The game thread runs the steps it would wait for** (`vita_side_jobs.cpp`, `wait_game`). Both side
workers now claim their jobs one by one, as the core 0 helper does.
- When the game thread waits for a particle step that no worker has taken yet, it runs the queued steps
  up to that one itself, in order.
- A step a worker has begun, it waits for.
- The fourth core's skinning jobs (`GXPortSideJobSubmit`) stay that worker's: the game thread waits
  until the worker takes one, then goes on.
- The steps it runs use the game thread's per-thread slot, as they did before the side workers. A wait
  inside one of them only waits.

**Side worker 2 at priority 157**, above the core 0 helper. The game thread waits for its jobs within
the tick. The FIFO processor runs the helper's unclaimed jobs itself while it waits.

The side workers' `[side jobs]` lines end with "and the game thread ran N of them itself while it
waited". Their "other threads" figure now counts only the time spent waiting. The `[particles]` line's
"waited for" figures include the steps the game thread ran.

What to look for in test 17:
- The Sheegoth rooms against test 16's 12 FPS and tests 7 to 10's 15.5 to 18.3.
- The `[particles]` waits and the `[side jobs]` lines.
- Particles, effects and projectiles looking as before.

### Test 17 result

**The game froze** soon after play began in Phendrana (`~/logs/t046_17.log`, 13679b0). The log's last
lines:
- `side worker: job 2243 has not run after 500 ms (the worker has run 2242)`
- then the watchdog: no frame for 5, 15 and 30 s, the game thread in "game tick (UpdateTicks)".

The cause was a projectile's step. Job 2243 was one the game thread was running itself, in its
wait. A projectile's particle step calls its own accessors (`UpdateChildParticleSystems`,
`IsSystemDeletable`), and each of those first finishes the step (`PORT_PW_FINISH`). Those finishes are
skipped "on the side worker itself", which `CProjectileWeapon::PortFinishStep` tells by
`PortThreadSlot() != 0`. In the game thread's slot 0 the check failed, so the step waited for its own
job, which could never end while it waited. Its finish waits again after each 500 ms, so the game
thread stayed there.

## 0.46.0 test 18: the game thread's side jobs in a slot of their own

**Slot 3** (`port_thread_slot.h`). While the game thread runs a side job in its wait (`run_stolen`), the
part of its stack below the wait is per-thread slot 3.
- The job's code sees itself on a side job, as on a worker: a projectile's step skips its own finishes
  again.
- The particle code's statics there (the random generator, the variables in scope, the child seeds, the
  globals) are the job's own. The game thread's slot-0 state, that of the code around the wait, stays
  as it was.
- The game thread's stack bottom comes from the kernel at its first wait (the log says so once). If
  the kernel does not give it, the game thread runs no jobs and only waits.

**A side worker's job is done once it has run**, whatever ran ahead of it (`ran`). Before this, `done`
and `wait` asked for every job up to it. Two threads now run a worker's jobs, so a step could wait on
another one running ahead of it. The helper keeps the old rule, and so does a submit waiting for a
free slot.

What to look for in test 18:
- No freeze, especially when firing and in fights.
- The log's "the game thread runs the particle steps" line, once.
- The Sheegoth rooms against test 16's 12 FPS (and tests 7 to 10's 15.5 to 18.3).
- The side workers' `[side jobs]` lines: "the game thread ran N of them itself".
- Particles, effects and projectiles looking as before.

### Test 18 result

`~/logs/t046_18.log` (e51156d, 3.5 minutes in Phendrana): a clean exit, no freeze. The game thread
took slot 3 at its first wait (its stack below the wait: 0x84a01000 to 0x84e00330).
- **The Sheegoth rooms ran at 17.7 FPS** (median, 60 windows), against test 16's 12.0, and as tests 7
  to 10 did (15.5 to 18.3): 3.4 ticks a frame of 9.3 ms each, Think 3.0 ms a tick.
- **The particle waits fell to 0.4 ms a frame**, from test 16's 10.9 (`~/tools/pwaits.py`).
- The game thread ran 170 to 540 of the fourth core's worker's jobs itself in each 12 to 18 s report,
  about 0.1 ms each (21 to 51 ms a report). It ran 13 to 210 of side worker 2's.
- Its waits on side worker 2 came to 0.5 to 9 ms a report (test 16: 1.4 to 2.4 s).
- **Gyro aim turned the wrong way left and right** (the user: "the gyro aim's x-axis is reversed").
  Up and down were right. See test 19.

## 0.46.0 test 19: the Vita's gyro aim turns the right way

The Vita's pad has no motion sensor in SDL, so gyro aim reads the console's own through SDL's sensor
API (the "phone" source in `platform/debug_ui/input.cpp`). SDL's Vita driver passes `sceMotion`'s
readings through as they are. Its accelerometer gives gravity's own direction: lying face up, z is
-1 g. SDL's axes, and GamepadMotionHelpers after them (gravity = -accel), take a reading at rest to
point away from the ground. So the gravity the port tracked was upside down.
- In player space (`gyro_space=1`, the default) and world space, yaw is turned about gravity, so it
  came out reversed.
- Pitch is the gyro's own x axis, so it was right.
- The local spaces do not use gravity.

On the Vita the accelerometer reading is now negated before it is used. The first reading is logged
as SDL gives it (`port: gyro: the Vita's motion sensor, first reading ...`). Held normally, its y
should be negative, which confirms the sign.

What to look for in test 19:
- Gyro aim turning the same way as the console, left and right, with up and down as before.
- The log's "first reading" line.

### Test 19 result

`~/logs/t046_19.log` (df0d11a, 3 minutes in Phendrana, the Sheegoth fights): a clean exit. The user:
"the good ol' music overlapping itself", "the sheegoth briefly turn all black the moment [I] damage
them", and "hold to aim" and "toggle to aim" seemed to do nothing.
- **The gyro's sign is confirmed.** The first reading, held normally: accel -0.32 -7.00 -6.83 m/s²,
  toward the ground as predicted. No more word of a reversed x-axis.
- **Programs starved on the card.** 115 programs were loaded from the card's cache in all (one went
  to the compiler), taking 25 s, about 220 ms each. Where the Sheegoths fought, four took 10 s between
  them, one 6.2 s. A program's draws are fixed-function until it is linked, and the Sheegoths' hit
  flash needs a program of its own: black when hit. The "program cache" thread did these reads on
  core 0 at priority 170, below everything there. In the fights core 0 was full: render worker 43%,
  core 0 helper 32-46%, side worker 2 5-12%, the DVD worker. It ran 0-1% of the time.
- **A feedback loop.** Meanwhile 28 skinned draws a frame were drawn fixed-function, at 0.45 ms each
  (13 ms a frame, five times a program draw's cost). That kept the render worker at 99%, which starved
  the loads further.
- **The music came late in that same stretch** (log lines 12140-13159, render worker 99-100%). Four
  `refill late` lines: the streams' reads from the disc image waited more than 2 s on the card, where
  the starved card thread's reads were under way.
- **Hold to aim and toggle to aim.** The Gyro aim button's default is the right stick's click. The
  Vita's own controls have none: SDL reads them with `sceCtrlPeekBufferPositive2`, which gives no
  L3 or R3 on the handheld.

## 0.46.0 test 20: the program loads on the fourth core, the rear pad as the Gyro aim button

**The program cache thread on the fourth core** (`vita_gl.cpp`, `card_thread`), at priority 131.
That is below the side worker, the mixer and the background lane (128 to 130), which keep the core
about 55% busy in play. Without that core it stays where it was (core 0, 170). Through a movie the
background lane fills the core; a load then waits, but so do the movie's own reads, which the lane
waits on, so the wait ends itself.

**The rear touch pad is the Gyro aim button on the Vita** (`port_input_devices.cpp`). A finger on it
holds the button: hold to aim aims while it is there, hold to pause pauses, and toggle switches on
each fresh touch. It is read from the panel itself (`sceTouchPeek`); SDL's rear touches stay off, as
since b9b0124. A PS TV pad's right stick click still works. The Port menu's Gyro aim help says so.

**The music's late refills say how long they took** (`CDSPStream.cpp`): the `refill late` line adds
when its read was asked for, and a `late refill read done ... after N ms` line follows when it ends.

What to look for in test 20:
- The Sheegoths no longer black when hit, or for a frame at most.
- The music in the fights, and any `refill late` lines with their times.
- Gyro aim with hold to aim, hold to pause and toggle: a finger on the rear touch pad.
- The `presents` lines' program loads ("N loaded from the card ... ms").

### Test 20 result

`~/logs/t046_20.log` (1698cea, about 35,000 frames). It covers a new game's opening (the intro, the
space scene, the frigate), mods turned on mid-game, and the Fusion Suit unlock tried. No hang or error.
The log stops without its closing line: the game was closed from the home screen. (The crash dump in
Downloads from that afternoon is another app's, `msr-headless`.)
- **Program loads are fast again.** The program cache thread ran on core 3 at 131. 158 loads from the
  card took 42 ms each on average (test 19: about 220 ms); the slowest report's took 160 ms each.
- **Fresh compiles.** 106 programs were not on the card yet (the frigate's, and the materials whose
  shaders test 15's intensity textures changed). They were compiled on the Vita, 2.2 s each. Their
  draws were fixed-function meanwhile, up to 9,600 in a 300-present report. A one-time cost: the card
  keeps them.
- **The music came late four times**, all in the frigate (`int_introstage`). The late reads took 2.0 to
  2.4 s, one 4.1 s. Several came just after new programs were written to the card, but other bursts
  of programs passed without one. The card stalls now and then, and a refill has 2 s of slack: the
  streams' 4 s ring (0x11DC0 bytes, `CDSPStream`), refilled half at a time.
- Turning mods on reloaded them (the Echoes suits, 195 resources): a 3.1 s stall, then play went on.
- The frame rate through the run: median 36 FPS, 23 to 46 by stretch.
- Neither gyro aim nor the Sheegoths came up in this run.

## 0.46.0 test 21: the music's ring doubled

**The music's two streams have an 8 s ring on the Vita** (`CDSPStream.cpp`, `PortRingBytes`):
0x23B80 bytes of 32 kHz ADPCM, read 4 s at a time, where the game's is 0x11DC0 (4 s, read 2 s at a
time). A refill now has 4 s to arrive before its channel replays a half and falls behind the other.
That covers test 20's late reads but the 4.1 s one.
- The one-shot streams (2 and 3: jingles such as the item fanfare) keep the game's ring. A stream
  ends one refill after its first silent half, and a longer half would hold a jingle's end, and the
  music coming back after it, up to 2 s later.
- The cost is RAM, not ARAM. In the port's MusyX (its PC target), a stream's ring is host memory
  (`hw_aramdma.c`, `aramAllocateStreamBuffer`: malloc), and samples are not stored in the emulated
  ARAM either (`aramStoreData` returns NULL). So the bigger rings take nothing from the sound effects:
  292 KiB more in all (each music stream's staging half-buffers and its MusyX ring).
- The log says the sizes once at the start (`mpstream: music streams: a ring of 146304 bytes ...`).

What to look for in test 21:
- The music playing and looping as before: the frigate's, Phendrana's, the item fanfare and the
  music's return after it.
- `refill late` lines, now only past 4 s (`asked for ... ms ago`).

### Test 21 result

`~/logs/t046_21.log` (08825a0, 4.5 minutes, the front end then Phendrana with the Spider Ball): a
clean exit. The music's ring line is there (146304 bytes, 8001 ms, refilled 4000 ms at a time).
- **One late refill, a 7.9 s read** (`ice_worldmainR`). Nothing else was logged around it: the card
  stalled past even the new 4 s of slack.
- **The title and file select menus ran at 24 FPS**: "the main menu+save select menu became slow".
  On their movie screens tests 18 to 20 ran at 36 to 41 FPS, with 17 to 18 ms a present of the render
  worker's on textures. Test 21: 33 ms. Each movie frame is three new I8 textures (640x480 and two
  320x240 planes). Since test 15 they go to vitaGL as `GL_INTENSITY` from `GL_RED`, and vitaGL copies
  only like for like (its `fast_store`), so it put each of the frame's 460,000 texels through a read
  and a write callback: 20 to 40 ms a frame of the render worker's. Why this run was slower than test
  20 is not known; the cost was there before.
- **"Zoom it in to make it fit the Vita's screen (not by stretching)"**: the menus are drawn in
  proportion, their 4:3 frame centred. Their picture is a wide band letterboxed inside that frame,
  so black showed on all four sides (the user's photos).
- **"Even though you got the spider ball, it also still uses the default morphball model at the same
  time"**, with the Echoes suits mod on (the user: "I had the echoes mod enabled, silly me"). That
  import gives the Varia and Light Spider Balls their plain Echoes balls, as Echoes has none of its
  own. But Prime's spider ball glass, a shell for Prime's own spider ball, was still drawn round
  them: two balls at once. Only Dread's balls left it out.
- On the photos' run (not this log) the "Compiling shaders" toast counted 28 in the front end: the
  materials test 20's run had seen and not compiled, made at the start (the warm-up sends them).

## 0.46.0 test 22: menus at speed and zoomed, the spider ball's glass

**Intensity textures copied, not converted** (`vita_gl.cpp`, `Texture::bind`): given to vitaGL as
`GL_RED`, which it copies in as it is, then turned to `SCE_GXM_TEXTURE_FORMAT_U8_RRRR` once every
level is in (`sceGxmTextureSetFormat` on `vglGetGxmTexture`). U8_R and U8_RRRR are one layout with
two swizzles, so draws sample (I, I, I, I) as before. A movie frame's planes should now take a few ms.

**The menus zoomed to fill the screen** (Video > "Zoom menus to fill the screen", `zoom_menus`, on by
default on the Vita; off under Stretch menus and Original experience).
- `CFrontEndUI::Draw` scales GX's viewport about the screen's centre (`GXPortSetViewportZoom`, in
  `GXTransform.cpp`) until the 4:3 frame's 640 units span its width: 790/640, about 1.23, on the Vita.
- The layout is untouched, and `GXGetViewport` still gives the viewport as set. The Vita draws a
  viewport larger than the screen as it is, cut by the scissor.
- The zoom stops at 4/3, where a 16:9 band (three quarters of the frame's height) meets the top and
  bottom, for screens wider still.

**No Prime spider glass over a mod's ball** (`CMorphBall::LoadMorphBallModel`).
`PortDreadSamus::ModServesSpiderBall(suit)` is true when any mod's resources won the suit's Spider
Ball ANCS and main CMDL (`port_mods.cpp`, by the winning resources' mod names). The port's own
generated looks carry no mod name, so its Power Suit spider ball, Prime's recoloured, keeps the glass.

What to look for in test 22:
- The title and file select menus' frame rate (back near 40 or above) and their movies.
- The menus filling the screen's width, the band whole: the "MAIN MENU" title and the bottom legends.
- With the Echoes mod on: one ball with the Spider Ball. With it off: Prime's spider ball and its glass.
- HUD and other I8 textures looking as before.

### Test 22 result

`~/logs/t046_22.log` (3e97914, 6 minutes): a clean exit.
- **The menus run at 60 FPS**, the movie screens included: the render worker's textures took 2.0 ms
  a present (test 21: 33; test 20: 17). The user's photos show 60 FPS and the zoomed title filling
  the screen.
- **Press Start missing at the first title**, there after a return to the menu. Its texture's token
  (`TCachedToken`) was locked and tried once at `kP_LoadDeps`; at a first boot the texture was still
  loading then, and `GetObject` stays null without another `TryCache`.
- **Press Start "sitting at the bottom of the screen"**, against a PS4 port's picture the user sent.
  The frame is 640x480 (the GameCube's 480 lines; the Vita's is 846x480, so the zoom is about 1.32).
  The title movie (`Video/01_startloop.thp` from the user's disc, `~/tools/gcfst.py` and
  `~/tools/thpframe.py`) is a 16:9 band on lines 58 to 422 of its 480. The prompt (`TXTR_PressStart`,
  CMPR 192x32, `~/tools/pakfind.py`) is centred 72 above the frame's bottom, line 408: the band's last
  4%, beside the copyright. On a 4:3 TV the black under the band sits below it; zoomed, the band fills
  the screen and the prompt sits on its bottom edge. The PS4 port's is centred at 92% of the band.
- **The attract movies' "effect"** ("it looks kinda off"): uneven horizontal lines. Movies made
  without `deinterlace` (the attract movies, most endings) are drawn one field a frame, an 8x4 pattern
  keeping every other line by screen position. On an interlaced TV the two fields make one picture;
  on a progressive screen the other lines stay black, uneven on 544 lines and more so zoomed. The
  movie frames themselves are whole (attract0.thp: 640x480, letterboxed like the title).

## 0.46.0 test 23: Press Start at the first title and in the picture, movies whole

**Press Start at a first boot** (`CFrontEndUI::Update`): its texture is tried each frame of the title
(`x38_pressStart.TryCache()`), so it shows once it has loaded.

**Press Start in the picture when zoomed** (`CFrontEndUI::Draw`). With the menus zoomed, the prompt's
centre is 7.8% of the band's height above the band's bottom: line 394, as a 16:9 screen showing the
band places it. Unzoomed, it is where the game puts it. `PortMenuZoomFactor` gives the zoom to both.

**Every movie drawn whole** (`CCubeMoviePlayer.cpp`, `MyTHPGXYuv2RgbSetup`): the interlace pattern's
stage is left out on the port, as for the deinterlaced menu movies.

What to look for in test 23:
- Press Start pulsing at the first title, without a game first.
- Its place: inside the picture above the bottom, the copyright beside and below it.
- The attract movies (leave the title alone for a while) without lines.

### Test 23 result

`~/logs/t046_23.log` (02c63b1): a clean exit, with `pointer_menus=1`, `touch_controls=1` and the
Echoes mods on ("echoes-suits serves the arm cannon of 5 beam(s)"). The user raised no problem with
Press Start or the movies. Two others:
- **The main menu "kinda finicky"** by touch, "probably because it doesn't align with how the main
  menu is now". It doesn't: widgets record their hit rects as they draw (`PortGuiHit::RecordCorners`,
  `Project`), from `CGraphics`' projection and viewport. Test 22's zoom scales only GX's transform
  (`GXPortSetViewportZoom`), so the rects were the unzoomed menu's: about 1.32 times too close to the
  screen's centre. Rows away from the centre missed or hit their neighbours.
- **The Echoes arm cannon opening for a missile** (the user's photo: split open), which Prime 2's
  cannon never does. The import skins Echoes' cannon to Prime's gun skeleton by joint name
  (`ConvertCskr`, the beam ANCS with the model swapped, `PatchGunAncs`), so Prime's gun anims move
  it. A missile shot is Prime's anim 7 (`kGAT_MissileShoot`: open and fire), then the reload (8);
  7 s on, or at a beam shot, the cannon closes (5, `kGAT_FromMissile`). `ReturnToRestPose` plays 6
  (`kGAT_ToMissile`) after a fidget in missile mode. `CPlayerGun::Update` moves to its next state
  when the solid model's anim runs out, so these anims also set the missiles' timing.

## 0.46.0 test 24: menu taps where the zoomed menu shows, Echoes' cannon stays shut

**Hit rects zoomed with the menu** (`port_pointer.cpp`, `CurrentProjection`). The viewport the rects
are projected into is scaled about the zoom's centre as GX scales it (`GXPortGetViewportZoom`, new
beside the setter), so taps and clicks land on what is shown. It is 1 outside the front end.

**Echoes' cannon keeps shut for missiles** (`CGunWeapon::PlayAnim`). When the beam's gun is Echoes'
cannon (`mPortEchoesCannon`, from `LoadGunModels`), Prime's missile anims are played otherwise: the
missile shot as a beam shot (anim 4, its kick), the reload not at all, and the opening (6) and
closing (5) as the rest pose (0). The time of Prime's anim is kept (`mPortAnimStandIn`, counted down
in `Update`), and `CPlayerGun::Update` waits on it (`PortAnimTimeRemaining`), so missiles fire and
reload at Prime's pace. Any other anim the gun starts (shot, fidget, combo, return to default)
drops the stand-in. Sounds are unchanged. The log names each stand-in once ("gun: Echoes' cannon
...").

What to look for in test 24:
- Tapping each main menu and file select row: the row under the finger is the one chosen, at the
  screen's edges too. "Exit Game" (hold) on the file select as well.
- With the Echoes cannon: missiles fire with a kick and the cannon stays whole, also after waiting
  7 s or firing the beam. Missiles as fast as before.
- Super missiles and the other charge combos still play Prime's combo anims: say if those open
  the cannon too.

### Test 24 result

`~/logs/t046_24.log` (7ed9a9a): the stand-ins ran ("gun: Echoes' cannon plays anim 4 for Prime's
missile anim 7 (0.63 s)", the reload's 0.70 s left out, the closing's 0.30 s as the rest pose). The
user pointed at their Echoes disc: "I thought you needed the MP2 image so you can get the rocket
animation from there".

What the Echoes disc (G2ME01, `~/tools/mp2pak.py`) holds:
- `SamusGun.pak` has each beam's gun ANCS under Prime's ids ("Power", "Ice", "Wave", "Plasma"; no
  Phazon), pointing at ANIMs under Prime's ids too. Of the Power set's 18 anims, 14 pose every joint
  as Prime's do (`~/tools/animcmp.py`); the four missile anims differ.
- Prime's missile shot turns the four nozzle joints (`gNozTopRight_SDK`...) 55°: the barrel splits.
  Echoes' never turns them: `nozzle_SDK` turns 45° about the barrel and slides out 10 cm. The open
  (0.4 s in both) turns it to 45°; the shot (0.633 s in both) kicks back from there; the reload is a
  revolver turn (Echoes 0.333 s, Prime 0.7 s); the close turns it back (Echoes 1.1 s, Prime 0.3 s).
- Echoes adds `genericMissleFirstShoot` (B9A04995, 0.633 s): the shot from the beam's own pose,
  turning into the missile pose. Its transitions are Prime's (the snaps into a missile shot).
- Echoes' compressed ANIMs are Prime's layout one step on: 0x0101 where Prime has its EVNT id, u8
  joint ids, a scale track per channel, and channels with no track at all. The gun CINFs have the
  same 50 joints by name, parent and place (Echoes adds bind rotations, which its anims do not use:
  a joint without a rotation track is unrotated in both games).

## 0.46.0 test 25: Echoes' own missile anims for its cannon

**The import (echoes-suits version 9, `port_echoes_suits.cpp`).** For each beam's ANCS variant:
- `FindGunMissileAnims`: the ANIMs character 0 plays as anims 5-8 and their EVNTs.
- `ConvertGunAnim`: Echoes' ANIM of the same id (and the first shot) in Prime's layout under
  `Cannon(id)`. Joints are matched by name; channels with no track are left out; one with an offset
  alone gets a still rotation (Prime reads one for every channel); the keys are re-packed bit for
  bit; the EVNT is Prime's. Checked on the user's discs (`~/echoes_gun/check.cpp`, the Python
  prototype `~/tools/echoesmissiles.py`): every key of every joint decodes to Echoes' pose exactly.
- `AddGunMissileAnims`: `PortEchoes_FromMissile`, `_ToMissile`, `_MissileShoot`, `_MissileReload`
  and `_MissileFirstShoot` added to both characters and the animation set. Prime's missile anims
  stay; their transitions are mirrored onto Echoes'; the (ANIM, EVNT) list stays sorted (CCharacterFactory
  binary-searches it).

**The game (`CGunWeapon::PlayAnim`).** The Echoes cannon finds those anims by name (`LoadGunModels`)
and plays them for Prime's missile anims: the first missile shot (`mPortMissilePose` false) as
Echoes' first shot. The gun still waits Prime's anim's time (`PortAnimTimeRemaining`), so missiles
fire and reload at Prime's pace. An older import (no such anims) keeps test 24's stand-ins.

The mod has to be imported again (Mods > Echoes suits > Import, with the Echoes disc image in
`ux0:data/MetroidPrime2`), or the 15 files version 9 adds (`~/echoes_gun/mkpatch.cpp`, same code)
copied into `ux0:data/MetroidPrime/mods/echoes-suits` with the new stamp.

What to look for in test 25:
- Firing a missile from the beam: the nozzle kicks and turns into the missile pose, the barrel whole.
- More missiles: the revolver-turn reload between shots; 7 s on, or a beam shot, the slow turn back.
- Missiles as fast as before; each beam (Power, Ice, Wave, Plasma) its own open and close.

### Test 25 result

`~/logs/t046_25.log` (4b2825c): the user imported again on the Vita (the Echoes image in
`ux0:data/MetroidPrime2`). The 11 converted ANIMs came out byte for byte the size of the PC run's,
and Echoes' anims played: the first shot (character anim 24), the reload, the close, then a later
shot. The user's photos put Echoes (on a tablet) over the Vita, beam by beam, at rest and firing a
missile: "two arm canon uses the wrong textures and something all three have in common when you
shoot a rocket".
- **The textures.** The cannon's five sets, decoded from the Echoes disc (`~/tools/gunsets.py`):
  0 a grey-green body with a yellow glow (Power), 1 purple (Dark), 2 a blue body (Light), 3 an olive
  body with white streaks (Annihilator), 4 blue with electric veins. The photos pair the Vita's
  Ice with Echoes' Dark (right), Wave with Light and Plasma with Annihilator (by the HUD's selected
  beam in each). Mod versions 5-9 (upstream's, from 0.42.0) dressed Wave in Annihilator's set and
  Plasma in Light's. Echoes keeps Prime's gun ANCS slots for its beams (Dark the "Ice" slot, Light
  "Wave", Annihilator "Plasma"), so the swap also split each cannon's set from its own anims.
- **The missile shot.** The three photographed beams are the non-Power ones. Every other anim of
  their ANCS matches Prime's (`~/tools/beamcmp.sh`: base poses, shots, charges, fidgets, holsters);
  only the missile anims differ, as for Power, and the conversion gives Echoes' poses exactly. The
  rotation axes agree with Prime's where both animate a joint (the shot's recoil), so the two games
  read rotations alike. What "all three have in common" was not found from the photos alone: asked.

## 0.46.0 test 26: the cannon's Light and Annihilator looks on the beams Echoes gives them

**echoes-suits version 10** (`kGunSets`, port_echoes_suits_mod.h): Power, Ice, Wave, Plasma and
Phazon wear Echoes' sets 0-4 in order: Ice Dark, Wave Light, Plasma Annihilator. Imported again
from the Mods menu (the Echoes image stays in `ux0:data/MetroidPrime2`).

What to look for in test 26:
- After the import: Wave's cannon blue like Echoes' Light Beam, Plasma's olive with white streaks
  like the Annihilator; Power and Ice as before.

### Test 26 result

The user imported again (Wave's cannon now Light's blue) and sent a video of Echoes on a tablet
over the Vita, firing a missile in each: "when you shoot a rocket they're supposed to retract and
then extend open".
- Frames: Windows' own decoder (`frames.ps1` in the session scratchpad, Windows.Media.Editing
  thumbnails; it decodes this portrait video whole only at 540x960) and, at full size, the built-in
  browser with the video as a blob (seekable) drawn into a canvas grid.
- **The Vita does it.** Closed cone at rest (5.5 s), the shell sliding back over the barrel as the
  missile leaves (6.00-6.07 s), the kick (6.1-6.4 s), the plates open into a crown by 6.5 s, closed
  again by 16.5 s: Echoes' cycle in Echoes' own footage (closed until 3.3 s, open 4.5-11 s). The
  first-shot anim was rendered on the Echoes model posed as Prime's engine poses it
  (`~/tools/gunrender.py`, triangles from the CMDL, skinned by the CSKR): the four petals (`gNoz*`,
  material 1) are the mint shell; they spread and slide back as the nozzle retracts (frames 1-4),
  then push out turning 45°.
- **What differs is the shine.** The petals' mint is a TEV stage adding (a grey mask) x
  `268C5D8B`, a sphere map: a dark green ball on a mint ground. UV mode 6 (`CCubeMaterial
  HandleAnimatedUV`) takes it by the world normal plus 0.05 x the model's world position, which
  scrolls Prime's own streaky reflection `7ED07D1F` as Samus moves. On the sphere map that lands on
  the dark ball or the mint ground by where she stands: mint in the Phendrana photos, dark teal in
  the video's room, so the retract and open barely show.

## 0.46.0 test 27: Echoes' cannon shine from the camera

`CCubeMaterial::PortSetCameraModelUv`: while `CGunWeapon::Draw` draws Echoes' cannon, UV mode 6
uses the model matrix relative to the camera (`GetViewMatrix().GetQuickInverse() * model`), so
its position term is a few hundredths and its normals are the camera's. The lookup is then
steady: faces up, sideways or toward the camera take the mint ground, as Echoes' footage shows.
Prime's own gun keeps the world lookup. Echoes' renderer is not known to do exactly this (an
approximation of its look).

What to look for in test 27:
- The Echoes cannon's front shell mint at rest, in any room, and the plates visibly mint as they
  pull back and open on a missile shot.
- Prime's cannon (Echoes cannon setting off) as before.

### Test 27 result

The user sent two photos (Echoes on the tablet over the Vita, the video's rooms), at rest and in
missile mode: "How can you not see the difference?"
- Cropped to the cannon at one scale (the rings' width), the plates match: the same closed shell
  at rest; in missile mode the same crown, plates at the top and left over the rings, the same
  size and the same 45° turn.
- The colours do not: Echoes' open plates dark green with white edges and its body blue, the
  Vita's plates flat cyan and its body grey-green. Materials 1, 2, 4, 6 and 7 of the cannon all
  shine by mode 6 (the body's map is `7F202198`), so test 27's camera lookup tinted all of them.
- The user: "the difference is not color". It is the gills (the barrel's side panels). The Light
  Beam spreads them at rest (the light centre strip shows between them, in both games' rest
  photos). In Echoes' missile photo they are shut and the strip is covered; on the Vita they stay
  spread and the strip shows. See the gill item below and test 28.
- **Echoes' mode 6 is Prime's.** Echoes' DOL holds the same post matrix
  {0.5,0,0,0},{0,0,0.5,0},{0,0,0,1}, followed by mode 7's as in Prime's, and the code that loads it
  (0x80304fc4) reads 0.05 from SDA2 right after: the model's world position x 0.05
  (`~/tools/dolsearch.py`, `~/tools/dolref.py`). Echoes' own plates change from room to room
  (pale mint in the Phendrana-time photos, dark green in this room), as the world lookup does.
- **The gills.** Ice's, Wave's and Plasma's base poses (Echoes' Dark, Light and Annihilator)
  move the gill joints (`g1`-`g12`, `gBase`, `gPanel`, `L_g*`): Dark up and down by up to 4.7 cm,
  Light outward by up to 2.4 cm (and `g5` turned 20°), Annihilator forward by up to 9.4 cm.
  Power's is the rest pose. Each beam's open anim ends with the gills at rest and its close anim
  starts there; Echoes' generic shot, reload and first shot list the gill channels with no
  tracks. Prime's own generic missile anims carry every gill at rest instead.
- Prime's engine (`CAnimData::RecalcPoseBuilder`) inserts every segment's rotation each frame (a
  missing one is the identity) but an offset only when the anim has one. Its pose builder
  (`x2fc_poseBuilder`) persists, so a joint the anim leaves out keeps its last offset. Test 25's
  conversion left the gill channels out, so on the Vita the beam's gill offsets stayed through the
  missile anims: Light's gills spread, the Annihilator's forward over the rings (as in the user's
  test 25 photo, where its rings do not show). Echoes shuts them.
- The first shot's kick (`Skeleton_Root` 20 cm back) is in Prime's missile anim too; it plays
  the same on both.

## 0.46.0 test 28: Echoes' gills shut for missiles, its shine back to the world lookup

- **echoes-suits version 11** (`ConvertGunAnim`, port_echoes_suits.cpp): every channel Echoes
  lists is written. A missing rotation track becomes a still identity, a missing offset track a
  still offset at Prime's skeleton place (head minus the parent's head, in offset steps). So the
  generic missile anims hold the gills at rest, as Echoes does. Checked on the PC with the
  import's code (`~/echoes_gun/check`): all four beams convert, 50 channels each; every joint
  poses as Echoes' anim does. `animcmp.py` flags only the elbow, by its own fault: it takes a
  missing offset as the joint's model-space head, and the shoulder is not at the origin. The new
  offset is the one Prime's layout gives the elbow anyway.
- Test 27's `PortSetCameraModelUv` is gone (CCubeMaterial.hpp/.cpp and CGunWeapon.cpp as in test
  26): the Echoes cannon's mode-6 shine takes the world lookup again, as Echoes' own code does.
- Needs the Echoes suits imported again (Mods menu; the image stays in `ux0:data/MetroidPrime2`).

What to look for in test 28:
- After the import, a missile with the Light Beam: the side panels close flush (the light centre
  strip covered) and open again when the cannon leaves missile mode. With the Annihilator, the
  gills pull back from the rings. With Dark, they settle level. Power as before.
- The Echoes cannon's colours change with the place, as in Echoes (no even cyan cast).

### Test 28 result

The user re-imported (no stale-import note in the log; Echoes' first shot, reload and close
played) and moved on to an idea: pick each beam's cannon, and each suit's look (Prime's Power
Suit with Echoes' Varia helmet; Echoes' Varia for Varia; Echoes' Varia in the Gravity and Phazon
colours, or the Dark and Light Suits, for those two).

## 0.46.0 test 29: each beam picks its Echoes cannon

`echoes_cannon_looks` (Mods > Metroid Prime 2: Echoes, "... Beam's cannon"; default
`power,dark,light,annihilator`): each of Power, Ice, Wave and Plasma wears one of Echoes' four
cannons. `PortEchoesSuits::GunAncs` hands `CGunWeapon::LoadGunModels` that slot's ANCS variant (its
anims: the barrel's shape, shooting, charging and missile motions) and its material set index
(`x20c_shaderIdx`; the variant's sets are in slot order). Safe to borrow: every slot's gun
character has the same 20 anims (slots 9 and 10 are its beam-change poses), the anims' events
are the same panel and missile-switch sounds in every slot, and the combo's fire cue is a user
event of value 0 at LBEAM in each (`CPlayerGun` reads the value). Phazon keeps its own. No
re-import.

What to look for in test 29:
- Mods > Echoes: set, say, every beam to the Echoes Power Beam, then switch beams: each takes
  the Power Beam's closed barrel and colours, while shooting its own shots (Wave's arcs, Ice's
  freezing shots...). Then try other mixes. Charge combos still fire.

## 0.46.0 test 30: what each suit wears (echoes-suits 13)

`echoes_suit_looks` (Mods > Echoes, "Power Suit" ... "Phazon Suit"; EPlayerSuit order, default
`helmet,colours,varia,colours`): Prime's own, Echoes' Varia, Dark or Light Suit, "Echoes Varia in
its colours", or "Prime's, Echoes helmet". (Built first as mod version 12 without the helmet; that
build was not staged, so one re-import covers both.)
- **The helmet** (`SwapHead`): each suit's Prime cutscene, in-game and doll body (`ReadCmdl` reads
  the retail CMDL) with every triangle that has a vertex weighing most on Head_1 or a joint under it
  (eyes, lids, brows, Visor_LCTR) dropped, and the Echoes Varia's (in the suit's colours) that have
  one merged in (`KeepTriangles` keeps triangles by position index, wound as drawn; `MergeCmdl`;
  the skins' groups one after the other, a one-vertex group over the body's padding vertex
  between). Then `CompactSkinned` keeps only the positions and normals the lists draw (the donor's
  whole body came in): the cutscene helmet body has 4976 vertices to Prime's 5029. Under
  `HelmetId` of Prime's ids; the loader serves them over the suit and counts them apart
  (`HasOwnHelmet`): the doll draws Prime's boots and parts but not its visor model
  (CMDL_InvVisor), which the Echoes helmet replaces with its own glass. Prototyped in Python
  (`~/tools/helmetswap.py`, back-face culled renders) before the C++: the swap is clean at the neck
  with "any vertex on the head" both ways.
- **The import writes each look once under its own ids**, `Wear(worn, Varia's id)` (WornLook:
  Varia, Dark, Light, and the Varia in Gravity's and Phazon's colours): cutscene, in-game, doll and
  grapple-arm bodies with their skins, the right arm, and the balls fit to each Prime ball ANCS
  (plain 9FE58F7A, spider F37654A2, Phazon's D7F7C8EC). `kCopies` is gone.
- **PortMods serves a look over the suit wearing it** (`kSuitResources`: each suit's bodies,
  skins, grapple arm and right arm; the balls under the suit's variant ids, main CMDL included),
  plus the Varia's second cutscene skin 8C72FDAC and Tallon's Phazon body B333E1D7. Dark Samus is
  served after, over the bodies. `kBodies` now names Power's bodies too.
- **The colours** (`RecolourVaria`): Prime's Gravity Suit is its Varia with the limbs repainted:
  measured per joint on Prime's own bodies (`~/tools/bodymesh.py`: purple on the shoulder pads,
  shoulders, elbows, wrist and fingers, hips, knees, ankles, toes; the chest, abdomen, pelvis and
  helmet keep the Varia's; greens go cyan). Phazon's is near black (torso and helmet grey), its
  red in glow maps. The import votes each texel of the Varia's textures limb or not by the
  triangles over it (a body triangle by the joints its vertices weigh most on; the arm and ball
  all limb), recolours (Gravity: warm limb texels to Prime's purple, hue 246.4 + 0.35 x offset,
  saturation x 0.41, value x 0.50; greens + 75 deg. Phazon: warm texels saturation x 0.15, value x
  0.16 on limbs, 0.30 elsewhere; greens to red; the rest x 0.6), and writes them as CMPR (its own
  encoder, `EncodeCmpr`, with the source's mips) under Wear of their ids. 19 textures for Gravity,
  25 for Phazon.
- Run on the PC with the user's two discs (`~/echoes_suits/import`, the import's own code): 297
  files; the Gravity and Phazon cutscene bodies rendered from its output as the prototype
  (`~/tools/variarecolour.py`, `~/tools/bodyrender.py`).
- Needs the Echoes suits imported again.

What to look for in test 30:
- After the import (the mods reload): the Power Suit is Prime's with Echoes' Varia helmet; Varia
  is Echoes' Varia; Gravity and Phazon wear it in their colours (pause screen doll, cutscenes, the
  morph transition, first-person arms, the ball).
- The pause screen doll with the Power Suit: Echoes' helmet with its own visor, no second visor,
  Prime's boots and legs.
- Mods > Echoes: change a suit's look (say Gravity to Echoes Dark Suit), reload the mods, check it.

## 0.46.0 test 31: two choices each; the cannons' original looks (echoes-suits 14)

The user set the choices out after test 30 was staged (before trying it): "only the textures only
for the arm canons ... only two options for each beams and there should not be an option for the
power beam. the power should have only two options, prime's own or echoes helmet. the gravity suit
should have only two options, dark suit or the echoes varia in it's colors and the phazon suit is the
same with only light suit or the echoes varia in it's colors, there should not be an option for the
varia suit". Then, for the cannons: "Ice beam: original ice beam texture or dark beam. wave beam:
original wave beam texture or light beam. plasma beam: original plasma beam texture or annihilator
beam".
- **Cannons** (`echoes_cannon_looks`, same key; `prime` for the original): Ice, Wave and Plasma each
  wear their Echoes beam's set (Dark, Light, Annihilator) or Prime's original look. `GunAncs` always
  hands the beam its own ANCS variant (Echoes' barrel shape and missile motions; Prime's own gun
  models stay unused, as the user asked); only `x20c_shaderIdx` follows the choice. Power has none.
- **The originals** are new sets 5-7 of the cannon (`kOriginalSets`; `OriginalGunSet` in the
  import). Both discs' gun CMDL 19AE626C compared material by material: Echoes built its beam sets
  on Prime's. Its material 0 is Prime's material 0 byte for byte (TEV, texgens, UV scroll), and its
  material 3 Prime's material 2, the beam's own scrolling pair. Only Plasma's scroll differs there
  (Prime 6.67, Echoes 10). Its material 2 is a body texture and shine of each beam's own, which
  Prime does not have (Prime's body is the same for every beam). So each original is Echoes' set
  of that beam with Prime's textures in materials 0 and 3, Prime's material bodies where the vertex
  layouts match (all of them on these discs: Plasma keeps its 6.67 scroll), and the Power set's
  body and shine (8C15F00E, 7F202198) in material 2. Prime's textures stay on the retail disc. The
  game uses the originals only from an import of version 14 (`sEchoesImportVersion`, read from the
  stamp); an older import's cannon has five sets, so the choice falls back to the Echoes look.
- **Suits** (`echoes_suit_looks`, same key): `kSuitLookChoices` gives each suit's two looks: Power
  Prime's own or Echoes' helmet; Gravity the Dark Suit or the Varia in its colours; Phazon the
  Light Suit or the Varia in its colours; Varia only Echoes' Varia. `SetSuitLook` takes no other.
  The defaults stay (helmet, colours, varia, colours).
- The menu shows a two-way choice for Ice's, Wave's and Plasma's cannon ("Ice Beam (original)" or
  "Dark Beam (Echoes)"...) and for the Power, Gravity and Phazon Suits.
- Run on the PC with both discs (`~/echoes_suits/import_repo`): 321 files, only the arm cannon CMDL
  differs from version 13's; its sets 5-7 carry Prime's Ice (842E5E79; 2EE307AE, 142BF3A2), Wave
  (73934A6F; DCA4E794) and Plasma (E58C24C1, 20BEA729 in both) textures.
- Needs the Echoes suits imported again (version 14; it covers test 30's suits too).

What to look for in test 31:
- After the import: Mods > Echoes, Ice's cannon to "Ice Beam (original)": Prime's icy blue strips
  on Echoes' barrel, the body grey as the Power Beam's; the missile opening is still Echoes'. The
  same for Wave (Prime's purple) and Plasma (Prime's orange-red). Back to the Echoes beam: Dark's,
  Light's, Annihilator's colours again.
- The suit choices as above; Varia has none.

### Test 31 result

"looks good": the log has the version 14 import (321 resources), `echoes_cannon_looks` at
`power,prime,prime,prime`, the suit choices tried and reloaded (Gravity Dark, Phazon Light, back to
the colours, Power with the helmet). Three photos of F1 > Video: Quality and Frame rate at 1x, and
Quality with the resolution scale above 1x (dynamic resolution off), where the menu's text is solid
boxes and everything else in the menu draws right.

## 0.46.0 test 32: the menu's text at any scale; the Vita's ranges

The user: "change the resolution scale to 480p to 720p because realistically you can't play at
higher resolution, the settings text do become unreadable when I increase the resolution above 544p
to 720, you can probably fix that. change the fps cap scale to 30 t0 60 because there ain't no way
game will run higher than 60 and the sim hz to 30 to 60".
- **The text**: a frame drawn at another scale goes to the display through the copy program
  (`draw_world_to_display`). Its `use_program` turns each unit's coordinate array off with
  `glClientActiveTexture`, which leaves the client's unit at the last one; `glActiveTexture` went
  back to 0 but the client's unit did not. ImGui's GL2 backend sets neither: its
  `glTexCoordPointer` went to the last unit, and unit 0, where it binds the font, had no
  coordinates. Every glyph read one texel of the atlas, its white corner: a solid box (the panels
  and the check marks use that white texel anyway, so they looked right). At 1x the overlay
  follows `draw_world`, which ends on unit 0 for both. `draw_world_to_display` now ends there too.
  Below 1x it was the same (dynamic resolution's lowered frames too).
- **Resolution scale** (Vita): 480 to 720 lines (`PortDebug::kVitaScaleMin`/`Max`, 480/544 to
  720/544), a slider of 16-line steps (480, 496 ... 544 ... 720; one a D-pad press), labelled as
  before ("1.00x (544p)"). `SetRenderScale` and the saved `render_scale` keep a fixed scale inside;
  auto (0) stays the screen's. "Maximum resolution scale" (dynamic resolution, User) is the same
  slider. The dynamic minimum keeps its range.
- **FPS cap** (Vita): 30 to 60, where 60 is no cap (saved 0: the screen's pace, as "Unlimited" was);
  `ClampFpsCap` makes a saved cap of 60 or more none. `kFpsCapMax` is 60 on the Vita, so dynamic
  resolution's "Frame rate target" stops at 60 too.
- **Sim Hz** (Vita): 30 to 60 (`kSimRateMin`/`kSimRateMax`; the saved `sim_rate` and `MP_SIM_RATE`
  too). The game's 60 stays the default.

What to look for in test 32:
- F1 > Video > Quality, Resolution scale at 720p (1.32x): the menu's text reads; also at 480p.
- The slider goes from 0.88x (480p) to 1.32x (720p), 16 lines a step; FPS cap 30 to 60 (60 the
  top, no "Unlimited"); Sim Hz 30 to 60.

## 0.46.0 test 33: 4:3 pillarboxed

The user: "the 4:3 aspect ratio is not working". `ApplyAspectMode` narrows GX's framebuffer to 4:3
(`fbWidth` = `efbHeight` x 4/3, 640x480) and the cameras follow, as on the PC, where Aurora presents
the framebuffer in the window at its own shape. The Vita's renderer stretched any framebuffer over
the whole screen instead (`screen_rect`: x by 960/fbWidth, y by 544/efbHeight), so 4:3 came out as
the 16:9 screen with everything widened a third.
- `vita_gl::fit_to_screen` (vita_gl.hpp): where a framebuffer is drawn. One within 1% of the
  screen's shape fills it as before (the default "follow window" framebuffer is 790x480 or so, 16:9's
  852x480: 1.76 and 1.78 to the screen's 1.765). Another keeps its shape, as large as fits, centred
  on whole pixels, as Aurora's PC present fits it: 4:3 is 725x544 at x 117, black beside it.
- `screen_rect` maps GX's rectangles (viewports, scissors, copies' sources, clears) into that place
  and clamps scissors and clears to it, so nothing draws into the bars; the frame's clear leaves them
  black. Line and point widths scale by it (`screen_width`). It is worked out once a framebuffer size
  (`framebuffer_fit`). Copies read the picture's own pixels, so the visors, the pause screen and the
  reflections take the 4:3 picture.
- Touch: `PortPointer`'s layout (menus, the map's taps) already fitted the framebuffer to the window
  at its own shape, as the PC presents it, so it took 4:3 as pillarboxed while the picture was not.
  On the Vita it now asks the renderer's own fit (`GXPortGetScreenFit`, in fractions of the screen),
  so the two cannot drift (16:9 included, which the PC's fit would bar by two pixels).
- Nothing changes for "Follow window" (the default) or 16:9.

What to look for in test 33:
- F1 > Video > Display, Aspect ratio 4:3: the game in a 4:3 picture, black at both sides, in
  proportion (round things round); the menus, the pause screen and the visors inside it.
- Touch in the menus and on the map under 4:3: taps land where they are drawn.
- Back to "Follow window": the whole screen again.

## 0.46.0 test 34: the inner thighs as the abdomen; Prime's Spider Balls back (echoes-suits 15)

The user, with the earlier render of the Varia in Gravity's and Phazon's colours marked and photos of
Prime's own Gravity and Phazon Suits on the pause screen: "the color of the Echoes' varia suit's inner
thigh part (I've marked it) does not match the lower torso color, and bring back the spiderball
instead being recolors of the echoes morphball, don't need to change the color of the spiderball
since it already has the color based on all 3 Varia, Gravity, & Phazon suits", then "we just need to
revert the Echoes' morphball model from overwriting the MP1's spiderball models".
- **The thighs.** Echoes' Varia has a yellow undersuit panel on each inner thigh, the abdomen's
  yellow, between the orange plates. The recolour (`RecolourVaria`) votes each texel limb or torso
  by the joints its triangles weigh on, so the panel (all on the hips) was limb: purple in Gravity's
  colours, darker than the abdomen in Phazon's. Prime's own Gravity Suit keeps it yellow between
  purple plates (its thighs: 153 purple triangles, 105 of the undersuit's yellow-orange), its Phazon
  Suit as dark as the abdomen. Now a hip triangle votes with the torso when it shows the undersuit
  (`ShowsUndersuit`: the mean of its texels 36 to 70 degrees; measured per triangle, the armour's
  orange is 12 to 34, the abdomen 36 to 54) and does not face well out (`FacingMiddle` at least
  -0.5: the in-game body's plates have lighter patches of the same hue, facing out at -0.8 to -1,
  and there is a strip of yellow behind the outer plates, both purple in Prime's). Cut at -0.1 first,
  the front of the panel, which leans a little out, went purple too; the user's answer to the render
  of the version with no cut at all: "You did had it look like how I expected it to look like". At
  -0.5 the front renders are pixel for pixel that version's. Within such a triangle, a texel of the armour's red-orange
  (below 25 degrees) still votes limb (`VoteUndersuit`), or the in-game body's small texture left
  orange specks at the panel's edge. `ForEachUvTriangle` now winds a strip's odd triangles back.
  Checked on the PC with both discs: the cutscene, doll and in-game bodies rendered per pixel
  (`~/tools/bodyrender.py` legs, legs3q, legsback, legsside views) from the front, three-quarter,
  behind and the side; the shoulder pads, knees and arms are as before.
- **The Spider Balls.** Echoes has a Spider Ball for the Dark Suit only; the Varia and Light looks,
  and the Varia in Gravity's and Phazon's colours, had their plain Morph Ball written over Prime's
  Spider Balls (F37654A2, and Phazon's D7F7C8EC), since mod version 7. `kBalls` keeps the Dark Spider
  Ball alone, so the loader, serving only what the import wrote, leaves Prime's own (each suit's
  colours, and the spider glass around it). 293 files where version 14 wrote 321.
- Needs the Echoes suits imported again (version 15).

What to look for in test 34:
- After the import, Gravity in the Varia's colours (the default) on the pause screen: the inner
  thighs yellow as the lower torso, the plates purple; Phazon's inner thighs the abdomen's dark grey.
- The Spider Ball with Varia, Gravity and Phazon in the Varia looks: Prime's own, in each suit's
  colours. Gravity with the Dark Suit: Echoes' Dark Spider Ball still.

### Test 34 result

Two photos of the pause screen's doll from behind, Phazon's and Gravity's colours, with the
wedges circled: lighter triangles (yellow-orange, light grey) in the plates at the back of both
thighs. The import had taken them for the panel: they show the undersuit's hue (the plates'
lighter patches) and face out by less than half (-0.5), as the panel's front does, but backward.

## 0.46.0 test 35: no wedges at the back of the thighs (echoes-suits 16)

- `FacingOf` gives a triangle's lean toward the body's middle and forward. A thigh triangle of the
  undersuit's colour votes with the torso when it faces in or across (to -0.1), or out by up to
  half (-0.5) while facing forward: the panel's front. Facing back, that lean is the plates' wedges
  (22 triangles on the doll and cutscene bodies, 11 a thigh); they vote limb again. The in-game
  body has none in that band.
- Checked on the PC with both discs: the front renders pixel for pixel the look the user approved;
  from behind (`bodyrender.py` legsback) the wedges are gone in both colours.
- Needs the Echoes suits imported again (version 16).

What to look for in test 35:
- After the import, the doll from behind in Gravity's and Phazon's colours: the plates whole at the
  back of the thighs; from the front, as test 34.

### Test 35 result

The wedges were gone, but "you sorta fixed it but made another tiny mistake and that's what puzzles
me because the default Echoes Varia suit has no problem with this": slivers along the panel's edge
at the back (circled on the PC render), with a photo of Echoes' own Varia from behind, its panel and
plates parted by a clean red line. Then "The low poly models need to be fix too", with photos of the
in-game body (Gravity's and Phazon's colours beside Echoes' Varia on a PC): ragged panel edges.

The cause: whole triangles took one side or the other. Echoes draws the panel's edge as a line
across triangles, so a triangle over it came out all panel (the plate's orange left beside the
purple) or all plate (purple into the yellow); the in-game body's few big triangles over a small
texture made it ragged. Echoes' own Varia, not recoloured, shows the line as drawn.

## 0.46.0 test 36: the thighs' panel texel by texel, grown from the inner thigh (echoes-suits 17)

- `KeepUndersuit`: the thigh triangles facing the panel's way mark where it may be
  (`MarkPanelSide`: 2 facing in or across, to -0.1; 1 leaning out by up to half, to -0.5, front or
  back; they vote limb as any limb triangle). After the votes each texel there is judged by its own
  colour. Measured texel by texel, the panel is 39 degrees and up, the plates 36 and down (doll: 88%
  of each on its side; in-game alike). A yellow texel (38 to 70) is yellow when its 5x5 neighbourhood
  holds more yellow than orange; any other texel (orange, the line, a shade) only deep in yellow (ten
  more of it round it), so the line Echoes draws stays the plate's.
- The panel is the yellow texels joined, side by side through yellow, to one where it surely is
  (mark 2). It ends along Echoes' line and reaches round the back of the thigh as far as the panel
  does there. A plate's lighter patch, as yellow but cut off from the panel by that line or the
  plate's orange (test 34's wedges at the back), is not joined and stays purple.
- On the way: the user's circles on the PC render of the first version of this (back of both
  thighs, both bodies) were a strip of the panel beside the line painted purple. Traced pixel by
  pixel to the triangles under them: the facing cut of test 35 (leaning out only facing forward),
  5442 of 5455 such pixels on the doll, 3527 of 3532 in-game. Cut at -0.5 throughout instead, the
  wedges came back (their texels are as yellow as the panel); grown from the inner thigh, they do
  not.
- Checked on the PC with both discs: the front renders pixel for pixel the version before (the look
  the user approved); at the circled spots the strip is yellow to the line (Varia's yellow painted
  purple there: 5502 to 3689 pixels on the doll, 3551 to 1181 in-game, the rest the plates' light
  patches, rightly purple); no wedges; the side's outer plates whole.
- Then, on that version's render: "they are quite small to notice but noticeable for people who
  likes to pay attention to small details", circling the back of the inner thigh round the curve
  Echoes draws there: purple bits along the curve and the line down the leg, and at the crotch
  piece. Traced to their texels (237 in the circled box on the doll): the panel's borders, which
  Echoes shades a deeper yellow (36 to 37 degrees, as some of a plate's orange), and the darker
  orange either side of its drawn lines, neither yellow enough to grow through. Two more steps:
  - a piece of the texture all panel goes whole: covered texels side by side, bounded by the red
    line and the gaps between pieces, holding some where the panel surely is and of the panel's
    shades (warm, 36 degrees and up) for 80% of their warm texels. A piece holding a plate too keeps
    the texel-by-texel growth.
  - what is left round the lines drawn on the panel (lines, the orange either side, shades; not the
    bright red line, which stays the plate's) goes with the panel where the panel surrounds it more
    than solid plate does (three rounds). The growth also passes the dark lines drawn on the thigh,
    though not the uncovered gaps between the texture's pieces.
  Seven texels left in the box, five of them a near-black blue the same in both colours. The front
  and side renders of the doll are unchanged pixel for pixel; every changed pixel elsewhere is on
  the panel (purple to its yellow) or a CMPR block's shift within it.
- Known, from before test 34: a sliver of the red line, a few texels, stays red where it meets the
  cables at the top of each thigh plate at the back (texels the pelvis's triangles hold).
- Needs the Echoes suits imported again (version 17).

What to look for in test 36:
- After the import, the doll from behind and the in-game body up close, in Gravity's and Phazon's
  colours: the panel's edges where Echoes' Varia has its red lines, the strip beside them at the
  back of the thighs yellow (Phazon: the abdomen's grey), no wedges, no slivers or ragged bits.

## 0.46.0 test 37: a stand-in while a light count's program is on its way

The user, with test 36's log (480p, the Phazon Mines from the save station through the Elite
Pirates, the elevator and the drones): "I noticed some flickering in Phazon Mines".
- The log: draws drawn fixed-function while their program was on its way, up to 4,135 a report (300
  presents; 43 list draws a frame at the worst), and up to 977 left out so. 395 programs were loaded
  from the card in the session at 123 ms each (100 to 300 in the Mines; test 20's frigate 42), and
  86 compiled at 3.5 s each, one at a time in the fourth core's spare time.
- Why it flickers: a material's program is made for the lights each channel sums
  (`program_lights`: none, 1, 2, 4 or 8). A beam's shot, an explosion or a pirate's shot lights the
  walls and bodies it passes, one light more, and each count is another program, which the first
  draw with it waited for: drawn fixed-function meanwhile (its texture by its vertex colour, flat)
  or left out, and back once the light had passed. So surfaces flashed at shots and fights. The
  Mines are new ground for these programs (new materials, new counts), so few were on the card.
- `tev_stand_in` (gx/vita_tev.cpp): a draw whose program is not linked takes a linked program of its
  family (`tev_program`'s: the same state, other light counts), with rows made for it
  (`tev_vertex_rows`): one summing at least its lights when there is one (the rest dark: the same
  picture), else the one leaving out the fewest (the mask's last lights, for the frames till its
  own is linked). Its own is built on meanwhile (`ProgramInputs::wanted`: the render worker's
  `ready` as at a draw of its own). A material whose family has none linked yet (its first sight in
  a session) is drawn fixed-function as before.
- Nothing changes on the card: the same programs and keys, nothing compiled again.
- The log: the presents line counts "with a stand-in while theirs was on its way", the materials
  line "of the draws with another light count's".

What to look for in test 37:
- The Phazon Mines' rooms, shooting near walls and in the pirates' fights: no surfaces going flat or
  missing for a moment and back. A material seen for the first time in a session may still show flat
  briefly before its program loads.
- In the log, stand-ins where the fixed-function counts were, and those counts far lower.

## 0.46.0 test 38: the patched disc's pause screen as its patch draws it; the doll's face (echoes-suits 18)

The user, with photos of the pause screen: "I like that the port gives you a widescreen hud but I
hate how it handles the pause menus, it looks terrible spread apart like that compared to my patched
game image, also her face isn't visible when you view the suit in the pause menu and there are
visible lines on the the visor with the echoes mod, also the light suit has it's own morphball
model, you mistakenly swapped it with the spiderball model". Then: the Desktop's
`Metroid Prime.iso` is the modified image (the one on the Vita), `Metroid Prim (Rev 2).iso` the
unmodified one.
- **The disc's patch**: against Rev 2, its GGuiSys.pak differs only in eight FRMEs (the combat,
  scan, thermal and ball HUDs, the map screen, the pause screen and its instructions), each a few
  floats of widget transforms: a 16:9 patch for a picture a TV stretches across a 16:9 screen. The
  pause screen's head widget is widened by 1.12 and two of its widgets narrowed by 0.75 against the
  stretch. The port's widescreen spread (the pause frames' cameras match the screen's aspect and,
  with the Widescreen HUD, spread the sides to the edges) pulled the patched panels apart; the
  user's second photo is the patched frame stretched.
- `PortPauseCamera` (CPauseScreen.cpp): a pause frame whose head widget is scaled across (more than
  1% off 1) is drawn as its patch expects, its authored projection stretched across the screen
  (`SetAspectMatch(false)`). Any other pause frame is as before (matched; spread with the
  Widescreen HUD). The doll keeps its proportions (CSamusDoll's perspective takes the viewport's
  own); touch takes the drawn rectangles. The in-game HUD and the map screen are as before.
- **The face** (echoes-suits 18): the doll draws its body three times
  (CModelData::MultiLightingDraw): depth alone, then lit and then its pulse, both against that
  depth. Prime's doll bodies have no glass (the visor is CSamusDoll's InvVisor, drawn after), but
  Echoes' have it in the body, as Prime's cutscene bodies do: a glass at 27% (alpha-blended, konst
  alpha 0x45) and an added glow over the face (484 triangles, texture 94CC8E0E), both depth-sorted
  and writing depth. The depth pass wrote the glass's depth, so the face never drew, and the glass
  and glow over the dark read as a solid red-orange visor, their textures' outlines as lines (a
  render of that order matches the photo). The import's doll bodies (Varia, Dark, Light, and the
  Varia's colours and helmet dolls made from it) now write no depth from their depth-sorted
  materials (`SortedWithoutDepth`): the face draws, the glass over it. The Light Suit's visor takes
  its alpha from its texture (72% on average), so its face shows faintly, as in Echoes.
- **The Light Suit's ball** (echoes-suits 18): it stands for its Spider Ball too (kBalls), over both
  of Prime's Spider Ball models; the Varia and the colours looks keep Prime's (test 34's choice).
  Prime's spider glass is left out over it (test 22's `ModServesSpiderBall`).
- On the PC with both discs: 300 files; against version 17's last output only the nine doll bodies'
  materials (their depth-sorted ones' depth write) and seven new Light Spider Ball files differ.
- Needs the Echoes suits imported again (version 18).

What to look for in test 38:
- The pause screen (inventory, options, log book) with the patched disc: its panels together, as in
  the user's photo of the patched game; the in-game HUD as before.
- After the import, the pause screen's doll zoomed in: her face through the visor (Dark Suit's
  orange, Varia's green; faintly through the Light Suit's); the visor's lines faint.
- The Light Suit look with the Spider Ball: the Light Suit's own ball.

## 0.46.0 test 39: the patched pause screen on an unpatched disc too

The user, with test 38: "The pause menu is still the same with widescreen hud enabled with the
unpatched image". Test 38 drew only a patched disc's pause frames the patch's way; an unpatched
disc's were matched and spread as before.
- The patch's pause screen edits (test 38's diff against Rev 2): the head widget's m00 1 to 1.12 in
  FRME_PauseScreen and FRME_PauseScreenInstructions, and in FRME_PauseScreen the title's text pane
  (`textpane_title`, the "[ Inventory ]" heading) and the zoom prompt's icon (`basewidget_yicon`)
  1 to 0.75 (each the first float of its orientation, where the two FRMEs differ).
- `PortPauseCamera` (CPauseScreen.cpp): with the Widescreen HUD on and a wide screen (the aspect
  not 4:3), an unpatched disc's pause frame takes those edits as it loads (`ScaleAcross` on the
  widgets' idle transforms) and is drawn stretched as the patched one is: both discs' pause
  screens the same. With the Widescreen HUD off it keeps its proportions, unspread, as before. A
  patched disc's frames are drawn the patch's way either way. Toggling the Widescreen HUD in the
  pause screen's options applies from the next time it opens.

What to look for in test 39:
- With the Rev 2 disc (or any unpatched one) and the Widescreen HUD on: the pause screen as with the
  patched disc, its panels together, the "[ Inventory ]" heading and the zoom prompt in proportion.

## 0.46.0 test 40: the pause screen matched and never spread

The user, with a photo of test 39 beside their photo of the patched game: "it's too wide" (the panels'
outer icon columns off both edges of the screen).
- The head widget's scale counts twice: the camera hangs from the head widget as the panels do
  (FRME_PauseScreen: the camera, the lights and `basewidget_pivot`, everything else's root, are
  its children), and the view takes the camera's rotation transposed
  (CGraphics::SetViewPointMatrix, the original's), not inverted. The patch's 1.12 widens the layout
  1.25, for a picture drawn at the screen's own aspect. The user's photo of the patched game is that:
  an earlier build (its prompts the GameCube's Y and Start), the pause screen matched to the screen
  and not spread (the Widescreen HUD off then: test 36's log still had `hud_wide=0`). Stretched
  across the screen as in tests 38 and 39, it came out 1.32 times wider again.
- `PortPauseCamera`: the pause frames are matched and never spread (`SetAspectMatch(true, false)`),
  whatever the Widescreen HUD (the in-game HUD keeps its spread). An unpatched disc's frames take the
  patch's edits with the Widescreen HUD on a wide screen (as test 39), so both discs look the same;
  with it off, the original's layout, unspread. The instructions frame is the pool's: widened once,
  it reads as patched after, so it is not widened again.

What to look for in test 40:
- The pause screen with the patched disc, and with the Rev 2 one and the Widescreen HUD on: as the
  user's photo of the patched game, the icon columns at both sides on the screen.

## 0.46.0 test 41: switches to find the Phazon Mines' dark patches

The user, with seven photos: the flickering in the Phazon Mines is "these floors and grate like
textures", dark patches on them, blocky (screen-aligned squares about 32 of the frame's pixels
across at the 0.88 scale's 848x480: the GPU's tile size) and spiky, changing from frame to frame.
Test 37's stand-ins were for something else.

Ruled out from the log and the code:
- No EFB copy goes unmade or comes late in any report window (copies41.py), and the copies'
  coordinates are right.
- Upscale sharpening is off for the user (`upscale_sharpen=dynamic` with dynamic resolution off).
- No texture pack: `texture_pack=1`, but 0 replacements loaded from user_textures.
- No draws left out or drawn fixed-function there, nor the room geometry kept on the GPU, the
  port's shadows, projected decals (HDR only), anisotropy (vitaGL has none), the indirect state's
  dirty bits.

A suspect in vitaGL: an uncompressed texture's (RGBA8, I8, IA8) levels after the first are not the
port's. glTexImage2D at a level above 0 calls gpu_alloc_mipmaps, which ignores the data and makes
the chain again from the first level with GPU transfer downscales (`sceGxmTransferDownscale`, in
U8_R and U8U8_GR for the intensity ones). BC1 levels are copied in as given. (Its chain is sized
till the shorter side reaches 1; the port's levels stop at 16 texels, so none lies past it.) Floors
seen at a low angle and fine grates are where the small levels are sampled, and the Mines come
after test 14, when the non-CMPR textures got their levels.

F1 > Performance > Diagnostics (PS Vita), for the session only: never saved, all off at each start.
Each box logs `port: diagnostic "..." on/off`, and the renderer's own log
`diagnostics: ...`.
- No mipmaps, one box per texture kind (`GXPortSetVitaDiagnostics`, vita_gl's `kDiagNoMips...`,
  `Texture::mips_off`): BC1 (GX's CMPR), RGBA8 (the other colour formats, paletted ones among
  them), and intensity (I4/I8/IA4/IA8 as I8 and IA8). That kind is sampled from its first level
  alone, as before it had levels. A texture already bound takes the change at its next bind.
- Leave out, each a check at the top of its draw (`PortDebug::DiagOff`): floor reflections
  (`CCubeRenderer::CacheReflection` draws and copies nothing, `GetRealReflection` gives black),
  screen-distorting effects (`CElementGen::RenderParticlesIndirectTexture`, its screen copy too),
  decals (`CDecal::Render`), projected shadows (`CProjectedShadow::Render`), blob shadows
  (`CSimpleShadow::Render`), fog volumes (`CCubeRenderer::RenderFogVolume`).
- Work out every draw's material again (`kDiagMaterials`): `current_material` rehashes all of GX's
  state for each draw, as if every register were written, and rebuilds the material in place
  instead of taking the last draw's or the cache's.
- Wait for the GPU after each frame (`kDiagFinish`): `sceGxmFinish` after the swap. A scaled
  frame's first scene is its framebuffer's, which waits for no display buffer, so at a scale the
  GPU can be further behind the render worker than at the screen's size; with this nothing a frame
  drew with is let go or written again while the GPU may still read it.
- Not a display-list switch: decoding every list each frame would make and upload every room mesh
  again each frame, too much memory.

Why 4:3 and the original experience run faster (the user's question): heavy rooms are GPU-bound,
and 4:3 draws a narrower 3D view: a quarter fewer pixels, and less of the room in view. The
original experience is 4:3 at 1.00x drawn straight to the display (no 848x480 framebuffer and its
draw to the screen), with mods and the port's extras off. In the user's log the GPU was busy
28 ms a frame before it was switched on and about 15.5 ms after, though the user was moving.

What to look for in test 41:
- In a Mines room with the patches: which box makes them stop, ticked one at a time (and which
  frame rate the slow ones give). Resolution scale 1.00x too: the screen's own size, no framebuffer.

## 0.46.0 test 42: the scaled frame's scissor; resolution scale 360 to 544 lines

The user, without trying test 41's switches: the patches come "only when the resolution scale is
below or over 544p", and they did not want the diagnostics. Test 41's code is out again (its notes
above stay as the record).

So the patches are the scaled frame's, drawn into a framebuffer of its own (WorldTarget) rather than
the display. The photos again: screen-aligned blocks about 32 of the frame's pixels across (one
lighter square in photo 1 measures 32x32: the GPU's tile), some darker and some lighter, the floor's
pattern in them as if drawn twice or offset, and spiky edges along the grates' holes.
- The vitaGL the port links is not the source read so far: the vdpm package
  vitaGL-0.0.0.r1488.g2bdbe89, commit 2bdbe89 of 2026-09-23 (`~/vgl2bdbe89` holds its source, from
  `git archive` of ~/src/vitaGL), 37 commits before that checkout. Built without shared render
  targets (no `get_free_render_target` in the library) and without its texture cache.
- Ruled out in it: the framebuffer's render target (made for the 8 scenes the port asks), the
  framebuffer struct the port mirrors (`VglFramebuffer`: the depth surface at 56, 104 bytes, as
  2bdbe89's), the depth surface's memory and its load and store across copies, the tile clipper's
  orientation at a framebuffer's scene start (set upside down there, but always set again right
  after: every such scene follows another framebuffer's).
- The difference found: vitaGL's scissor is a mask bit in the depth surface. Each scissor change
  (`update_scissor_test`) clears the mask over the whole target with a pass whose stencil test never
  passes, then sets it inside the rectangle. The display's depth surface has a stencil (DF32M_S8); a
  framebuffer's has none (DF32M) unless a depth-and-stencil renderbuffer is attached. Without a
  stencil that test cannot fail, the mask stays set everywhere, and a draw is kept to its scissor
  only by the GPU's tile clipper (`sceGxmSetRegionClip`), to the 32-pixel tile.
- The fog volumes are drawn a screen chunk at a time, each chunk under its own scissor
  (`CCubeRenderer::ReallyRenderFogVolume`): spilled to the tiles' edges, the chunks overlap each
  other and the scene around the volume, in tile blocks, changing as the camera moves. The Mines'
  pits under their grates hold fog.

The fix (`make_world_target`): a `GL_DEPTH24_STENCIL8` renderbuffer attached to the frame's
framebuffer (`GL_DEPTH_STENCIL_ATTACHMENT`). vitaGL then makes its depth surface DF32M_S8 at its
first scene, still in the framebuffer's own `depthbuffer`, so `keep_depth` is unchanged; about
0.4 MiB more at 480 lines. 2bdbe89 frees only a framebuffer's depth when it goes (its fbb190f frees
the stencil too), so `release_world_target` hands the stencil to vitaGL's collector
(`vglLazyFree`); to be taken out if vitaGL is updated past fbb190f.

The resolution scale's range, as the user asked: 360 to 544 lines (was 480 to 720), in steps of 8
lines (16s from 360 would miss 544); the saved 480 lines stays as it is. Nothing is drawn above the
screen's size now.

What to look for in test 42:
- The Mines' floors and grates at 480 lines (and any scale under 544): no blocks.
- The resolution scale slider: 360 to 544 lines, 8 lines a step.

## 0.46.0 test 43: committed to 544 lines

The user, after test 42: "I guess we don't need Dynamic resolution, auto render scale, resolution
scale, and sharpen when upscaled. We're committing to 544p", and on the saved settings: "I can still
reset the configuration or delete it".
- F1 > Video > Quality on the Vita no longer offers dynamic resolution (with its minimum, target and
  status), the auto render scale, the resolution scale or the upscale sharpening (`video.cpp`,
  `#ifndef __vita__`); nor does the settings search (`port_settings_index.inc`). Other platforms keep
  them.
- Nothing else changes: the settings are still read and saved as before. A saved `render_scale`
  still applies, kept within 360 to 544 lines (`kVitaScaleMin`/`Max`, now only for that), till the
  settings are reset; the defaults are the screen's own size with dynamic resolution off. The
  scaled framebuffer path stays in vita_gl.cpp (test 42's stencil with it), reached only through
  such a saved scale.

What to look for in test 43:
- F1 > Video > Quality: anisotropic filtering, then the HD font; none of the four. After resetting
  the settings, the log's `port: settings:` line says `scale auto` (the screen's) and
  `dynamic-res off`.

## 0.46.0 test 44: the map screen's crash; the section clocks' hitch

Test 43 died at the full map screen (the log's last lines; the settings reset, frames at 1.00x):
`vita: out of memory: operator new(5309088) failed on thread 0x4002025F` (the FIFO processor), the
heap at 181.9 MiB in use of 224.2 MiB taken, 42.3 MiB free but not in one piece of 5.3 MiB, the
system's 229.4 MiB nearly all taken; then `std::bad_alloc`, uncaught.
- The stack (01952f0's ELF): `vita_gl::record(Draw&&)` from `record_draw`: the frame's draws, one
  `std::vector`, growing. 5,309,088 bytes are 11,442 draws of 464 bytes: the vector, reserved at the
  last frame's 5,721, doubled past it. (Written as "5,721 draws of 928 bytes" at first; test 45
  measured a draw at 464 with the Vita's compiler.)
- The map screen opened: the map's GX begins went from the minimap's 100-270 a frame to 6,941
  (82,944 vertices), 5,718 draws a frame, 6.3 FPS. Each room's faces and outlines are drawn with the
  room's matrix and colours, so few join the draw before them (`merge_into_last`). The same as
  0.42.0 test 3's Chozo map (merging made it rarer) and test 9's vertices (chunked then).
- The fix: a frame's draws in blocks of 128 (`FrameDraws`, 58 KiB a block, from a pool keeping 48),
  and its programs' constant rows in chunks (`FrameChunks<std::array<float, 4>>`, 128 KiB chunks;
  they were one vector reserved anew each frame at the last one's size). No per-frame piece is larger
  than a chunk now; the same pieces come back frame after frame. `ChunkPool` takes its chunk size
  and how many it keeps as template parameters; `FrameChunks::holds` checks a draw's rows at the
  render worker (was `at + count > size`).

The section clocks (`[draw path]`, `[fifo path]`) timed one frame in 8: some 10,000 and 14,000
clock reads (0.77 us each) in that frame, 8 and 11 ms more on the render worker and the FIFO
processor, a hitch every eighth frame. Now one in 64 (`kTimedEvery`).

Where a frame goes at 544 lines (test 43's log, a gameplay window, ~1,080 draws a frame, 23-31 FPS,
frames 1 to 5 vblanks apart):
- FIFO processor 34-36 ms a frame busy (71%): GX state 6.2, recording the draws 5.7 (464 bytes
  each, built then copied), materials 4.8, programs' rows 3.9, Aurora's own 3.2, dispatch 2.4,
  other draws 2.4, immediate 2.0, list lookups 1.6, meshes 1.0.
- Main thread ~32 ms a frame (79%): the game's draw 14.6 (`CCubeModel::DrawSurfaces` 5.5, the HUD
  2.9), two 60 Hz ticks 10.0, waiting for the FIFO 3.3, input 1.7, the loader 2.2.
- GPU 26.7 ms a frame busy (70%); render worker ~20 ms (40%).
For a steady 30 (33.3 ms) the FIFO processor is the first to bring down, then the main thread's draw;
a program draw uses little of its 464 bytes (the fixed-function units, 96, its matrices, 112, held in
its rows too, and its state, 32, are for the fixed-function path).

What to look for in test 44:
- The full map screen, opened and kept open in a large area: no crash (it stays slow).
- Fewer hitches: the section clocks' frame comes every 64 now.

## 0.46.0 test 45: optimization round 1, a draw's fixed-function part apart

The user: "We still need some optimization to make it reach around a stable 30fps", and for round 1
(the FIFO processor) "Yes, start".

A draw (`vita_gl::Draw`) was 464 bytes (measured with the Vita's compiler: `ProgramInputs` 148,
`DrawUnit` 48, `RenderState` 32, `FrameMesh` 20), built zeroed on the FIFO processor's stack for each
draw, filled, compared with the last for a merge and copied into the frame. 240 of them, its
position matrix (48) and projection (64), its two fixed-function units (96: each a texture kept, a
sampler and a matrix worked out from GX's texture matrix) and its fixed-function state (32), are read
only where it is drawn fixed-function: without a program, or while its program is not linked. In
play nearly every draw has a linked program (test 43's log: 0 fixed-function waiting a window).
- `DrawFixed` holds that part, in the frame's own chunks (`Frame::fixed`, `FrameChunks<DrawFixed,
  128>`, 30 KiB a chunk), recorded only for a draw whose program (its own or a stand-in) is not linked
  as it is recorded (`record_fixed`; `Draw::fixed` its place, else `kNoFixed`). A program once linked
  stays so, so the render worker never needs the part of a draw recorded without one; if it ever did,
  the draw is left out rather than read amiss.
- A draw is 228 bytes now. A draw with a linked program skips its units' matrices (`tex_matrix`,
  two a draw) and their textures' keep, and its matrices are worked out into the FIFO processor's own
  (for its program's rows) rather than into the draw.
- Merging (`merge_into_last`) compares what the program path draws with: the program, its textures,
  samplers and rows (the matrices among them), viewport, scissor and width; no longer the matrices,
  units and state the program path does not read. A draw with a fixed-function part joins none.
- A palette copy's two textures (the copy and its palette) go in `program.textures[0]` and `[1]`
  (were units 0 and 1; a copy has no program). The masked clear (a fixed-function quad) records its
  part. The GPU cost test sorts by the state the draw draws with (its part's, else its program's).
- The materials line counts the draws recorded with a fixed-function part.

What to look for in test 45:
- The same picture everywhere (a draw drawn fixed-function only while its program loads, as before).
- The `[fifo path]` line's record section against test 43's 5.7 ms a frame, its busy total against
  34-36 ms; the presents line's draws a frame (more join now), the materials line's fixed-function
  count (near 0 in play).

## 0.46.0 test 46: the dust's particles cheaper on the GPU; the Phazon Suit's aura

The user, after test 45 (a photo at 9 FPS in a dusty tunnel): "the phazon suit's aura barely
shows and the dust effect cause the fps to drop and since there's a lot in Transport Tunnel B, the
fps gets really bad".

**The dust.** Test 45's log, Tallon Overworld's tunnel: 10.3 FPS, the GPU busy 80.5 ms a frame (99%;
32-45 ms elsewhere at about the same 1,030 draws a frame), the render worker waiting on it 98.5 ms a
frame. The dust is CElementGen's particles (no envfx there): some 220 big blended quads a frame,
overlapping across the screen, all pixels: the GPU runs at its top clock (222 MHz, vita_platform.cpp),
so it is the fragment program's length times the pixels. Compiled on the PC with the Vita's own
compiler (the oracle of tools/vita_precompile), the particles' program (texture times the vertex
colour, fog, CElementGen's alpha test "alpha > 0") is 40 USSE instructions a fragment (43 with
exponential fog):
- 13 of them the alpha test: its comparison and the `discard`, which also gives the program the
  discarding pass type (the depth unit waits on each fragment's verdict; test 16's cost test: 10-29
  alpha-tested draws 8-17 ms).
- 6 the clamps of the texture's and the raster colour's values (GX takes a, b and c as 8 bits;
  those inputs are within [0, 1] already), and 4 the clamp of a stage's sum that cannot leave [0, 1].
- 13 the fog: the window depth's division, then GX's A / (B - z)'s; 5 more for clamps of the colour
  and of the fog factor that are within [0, 1] already.

The changes, each giving the same picture:
- `alpha_test_unseen` (vita_draw.cpp): GX's "alpha > 0" alone, blended SRCALPHA onto INVSRCALPHA or
  ONE, no depth written, no GXSetDstAlpha constant: a fragment it fails (alpha under 0.5/255) adds
  under half a step of 8 bits, nothing. Such a material is worked out without the test (its state,
  program and record), so its program has no discard. CElementGen's particles are such draws, unless
  they write the depth (ZBUF) or blend ONE/ONE.
- Their programs are made lean (`tev_program`'s `lean`): no clamp on an input within [0, 1] (a
  texture's, the raster colour's, a konst, a constant; the registers keep theirs), none on a sum of
  such without d, bias or scale, none on a fog factor (every curve keeps fogF's [0, 1]) or on a colour
  a stage clamped; and the fog's depth fraction from two vertex rows after the rest (`tev_vertex_rows`:
  A*w over (B - 1/2)*w - z/2, orthographic A*(z + w)/2 over w), each linear in the position, so
  interpolated exactly, divided once per fragment. The particles' program: 12 instructions (vertex
  program 10). `kProgramVertexConstants` has the two rows more.
- The material record keeps the choice (its padding byte, now `flags`, bit 0), so the warm-up and
  the PC's precompile tool make the same lean program. Every other program's source is unchanged:
  the old card's 678 records make the same 552 sources byte for byte (checked on the PC), so the card
  keeps them all; only the particles' new programs compile, once.
- tools/vita_precompile: `program_ready`'s stub (the tool no longer linked since test 37), and
  `--self-test lean`.

The lean way would save about 17% of every world program's instructions too (24 programs of the old
card's, compiled both ways on the PC), but every program would compile again: that needs the card's
current `vita_programs/materials.bin` on the PC to precompile them all first.

**The aura.** The Phazon Suit's aura is its mask (the suit drawn with GXSetDstAlpha's 255 into the
EFB's alpha) blurred and drawn through. The port drew the constant only where the colour's blending
reads no source alpha; a blended draw kept its blended alpha. The pause screen's Samus
(`CModelData::MultiLightingDraw`) is drawn blended: a depth pass of alpha 0, then the colour blended
by its alpha, then an additive pulse. GX writes 255 for every one of those fragments; the port wrote
nothing for the depth pass and the texture's alpha squared for the rest: a thin, partial mask, a
faint aura. Now (`DstAlpha::Second`) such a draw is drawn as before, then again into the alpha alone,
unblended, its program returning the constant (`alpha_pass_state`: its depth test, LEQUAL where it
wrote the depth; the constant program does the alpha test). Its program (the material's
constant-alpha one) compiles at its first draw, which is left out meanwhile. The `phazon_shell`
setting (Remastered's shell) draws nothing on the Vita (no scene copy) and stays off without the
room-environment mod (this card has none), so the original aura is the one drawn.

What to look for in test 46:
- The dusty tunnel's frame rate and the presents line's GPU busy against 80.5 ms; the materials line
  counts the materials whose alpha test was left out and the lean programs made.
- The particles' look unchanged (the "Compiling shaders" toast once, for their new programs).
- The pause screen with the Phazon Suit: the aura around the whole suit (the toast once, for the
  alpha passes' programs); the materials line counts the alpha passes.

## 0.46.0 test 47: every generated program the cheaper way, precompiled on the PC

For test 46's closing question (every program lean needs the card's records on the PC to precompile
them), the user sent their card's `vita_programs/materials.bin` (2,485 records, 2026-11-21).
- Every program is now made the way test 46 made the particles' (`tev_program`, `fragment_body`):
  no clamp of a TEV input within [0, 1] already, of a sum of such without d, bias or scale, of the fog
  factor, or of a clamped colour going into the fog; the fog's depth fraction from two vertex rows
  (`tev_vertex_rows`) with one division. The same results. Compiled on the PC, 24 of the old card's
  programs had about 17% fewer fragment instructions; 46 others, all made lean, compiled (none
  failed), 29 instructions a fragment on average.
- A program's key is its source's hash, so every key changes and the card's binaries go unused. The
  user's records make 1,426 programs (847 with fog), all compiled on the PC with the Vita's own
  compiler (tools/vita_precompile) into `precompiled.bin`, which goes in
  `ux0:data/MetroidPrime/vita_programs/` beside them. The warm-up finds each in the pack
  (`program_known`) and its first draw links it from there, read on the card thread, instead of
  compiling. Materials the records lack (rooms not visited yet, the alpha passes' constant-alpha
  programs, which are never recorded) compile on the Vita as before.
- Test 46's record byte (bit 0, lean) is read by nothing now (`_p1` again); its `--self-test lean`
  is `--self-test plain` (an out dir for the first material's sources).

What to look for in test 47:
- At the start, the log's "programs on the card: N; precompiled: 1426"; in play the presents line's
  "from the precompiled pack" count growing and few or none sent to the compiler thread (the
  "Compiling shaders" toast seldom).
- The same picture everywhere (fog, lit surfaces, the lava, indirect effects).
- The GPU busy time in heavy scenes against test 45's (32-45 ms a frame in Tallon Overworld).

The pack: 1,426 programs compiled on the PC in 80 minutes (12 jobs), none failed, 2,726 KiB; its
fragment programs 33 instructions on average (the old card's 552 legacy ones: 45, other materials).

## 0.46.0 test 48: the shaders in ux0:data/MetroidPrime/shader_cache

The user: "Been meaning to ask to move the shaders location to "ux0:data/MetroidPrime/shader_cache/"
where it should be" (upstream keeps its pipeline cache in `<data>/shader_cache`,
port_data_folder.cpp).
- The generated programs' binaries, `build`, `materials.bin` and `precompiled.bin` are read and
  written in `shader_cache/` (`g_programCacheDir`), vitaGL's glCompileShader cache (nothing compiles
  through it) in `shader_cache/vitagl/`.
- `move_shader_folders` (vita_gl.cpp, `initialize`, before anything there is made): at the first
  start `vita_programs/` becomes `shader_cache/` with one rename (VitaSDK's `rename` is
  `sceIoRename`); if `shader_cache/` is there already (made by hand for `precompiled.bin`), each file
  of `vita_programs/` it lacks moves in, and `vita_programs/` goes when that leaves it empty. The old
  `vitagl_shader_cache/` (vitaGL's empty folders) goes when it holds no file. The log's
  "shader folder:" lines say what moved.
- vitaGL's fixed-function cache stays in `ux0:data/shader_cache/` (outside the game's folder, shared
  by every vitaGL program): its path is built into the vitaGL the port links (vgl.c's
  "ux0:data/shader_cache/v%d").
- tools/vita_precompile's notes name the new folder.

What to look for in test 48: the "shader folder:" lines once, then "programs on the card: N;
precompiled: 1426" as in test 47; no compile storm at the start (the warm-up finds the records and the
pack in their new place).

## 0.46.0 test 49: the Echoes suits and arm cannon switched apart

The user, for a player: "Someone asked if it would be possible to separate the Echoes suit models
from the arm cannon models, so if one were to be able to turn off the Echoes suit models but wanna
keep the echoes arm cannon enabled, vice versa", then "Yes, add it". Echoes' suits with Prime's cannon
was there already ("Echoes arm cannon in first person" off); Prime's suits with Echoes' cannon was
not: the mod's own switch takes both, and only the Power Suit's looks offered Prime's own.
- "Echoes Suit models" (`echoes_suit_models`, on by default; console `echoessuits 0|1`), under the
  mod's switch, now "Use the Echoes models": off, every suit wants Prime's own look
  (`PortEchoesSuits::WantedSuitLook`), so the loader serves none of the looks' bodies, dolls,
  first-person arms or balls over Prime's (port_mods.cpp); the cannon (`sEchoesGuns`) is served apart
  and keeps its switch. From the next mod reload, as the looks; the "Reload the mods to apply" note
  compares the wanted looks with the served ones. The suits' look choices are kept (greyed while it
  is off) and come back with it. "Play as Dark Samus" stays a switch of its own.
- MODS.md, SETTINGS.md and the settings search index name it.

What to look for in test 49: F1 > Mods > Built-in mods > Metroid Prime 2: Echoes, "Echoes Suit
models" off and the mods reloaded: Prime's suits everywhere (cutscenes, the pause screen, the
first-person arm, the balls) with Echoes' cannon in first person; on again and reloaded, the looks
as chosen. The log says "the suit models are off" at the load.

## 0.46.0 test 50: the Federation Marine mod (federation-marine 1)

The user asked for Echoes' Federation Marine in place of Samus, from their own Echoes disc, as a mod
of its own:
- the marine for the Power, Varia and Gravity Suits (cutscene and third-person bodies, the pause
  screen's Samus, the first-person arms);
- the Phazon Suit as Echoes' Dark Trooper, its broken visor replaced by the marine's whole one,
  opaque;
- the Gravity marine with the parts they circled on a picture (both arms, the thighs and knees, the
  lower legs and boots) in the Gravity Suit's purple and the visor in its visor's colour;
- the Morph Ball in each look's armour colour;
- the marine's rifle in place of the first-person arm cannon.

After the survey they chose the defaults offered: the Spider Balls recoloured too, every visor opaque,
and the marine winning over the Echoes suits when both mods are on.

What the Echoes disc has (all in Metroid1.pak, Temple Grounds; survey tools in `~/marine`):
- `Galactic_Trooper` (the marine; ANCS 46B2D36D, only dead poses): CMDL A93225BD, CSKR C21D8A6B, CINF
  0A32C29A. Four materials:
  - the body (atlas 35E1A1D2, glow 947A82B4, reflection mask 5645A0A9 × sphere map E4111B76);
  - the rifle (8DC49C2A, all on the right wrist);
  - the visor's glass (64% opaque, depth-sorted);
  - the visor's glow (the same 12 triangles, added: the teal you see, 43,139,139).
- `Dark_Trooper` (ANCS A2CC5B52): CMDL F7507082, CSKR 9C7FDF54, CINF B69EF3E2. The same mesh plus the
  Ing growths; its broken visor is 22 triangles (crack F79C1D91) in exactly the marine visor's box.
- Also there, not used: `TrooperWface` (a face behind a see-through visor) and `missle_Trooper`.
- The troopers' skeleton (39 joints) is a copy of Echoes Samus's: the same names (her Varia
  shoulder-pad joints, which carry the marine's pads, and her left-hand fingers) and every head
  within 3.2 mm of hers. So `ConvertCskr` fits them onto Prime's cutscene, in-game and doll skeletons
  unchanged; R_wrist (hand and rifle) folds onto R_elbow, where Samus's cannon rides.

The import (`platform/port_federation_marine.cpp`, `PortFederationMarine::Import`) writes 66 files,
each look once under `Wear(look, Prime's Varia ids)` ("FMM", "FMG", "FMD" variants):
- Bodies of kinds 0-2 from the converted troopers, the visor's glass made opaque (not depth-sorted,
  blend ONE/ZERO; the glow stays added). The Dark Trooper's glass and glow surfaces are swapped for the
  marine's (`SwapVisor`: MergeCmdl, the skins' groups end to end, `CompactSkinned`, `PruneMaterials`).
  The doll's sorted glow writes no depth (`SortedWithoutDepth`).
- The Gravity marine's two textures (atlas and glow):
  - its texels voted limb by the body's triangles (`IsLimbJoint`: shoulder pads, arms, hands, hips,
    knees, ankles, toes);
  - except the hip guards: a hip triangle's UV piece reaching above the hip joint stays the waist's
    (144 triangles; the user's circles start below them);
  - limb plates (saturation < 0.3) in Prime's Gravity purple (246.4°, 0.356, the value scaled 0.702
    from the plates' median 0.686 to 0.482);
  - the visor's glow (its texels marked by the glow material's triangles) in the Gravity glass's aqua
    (169.5°, 0.83, brighter by 0.89/0.545); the other teal lights aqua.
- The first-person rifle (`BuildRifle`):
  - the converted body's right-arm triangles out from |x| 0.33 (the upper arm's last 20 cm, the
    forearm, the hand and the rifle);
  - turned down the barrel (`RifleFrame`: +x to +y, up stays up) with the muzzle locator
    (GUN_Particle_LCTR) at the origin;
  - the upper arm bent by its shoulder weight toward the gun skeleton's shoulder (about 30°);
  - unskinned, with its bounds.
  - The muzzle is 0.54 m from the elbow; Prime's LBEAM is 0.583.
- The first-person left arm (`BuildGrappleArm`):
  - the left arm's triangles the same way, turned into the grapple arm's frame (`GrappleFrame`: -x to
    +y, the thumb forward to up), elbows together;
  - each vertex moved by its joints' head offsets, so the wrist and fingers pivot where Prime's do;
    the upper arm bent toward its shoulder;
  - skinned by name, case aside (L_Index_1 on L_index_1; joints above the shoulder on the shoulder).
- The balls (`RecolourBall`):
  - Prime's Morph Ball (Power set: yellow shell C01FFF01) and Spider Ball (Varia set: orange 2EE6F56F)
    with their low-poly models;
  - the shell (13-70°) at the look's armour colour, its median there and its shading kept: the
    marine's white (195°, 0.05, its plates' value), the Gravity purple (its hue spread a third), the
    Dark Trooper's grey (measured: 43°, 0.09, 0.35);
  - the lights (red, the low spider's green) teal (180°), aqua (169.5°), or left red for the Dark
    Trooper;
  - the ANCS's character 0 pointed at the recoloured model, its skin kept;
  - the Spider Ball's glass is Prime's AE78BEBE with one set: teal (0) for the marine, aqua (2) for the
    Gravity marine. The Varia's own set is green.
  - The Dark Trooper keeps Prime's Phazon Spider Ball and its glass, dark already.

The game:
- port_mods.cpp serves each suit's look (`kSuitLooks`: marine, Gravity marine, marine, Dark Trooper)
  after the Echoes looks and before Dark Samus:
  - its bodies and left arm over the suit's own ids, with the cutscene copies 8C72FDAC and B333E1D7;
  - its balls under the suit variant ids, and the glass under `Id(AE78BEBE, suit)`;
  - its rifle under `PortDreadSamus::FirstPersonCannon(suit)`;
  - an Echoes helmet body there leaves `sEchoesHelmetBodies`, and an Echoes ball where the marine has
    none (the Phazon Suit's spider ball) is taken back to Prime's.
- `sMarineBodies` makes `Replaces` and `HasOwnCannon` hold, so Prime's boots, visor and cannon are not
  drawn on them.
- The marine's spider balls are not "a mod's spider ball" (`sModSpiderBalls`): Prime's glass shell
  stays.
- `ReplacesFirstPersonCannon` holds for the rifle while "Marine rifle in first person"
  (`marine_first_person_rifle`, console `marinerifle`) is on. CGunWeapon draws it the Dread cannon's
  way, on the live LBEAM, in place of the gun and the suit arm, with `mPortCannonRoll` 0 (Dread's 30).
- F1 > Mods > Built-in mods > "Federation Marine (Echoes)": the mod's switch, the rifle's, and the
  import. It reads the same disc as the Echoes suits (`ux0:data/MetroidPrime2`), with one image field
  for both. `--import-federation-marine <image>` from a terminal. Its stamp is
  `.federation-marine-version` 1.
- The Echoes import's shared helpers (`ParseSet`, `KeepTriangles`, `CompactSkinned`, `SkinGroups`,
  `MainJoints`, `ForEachUvTriangle`/`ForEachMaterialUvTriangle`, `TexelVotes`, `VoteTriangle`,
  `LimbTexels`, `RgbToHsv`/`HsvToRgb`) are in port_echoes_suits.h now; `EchoesJoint` keeps its bind
  position. The echoes-suits import's output is unchanged.
- Checked on the PC: `~/marine/import_repo` (the real import against both Desktop discs) and
  `~/marine/render2.py` (bind-pose renders with the glow and blend passes, and first-person views from
  the PlayerGun tweak's gun position 0.25, 0.30, -0.35 from the eye).

What to look for in test 50:
- Import: put the Echoes ISO in `ux0:data/MetroidPrime2` (it is there already for the Echoes suits),
  then F1 > Mods > Built-in mods > Federation Marine (Echoes) > Import. Then switch "Use the Federation
  Marine models" on, if it is not, and reload the mods.
- In third person, cutscenes and the pause screen:
  - Power and Varia are the white marine with the teal visor, holding its rifle where the cannon was.
  - Gravity has purple pads, arms, thighs, shins and boots, an aqua visor and lights, and white hip
    guards, torso and helmet.
  - Phazon is the Dark Trooper with the marine's teal visor.
- In first person:
  - the marine's forearm, hand and rifle at the lower right: white, or with a purple forearm for
    Gravity, or dark for Phazon;
  - shots from its muzzle;
  - the left hand when grappling.
- The Morph Ball is white with teal lights (Gravity purple with aqua, Phazon dark grey with red). The
  Spider Ball is the same, inside its teal or aqua glass; Phazon's is Prime's own.
- The log: "mods: federation-marine: suit N wears the ..." for the four suits, and "serves 13 Samus
  body model(s) and 4 first-person rifle(s)".

### Test 50 result

The user imported it and sent photos:
- the pause screen's Varia with the Echoes look and with the marine;
- first person with the marine's rifle and with the Echoes cannon;
- the Mods page.

Their words: "the Federation Marine's arm cannon is held sideways, I think a simple fix rotating that
and the hand, not the forearm. I think the arm canon is a little higher in first person and I want it
this mod to be a toggle under 'Play as Dark Samus' as 'Play as a Federation Marine', not a separate
catagory."
- Sideways: in the pause screen's idle the rifle's grip points in toward the body, not down.
  - The bind pose has the rifle upright (top +z, profile facing front), but Samus's animations roll
    her forearm for her cannon.
  - Posed with her own animations (`~/marine/roll.py`), the grip hangs down after turning the hand and
    rifle about the forearm by: -51° in the pause screen's idle (B_ready_itemscreen_samus), -45°
    standing ready, -50° walking, -49° landing, -82° running.
  - The first-person model is placed by the port, upright already (`~/marine/render2.py` views
    gtop/gside/gfront).
- Higher: the rifle stands 0.17 above its barrel, Prime's cannon 0.095. With the muzzles together, the
  rifle's top rose 7.4 cm over the cannon's.

## 0.46.0 test 51: the Federation Marine as "Play as a Federation Marine" (echoes-suits 19)

- The Federation Marine is part of the echoes-suits mod now, as the user asked:
  - "Play as a Federation Marine" (`echoes_federation_marine`, off by default; console `marine 0|1`)
    sits under "Play as Dark Samus" in F1 > Mods > Metroid Prime 2: Echoes, with "Marine rifle in
    first person" under it.
  - The two "play as" switches exclude each other (each setter turns the other off; if both are on,
    Dark Samus wins). The section's "Reload the mods to apply" note covers it
    (`PortFederationMarine::MarineServed`).
- The Echoes import (port_echoes_suits_import.cpp RunEchoes) runs `PortFederationMarine::Import`
  after the suits into the same staging folder: 300 + 66 files, no name shared.
  - echoes-suits `kVersion` 19, so the user imports again.
  - Moving the import into place removes test 50's separate `federation-marine` folder (told by its
    `.federation-marine-version`). Its files' ids are the same, and the later folder would shadow the
    new ones.
  - Test 50's separate section, import button, start hooks and `--import-federation-marine` are gone
    (those files are back to test 49's).
- The loader (port_mods.cpp):
  - serves the marine from the echoes-suits mod's files while the setting is on and Dark Samus is off;
  - records what it served: `sMarineBodies` (Replaces, HasOwnCannon), `sMarineRifles`
    (ReplacesFirstPersonCannon), `sMarineSpiderSuits` (whose spider ball keeps Prime's glass);
  - logs "echoes-suits serves the Federation Marine: 13 Samus body model(s), 4 first-person rifle(s)".
- The grip (`RollGrip`, kGripRollDegrees -50): the bodies of kinds 0-2 have the right wrist's
  vertices turned -50° about the bind pose's elbow-to-wrist axis, through the wrist, by their weight
  on it. The forearm stays. Done before the Dark Trooper's visor swap renumbers its vertices; the
  first-person rifle and left arm are built from the unturned bodies.
  - Checked by posing the doll in the pause screen's idle (`~/marine/posebody.py`, Prime's skinning
    and pose builder on F76E70A6). Before, from the right, the rifle showed its flat top; now it shows
    its profile with the grip down.
- First person: `PortDreadSamus::FirstPersonCannonDrop` (`kRifleDrop` 0.075 gun units) for the
  marine's rifle. CGunWeapon keeps it as `mPortCannonDrop` and `PortDreadMuzzleShift` adds it as
  -z, so CPlayerGun's shot origin (x418_beamLocalXf) moves with the drawn rifle and the shots still
  leave its muzzle.

What to look for in test 51:
- Import the Echoes suits again (Mods > Metroid Prime 2: Echoes > Import): one run makes both, and
  the old federation-marine folder goes. "Other mods" should show echoes-suits, switched on.
- Switch "Play as a Federation Marine" on and reload the mods. Dark Samus switches off with it.
- The pause screen and third person: the rifle hangs grip down, the hand around it, the forearm as
  before.
- First person: the rifle about 7.5 cm lower than in test 50, with shots still from its muzzle.

### Test 51 result

The user asked whether the shots line up with the rifle's barrel: "it looks like it's being shot
from way below".
- They do. x418_beamLocalXf (shots, muzzle flashes, charge effects) is the rifle's muzzle, the
  hexagonal mouth of the barrel under its top housing.
- Seen from the first-person eye, above and behind, the housing hides that mouth. The rifle's visible
  front is the housing's top front edge (rifle space y -0.10, z +0.17), 80 of 544 lines above the
  mouth (`~/marine/ridge.py`, render2's MARK crosses).
- The user: "if it does then there's no need to change". Then, with an in-game photo of the left hand
  resting on the rifle (the hand on the cannon when still) and a render of the rifle's left side: "I
  think moving the arm cannon back up would be a good idea, probably should make sure the hand doesn't
  appear because it clips through the arm cannon and it already has a grip so we'll just pretend the
  hand is holding that instead." Then: "I meant the left hand".
  - Commit a2510b5 had taken it for the right hand (left out of the troopers' meshes, echoes-suits
    20). It was never built or staged; test 52 puts the right hand back.

## 0.46.0 test 52: the rifle back up, the left hand off it

- First person: the drop is gone (`kRifleDrop`, `FirstPersonCannonDrop` and `mPortCannonDrop`
  removed). The rifle's muzzle sits on the cannon's again, as in test 50, the shots with it.
- The left hand (`CPlayerGun::DrawArm`): with the Federation Marine's rifle as the first-person
  cannon (`CGunWeapon::PortDrawsMarineRifle`: mPortDreadCannon and the new mPortMarineRifle, from
  `PortFederationMarine::ServesFirstPersonRifle` in LoadSuitArm), the grapple arm is not drawn while it
  plays the gun controller's animations (`CGrappleArm::kAS_GunControllerAnimation`).
  - Those are the poses that put Samus's left hand on her cannon: EnterFreeLook (the GameCube's R
    free look, and the port's hand on the cannon when still, #244), EnterFidget, EnterComboFire and
    EnterStruck.
  - The grapple beam's states draw the arm as before.
  - The rifle's own grip stands for that hand, as the user said.
- The import is test 51's again: the harness's output (`~/marine/out6`) is byte for byte test 51's,
  so echoes-suits stays at version 19 and the user's import stands.

What to look for in test 52:
- No new import: the Echoes import from test 51 stands.
- First person: the rifle back at test 50's height. The shots still come from under its housing's
  front.
- Stand still for a second (the hand on the cannon when still): the rifle settles, and no left hand
  comes over it. The same in free look and while firing combos.
- The grapple beam still shows the marine's left arm.

## 0.46.0 test 53: the Phazon Suit's aura in third person, every copy made

The user, with two photos (the Phazon Suit and the Dark Trooper in third person, the aura's outline
off to one side of the body) and test 52's log: "we still need to fix the phazon suit aura.
Sometimes it has problems following you in third-person".

The aura is drawn from a mask: the body drawn with GXSetDstAlpha's 255, the EFB's alpha blurred by
`CCubeRenderer::DoPhazonSuitIndirectAlphaBlur` and copied into the mask texture
(`DrawPhazonSuitIndirectEffect`). The chain takes six EFB copies a frame: the scene (RGB565, for the
warp), then five alpha copies (the halved alpha, the quarter, the PC's HDR round trip, the halo, the
mask). The Vita path made seven copies a frame at most and dropped the rest (`record_copy`,
`kCopiesPerFrame`). In third person with the sun's shadows on, a frame wanted nine: test 52's log has
windows of 2,700 copies in 300 frames, 1,500 of them alpha (five a frame: the chain ran every frame),
600 "past a frame's 7" (two a frame). The two dropped were the chain's last, the halo's and the
mask's: the aura was drawn with the mask an earlier frame had left, where Samus had been. Windows of
seven a frame (2,100) dropped none, so it came and went.
- `kCopiesPerFrame` 12. Each copy after a draw ends the display's scene (copies in a row share one
  end), and the display's render target keeps 8 scenes a frame (GXM's most, vitaGL's
  `MAX_SCENES_PER_FRAME`): a frame past that waits at its next scene for the GPU to finish one,
  rather than drawing a wrong picture. The bound stays for frames that would make many more
  (indirect particles copy each one).
- The PC's HDR round trip in the blur (an A8 copy of the quarter drawn back over the alpha, which
  clamps a float EFB's sum) is left out on the Vita (`!defined(__vita__)`): its 8-bit EFB saturates
  each blend already, as the GC's does, so the copy and its draw changed nothing. The chain is five
  copies there, four scene ends.

What to look for in test 53: third person in the Phazon Suit, moving and turning where the sun's
shadows fall: the aura round the body every frame. The presents report's copies line: "past a
frame's 12" at 0; the frame rate there against test 52's (about 33-38 FPS in those windows).

## 0.46.0 test 54: the credits zoomed; the menus' zoom options off the Vita's menu

The user, with a photo of the credits (a small 4:3 box with black all round): "Remember when we
zoomed in the main menu & save select menu? Gonna need to do that for the credits too", and of F1 >
Video > Display with "Stretch menus to the screen" and "Zoom menus to fill the screen" circled: "you
don't need to show the two options I have circled".
- The credits (`CCredits::Draw`) zoom as the front end's menus do (test 22): their background movie
  (creditBG.thp) is 4:3 with a 16:9 picture band, and the text and the port's bars are laid out for
  it (`PortCreditsWidth`, `PortDrawCreditsBars`), so GX's viewport scaled about the screen's centre
  (about 1.32 on the Vita) fills the screen with the band and cuts only the black above and below
  it. `PortMenuZoom` and `PortMenuZoomFactor` moved from CFrontEndUI.cpp to
  include/MetroidPrime/PortMenuZoom.hpp, shared by both.
- F1 > Video > Display leaves out the two checkboxes on the Vita (and the settings search its
  "Stretch menus" entry): `stretch_menus` and `zoom_menus` keep their saved values (by default off and
  on there), as test 43 left the resolution scale's.

Test 53's log, the ending (MLVL 13D79165, the user: "FPS drops in at the ending cutscene ... It's
either the fire effects ... or something else"): about 17 s at 13-15 FPS. Every core full at once:
- The game thread 85% busy, about 70 ms a frame: 4.2 ticks a frame (the simulation keeps its 60 Hz)
  of 8 ms, 35 ms, with 11 ms of them waiting for the side worker's particle steps; the draw 27 ms, 18
  of them `CElementGen::Render`. Some 5,600 particles updated a frame in 300 systems, 211 made.
- About 650 of the frame's 1,218 draws are particle models (`CElementGen::RenderModels`, a display
  list a particle): each one a whole draw on the game thread, the FIFO processor and the GPU.
- `CEnvFxManager` (the falling ash) draws 64 immediate batches of some 21,000 vertices a frame,
  decoded by the core 0 helper; the FIFO processor (78% busy) ran 30% of its time in the helper's jobs
  itself, waiting for it.
- Core 3: the side worker's particle jobs 31% and the MusyX mixer some 55% (13 voices a frame: 386 ms
  a second rendered, 167 under its lock).
- The GPU 51 ms a frame. The render worker waits on it (39% busy).
None of this is changed in test 54: a round of its own (see the reply).

What to look for in test 54: the credits fill the screen (only the black above and below their band
cut); the title and file select menus as before; F1 > Video > Display without the two checkboxes.

## 0.46.0 test 55: the ending, round 1 (the game thread's waits, the helper's vertices)

The user, to "Start an optimization round for the ending cutscene?": "Yes, start it". Test 53's log
again, closer, for the ending's 13-15 FPS (MLVL 13D79165):
- The game thread's waits in the tick: 13.8 ms a frame in "SM: effects' Think finished", 77 of the
  frame's 214 effects' steps waited for (12.25 ms). Each one it waited for was already running on a
  side worker: it waits for them in the order they began, the workers take them in that order, so it
  was always behind them. It ran 9 steps a frame itself (`wait_game` ran only those queued before the
  one it waited for), while the side worker on core 3 was 29% busy and side worker 2 20%.
- The core 0 helper's work: its jobs' time and the FIFO processor's runs of them, report by report
  across the log, fit 0.7 us an immediate vertex and 1.4 us a skinned call's vertex (calm scenes too:
  4,200 skinned calls, 1.06 million vertices, 1.4 s). In the ending, 25,400 immediate vertices a frame
  (the ash 21,600, the particles 5,200) and 17,000 skinned ones (21 calls of a large skinned model, 812
  vertices each): about 45 ms of work a frame, the helper 28 ms of it and the FIFO processor 20 (run
  at the frame's end, waiting for the helper). Three times the loops' instructions: the frame's
  vertices (56 bytes each, several MiB a frame) are out of the caches when written, each line a store
  misses waits for its fill from memory, and the GPU (77% busy) shares that memory.

Changes:
- `side_jobs::wait_game`: while its job runs on a worker, or waits behind one of the worker's own kind
  (a skinned model's vertices), the game thread runs the next untaken step of either side worker,
  those queued after its job too (`steal_next`, `can_steal`). The steps are each object's own and in
  no order (two workers run them side by side already), and each is needed within the tick these
  waits end. It wakes for the other worker's steps too.
- The helper's loops ask for the lines a few vertices on ahead (`prefetch_vertex`, PLD: a hint, which
  never faults): `decode_direct` (immediate draws), `write_skinned` and the positions and normals
  `pose_skinned` reads. `write_skinned` copies each vertex from the template and poses it in one pass
  (the copy of them all first, 45 KiB a call in the ending, had the posing read every line back). The
  snow's and the ash's layout (an s16 position, s8 coordinates) and the particles' (a float position,
  an RGBA8 colour, float coordinates) decode in loops of their own (`decode_direct_known`), as the
  general loop's steps do, without its switch's mispredictions.

Not changed: the particle models' draws (707 of the frame's 1,218 draws are their surfaces), the GPU's
51 ms a frame, the MusyX mix. The simulation keeps its 60 Hz (`sim_adaptive` is the
user's setting, off).

What to look for in test 55: the ending's frame rate against test 53's 13-15 FPS. In the log there:
the `[particles]` line's effects' "waited for (x ms)" (12.25 ms before) and the side jobs' "the game
thread ran N of them itself"; the core 0 helper's side-jobs line, its busy share and the FIFO
processor's runs of its jobs (6,070 ms in 19.8 s before); the `[split]` line's tick (35.9 ms a frame).
The ash, the particles and the skinned models look as before.

## 0.46.0 test 56: the ending, round 2 (the frame's vertices drawn where they are written)

Test 55's log (the user's play through the ending on a Rev 2 disc, USA 1.02): the ending at 14-15 FPS
still (log lines 9250-10880).
- The game thread's waits worked as meant: the effects' steps waited for 6.6 ms a frame (12.25 in
  test 53), the game thread running 11,500 steps in 16.6 s itself (2,700 in test 53's 19.8). The work
  moved onto it, and its tick came to 33 ms a frame (36).
- Test 55's prefetches changed nothing: across the log's reports the helper's work fits 0.74 us an
  immediate vertex and 1.31 us a skinned one (0.70 and 1.51 in test 53).
- The render worker held the frame: busy 80 to 100% (the `[split]` lines; 40% of the CPU in the
  `[threads]` line, the rest preempted on core 0 or waiting), the game thread waiting 3-12 ms a frame
  for it to begin the next. Its `[draw path]` lines, every timed present across the log, fit 2.1 us a
  draw and 16.6 us a KiB of the frame's own vertices: vitaGL copies those (client memory) into its
  per-frame pool at each draw, and that pool is uncached memory. The ending's frames have 2.5 MB of
  them (the ash 1.2, the skinned models 1.0, the particles), 57-61 ms of the render worker's draws a
  frame; the ash after the ending 1.85 MB in 164 draws, 13-30 ms, with the GPU at 62%.

Changes:
- `VertexArena` (vita_gl.cpp): 36 chunks of 8,192 vertices (15.75 MiB) from vitaGL's own pools,
  uncached and mapped for the GPU (physically contiguous first, then its RAM). A frame's vertices
  (`FrameVertices`, which replaces `FrameChunks<Vertex>`) take their chunks there, or from the heap's
  pool as before when none is free. The render worker draws an arena mesh through one buffer object
  pointed at its first vertex as the draw is sent (`vglBufferData`: vitaGL reads the buffer's address
  at each draw, in the program path and the fixed-function one; GXM's attribute offsets are 16-bit, so
  a mesh's place in its chunk cannot be one): nothing copied. The indices are still copied (small).
- A chunk its frame lets go of is free again once the GPU is past the present being built then: the
  fence of that present's last scene (vitaGL ends every scene with `query_fence`), noted at each swap
  (`VertexArena::swapped`).
- vitaGL's per-frame pool from 32 MiB to 16 (`vglSetCircularPoolSize`): the arena takes what it gives
  back. It still holds the indices, the legacy pool, the fixed-function arrays and uploads' staging.
- The frame's vertices are written once each, whole (made in a local, stored in four 16- and 8-byte
  blocks): in uncached memory each store goes out on its own. The decode loops (`decode_direct`,
  `decode_vertices`), `write_skinned` and the masked clears. Test 55's prefetches of the lines being
  written are gone; those of what the loops read (a skinned template's vertices, the posed arrays)
  stay.
- The writes completed before the GPU can read them (DSB): at the end of each core 0 helper job and
  in `fifo::process_to` before the FIFO's progress is shared (the FIFO thread, or the game thread at a
  drain); the threads' own barriers order them for the other threads alone.
- The log: a `frame vertices:` line after each presents report (draws of the frame's own meshes drawn
  in place, copied, the arena's chunks taken and the times none was free), and the `[draw uploads]`
  lines count only what vitaGL copied.

What to look for in test 56: the ending's frame rate (14-15 FPS before) and the ash scene after it
(21-23 FPS before); in the log the `[draw path]` lines' program "draw" share there (57-61 and 13-39 ms
before), the `frame vertices:` line (in place against copied, "times none free" near 0), the core 0
helper's work and the FIFO processor's runs of its jobs (now writing to uncached memory). The ash,
the snow, Samus's model, particles, the masked clears (the Phazon Suit's) and GX's lines (the map's
outlines, the rain) look as before: a flicker, a stray triangle or a spike from one of them would be
a chunk taken again too soon.

## 0.46.0 test 57: the ending, round 3 (the core 0 helper's jobs on the fourth core too)

Test 56's log (the user: "I guess we start round 3, right?"): the arena held, every frame's own mesh
drawn in place ("0 copied", "0 times none free" in every report), no error. The ending at 16-17 FPS
(log lines 7355-8990; 14-15 in test 55), the scenes after it 37-38 FPS (28-31) and 26-27 (21-23).
- The render worker's program draws in the ending 11-15 ms a present (57-61), 27% busy.
- The game thread 85% busy: ticks 26 ms a frame (3.6), 6 of them particle steps it ran itself while
  it waited; the draw 26 ms (`CElementGen::Render` 16.6). It waited 5-7 ms a frame at `begin_frame`
  (`fifo::finish_frame_end`) for the FIFO processor to end the frame before.
- The FIFO processor 85% busy: some 37 ms a frame for 1,400 draws, and 15-20 ms at each frame's end
  (its `[fifo path]` "vita" share) for the core 0 helper's jobs: it ran 15,287 of them itself (4.5 s
  in 15.9). The helper's jobs were some 35 ms a frame (the ash's 21,700 vertices, the skinned model's
  14,000); the core 0 helper, below side worker 2 there, ran 20.
- The GPU 45.8 ms a frame (87%). The fourth core 42% idle (side worker 24%, mixer 32%).
- The ending's particle models (Metroid8.pak, 51 PARTs, 10 with models) are small: 21 to 133
  positions, 1 to 3 surfaces (scratchpad partmodels.py). 672 of the frame's 1,280 list draws are theirs.

Change: the core 0 helper's jobs are taken by a second thread too, the core 3 helper (priority 135,
below the side worker, the mixer, the background lane and the program cache; above the compiler and
the texture workers). It takes a job only while two or more wait unclaimed, and sleeps on a condition
of its own (`Worker::extraThread`), so a wake-up for the core 0 helper never goes to it; below that
the jobs are left to the core 0 helper, which nothing on its core's own preempts the way the fourth
core's side worker and mixer preempt this one (the FIFO processor would wait at its frame's end for a
job a preempted thread holds). The skinned fills' posed pairs, two before (the helper's and the FIFO
processor's), are four, each fill taking a free one. The `[side jobs] core 0 helper` line gives the
core 3 helper's part.

What to look for in test 57: the ending's frame rate (16-17 FPS in test 56); in the log, the core 0
helper's line, the core 3 helper's jobs and the FIFO processor's runs of them (15,287 in 15.9 s
before), the `[split]` lines' begin-wait (5-7 ms) and the `[fifo path]` "vita" share (15-20 ms). The
game thread's own work (some 51 ms a frame) and the GPU's 46 then hold it: about 19 FPS at most.
Sound and the particle steps as before (the core 3 helper is below both).

Test 57's log: the ending at 14-17 FPS, mostly 15 (16-17 in test 56), the scenes after it 31-35 and
22-27 FPS (37-38 and 26-27). No error. The core 3 helper took 3,004 of the 25,379 jobs in 18.6 s, its
jobs 270 ms a second of its time but 6% of a core (`[threads]`): the side worker, the mixer and the
others above it on the fourth core preempted it with its jobs in hand, 4.5 times their length. The
FIFO processor waited for 238 of the helper's jobs, 1,023 ms in all and up to 20 ms (151, 40 ms and
2.6 ms in test 56), and its frame's end for the helper 22.7 ms a frame (15.1). Taken out again: the
helper's two files are test 56's (vita_side_jobs.cpp, gx/vita_draw.cpp), and the lesson kept: a thread
the FIFO processor may wait on at a frame's end must not run below busy work on its core.

## 0.46.0 test 58: the ending, round 4 (smaller vertices for the skinned calls and the immediate draws)

The user: "Yes, start round 4". The core 0 helper's jobs, which the FIFO processor ran at its frame's
end for 15-20 ms a frame in test 56's ending, are the frame's own vertices: a skinned call's posing at
1.31 us a vertex (the template's 56-byte vertex read, posed, written whole), an immediate draw's decode
at 0.74 (56 bytes written). A vertex is now written smaller (vita_gl.hpp `VertexLayout`):
- A skinned call drawn by a program (by index, from its template's unique vertices) writes only its
  posed part into the frame (`PosedPart`: position and normal, 16 bytes), no template vertex read. Its
  template keeps its unique vertices on the GPU (`SkinnedTemplate::unpaired`, a Mesh made once at the
  template's making, uploaded at its first draw), and the draw has it as its Mesh: the render worker
  points the raw colour and the sets there and the position and the normal at the posed parts
  (vitaGL then reads each attribute as a stream of its own, none copied). A part the arrays do not
  give (a two-component position's z, a normal outside its array) keeps the template's, as before.
- An immediate draw whose vertices all sit in the list with a position, at most a colour and one
  coordinate pair, no normal, and whose program and alpha pass read nothing else (`TevProgram`'s
  `normalInput`, `uvInputs`, from its generation: no source or key changed) writes compact vertices
  (`CompactVertex`: position, raw colour, the pair; 24 bytes), on the core 0 helper as before
  (`compact_on_helper`) or the FIFO processor (`decode_compact`). The snow's and the ash's, the
  particles' quads.
- Both are drawn fixed-function too while their program is on its way (a compact one's raw colour as
  its colour). Neither joins the draw before it (`merge_into_last`: whole vertices alone).
- The log: the materials line's "posed in parts" and "immediate draws in compact vertices".

What to look for in test 58 (superseded by test 59, below): the ending's frame rate (16-17 FPS in test 56); in the log, the core 0
helper's line and the FIFO processor's runs of its jobs (15,287 in 15.9 s in test 56), its frame's end
waits, the `[split]` lines' begin-wait. Samus's model and the other skinned models (lighting, textures,
the Phazon Suit's aura), the ash, Phendrana's snow, the particles and the rain look as before; a wrong
one would be a stretched, black or speckled skinned model or particle.

## 0.46.0 test 59: the Metroid Prime fight's 4 FPS (a skinned list's templates, one for each state)

The user, with test 58's log: "The performance is so bad fighting Metroid Prime (the ending boss
fight)". The log (83,805 lines, the build 4573fd6, no error) has the fight at 18-40 FPS, and then some
90 s at 4.0-4.2 FPS (lines 73584-83801). There the FIFO processor spent 240 ms a frame in 63 skinned
list calls (`vita: skinned list call` 285 ms a frame, 4.5 ms a call), the game thread waited 152 ms a
frame for it (`begin_frame`), and the simulation fell to 40 ticks a second. Their reports: 3,036,
4,416 and 1,357 templates made for 7,726, 6,048 and 5,082 calls (0 to 109 in every other report): the
boss's lists were called in two states in turn (its body drawn twice a frame), and a list kept one
template (`sSkinned`, a template a list), so each call in the other state made it again (the list
decoded whole, its distinct pairs and unique vertices worked out, and since test 58 their copy for the
GPU made and uploaded).
- A skinned list keeps a template for each state it is called in, up to four (`SkinnedList`, as
  `CachedList` keeps a resident list's triangles), the one made again past four in turn; each stays
  where it was made (the core 0 helper's jobs read it there), and a list let go of waits for every one.
- A template's unique vertices go to the GPU at its second call (`SkinnedTemplate::calls`), not at its
  making: one made for a single call is drawn whole, as before test 58, with no copy and no upload.

The rest of the fight (18-30 FPS in most of it): the game thread's own work, some 48 ms a frame in the
log's steady 20 FPS stretch (3 ticks of 8.6 ms, the draw 16.5); not changed here.

What to look for in test 59: the fight's frame rate where test 58 had 4 FPS (the same as the fight's
other parts, 18-30), and the materials line's "templates made" in its reports (a few, not thousands).

## 0.46.0 test 60: the ending, round 5 (the colour maths in half precision)

The user ran the GPU cost test in the ending three times (the cutscene is shorter than the test's
21 s; "third time's a charm, right?"). The third run (4a86360) had its last eight phases in the heavy
part:
- Every draw into one pixel took the GPU to 11.6 ms a frame where it had 50-80, and the frame stayed
  at 54-55 ms: the CPU (the game thread 85% busy, the FIFO processor 81%) holds the heavy part at about
  18.5 FPS, and the GPU makes its dips (72-80 ms a frame for some 4 s, 12-14 FPS).
- At the dips, no display lists (the room and some 670 particle models a frame) left the GPU at 80 ms,
  and no skinned draws saved about 7; just before them, no blended draws took 30-40 ms to 10. So the
  dips are the blended particles' pixels: the effects' sprites and the ash, immediate draws.
- The particles' program was lean already (test 46: 12 instructions). On the Vita's compiler (PC
  oracle) it is 9 in half precision, 5 for 7 without fog, with its colour varying and its result half
  too (half maths with a float varying gained nothing with fog; half constants nothing more).

So the generated fragment programs do their colour maths in half (`with_precision`, `half_precision`):
- In half: the TEV's values (textures, the raster colour, konst, the registers, each stage's sum), the
  vertex colour and the lights' sums as they come in (`vRaw`, `vLit`), and the result. Half is finer
  than GX's TEV over the TEV's clamped range, -4 to 4 (a step of 1/512 at worst, GX's 1/255).
- Float as before: the texture coordinates, the indirect stages, the fog's factor (cast for its mix)
  and the alpha test's 8-bit value; and whole programs with a TEV compare (GX's packed 16- and 24-bit
  compares need more than half's 11 bits) or indirect ones with fog of five stages or more (the
  compiler made those longer in half: 18 programs, 66 instructions to 69 on average). Their sources
  are byte for byte as before (`with_precision`'s tokens left out), and so are their keys.
- The vertex programs are unchanged. vitaGL links a half program as any other (its own GLSL
  translator makes fragment programs half), into the same UCHAR4 output register.

Compiled on the PC with the Vita's own compiler for the user's records (2026-11-21's 2,485 and the
card's 1,494 of 2026-11-23): 1,559 programs, 1,534 of them half, none failed. The 1,493 with a test 47
program to compare have 8.4% fewer fragment instructions (33.2 to 30.4 on average; one stage 7.6%
fewer, lit 8-10%, alpha-tested 3%, indirect 7%).

The pack: every key but 25 changed, so `precompiled.bin` is essential (without it the Vita compiles
each program at about 3.8 s). The new one holds test 47's programs and test 60's (2,960, 5,629 KiB). It
goes in `ux0:data/MetroidPrime/shader_cache/` in place of the old one, and tests 47-59 find theirs in
it too.

What to look for in test 60:
- The same picture everywhere: fog, lit surfaces, the lava, water and the other indirect effects, the
  visors, particles, alpha-tested edges (grates, foliage).
- At the start "precompiled: 2960"; in play few or no programs sent to the compiler thread.
- The ending's dips (12-14 FPS for some 4 s in its heavy part, the GPU 72-80 ms a frame), its last shot
  (22-28 FPS), and the GPU busy time in heavy scenes elsewhere against test 59's.

## 0.46.0 test 61: the music's refills before the shader cache's writes

The user, with test 60's log: "mr music repeating and overlap has returned". The log (59,519 lines,
e3654cf, Tallon Overworld) began with the card's shader folder empty but for the new pack ("programs
on the card: 0; precompiled: 2960; material records: 0 KiB"); no program failed.
- The music's refills (CDSPStream, read by Aurora's DVD worker from the disc image) came 4 to 8.5 s
  late ("refill late ... asked for 3996 ms ago", "late refill read done ... after 5708 ms") in every
  stretch where material records were saved (226 over the session), and in none where none were
  (log lines 6192-9481 and 17159-17955).
- Each new material's record was appended to materials.bin on the card thread at once (a batch of a
  record or a few), and every program linked, from the pack too, had its binary written to the card:
  writes on the card the DVD worker's reads of the disc image waited behind.

So:
- A program linked from the precompiled pack is not written to the card (`link_compiled`): the pack
  gives it back the next session as quickly. One compiled on the Vita is, as before.
- The material records go to the card in batches, every three minutes at most or once 64 KiB wait
  (`queue_materials_locked`, looked at in `save_material` and at each presents report), the first
  three minutes from the start. Records not written yet when the game is closed are saved again the
  next time their material is drawn.
- Nothing needs deleting from the shader folder for a new build: binaries of other sources are left
  unread, and the pack holds tests 47-61's programs.

What to look for in test 61: no "refill late" lines in the log (or a few, in a batch's minute) and
the music whole; "material records: ... N saved since" growing as before.

## 0.46.0 test 62: the Federation Marine's left hand on its rifle's grip (echoes-suits 20)

Since test 52 the marine's first-person left arm was left out of Samus's poses on the cannon (it went
through the rifle). The user: "Was thinking of bringing back the left hand and make it hold the arm
cannon grip instead of on top the arm cannon, show me a preview first". They circled the barred guard
under the rifle's muzzle.
- Three previews (`~/marine/leftgrip.py`, rendered on the PC, nothing built):
  - the hand closed on the guard, the forearm along the rifle: "kinda looks like a floating hand";
  - the forearm pointing down: "too low, and now the hand is not holding the grip properly";
  - the first grip, the wrist bent so the forearm runs back 14° (A) or 28° (B) below level. They
    picked A.
- The import (`BuildLeftGrip`, port_federation_marine.cpp) writes the trooper's left forearm and hand
  as a static model in the rifle's space, Wear(look, `kLeftGripBase`) per look (the Gravity marine's
  purple, the Dark Trooper's own).
  - Posed by linear blend skinning on the Echoes skin: the fingers closed 55° and 60° at their two
    joints about the knuckle line, the thumb swung under, the forearm turned 34° about the wrist.
  - Placed with the knuckle line along the guard (index finger at its top) and the gripped point on
    its middle. 146 vertices.
  - echoes-suits `kVersion` 20, so the user imports again. The harness's output is test 61's plus the
    three grip models.
- The loader records the looks that have one (`sMarineGrips`, `PortFederationMarine::ServesLeftGrip`).
  CGunWeapon loads it with the rifle (`mPortGripModelData`, LoadSuitArm; a mods reload that brings or
  removes it loads the arm again).
- CPlayerGun::Update raises `mPortGripBlend` over 0.2 s while the grapple arm plays the gun
  controller's animations (`kAS_GunControllerAnimation`: free look, the hand on the cannon when still,
  fidgets, combo fire, a strike's flinch), and lowers it after. DrawArm still leaves the animated arm
  out with the rifle.
- CGunWeapon draws the grip on the rifle's transform, slid down and back by what the blend lacks
  (`PortLeftGripXf`): in the normal visors (outside the beam's tint), thermal, X-ray and beam changes
  (the hologram path), and the depth pass.

What to look for in test 62:
- Import the Echoes suits again (version 20). The log's "serves the Federation Marine" line ends
  "3 with the left hand on them".
- Stand still for a second, or free-look: the marine's left hand and cuff rise under the rifle's
  lower left and stay while you turn and fire; walking drops them.
- The grapple beam still shows the whole left arm.

(Not in the build: since test 52 the user also has `echoes-suits-hd`, an add-on mod of higher-resolution
textures for the marine and the Dark Trooper made from their own pictures. It works with test 62 as it
is.)

## 0.46.0 test 63: the marine's rifle flat, its first-person arms straight and to the shoulder (echoes-suits 21)

From renders on the PC (nothing on the Vita between the two builds), the user asked for three changes,
each previewed before they said "Build them".
- "rotate the arm cannon completely flat with the top facing forward" (a T-pose render). The bodies'
  hand and rifle are now turned 90° about the forearm (`kGripRollDegrees`), not 50°.
  - The preview showed what that does in Samus's own poses: test 51's 50° hung the grip straight
    down (her animations roll the forearm about 51° standing, 50° walking, 82° running), while 90°
    leaves it about 40° off down in most of them. The user: "looks good".
- "Samus' arm is not bent... but ours" (the two first-person arms side by side). BuildRifle no longer
  bends the upper arm's end 30° up toward the gun skeleton's shoulder joint, which Samus's suit arm
  (PowerArm 07111AA7) doesn't follow. The arm runs straight back; BuildRifle needs no gun skeleton now.
- "extend both of the left and right arm".
  - The first-person rifle's arm goes on to the shoulder joint (`kRifleArmCut` 0.27, was `kArmCut`
    0.33). It now ends 20 cm behind the gun's elbow; Samus's suit arm ends 23 cm behind it.
  - The left hand on the grip gets its upper arm, bent at the elbow toward Samus's left shoulder
    (`kGripShoulder`). It ends about 40 cm short of it: the grip is held as far out as her cannon.
  - The shoulder pads stay out. The grapple arm is as before.
- From the first-person camera nothing changes: the new parts sit behind and below the frame.
- The harness's output is byte for byte the preview's (`~/marine/out13`). Against test 62's, only the
  nine bodies, three rifles and three left hands differ.

What to look for in test 63:
- Import the Echoes suits again (version 21).
- Third person, cutscenes and the pause screen: the rifle about 40° off grip-down standing and
  walking, as the preview showed.
- First person: as test 62.

## 0.46.0 test 64: the marine's first-person left arm straight (echoes-suits 22)

After test 63 (not yet played), with a render of its first-person arms: "Let's see what it looks with
the left arm straightened", an arrow drawn straight back along the left upper arm. A preview left
BuildLeftGrip's elbow bend out, so the upper arm runs on along the forearm's line, back 14° below level.
The user: "Build it straight".
- `kGripShoulder` and the elbow's turn are gone; the upper arm takes the forearm's pose.
- Only the three left-hand models change (`~/marine/out16`, byte for byte the preview's `out15`).
- From the first-person camera nothing changes.

What to look for in test 64: import the Echoes suits again (version 22); otherwise as test 63.

## 0.46.0 test 65: the marine's rifle upright in third person

After test 64, the user: "Don't know what's wrong with the right arm in third person. They seem to love
rotating when looking up and down".
- The cause: Samus's aim clips (ANCS C2E1F513), which the third-person body blends by the aim's pitch
  and yaw, turn her forearm about its own line as they point the cannon. The rifle rides the forearm, so
  it rolled with them. To hang its grip straight down the forearm needs +23° at the level aim, +130°
  aiming up, and +145° and +127° aiming down right and left (`~/marine/aimroll.py`, `aimtwist.py`). So
  looking up or down, the rifle turned nearly upside down.
- The fix: `LevelRifle` (port_third_person_body.cpp) runs after `AimArm`. It turns the marine's forearm
  (R_elbow) about its line until the grip points as near the ground as it can.
  - With the barrel steeper than 73°, the ground's way across it swings fast. There the grip goes the
    way a rifle pitched that far from level holds it: ahead of the body aiming up, behind it aiming down
    (wholly past 87°). The two agree for an aim straight ahead.
  - It applies aiming or not, so standing and walking hold the rifle grip-down too (test 63: about 40°
    off down).
  - The arm has no wrist joint, so the turn is at the elbow. Six of the body's vertices share the elbow
    50/50; at the largest turn (145°) renders show no visible pinch.
- Only the marine's bodies (`PortFederationMarine::ServesBody`). Samus's bodies, cutscenes and the
  pause screen are as before.
- The import is unchanged (`~/marine/out17` matches `out16` byte for byte), so there is nothing to
  import again.

What to look for in test 65: in third person as the marine, aim up and down, standing and walking.
The rifle should stay upright, its grip toward the ground. Echoes-suits stays version 22.

## 0.46.0 test 66: Samus's cannon upright in third person

Test 65 levelled the marine's rifle only. The user, of Samus's own arm: "Yes, Samus too".
- The aim clips leave her right forearm at these rolls about its line (the bind's -z from straight
  down, as test 65's `LevelRifle` measures; `~/marine/aimroll.py`): -67° at the level aim ahead or
  left (the ready clip), +23° aiming right (B_aimrright_samus), +40° up ahead, -56° up left, -62° up
  right, +37° down left, +55° down right. Blended by the aim's pitch and yaw, the cannon turned up to
  107° about its line looking up or down, and back again as the aim crossed the up row.
- `LevelCannon` (port_third_person_body.cpp) turns her forearm about its line to the level row's roll
  at the aim's yaw (`LevelRollRadians`: -67° ahead and left, toward +23° at the right), at every
  pitch, by the aim's weight (with the cannon lowered, the clips' own roll).
  - It runs before `AimArm`, over the clips' shoulder as AimArm starts from it, so the bore still goes
    onto the aim, and the roll is worked out from the same pose every frame (the clips' forearm
    rotation kept as the shoulder's is).
  - Samus's bodies and the Echoes suits' (Prime's skeleton, the same clips); not the marine's (test
    65's grip-down roll) or the dread body.
- Offline, on aimtwist.py's harness with Samus's Power Suit body (CMDL F39476F2): the turns are +107°
  up ahead, +104° down left, -85° up right, +32° down right, +11° up left, 0 level; the elbow's
  close-ups show no pinch (her cannon is a model of its own, not drawn there).

What to look for in test 66: in third person as Samus, aim up and down, ahead and to the sides. The
cannon should keep the roll it has aiming level; with the cannon lowered, as before. The marine as in
test 65.

## 0.46.0 test 67: the third-person arms bent one way; the free arm off the chest's turn

The user, before trying test 66: "The upper arm also twists when looking up and down and the left
arm does this weird motion when turning left and right". Measured on the ANCS on the PC (the
scratchpad's hinge.py, hinge2.py, twistrange.py, leftarm.py on `~/marine`'s harness):
- The body's clips come in two kinds. The ready, idle, stop, walk and run clips and the aim clips up
  left and up right bend the right elbow about its upper arm's local +z and the left about -z, each
  forearm turned +1 to +4° about its line under the upper arm. The slow walk, the turn on the spot and
  the other aim clips (up ahead, right, down left, down right) have the right upper arm rolled half
  round and the left a quarter, each forearm turned +88 to +90°: the same arms, the bones rolled
  otherwise. Each clip's turn is constant through it.
- Blended across the two kinds, the upper arm spun about its line: the right one 164 to 220° from the
  level aim to up ahead, down or right. The left rolled a quarter turn as the turn on the spot (or the
  slow walk) blended in.
- The left arm, free while aiming (the legs' pose), hangs from the chest, and the aim clips turn the
  chest some 110° from aiming left to right: the left hand swung 1.2 m, from behind her to in front.

So:
- `CanonicalArms` (in `Sample`, so every clip the body plays) puts each sampled pose's arms the ready
  clip's way (`MeasureArms`): the upper arm turned about its line until the elbow bends about the ready
  clip's axis, the forearm kept where it was in the model; then the forearm turned about its line to
  the ready clip's turn under the upper arm. Offline, the right upper arm's spin over those blends is
  3 to 37°, and every clip's elbow is the ready clip's.
- The cannon's roll then follows the arm: the bind's -z from straight down -41 to -78° over the aim
  clips (they had -67 to +55). Test 66's `LevelCannon`, which held it to the level row's roll at the
  aim's yaw, is gone: aiming right its table wrung the elbow up to 98°.
- While aiming, the free arm's upper arm (`freeShoulder`) is held as the legs' pose holds it in the
  model, by the aim's weight; only its shoulder moves with the chest (the hand some 0.5 m).
- The marine's `LevelRifle` (test 65) is unchanged, after the aim as before.

What to look for in test 67: in third person, look up and down and turn left and right, standing and
walking (slowly too), as Samus and as the marine. The upper arms should keep their facing, the
cannon its roll, the left arm hanging as it does when walking, and the elbows no wrung look.

## 0.46.0 test 68: a 30 FPS cap, the attract movies' field lines, Press Start at once

The user, with test 67: "I'd like to have a '30 FPS Cap (Target)' option between the '60 FPS Cap
(Target)' and the cap scaler also the 'Press Start' doesn't appear when you start the game and boot
into the main menu", and then "reimplement the interlace effect when the main menu shows videos".
- **30 FPS cap (target)** (Video > Frame rate, saved `frame_limit_30`): a checkbox between the 60 FPS
  cap and the FPS cap slider. On, the frames are paced to 30 a second (`FrameRateCap`, the dynamic
  resolution's target and the timing line too), each up two of the screen's refreshes; the game still
  steps at 60 Hz. One cap or the other: checking either clears the other, and the slider is off
  under both. Original experience still holds 60.
- **The attract movies' field lines** (`CMoviePlayer::PortSetFieldLines`, `PortFieldLineRow`). Test 23
  drew every movie whole. The front end's attract movies (made without `deinterlace`) draw the
  GameCube's field lines again: every other line, by the 8x4 pattern's alpha test as before, but one
  line to a row of the screen's pixels rather than to a line of the 480-line frame (uneven at 544 rows
  and more so zoomed: why test 23 took them out). The pattern's texgen row maps the quad (the ortho's z,
  which runs up) through the menus' zoom (`GXPortGetViewportZoom`) and the frame's place on the screen
  (`GXPortGetScreenFit`, `GXPortGetScreenRows`: 544) to the screen's rows, each row's centre on a
  texel's centre. The title, file select and ending movies stay whole.
- **Press Start**: test 67's log drew it in 11 frames, and then the title went on to file select. The
  disc's `PressStartDelay` (1 s on the user's disc) starts the prompt and the title's taking Start
  together, and the prompt's pulse began from nothing (half a second to full): pressed at once, it
  was gone before it showed. Its pulse begins at its brightest now. Its texture loaded as it should
  (no read failed, and the loader was idle on the title).

What to look for in test 68: the new checkbox (30 FPS when on, the slider greyed); the attract movies
(leave the title alone for 30 s) with even lines a pixel apart; Press Start bright as soon as the
title takes Start.

## 0.46.0 test 69: Press Start shown (Render2D off the near plane), the attract movies at 4:3

The user, with test 68: "4 attract videos later, the press start prompt still didn't appear, also, is
it possible to unzoom and put the attract videos back to 4:3?"
- **Press Start**: test 68's log drew it on every title frame (the reports' one single-texture draw a
  frame, blended, with its own program), so it was drawn and not seen. `CGraphics::Render2D` lays its
  quad at depth 1 in an ortho whose near plane is 1 (`MTXOrtho(..., -1, -10)`): on the near plane
  itself. GX's clip depth there comes out -1.0 in float, kept on the GameCube and the PC. The PS Vita
  takes GX's clip depth to GL's (2 z + w) in the projection's rows, and in float that lands one step
  past the near plane (-1.00000012, worked through with the game's own `MTXOrtho`): the GPU clipped
  the whole quad. With a depth range set (a game's, left on the front end after quitting) it lands
  inside (-0.9375), which is why it once showed only after a save was loaded and quit. On the Vita
  the quad now sits a hair inside (depth 1.0001: 2.2e-5 of the clip depth, the depth 1.1e-5, still
  in front of everything). The other Render2D draws (the Dolby splash screen, the in-game on-screen
  textures) get the same depth.
- **The attract movies at 4:3**: `CFrontEndUI::Draw` leaves the menus' zoom off while the attract
  movie is the one shown (`PortMenuZoom(false)`), so they play as the 4:3 picture they are, in the
  middle of the screen. The title, file select and their movies stay zoomed. The field lines follow
  (their rows read the zoom).

What to look for in test 69: Press Start pulsing on the title from first boot (and the Dolby logo
among the splash screens); the attract movies as a 4:3 picture with bars at the sides, their lines a
pixel apart; the title zoomed again after them.

## 0.46.0 test 70: the 30 FPS cap's frames two ticks each, unblended; Dark Samus's arm cannon in first person

The user, with test 69: "third person movement looks jittery when moving with the 30 FPS Cap", and "I
want the dark samus mod to have her arm cannon in first person too".
- **The 30 FPS cap's cadence** (`PortDebug::FrameCadenceTicks`, `FixedStepClock::Advance`): test 69's log
  held 30 frames a second with stretches of one and three ticks a frame in turn (1 13, 2 4, 3 13)
  rather than two: the clock rounded at a tick's edge, and only the 60 FPS cap had leeway there. A cap
  below the simulation rate that divides it (the 30 FPS cap: 2 ticks) now runs that many ticks a frame
  while the clock is within a whole tick of them either way; only a whole tick of drift adds or holds
  back one, after which the remainder is near nothing again.
- **Unblended at 30**: frames were blended between ticks under the 30 FPS cap (only the 60 FPS cap
  turned it off). The camera was drawn a varying fraction of a tick behind, and the third-person body
  was not, so she slid back and forth against the camera as she moved. Under the 30 FPS cap each frame
  now shows its tick's own state, as under the 60. Where frames are still blended (an FPS cap that is
  not a whole number of ticks a frame, or none), the third-person body is drawn at the blended place,
  as her other models are (`PortPresentedView`).
- **Dark Samus's arm cannon in first person** (echoes-suits 23, `PortEchoesSuits::kDarkCannon`
  F60324CB): the import makes it from her cutscene body as the Federation Marine's first-person rifle
  is made: her right arm's triangles from the shoulder joint out (upper arm, forearm, cannon), her
  `GUN_Particle_LCTR` at the origin, the T-posed arm turned down the gun's barrel, unskinned (400
  vertices). While she is played the loader serves it as every suit's first-person cannon, and the gun
  draws it on its muzzle in place of the arm cannon and the suit arm, upright. Mods > Echoes: "Dark
  Samus's arm cannon in first person" (`dark_samus_first_person_cannon`, on). The first-person left arm
  stays the suit's. Needs the Echoes suits imported again.

What to look for in test 70: third person at the 30 FPS cap moving smoothly with the body steady in
the view; after re-importing the Echoes suits, Dark Samus's cannon in first person with every beam.

## 0.46.0 test 71: Dark Samus's first-person cannon upright

The user, marking a side of test 70's preview "TOP": "arm cannon is sideways". Her cannon comes out of
her T-pose with its top (the side split in a V at its back, where the forearm goes in) to the left.
`PortDreadSamus::FirstPersonCannonRoll` turns it 90 degrees about the barrel (the gun's RotateY, as the
Dread cannon's 30), which puts that side on top; the previews before and after were checked from the
first-person eye and from above. The import is unchanged (echoes-suits stays 23).
