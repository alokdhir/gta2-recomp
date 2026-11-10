#!/bin/sh
# One-command setup: check the game, translate gta2.exe to C, build GTA2.app.
#
#   scripts/setup.sh --dir "/path/to/installed/GTA2"   # first time: copy the game in
#   scripts/setup.sh                                   # later: rebuild only
#
# The game folder must contain gta2.exe v9.6 (SHA-1 below) and its data/ folder.
# Nothing from the game is ever committed: game_files/ and generated/ are gitignored.
# RETRANSLATE=1 forces a fresh translation (needed after editing overrides.c).
# Optional texture packs: scripts/make_texpack.sh (see README).
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
WANT_SHA1=646e1d479fa2aea12b3ff448fa23f6362f9e012f
SRC=""

while [ $# -gt 0 ]; do
    case "$1" in
    --dir) SRC=$2; shift 2 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'setup: %s\n' "$*" >&2; exit 1; }

say "Checking tools"
[ "$(uname -m)" = arm64 ] || die "needs an Apple Silicon Mac (arm64)"
OSV=$(sw_vers -productVersion | cut -d. -f1)
[ "$OSV" -ge 14 ] || die "needs macOS 14 or later (this is $(sw_vers -productVersion))"
command -v clang >/dev/null || die "Xcode command line tools missing: xcode-select --install"
command -v cmake >/dev/null || die "cmake missing: brew install cmake ninja"
command -v ninja >/dev/null || die "ninja missing: brew install ninja"
command -v python3 >/dev/null || die "python3 missing"

if [ -n "$SRC" ]; then
    [ -d "$SRC" ] || die "$SRC is not a folder"
    EXE=$(find "$SRC" -maxdepth 1 -iname gta2.exe | head -1)
    [ -n "$EXE" ] || die "$SRC does not contain gta2.exe (point --dir at the installed GTA2 folder)"
    GOT=$(shasum "$EXE" | cut -d' ' -f1)
    [ "$GOT" = "$WANT_SHA1" ] || die "gta2.exe is not the supported v9.6 build (SHA-1 $GOT)"
    say "Copying game files from $SRC"
    mkdir -p game_files
    rsync -a --exclude 'unins000.*' --exclude '*.jpg' "$SRC/" game_files/
    [ "$(basename "$EXE")" = gta2.exe ] || mv "game_files/$(basename "$EXE")" game_files/gta2.exe
fi
[ -f game_files/gta2.exe ] || die "no game files yet: run with --dir /path/to/GTA2"
GOT=$(shasum game_files/gta2.exe | cut -d' ' -f1)
[ "$GOT" = "$WANT_SHA1" ] || die "game_files/gta2.exe is not v9.6 (SHA-1 $GOT)"

say "Python environment (Capstone 5.0.7, build-time only)"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -c 'import capstone, sys; sys.exit(capstone.__version__ != "5.0.7")' 2>/dev/null ||
    .venv/bin/python -m pip install --quiet --disable-pip-version-check capstone==5.0.7

if [ ! -f generated/gen/recomp_functions.h ] || [ "${RETRANSLATE:-0}" = 1 ]; then
    say "Translating gta2.exe to C (about a minute)"
    mkdir -p build
    scripts/translate.sh > build/translate.log 2>&1 || { tail -20 build/translate.log; die "translation failed"; }
fi

if command -v ffmpeg >/dev/null; then
    find game_files/data -iname '*.bik' | while read -r bik; do
        mp4="${bik%.*}.mp4"
        [ -f "$mp4" ] && continue
        say "Converting $(basename "$bik") to MP4 (intro movie)"
        ffmpeg -hide_banner -loglevel error -y -i "$bik" -c:v libx264 -crf 16 -preset slow \
            -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart "$mp4"
    done
else
    echo "  (no ffmpeg: the intro movie will be skipped; brew install ffmpeg and run setup again to get it)"
fi

say "Building GTA2.app"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
ninja -C build | tail -1

say "Done"
echo "  Play:      open build/GTA2.app      (or ./build/gta2 for a terminal log)"
echo "  Textures:  scripts/make_texpack.sh   (optional AI-upscaled packs for the 3D renderer)"
echo "  Settings:  ~/Library/Application Support/GTA2/registry.plist"
