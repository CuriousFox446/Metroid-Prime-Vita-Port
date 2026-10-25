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
