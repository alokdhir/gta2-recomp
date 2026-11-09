# gta2-recomp

Static recompilation of **GTA2** (PC, `gta2.exe` v9.6) to native arm64 macOS.
The x86 code is translated ahead of time to C; the host layer is Apple frameworks only:
Metal, AppKit, Core Audio, GameController. No SDL, no emulator.

Bring your own game: `game_files/` (gitignored) must hold a v9.6 install;
`game_files.sha1` lists the expected hashes (gta2.exe `646e1d47…`).

## Pipeline (current state)

```sh
python3 -m venv .venv && .venv/bin/pip install capstone==5.0.7   # build-time only
.venv/bin/python -m tools.pe_parser game_files/gta2.exe --json generated/gta2_analysis.json
.venv/bin/python -m tools.disasm game_files/gta2.exe --analysis-json generated/gta2_analysis.json -o generated/disasm
.venv/bin/python -m tools.func_id game_files/gta2.exe --functions generated/disasm/functions.json \
    --strings generated/disasm/strings.json --xrefs generated/disasm/xrefs.json -o generated/func_id
.venv/bin/python -m tools.recomp game_files/gta2.exe --game-name GTA2 --all --header --split 1000 \
    --gen-dir generated/gen --disasm-dir generated/disasm --func-id-dir generated/func_id -o generated/gen
scripts/compile_gen.sh
```

`tools/` vendors the x86→C pipeline from xboxrecomp (MIT; see `tools/xboxrecomp/`),
with a PE front-end added (`tools/PE_CHANGES.md`).
