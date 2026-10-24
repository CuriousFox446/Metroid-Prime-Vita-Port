# PS Vita (headless, Phase 1)

This build runs the whole game and Aurora's renderer on a PS Vita **without drawing
anything**. It exists to answer one question before a real Vita graphics backend is
written: can the Vita's CPU run the game logic plus the GX translation at 60 Hz?
The screen stays black; the answer is in the log.

## Phase 1 summary

**Answer: not at 60 Hz, and not yet at 30.** On a Vita at 444 MHz the game runs stably
(the last run: 9.5 minutes, intro to the Exterior Docking Hangar, no crash, heap about 138
of 224 MiB) at about **15.6 FPS** in the hangar's heavy seconds (1,500 draws or more), with
the simulation at about 39 of its 60 ticks a second; lighter scenes run at 30 to 65 FPS.
All runs had the zone profiler on, which costs a little.

| Step | Hangar, heavy seconds |
|---|---|
| Retail display lists sent every primitive | about 7.5 FPS |
| World and model geometry resident (`MP_RETAIL_RESIDENT`) | about 10 |
| Fewer FIFO commands, PC lighting off, 4 MiB uniforms | 8.6 to 9.5 |
| VFX scene copy on the FIFO thread | 9.6 |
| Frame end deferred to the FIFO thread (`AURORA_DEFERRED_FRAME_END`) | 14.7 |
| Controls profile once a frame, smooth frames off | 15.0 |
| Pad reads once per event pump | 15.6 |

Where a heavy frame goes now (about 64 ms):

- **Game thread, about 59 ms of work.** Input 3 ms. Ticks 27.5 ms: 3.8 a frame at about
  7.3 ms each (entity Think 8.3, of which particles about 3; actor touches 3.2; HUD 2.7;
  platforms and doors 2.1; PreThink 1.7; sorted lists 1.2; input generator 3.3). Draw
  28.6 ms (opaque world 11; sorted geometry 2.6; PreRender 3.5; CPU skinning 2.6; HUD 1.7;
  gun 1.2). It waits a few ms more for the FIFO thread at the next frame.
- **FIFO thread, about 47 ms.** About 1,790 draws at about 23 µs each, nearly all in
  `push_gx_draw`: bind groups 9.5, uniforms 9.0, texture resolve 5.3, the rest pipeline
  lookup and recording. The ~13,000 extension commands a frame are a small part.
- **Particles** are not the lever they looked: about 3 ms a frame, mostly updating
  existing particles in busy moments (about 800 particles and 57 system steps a frame);
  the port's Remastered extras on them cost next to nothing.

What this means for Phase 2:

- **The FIFO thread's cost is mostly the WebGPU translation.** A native GXM backend would
  replace most of `push_gx_draw`'s bind group and uniform work. Separate GXM prototypes on
  the same Vita drew at about 5 µs a draw on the CPU.
- **The game thread is the next limit.** At 30 FPS a frame would run two ticks (about
  15 ms) plus the draw (about 29 ms) plus input: about 47 ms, over the 33 ms budget. The
  game's own draw code (per-surface material and GX state setup in the world pass) has to
  get cheaper too, whatever the backend.
- **The GPU is a limit of its own.** The GXM prototypes drew the Landing Site from inside
  at 30 to 35 FPS with simple shaders; full TEV shading will likely need a lower render
  resolution.
- **Shaders must be compiled ahead or cached.** The Vita compiles a Cg shader in 0.1 to 6 s
  (libshacccg), and Aurora makes one per pipeline configuration, hundreds in the hangar.

Realistic target after Phase 2: 20 to 25 FPS in the heaviest rooms, 30 in lighter ones,
with more game-side draw work to go further.

## What is different from the desktop build

| Part | Desktop | Vita |
|---|---|---|
| WebGPU | Dawn | `extern/aurora/platforms/vita/webgpu_null`: the WebGPU C API, accepting everything and drawing nothing. Objects, mapped buffers and callbacks behave as WebGPU says; GPU results read back as zeros. The renderer above it is unchanged. |
| Disc reader | nod (Rust) | `extern/aurora/platforms/vita/nod`: the same C API over a **plain** GameCube image (`.iso`/`.gcm`). RVZ, WIA, CISO, NKit and Wii images are refused. |
| Process setup | | `platform/vita/vita_platform.cpp`: 224 MB heap, 4 MB main stack, 444 MHz CPU, data folder, default variables. |
| Enums | 4 bytes | 4 bytes too (`-fno-short-enums`), as the game needs: it reads disc data in place into structs with enum members. ARM EABI's default is short enums, so SDL3 and the other libraries are built from source with the flag; only newlib and libstdc++ stay prebuilt (their enums are plain ints, scoped, or padded to int). |
| Crash handler, Discord, log copier | signals, Unix socket, forked process | none, unsupported, stdio reopened onto the log file |

