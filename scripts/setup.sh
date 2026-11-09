#!/bin/sh
# One-command setup: check the game, translate gta2.exe to C, build GTA2.app.
#
#   scripts/setup.sh --dir "/path/to/installed/GTA2"   # first time: copy the game in
#   scripts/setup.sh                                   # later: rebuild only
#
# The game folder must contain gta2.exe v9.6 (SHA-1 below) and its data/ folder.
# Nothing from the game is ever committed: game_files/ and generated/ are gitignored.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
WANT_SHA1=646e1d479fa2aea12b3ff448fa23f6362f9e012f
SRC=""

while [ $# -gt 0 ]; do
    case "$1" in
    --dir) SRC=$2; shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'setup: %s\n' "$*" >&2; exit 1; }

say "Checking tools"
command -v clang >/dev/null || die "Xcode command line tools missing: xcode-select --install"
command -v cmake >/dev/null || die "cmake missing: brew install cmake ninja"
command -v ninja >/dev/null || die "ninja missing: brew install ninja"
command -v python3 >/dev/null || die "python3 missing"

if [ -n "$SRC" ]; then
    [ -f "$SRC/gta2.exe" ] || die "$SRC does not contain gta2.exe"
    GOT=$(shasum "$SRC/gta2.exe" | cut -d' ' -f1)
    [ "$GOT" = "$WANT_SHA1" ] || die "gta2.exe is not the supported v9.6 build (SHA-1 $GOT)"
    say "Copying game files from $SRC"
    mkdir -p game_files
    rsync -a --exclude 'unins000.*' --exclude '*.jpg' "$SRC/" game_files/
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

say "Building GTA2.app"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
ninja -C build | tail -1

say "Done"
echo "  Play:      open build/GTA2.app      (or ./build/gta2 for a terminal log)"
echo "  Settings:  ~/Library/Application Support/GTA2/registry.plist"
