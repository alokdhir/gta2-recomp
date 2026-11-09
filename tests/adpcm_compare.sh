#!/bin/sh
# Decode an IMA ADPCM WAV with our mixer and with macOS afconvert; they must match exactly.
# Usage: tests/adpcm_compare.sh [file.wav]   (default: a game speech clip)
set -eu
cd "$(dirname "$0")/.."
F=${1:-game_files/data/Audio/Vocals/120PeopleDown.wav}
T=$(mktemp -d)
clang -O1 -w -Iruntime/src -Iruntime/include -o "$T/cmp" tests/adpcm_compare.c -framework AudioToolbox
"$T/cmp" "$F" "$T/ours.raw" >/dev/null
afconvert -f WAVE -d LEI16 "$F" "$T/ref.wav"
python3 - "$T/ours.raw" "$T/ref.wav" <<'PY'
import sys, struct
ours = open(sys.argv[1], 'rb').read()
d = open(sys.argv[2], 'rb').read(); i = d.index(b'data'); n = struct.unpack_from('<I', d, i + 4)[0]
ref = d[i + 8:i + 8 + n]
print("PASS: identical" if ours == ref else f"FAIL: ours {len(ours)} bytes, afconvert {len(ref)} bytes")
sys.exit(0 if ours == ref else 1)
PY
rm -rf "$T"
