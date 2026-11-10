# gta2-recomp

**GTA2** (PC, 1999) running natively on Apple Silicon Macs.

`gta2.exe` is statically recompiled: its x86 code is translated ahead of time
into C and compiled to arm64. Nothing is emulated. The Windows pieces the game
leans on are rebuilt on Apple frameworks only, with no SDL or other third-party
runtime libraries:

| Original | Here |
|---|---|
| `d3ddll.dll` renderer (`gbh_*`) | Metal (`runtime/src/render`), plus an optional true-3D renderer (`runtime/src/render3d`) |
| `dmavideo.dll` video device (`Vid_*`) | Metal presenter (`runtime/src/video`) |
| Bink intro movie | AVFoundation (converted to MP4 at setup) |
| Miles Sound System (`mss32.dll`) | Core Audio mixer, IMA ADPCM streams (`runtime/src/audio`) |
| DirectInput 8 | AppKit keyboard + GameController pads (`runtime/src/input`) |
| Win32 (kernel32, user32, advapi32) | `runtime/src/winapi`: AppKit window, plist registry |
| GTA2 Manager (options program) | native settings dialog |

## What's new compared with the PC original

- **Controller movement on foot that makes sense.** The original uses tank
  controls: left and right rotate the player, and up walks forward. Here the
  player faces wherever you push the left stick and walks that way. Cars, the
  d-pad and the keyboard keep the original controls, and tank mode is a
  setting away (see [Playing](#playing)).
- **Widescreen** at any resolution. The game's visible area is widened too,
  so cars and people don't pop in and out at the screen edges.
- **An optional true-3D renderer.** It draws the city as real geometry, with
  per-pixel lighting, shadows, ambient occlusion, bloom and upscaled textures,
  including optional AI-upscaled packs. Switch between it and the original
  look with ⌘1.
- **Native Mac app.** Settings dialog, full screen, controller support and an
  app icon.

## Bring your own game

This repository contains **no game code or data**. You need an installed copy
of GTA2 **v9.6** (the Rockstar Classics release, freely distributed by Rockstar
in 2004). Its `gta2.exe` must have SHA-1
`646e1d479fa2aea12b3ff448fa23f6362f9e012f`; setup checks this. The installer
is a Windows program: install it on a Windows PC or with Wine/CrossOver, then
copy the installed `GTA2` folder (with `gta2.exe` and `data/`) to your Mac.

Your game files are copied into `game_files/`, which is gitignored, as is
everything generated from them (`generated/`, `build/`).

## Build

You need an Apple Silicon Mac with macOS 14 or later, plus:

```sh
xcode-select --install          # Clang and the macOS SDK
brew install cmake ninja        # build tools
brew install ffmpeg             # optional: converts the intro movie
```

Then:

```sh
scripts/setup.sh --dir "/path/to/installed/GTA2"
open build/GTA2.app
```

Setup does the following:

1. Checks `gta2.exe` and copies the game into `game_files/`.
2. Creates a Python venv with Capstone, used at build time only.
3. Translates `gta2.exe` to C in `generated/`. This takes about a minute.
4. Converts the intro movie to MP4 if ffmpeg is installed.
5. Builds `build/GTA2.app`.

To rebuild after a change, run `scripts/setup.sh` again without `--dir`. Set
`RETRANSLATE=1` to force a fresh translation. To build by hand, use
`ninja -C build`.

`./build/gta2` runs the same app with its log in the terminal. Keep the app
where it is: it finds its game files in `game_files/` next to the build folder.
To run it from elsewhere, set `GTA2_GAME_DIR`.

### Optional: AI-upscaled textures

The 3D renderer can use 4× texture packs made with
[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN):

```sh
scripts/make_texpack.sh            # world tiles of every level, from the game files
```

The script downloads `realesrgan-ncnn-vulkan` into `.cache/` the first time,
along with Pillow and numpy for the venv. It writes
`game_files/textures/<level>/tiles/`. Expect it to take a while: roughly half
an hour per level.

Sprites (cars, people, the HUD and the menu font) are captured as the game
draws them, so every colour and damage variant is covered:

1. Play for a while in the 3D renderer (Cmd-1), started like this:
   ```sh
   GTA2_DUMP_SPRITES="$PWD/game_files/sprite_dumps" ./build/gta2
   ```
2. Upscale what was captured:
   ```sh
   scripts/make_texpack.sh --sprites
   ```

Running it again only upscales new sprites. For hand-tuned results (a
different model or pixel-doubled input per sprite), use `scripts/texpack.py`
directly; its header documents each stage. Set `GTA2_NO_PACK=1` to play
without the packs.

## Playing

Use the player name **ITSALLUP** to unlock every level.

Options on the main menu opens a settings dialog. It sets the gameplay
resolution (widescreen supported) and full screen, then restarts the game.
Settings are stored in `~/Library/Application Support/GTA2/registry.plist`.

| | Keyboard | Controller |
|---|---|---|
| Move / steer | arrows | left stick, d-pad |
| Accelerate / brake | ↑ / ↓ | RT / LT |
| Attack | Ctrl | X |
| Enter / exit vehicle, menu select | Return | B |
| Jump / handbrake | Space | A |
| Special | | Y, L3, R3 |
| Previous / next weapon | Z / X | LB / RB |
| Pause | Esc | Menu |
| Skip intro movie | any key or click | any button |
| Full screen | ⌘F | |

On foot, the left stick points the player where you push it. To get the
original tank-style turning back, set `mac_tank_controls` to `1` under
`screen` in the registry plist.

### Renderer

| | Keys | Controller |
|---|---|---|
| Switch renderer: original or 3D (remembered) | ⌘1 / F9 | View + LB |

The 3D renderer draws the city as real geometry. It adds per-pixel lighting
from the game's own lights, sky-light shadows, ambient occlusion, bloom and
light halos. It also upscales textures (Scale2x by default, or the packs
above).

## Environment variables

| Variable | Effect |
|---|---|
| `GTA2_GAME_DIR=dir` | where the game files are |
| `GTA2_LOG=file` | runtime log (default stderr) |
| `GTA2_NO_MOVIE=1` | skip the intro movie |
| `GTA2_MUTE=1` | silence |
| `GTA2_UPSCALE=1\|2\|4` | built-in texture upscaling factor in 3D (default 2) |
| `GTA2_NO_PACK=1` | ignore texture packs |
| `GTA2_NO_SHADOWS`, `GTA2_NO_AO`, `GTA2_NO_BLOOM`, `GTA2_NO_HALOS`, `GTA2_NO_EMISSIVE` | turn a 3D effect off, for comparison |
| `GTA2_DUMP_SPRITES=dir` | capture sprites for texture packs |
| `GTA2_DEBUG=1` | developer tools: a Debug menu with ⌘2 snapshot (original and 3D screenshots plus a map log; also F10, View + RB), ⌘3 instant replay of the last 3 s as PNG frames (also View + Y or the touchpad) and ⌘4 memory snapshot, all saved to `game_files/captures/` |
| `GTA2_KEYS="3000:return,5000:up"` | scripted key presses (ms after start) |
| `GTA2_CAPTURE=out.png GTA2_CAPTURE_FRAME=n` | save presented frame *n* |
| `GTA2_DUMP_TILES=1` / `GTA2_DUMP_FRAME=n` | log every primitive of one scene |
| `GTA2_TRACE_AUDIO=1` | sample and stream starts, mixer level every 2 s |
| `GTA2_HEADLESS=1` | no sound, controller or movie; message boxes go to the log |
| `GTA2_STRICT=1` | abort on any unimplemented import |

A crash prints the guest function it happened in and the guest call chain.

## How it is put together

- `tools/` is the x86→C pipeline. It is vendored from
  [xboxrecomp](https://github.com/MattSG/xboxrecomp) (MIT), with a Win32 PE
  front end and lifter fixes added; see `tools/PE_CHANGES.md`.
- `scripts/translate.sh` runs PE parse → disassemble → identify → lift into
  `generated/`. Indirect-call targets the disassembler missed are recorded at
  run time in `generated/icall_seeds.json`, and the next translation picks
  them up.
- `runtime/src/overrides.c` holds hand-written replacements for individual
  game functions (`void sub_XXXXXXXX(void)`), which are excluded from
  translation. After changing it, run `RETRANSLATE=1 scripts/setup.sh`.
- `runtime/` holds guest memory (a 32-bit address space at an offset), the
  import binder and the native modules listed above.

Renderer interface facts (signatures, vertex layout, flags) and the widescreen
view technique come from
[gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT) and from
reading the original DLLs. The implementation is our own.

## Status

Single-player works: menus, levels, sound and music, the intro movie,
keyboard and controllers, and widescreen. Multiplayer (DirectPlay) is not
supported.

## Credits

- **GTA2** by DMA Design, published by Rockstar Games. GTA2 is © Rockstar
  Games / Take-Two Interactive. This project is not affiliated with or
  endorsed by them, and it contains none of their code or data.
- [xboxrecomp](https://github.com/MattSG/xboxrecomp) (MIT) is the base of the
  x86→C pipeline in `tools/`. Its licence is in `tools/xboxrecomp/LICENSE`.
- [gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT) supplied
  research on the renderer interface and the widescreen view technique.
- [Capstone](https://www.capstone-engine.org/) (BSD) does the disassembly,
  at build time only.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (BSD-3-Clause) and
  its [ncnn-vulkan port](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan)
  (MIT) power the optional texture packs. They are downloaded at build time
  and not redistributed here.
- [FFmpeg](https://ffmpeg.org/) converts the intro movie once at setup, if
  you have it installed.

## License

The code in this repository is MIT licensed; see `LICENSE`. Vendored code
keeps its own licence (`tools/xboxrecomp/LICENSE`). The game and its data
are not covered and are not included.
