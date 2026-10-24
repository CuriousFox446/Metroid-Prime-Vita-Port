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
