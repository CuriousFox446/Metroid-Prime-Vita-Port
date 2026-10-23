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
| Enums | 4 bytes | ARM EABI short enums (what every Vita library is built with). The GX enums keep a fixed 32-bit type (`GXEnum.h`), as in the GameCube SDK. |
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

## Known risks, not yet seen on hardware

- **Never run on a Vita yet.** Compiles, links and packages; the two new components
  pass the host test. Nothing else is verified.
- **Enum size in game data.** Short enums change the layout of any struct with an
  enum member. Code that reads raw disc or save bytes straight into such a struct
  would misread them; the decompiled game mostly parses field by field, but this is
  the first suspect for wrong behaviour that the desktop build does not show.
- **Memory.** About 52 MB of static data plus the 224 MB heap needs the extended
  memory budget (`ATTRIBUTE2=12` in `param.sfo`). If it does not start at all,
  lower `_newlib_heap_size_user` first.
- **SQLite file locking** on the Vita's newlib is unknown. Aurora's pipeline and
  Dawn caches carry on without their databases, so a failure there only costs a
  warning in the log.
