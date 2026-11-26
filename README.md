# Metroid Prime Unbound for PS Vita

> **Disclaimer:** this is an unofficial fan project. It is not affiliated with or endorsed by
> Nintendo or Retro Studios. It builds on the [PrimeDecomp](https://github.com/PrimeDecomp/prime)
> decompilation and on the Metroid Prime Unbound native port, but neither project maintains or
> supports this Vita build. Please report problems with it [here](../../issues), not to them.

**Metroid Prime, running natively on the PlayStation Vita.** No emulator is involved:
- The game code is the [PrimeDecomp](https://github.com/PrimeDecomp/prime) decompilation, compiled
  for the Vita by way of the Metroid Prime Unbound native port (version 0.46.0).
- A renderer written for the Vita's GPU draws it.

**No game content is included.** You need your own copy of the game as a disc image, and the port
reads the game from it at run time. This repository contains no disc image and none of the game's
assets, and neither does any build made from it.

## Status

The whole game is playable, from the Space Pirate Frigate to the ending. The frame rate is still
being worked on:

- lighter rooms run at 30 FPS;
- the heaviest scenes run at about 16 to 25 FPS: Phendrana's snowfields, big fights, the final
  battle and the ending.

The game renders at the Vita's native 960×544, in 4:3 (pillarboxed) or 16:9.

## What the Vita build adds

- **Drawn by the Vita's GPU** through [vitaGL](https://github.com/Rinnegatamante/vitaGL). Each
  GameCube material gets its own shader, generated from its TEV setup, compiled on the Vita and
  kept on the memory card.
- **All four CPU cores.** The game, the GX command processor and the render worker each have a core.
  Skinning, particle updates, the sound mix and shader loading go to the fourth core when
  [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) opens it.
- **Controls for the Vita:**
  - a GameCube-style default layout, with the Vita's buttons shown in the game's prompts;
  - twin-stick aiming;
  - gyro aiming with the Vita's motion sensor, with the rear touch pad as the Gyro aim button;
  - the Remastered and Modern presets, mapped to the Vita's buttons.
- **The port menu** (Select + Start) holds the native port's settings. It works with the buttons or
  the touch screen, and its text fields open the Vita's own keyboard.
- **Frame rate caps** of 60 or 30 FPS (Video > Frame rate).
- **The third-person camera** (Video > Display).
- **The title and file select menus** zoomed to fill the screen.
- **Echoes suits** from your own *Metroid Prime 2: Echoes* disc, with **Play as Dark Samus** and
  **Play as a Federation Marine** (see [Echoes suits](#echoes-suits-optional)).

### Not on the Vita

- The Prime Remastered and Metroid Dread imports. They read Switch dumps, and their models are far
  heavier than the Vita can draw.
- Archipelago multiworld, and mouse and keyboard.
- Resolution scale and dynamic resolution. The Vita always renders at its own 960×544.
- The native port's extra lighting: per-pixel lighting, sun shadows, volumetric fog and bloom.
- Compressed and Wii disc images. The Vita reads plain GameCube `.iso` and `.gcm` images only.

## What you need

- **A PS Vita with homebrew enabled** (HENkaku / Ensō) and
  [VitaShell](https://github.com/TheOfficialFloW/VitaShell). The port is developed and tested on a
  PS Vita; the PS TV is untested.
- **`libshacccg.suprx` in `ur0:data/`.** It is the Vita's shader compiler, which every vitaGL game
  needs. If you don't have it yet,
  [extract it from your console](https://cimmerian.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx).
- **Recommended: [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker).** It opens the Vita's
  fourth CPU core to games.
  - Install: add `CapUnlocker.skprx` under `*KERNEL` in `ur0:tai/config.txt`, then reboot.
  - Without it the port runs on three cores, and runs slower.
- **Your own Metroid Prime GameCube disc image**, as a plain `.iso` or `.gcm`:
  - USA 1.00, 1.01 or 1.02 (`GM8E01`). PAL (`GM8P01`) is accepted but experimental.
  - The Vita can't read compressed images (RVZ, WIA, GCZ, CISO, NKit). Convert them back to a
    plain `.iso` first; Dolphin's Convert File can.
  - Metroid Prime Trilogy (Wii) is not supported on the Vita.
- **About 1.6 GB free on `ux0:`**, most of it for the disc image. An Echoes disc image needs about
  as much again.
- Optional: your own **Metroid Prime 2: Echoes** GameCube disc image, for the Echoes suits.

## Installing

1. Install `metroid_prime_port.vpk` with VitaShell. Builds are on the [Releases](../../releases)
   page, or see [Building](#building).
2. Make the folder `ux0:data/MetroidPrime/` and copy your disc image into it. If there are
   several, the first `.iso` or `.gcm` by name is used.
3. If the release has a shader pack (`precompiled.bin`), copy it to
   `ux0:data/MetroidPrime/shader_cache/`. Use the pack that came with the VPK.
4. Start **Metroid Prime** from the home screen.

**The first run compiles shaders.**
- The Vita compiles each material's shader the first time the game draws it. A "Compiling
  shaders" notice shows while it works.
- Until a surface's shader is ready, that surface can look wrong or be missing for a moment.
- Each shader is compiled only once, then kept in `ux0:data/MetroidPrime/shader_cache/`.
- A shader pack skips most of the compiling.

### Where things are on the memory card

| What | Where |
|---|---|
| Your Metroid Prime disc image | `ux0:data/MetroidPrime/` |
| Saves (USA disc) | `ux0:data/MetroidPrime/USA/Card A/` |
| Settings and controls | `ux0:data/MetroidPrime/` |
| Shaders | `ux0:data/MetroidPrime/shader_cache/`; vitaGL keeps its own in `ux0:data/shader_cache/` |
| Your Echoes disc image | `ux0:data/MetroidPrime2/` |
| The log | `ux0:data/MetroidPrime/metroid_prime_port.log`, with `stdout.txt` and `stderr.txt` |
| Crash dumps | `ux0:data/psp2core-*.psp2dmp` |

The saves are GameCube memory card files with Dolphin's names (`01-GM8E-MetroidPrime A.gci` and
`B`), so you can copy in a USA save from Dolphin:
1. Move the game's own `A` and `B` files out of the folder first.
2. Copy the Dolphin save in. The game loads the newer of the two files.

## Controls

The default layout is the GameCube controller's:

| Vita | GameCube | In game |
|---|---|---|
| Left stick | Control Stick | Move and turn |
| Right stick | C Stick | Switch beams |
| D-pad | Control Pad | Switch visors |
| Cross | A | Fire |
| Circle | B | Jump |
| Square | X | Morph Ball |
| Triangle | Y | Missiles |
| L | L | Lock on, scan |
| R | R | Free aim; Spider Ball in Morph Ball |
| Select | Z | Map |
| Start | Start | Pause |
| Select + Start | | The port menu |
| Rear touch pad | | The Gyro aim button |

You can change any of these under **Controls > Remap**.
- Its Remastered and Modern presets are adapted to the Vita: fire on R, lock-on with L, and the
  right stick aims.
- Gyro aiming is set up under **Controls > Touch & gyro**.

## Echoes suits (optional)

With your own *Metroid Prime 2: Echoes* disc, the port can dress Samus in Echoes' suits, or let you
play as Dark Samus or a Federation Marine. The models are converted on the Vita from your disc; none
are included.

1. Copy your Echoes GameCube disc image (a plain `.iso` or `.gcm`) to `ux0:data/MetroidPrime2/`.
2. Open the port menu (Select + Start) and go to **Mods > Metroid Prime 2: Echoes**. Once it shows
   your disc image, press **Import**. The models are converted once and kept on the memory card.
3. Make sure **Use the Echoes models** is on, then choose what you want:
   - **Echoes Suit models**: Echoes' Varia, Dark and Light Suits in place of Prime's Varia, Gravity
     and Phazon Suits.
   - **Echoes arm cannon in first person**: Echoes' arm cannon, on a switch of its own.
   - **Play as Dark Samus**: her body, Morph Ball and first-person left hand.
     **Dark Samus's arm cannon in first person** adds her arm cannon.
   - **Play as a Federation Marine**: the marine in place of Samus.
     **Marine rifle in first person** adds the rifle.

If an update changes what the import makes, the menu says "Imported by an older version:
re-import." Press **Import** again.

## Troubleshooting

- **The game closes at once, or shows nothing.** Check that `libshacccg.suprx` is in `ur0:data/`
  and that your disc image is in `ux0:data/MetroidPrime/`. `stderr.txt` in that folder names the
  disc it found, or says it found none.
- **The log says the disc isn't supported.** The image isn't one of the versions above. The log
  names the disc ID it found.
- **Slow everywhere.** Check that CapUnlocker is installed. The log says whether the fourth core is
  open to the game ("core 3 ... is open").
- **Settings in a bad state.** Port menu > System > Settings > **Reset configuration...** backs up
  and resets every setting and mapping. Saves are not touched.
- **Reporting a bug.** Open an issue with the **Bug report** form. Attach
  `ux0:data/MetroidPrime/metroid_prime_port.log`, and after a crash the newest `psp2core-*.psp2dmp`
  from `ux0:data/`, zipped.

## Building

You need Linux or WSL with [VitaSDK](https://vitasdk.org) (`$VITASDK`, default `~/vitasdk`),
CMake 3.25 or later, Ninja and Python 3. The port is built and tested with VitaSDK's 2026.08
release and CMake 4.2.3.

1. Install the Vita libraries:

   ```sh
   vdpm install sdl3 vitaGL vitaShaRK libmathneon fmt zstd zlib libpng freetype
   ```

2. Swap in the vitaGL the port is tested with, r1488 (commit `2bdbe89`). vdpm now installs a newer
   one, which can behave differently. This builds it the way vdpm's own package was built:

   ```sh
   git clone https://github.com/Rinnegatamante/vitaGL.git
   git -C vitaGL checkout 2bdbe89
   make -C vitaGL NO_DEBUG=1 -j"$(nproc)"
   cp vitaGL/libvitaGL.a "$VITASDK/arm-vita-eabi/lib/"
   cp vitaGL/source/vitaGL.h "$VITASDK/arm-vita-eabi/include/"
   ```

   A later `vdpm upgrade` puts the newer vitaGL back.

3. Build both packages:

   ```sh
   tools/vita_build.sh
   ```

The script configures `build/vita` and writes `build/vita/metroid_prime_port.vpk`. The first
configure also downloads and builds a few libraries from source (SDL3, Dear ImGui, fmt, FreeType,
libpng and others), with the 32-bit enums the game needs.

The build also makes `metroid_prime_port_demo.vpk`, **Metroid Prime Demo**. It is the same game,
frigate only, as a separate app that installs beside the full game and keeps its own data folder
(`ux0:data/MetroidPrimeDemo/`).

### On GitHub

Every push and pull request builds the VPK on GitHub as well, with
[`.github/workflows/vita.yml`](.github/workflows/vita.yml).
- It runs in VitaSDK's Docker image `vitasdk/vitasdk:2026.08-20261027`. That image's toolchain and
  libraries, vitaGL r1488 included, are the ones above.
- The VPK is kept under the run's **Artifacts**.
- Nothing is released from there: releases are made by hand.

### Shader pack

`tools/vita_precompile` builds a `precompiled.bin` on a PC from the `materials.bin` that a Vita's
shader cache records. Its programs are the ones the Vita would compile, byte for byte.
- It runs Sony's compiler (your own `libshacccg.suprx`) under
  [open-shacccg](https://github.com/frangarcj/open-shacccg)'s oracle and
  [Unicorn](https://www.unicorn-engine.org).
- The module is never copied into this repository.
- The steps are at the top of `tools/vita_precompile/precompile.py`.

## Other platforms

The native port's Linux, Windows, macOS, Android and iOS builds are still in the tree; see
[`docs/NATIVE_PORT.md`](docs/NATIVE_PORT.md), [`docs/ANDROID.md`](docs/ANDROID.md) and
[`docs/IOS.md`](docs/IOS.md). This repository's work and testing are on the Vita.

## Documentation

| Document | What it covers |
|---|---|
| [`docs/VITA.md`](docs/VITA.md) | The Vita build's first phase: building, running and reading the log |
| [`docs/VITA_PHASE2.md`](docs/VITA_PHASE2.md) | The Vita renderer, and every test build's changes and results |
| [`docs/SETTINGS.md`](docs/SETTINGS.md) | Every control, setting and environment variable |
| [`docs/MODS.md`](docs/MODS.md) | Texture packs and the mods folder |
| [`docs/MOD_FORMATS.md`](docs/MOD_FORMATS.md) | Mod file formats, for mod authors |
| [`docs/NATIVE_PORT.md`](docs/NATIVE_PORT.md) | The native port's reference: build options and platform notes |
| [`tools/README.md`](tools/README.md) | What each script in `tools/` does |
| [`PORT_NOTES.md`](PORT_NOTES.md) | Open follow-ups and non-obvious facts |
| [`decomp/`](decomp/) | The original decompilation's GameCube build and notes (not used by the port) |

## Credits

This port would not exist without the work of these projects. Thank you.

**The ports and projects it is built on**

- **Metroid Prime Unbound**, the native port by Odrannnn, which this is the Vita port of. Its
  repository (`Odrannnn/MetroidPrimePort`) is no longer online. The 0.46.0 source came from a
  fork's copy, which added French translations.
- [PrimeDecomp](https://github.com/PrimeDecomp/prime): the decompilation the game code is built
  from, and the decompiled Dolphin SDK.
- [Aurora](https://github.com/encounter/aurora): the GameCube graphics, input and platform layer.
  The Vita renderer is built into it.
- [MusyX](https://github.com/AxioDL/musyx): the audio mixer.
- [nod](https://github.com/encounter/nod): the disc image reader on the other platforms. The Vita
  has a plain GameCube image reader behind nod's interface.

**PS Vita**

- [VitaSDK](https://vitasdk.org): the toolchain and libraries.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK)
  and [math-neon](https://github.com/Rinnegatamante/math-neon) by Rinnegatamante. They provide
  OpenGL over GXM, compiling Cg on the Vita, and the maths underneath.
- [SDL3](https://libsdl.org)'s Vita port: the window, events and pads.
- [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) by GrapheneCt: the fourth CPU core.
- [open-shacccg](https://github.com/frangarcj/open-shacccg) by frangarcj and
  [Unicorn](https://www.unicorn-engine.org): the shader pack builder's oracle and CPU emulator.

**Randomizer and Archipelago**

- [randomprime](https://github.com/aprilwade/randomprime): the room patches for skippable cutscenes
  and Archipelago pickups, the room and elevator tables, and the custom pickup and door assets are
  generated from it.
- The [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime): the door table, and the
  game the Archipelago client connects to.
- The Metroid Prime AP PopTracker pack: the Archipelago logic rules.
- [Archipelago](https://archipelago.gg): the multiworld itself.

**Remastered import**

- [astc-encoder](https://github.com/ARM-software/astc-encoder): the ASTC texture encoder.
- tegra_swizzle, bcdec_rs and astc-decode, ported to C++ for the texture importer.

**Platform and input**

- [libadrenotools](https://github.com/bylaws/libadrenotools): custom Vulkan drivers on Android.
- [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers): gyro calibration and
  aiming spaces.
- [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) (CC0): the controller button icons.
- [Xelu's Free Controllers & Keyboard Prompts](https://thoseawesomeguys.com/prompts/) (CC0): the
  Steam Deck button icons.
- [Dolphin](https://dolphin-emu.org): the texture pack and save formats the port reads and writes.

**Libraries**

[SDL3](https://libsdl.org), [Dawn](https://dawn.googlesource.com/dawn),
[Dear ImGui](https://github.com/ocornut/imgui), [RmlUi](https://github.com/mikke89/RmlUi),
[fmt](https://github.com/fmtlib/fmt), [Abseil](https://abseil.io), [FreeType](https://freetype.org),
libpng, zlib-ng, [zstd](https://github.com/facebook/zstd), [xxHash](https://github.com/Cyan4973/xxHash),
[SQLite](https://sqlite.org) and OpenSSL.

## Licensing

Every third-party component keeps its own licence. [`NOTICE`](NOTICE) lists them all, with where
each licence text lives.

[`LICENSE`](LICENSE) covers this project's own work: the platform layer, the build and packaging
scripts, the tests, the tooling and the documentation. It does **not** cover the game code under
`src/` and `include/`, which comes from the decompilation and remains Nintendo's. See `NOTICE`.

Sony's `libshacccg.suprx` is not part of this repository or of any build. The Vita loads the copy
you extracted from your own console.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with Nintendo and Retro
Studios, and ships no game data.

## AI assistance

Parts of the native port were written with AI coding assistants, under its author's oversight. The
Vita port was written with an AI coding assistant (Claude), under my direction, and tested on my own
Vita.
