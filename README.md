# gta2-recomp

**GTA2** (PC, 1999) running natively on Apple Silicon Macs.

`gta2.exe` is statically recompiled: its x86 code is translated ahead of time
into C and compiled to arm64. Nothing is emulated. The Windows pieces the game
leans on are rebuilt on Apple frameworks only:

| Original | Here |
|---|---|
| `d3ddll.dll` renderer (`gbh_*`) | Metal (`runtime/src/render`) |
| `dmavideo.dll` video device (`Vid_*`) | Metal presenter (`runtime/src/video`) |
| Miles Sound System (`mss32.dll`) | Core Audio mixer, IMA ADPCM streams (`runtime/src/audio`) |
| DirectInput 8 | AppKit keyboard + GameController pads (`runtime/src/input`) |
| Win32 (kernel32, user32, advapi32) | `runtime/src/win32`: AppKit window, plist registry |

No SDL or other third-party runtime libraries.

**Bring your own game.** This repository contains no game code or data. You
need an installed copy of GTA2 **v9.6** (the Rockstar Classics release);
`gta2.exe` must have SHA-1 `646e1d479fa2aea12b3ff448fa23f6362f9e012f`.

## Build

Requirements: Apple Silicon Mac, macOS 14+, Xcode command line tools,
`brew install cmake ninja`.

```sh
scripts/setup.sh --dir "/path/to/installed/GTA2"   # copies the game in, translates, builds
open build/GTA2.app
```

Later runs of `scripts/setup.sh` only rebuild (`RETRANSLATE=1` forces a fresh
translation). `./build/gta2` runs the same app from a terminal.

## Controls

| | Keyboard | Controller |
|---|---|---|
| Move / steer | arrows | left stick, d-pad |
| Accelerate / brake | ↑ / ↓ | RT / LT |
| Attack | Ctrl | X |
| Enter / exit vehicle, menu select | Return | B |
| Jump / handbrake | Space | A |
| Previous / next weapon | Z / X | LB / RB |
| Pause | Esc | Menu |
| Full screen | ⌘F | |

Settings live in `~/Library/Application Support/GTA2/registry.plist`.

## How it is put together

- `tools/` — the x86→C pipeline, vendored from
  [xboxrecomp](https://github.com/MattSG/xboxrecomp) (MIT) with a Win32 PE
  front-end and lifter fixes; see `tools/PE_CHANGES.md`.
- `scripts/translate.sh` — PE parse → disassemble → identify → lift to
  `generated/` (gitignored). Indirect-call targets the disassembler missed are
  recorded at run time in `generated/icall_seeds.json` and picked up by the
  next translation.
- `runtime/src/overrides.c` — hand-written replacements for individual game
  functions (`void sub_XXXXXXXX(void)`), excluded from translation.
- `runtime/` — guest memory (32-bit address space at an offset), the import
  binder and the native modules listed above.

Renderer interface facts (signatures, vertex layout, flags) come from
[gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT) and from
reading the original DLLs; the implementation is our own.

## Debugging switches

| Variable | Effect |
|---|---|
| `GTA2_LOG=file` | runtime log (default stderr) |
| `GTA2_KEYS="3000:return,5000:up"` | scripted key presses (ms after start) |
| `GTA2_CAPTURE=out.png GTA2_CAPTURE_FRAME=n` | save presented frame *n* |
| `GTA2_DUMP_TILES=1` / `GTA2_DUMP_FRAME=n` | log every primitive of one scene |
| `GTA2_TRACE_AUDIO=1` | sample/stream starts, mixer level every 2 s |
| `GTA2_HEADLESS=1` | message boxes go to the log only |
| `GTA2_STRICT=1` | abort on any unimplemented import |

A crash prints the guest function it happened in and the guest call chain.

## Status

Boots, menus, gameplay, sound, keyboard and controllers work. Not yet:
Bink intro movies (skipped), multiplayer (DirectPlay), and night-level
lighting is implemented but not yet verified in-game.