## Build

In WSL or Linux, with [VitaSDK](https://vitasdk.org) installed (`$VITASDK`, default
`~/vitasdk`) plus its `sdl3 vitaGL vitaShaRK libmathneon fmt zstd zlib libpng freetype`
packages (`vdpm install ...`), and Ninja:

```sh
tools/vita_build.sh            # configure + build -> build/vita/metroid_prime_port.vpk
```

The null WebGPU's generated half is regenerated from Dawn's header with
`extern/aurora/platforms/vita/webgpu_null/gen_stubs.py` (see its docstring).
`extern/aurora/platforms/vita/tests/host_test.cpp` checks the GCM reader and the null
WebGPU on the build machine.

## Run

On a Vita with HENkaku/Ensō:

1. Install `metroid_prime_port.vpk` with VitaShell.
2. Copy your own disc image, a plain `.iso` or `.gcm` of *Metroid Prime* (USA 1.00,
   `GM8E01`), to `ux0:data/MetroidPrime/`. The first one found is used; `MP_DISC`
   is not settable on the Vita.
3. Start **Metroid Prime Port (headless)**. It boots straight into a new game
   (`MP_FAST_BOOT`), so the opening on the frigate runs without input. Leave it
   for a few minutes, then close it from the LiveArea.
4. Fetch `ux0:data/MetroidPrime/metroid_prime_port.log` (VitaShell FTP or USB).
   Before the log starts, early output goes to `stdout.txt` and `stderr.txt` in the
   same folder. A crash leaves a core dump in `ux0:data/` (`psp2core-*.psp2dmp`).

The numbers that matter are the once-a-second lines
`[timing] presented=… FPS throughput=… FPS simulation=… ticks/s`. **Throughput**
is frames per second of the main loop's own time: game logic, GX translation and
command recording, with no GPU. It must stay at 60 or above with room to spare,
because a real renderer will add its own CPU work.

Each is followed by a `[split]` line: the main thread's milliseconds per frame by
phase (input, game tick, begin-wait for the render worker, the game's draw calls,
loader time, end-scene, the rest) and how busy Aurora's render worker thread was,
which processes the GX FIFO and encodes the frame. A high begin-wait with a busy
worker means the GX translation is the bottleneck; a long tick or draw means the
game itself is.

Then `[zones]` lines (`MP_PROFILE`, on by default here): the main thread's most
expensive zones for the same second, in ms and calls per frame, longest first, and
the busiest zones on other threads. Zones are the game's draw stages (`PORT_ZONE` in
the world draw, renderer, CPU skinning, particles, HUD, gun) and Aurora's own Tracy
zones, which `extern/aurora/platforms/vita/profile` receives in place of Tracy.
Times are inclusive: a zone includes the zones it calls. `[zones] other` lines are
the other threads' busiest zones, the GX FIFO thread's among them (`process`,
`draw_prim`, `push_gx_draw`...). Past its first 64 calls a second, a zone is timed on
one call in 16 and weighted, since the Vita's clock is a kernel call: per-draw zones
timed on every call slowed the hangar from about 20 to 3 FPS.

First profiled run: the main thread's long draw phase is almost all `drain`, waiting
for the GX FIFO thread, whose `process` (mostly `draw_prim`, Aurora's GX-to-WebGPU
translation per draw) took about 200-300 ms a frame at the heaviest points. The
game's own draw code was about 20 ms, CPU skinning 1-2 ms.

