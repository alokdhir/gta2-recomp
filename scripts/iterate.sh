#!/bin/sh
# translate -> build -> run once; missed indirect-call targets feed the next translate.
set -u
cd "$(dirname "$0")/.."
scripts/translate.sh > build/translate.log 2>&1 || { tail -20 build/translate.log; exit 1; }
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
ninja -C build 2>&1 | grep -E 'error|FAILED' && exit 1
GTA2_LOG=$PWD/build/run.log GTA2_SEEDS=$PWD/generated/icall_seeds.json timeout "${RUN_SECS:-20}" build/gta2 > build/run.out 2>&1
echo "exit=$?"
