#!/bin/sh
# Compile every generated C file to an arm64 object in build/obj (parallel).
set -u
cd "$(dirname "$0")/.."
mkdir -p build/obj
find generated/gen -name '*.c' -print0 | xargs -0 -P "$(sysctl -n hw.ncpu)" -n 1 sh -c '
  b=$(basename "$1" .c)
  clang -arch arm64 -std=c11 -O1 -w -c -Iruntime/include -Igenerated/gen "$1" -o build/obj/$b.o 2> build/obj/$b.err || echo "FAIL $1"
' _
