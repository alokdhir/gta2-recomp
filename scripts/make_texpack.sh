#!/bin/sh
# Optional: build AI-upscaled texture packs for the 3D renderer (4x, Real-ESRGAN).
#
#   scripts/make_texpack.sh                 # world tiles of every level (no play needed)
#   scripts/make_texpack.sh --sprites       # plus sprites captured while playing (see below)
#   scripts/make_texpack.sh --force         # redo styles that already have a pack
#
# Tiles come straight from the game's style files. Sprites (cars, people, HUD,
# menu font) are captured from the running game, so every colour and damage
# variant the game makes is covered. To capture, play a while in the 3D
# renderer (Cmd-1) with:
#
#   GTA2_DUMP_SPRITES="$PWD/game_files/sprite_dumps" ./build/gta2
#
# then run this script with --sprites. Re-running only upscales new sprites.
#
# Packs go to game_files/textures/<style>/{tiles,sprites}; the game picks them
# up on its next start (GTA2_NO_PACK=1 ignores them). Expect roughly half an
# hour per level for tiles on an M-series Mac. Downloads Real-ESRGAN
# ncnn-vulkan (MIT, build-time only) into .cache/ on first use.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
SPRITES=0
FORCE=0
DUMP="$ROOT/game_files/sprite_dumps"
while [ $# -gt 0 ]; do
    case "$1" in
    --sprites) SPRITES=1; shift ;;
    --dump) DUMP=$2; SPRITES=1; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'make_texpack: %s\n' "$*" >&2; exit 1; }

[ -d game_files/data ] || die "no game files yet: run scripts/setup.sh --dir /path/to/GTA2 first"

say "Python environment (Pillow, numpy)"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -c 'import PIL, numpy' 2>/dev/null ||
    .venv/bin/python -m pip install --quiet --disable-pip-version-check pillow numpy
PY=".venv/bin/python -I scripts/texpack.py"

ESR_DIR="$ROOT/.cache/realesrgan"
ESR="$ESR_DIR/realesrgan-ncnn-vulkan"
ESR_URL=https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-macos.zip
ESR_SHA256=e0ad05580abfeb25f8d8fb55aaf7bedf552c375b5b4d9bd3c8d59764d2cc333a
if [ ! -x "$ESR" ]; then
    say "Downloading Real-ESRGAN ncnn-vulkan"
    mkdir -p "$ESR_DIR"
    curl -fL --progress-bar -o "$ESR_DIR/esrgan.zip" "$ESR_URL"
    GOT=$(shasum -a 256 "$ESR_DIR/esrgan.zip" | cut -d' ' -f1)
    [ "$GOT" = "$ESR_SHA256" ] || die "Real-ESRGAN download checksum mismatch ($GOT)"
    unzip -qo "$ESR_DIR/esrgan.zip" realesrgan-ncnn-vulkan 'models/realesrgan-x4plus*' -d "$ESR_DIR"
    rm "$ESR_DIR/esrgan.zip"
    chmod +x "$ESR"
fi
# upscale <in_dir> <out_dir> <model>
upscale() {
    mkdir -p "$2"
    "$ESR" -i "$1" -o "$2" -n "$3" -s 4 -m "$ESR_DIR/models" -f png > "$2.log" 2>&1 ||
        { tail -5 "$2.log"; die "Real-ESRGAN failed (log: $2.log)"; }
}

WORK="$ROOT/build/texpack"
for sty in game_files/data/*.sty; do
    style=$(basename "$sty" .sty)
    pack="game_files/textures/$style"
    if [ -d "$pack/tiles" ] && [ "$FORCE" = 0 ]; then
        echo "  $style: tiles already packed (--force to redo)"
        continue
    fi
    w="$WORK/tiles_$style"
    rm -rf "$w"
    $PY prep "$sty" "$w"
    [ -n "$(ls "$w/in" 2>/dev/null)" ] || { echo "  $style: no world tiles"; continue; }
    say "$style: upscaling $(ls "$w/in" | wc -l | tr -d ' ') tiles (slow)"
    upscale "$w/in" "$w/out" realesrgan-x4plus
    $PY finish "$sty" "$w" "$pack"
done

if [ "$SPRITES" = 1 ]; then
    [ -d "$DUMP" ] || die "no sprite captures in $DUMP (see the top of this script)"
    for d in "$DUMP"/*/; do
        style=$(basename "$d")
        pack="game_files/textures/$style"
        w="$WORK/sprites_$style"
        rm -rf "$w"
        if [ "$FORCE" = 1 ]; then $PY sprites-prep "$d" "$w" --all
        else $PY sprites-prep "$d" "$w" --all --pack "$pack"; fi
        [ -n "$(ls "$w/in" 2>/dev/null)" ] || { echo "  $style: no new sprites"; continue; }
        say "$style: upscaling $(ls "$w/in" | wc -l | tr -d ' ') sprites"
        $PY sprites-masks "$w"
        upscale "$w/in" "$w/out" realesrgan-x4plus-anime
        upscale "$w/mask_in" "$w/mask_out" realesrgan-x4plus-anime
        $PY sprites-finish "$w" "$pack"
    done
fi

say "Done"
echo "  Packs are in game_files/textures/. They are used by the 3D renderer (Cmd-1)."
