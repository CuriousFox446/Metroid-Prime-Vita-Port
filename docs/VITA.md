# PS Vita (headless, Phase 1)

This build runs the whole game and Aurora's renderer on a PS Vita **without drawing
anything**. It exists to answer one question before a real Vita graphics backend is
written: can the Vita's CPU run the game logic plus the GX translation at 60 Hz?
The screen stays black; the answer is in the log.

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