Second (sampled) run, with exact call counts: the hangar sends about 25,300 GX
primitives a frame (18,300 at the intro's heaviest), which Aurora merges into about
1,720 draws; every primitive's vertices and indices are copied and translated again
each frame (`draw_prim`, `push_verts`, `push_indices`). Aurora can keep retained display
lists on the GPU instead (resident geometry, `GXPortRetainResident`), but the port uses
it only for room-geometry mods, never the retail models. A game tick costs about
8.5 ms. Even sampled, ~75,000 zone entries a frame still inflate the times several
fold (hangar about 4 FPS profiled against about 20 without): trust counts and
ranking, not milliseconds.

So the Vita build now keeps the retail world geometry resident (`MP_RETAIL_RESIDENT`,
`platform/port_retail_resident.cpp`): each area's model arrays and surface display
lists are retained when the area's geometry is added and released when it goes, so
Aurora turns each display list into triangle lists once. The profiler is off by
default again (`MP_PROFILE=1` turns it on); the `[split]` line ends with the resident
geometry in use, which stays at 0 if the display lists could not be made resident.

On hardware every retained world display list was drawn resident (about 1,000-1,700
calls a frame in the hangar, no fallbacks, no conversions redone), and the intro's
heaviest draw phase fell from about 95 to 70 ms; but the hangar stayed near 7.5 FPS, so
most of the GX work is not world geometry. Now every loaded model (`CModel`: actors, the
gun, characters) keeps its display lists resident too, and the `[gxsrc]` lines count
what each stage of the frame sends through GX (immediate primitives, display lists sent
or resident), with a `[heap]` line, since each retained list is also copied on the CPU.

With every model resident the hangar ran at about 10 FPS, and the profile put the limit
on the FIFO thread: about 82 ms of work a frame, of which `push_gx_draw` (about 2,050
draws at roughly 24 µs, half of it bind groups and uniforms) is most and the ~13,000
other extension commands about 15 ms. The game thread spends about 50 ms of its draw
phase in two `drain` waits for it. So the Vita build now:

- sends the draw tag (which only names draws in Aurora's warnings) with the next draw
  and only when it changed, instead of twice a surface;
- turns off the PC port's own lighting, which is FIFO work the retail game never had:
  sun shadows (`MP_SHADOWS=0`), volumetric fog (`MP_VOLFOG=0`), bloom (`MP_BLOOM=0`),
  reflection probes (`MP_PBR_PROBE=0`) and projected actor shadows
  (`MP_PROJECTED_SHADOWS=1` gives the last back to the setting);
- starts the uniform staging at 4 MiB (`AURORA_STAGING_START_UNIFORM_KIB`; a frame used
  at most 3.7 MiB), 20 MiB less over the five buffers.

Two more lines say what is left. `[ops]` counts the extension commands the FIFO thread
handled per frame, most first. `[drain]` lines list where the game waited for the FIFO
thread, per frame, by call site; `tools/vita_drain_sites.sh <log>` names the sites from
the same build's ELF (`build/vita/metroid_prime_port`). Removing the waits alone cannot
beat the FIFO thread's own time per frame, which is the ceiling until it does less.

On hardware, the hangar's heavy seconds (1,500 draws or more) went from 8.6 to 9.5 FPS
throughput, the FIFO thread's `process` from about 72 to 63 ms a frame, and the heap to
about 140 MiB. `[ops]` then showed about 12,800 commands a frame: about 1,770 resident
calls, 1,740 draw tags (one a surface now), 1,640 texture loads and about 6,500 array
bases (eleven arrays per model draw). Of the two drains, one is `end_frame`'s; the other
was `vfx::capture_scene`, Remastered's opaque scene copy in the middle of the world draw,
which waited up to 32 ms a frame for the FIFO thread to catch up. The copy reads the
recorder that the FIFO thread owns, so it is now a FIFO command
(`GX_AURORA_PORT_VFX_CAPTURE_SCENE`) made where the FIFO thread reaches it, and the
game no longer waits there. The VFX draws that read the copy still drain first.

On hardware that wait was gone, but throughput stayed at 9.6 FPS: the time moved to
`end_frame`'s drain (about 35 ms). A frame was the game's ticks (about 38 ms, with the FIFO
thread idle), then its draw (about 30 ms, the FIFO thread working alongside), then the wait
for the FIFO thread's remaining work. So the Vita build now ends frames deferred
(`AURORA_DEFERRED_FRAME_END=1`): `end_frame` freezes ImGui and queues the rest of the frame
end (Aurora's texture sweep, `gfx::finish`, handing the frame to the render worker) as a
FIFO command, then returns, and the game's next tick runs while the FIFO thread finishes the
frame. The next `begin_frame` waits for that command only, so the wait left shows as the
`[split]` line's begin-wait. Anything that changes the EFB or swapchain (a resize, a quality
change, a GPU synchronize) waits for it first too. RmlUi frames still end the old way.
`platforms/vita/tests/fifo_frame_end_test.cpp` runs the FIFO side on the host.

On hardware (4.7 minutes, no errors, no crash) the hangar's heavy seconds went from 9.6 to
14.7 FPS throughput and 8.0 to 11.9 presented; end-scene fell from about 34 ms to 0.2 and
begin-wait stayed under 1 ms, so the game thread no longer waits for the FIFO thread at all.
It is now the game thread's own work that sets the frame: input about 3 ms, the ticks about
37 ms (3 to 4 a frame at about 12 ms each, the simulation at about 35 of 60 ticks a second)
and the draw about 31 ms, against the FIFO thread's roughly 65 ms alongside.

So the tick now has zones too, and `[zones]` lists up to 32 main-thread lines. `UpdateTicks`
holds a `tick` zone per tick (its actor snapshots, input generator and message pump);
`CMFGame tick` splits off the HUD (`CInGameGuiManager::Update`); `SM::Update` splits into its
stages (`SM: ...`: the port's per-tick hooks, PreThink, platforms, player input and collision,
touches, Think, camera, world, sounds, area streaming), with Think split into the player, AI
(`CPatterned`) and other entities. Particle updates (the outermost `CElementGen::Update`) and
animation (`CAnimData::DoAdvance`, `AdvanceParticles`) are counted across all of them. Around
the ticks: `ARQPoll + AIPortPoll`, `AsyncIdlePakLoading`, `ARAM garbage + DMAs`.

On hardware the ticks took about 39 ms a heavy frame (3.8 ticks): `SM::Update` about 26
(entity Think 11, with about 950 entities thinking a tick and particle updates of 3 to 12
ms inside; touches 3.3, platforms and doors 2.2, actor snapshots 2.0, PreThink 1.7, sorted
lists 1.5), the HUD 2.6, and the input generator 10.2: about 2.7 ms a tick in the port's
input engine, which rebuilt and compared the whole controls profile on every poll. Now:

- the controls profile is built once a frame (`PortInputDevices::BeginFrame` from the main
  loop), not once a tick; the input zones (`input: ...`) split what is left;
- smooth frames (actor, pose and particle interpolation between ticks) are off on the Vita
  unless `MP_SMOOTH_FRAMES=1`: frames come slower than ticks there, so the blend shows
  nothing and its snapshots cost every tick;
- the per-entity Think zones are gone again (2,000 to 3,600 entries a frame distorted the
  times); `SM: Think (all entities)` stays.

On hardware that took the heavy seconds from 14.7 to 15.0 FPS: the input generator from
10.2 to 7.7 ms a frame and the ticks from 38.9 to 35.3 ms, with the profile build now 0.6
ms a frame. Most of what was left was reading the devices (about 1 ms a tick) and `PADRead`
(about 0.5 ms a tick), dozens of SDL pad calls each, though SDL's pad state only changes
when events are pumped (`poll_events`, once a frame). Aurora now counts the pumps
(`window::pump_generation`) and `PADGetRawState` and `PADRead`'s extra buttons keep their
last read until the next one. The particle update has zones per stage (the port's mover
steps, `UpdateExistingParticles`, `CreateNewParticles`, the Remastered VFX update, the
bounds; child systems are inside their parents' stages) and a `[particles]` line counts the
system steps, particles updated and particles created per frame.

On hardware (the last Phase 1 run) that took the heavy seconds to 15.6 FPS: the input
generator from 7.7 to 3.3 ms a frame and the ticks from 35.3 to 27.5 ms (7.3 ms each). The
particle update averaged about 3 ms a frame over about 57 system steps and 800 particles;
in busy moments `UpdateExistingParticles` was most of it, and the other stages (the port's
mover steps, the bounds, the Remastered VFX update) were negligible. See the summary at the
top.

## On hardware so far

1. Aborted before `main`: `std::call_once` threw `system_error(-1)` because
   libstdc++'s threads-active test (`pthread_cancel`) was not linked from the static
   libpthread. Fixed by linking the whole archive.
2. Exited after starting the null device: the port treats Null as "no GPU". Fixed by
   setting `MP_NO_DISC_DIALOG` (no boxes, keep running) in the Vita setup.
3. Booted, mounted the disc, played the intro cinematic with its streamed music for
   about five minutes, then a data abort in `CMapArea::CMapAreaSurface::Draw`: the
   32-bit in-place MAPA path, misaligned by 1-byte enums in `CMappableObject`. Fixed
   by building everything with 32-bit enums. Throughput in that run was 130-157 FPS
   at first, then slid from 58 to 7 FPS during the cinematic.
4. Past the map, through the intro into the Exterior Docking Hangar, then a data
   abort in strlen under printf: the Vita's newlib has no C99 formats, so a `%zu`
   printed "zu", consumed nothing, and a later `%s` took a number for a string.
   Fixed by wrapping the printf family (`platform/vita/vita_printf.cpp`). The
   cinematic slide was unchanged by the enum fix, so it is load, not a leak:
   18-25 FPS in the hangar, about 7 in the heaviest stretch, with the game logic
   still at 60 ticks/s. The `[split]` lines now say where a frame's time goes.

## Known risks

- **32-bit paths no other host runs.** The port's desktop and Android builds are all
  64-bit; code under `UINTPTR_MAX == UINT32_MAX` was last run on a GameCube.
- **Memory.** About 52 MB of static data plus the 224 MB heap needs the extended
  memory budget (`ATTRIBUTE2=12` in `param.sfo`). If it does not start at all,
  lower `_newlib_heap_size_user` first.
- **SQLite file locking** on the Vita's newlib is unknown. Aurora's pipeline and
  Dawn caches carry on without their databases, so a failure there only costs a
  warning in the log.
