#!/bin/sh
# Regenerate generated/ from game_files/gta2.exe (pe_parser -> disasm -> func_id -> recomp -> imports).
set -eu
cd "$(dirname "$0")/.."
PY=.venv/bin/python
EXE=game_files/gta2.exe
G=generated
mkdir -p $G
$PY -m tools.pe_parser $EXE --json $G/gta2_analysis.json
# Direct call/jump targets the disassembler misses become empty stubs in
# recomp_stubs_unresolved.c and silently do nothing at run time (e.g. the
# static constructor that fills the sprite-size table). Feed them back as
# seeds and translate again until none are left (at most 3 passes).
# Indirect-call targets can only be found at run time: the game records misses
# in generated/icall_seeds.json; the known ones are kept in scripts/icall_seeds.json.
for pass in 1 2 3; do
    SEEDS="--seed-functions scripts/icall_seeds.json" # indirect-call targets found by playing (shipped)
    [ -s $G/icall_seeds.json ] && SEEDS="$SEEDS --seed-functions $G/icall_seeds.json"
    [ -s $G/stub_seeds.json ] && SEEDS="$SEEDS --seed-functions $G/stub_seeds.json"
    $PY -m tools.disasm $EXE --analysis-json $G/gta2_analysis.json -o $G/disasm --force $SEEDS
    $PY -m tools.func_id $EXE --functions $G/disasm/functions.json --strings $G/disasm/strings.json \
        --xrefs $G/disasm/xrefs.json -o $G/func_id
    rm -rf $G/gen && mkdir -p $G/gen
    RECOMP="$PY -m tools.recomp $EXE --game-name GTA2 --all --gen-dir $G/gen --disasm-dir $G/disasm
        --func-id-dir $G/func_id --abi-dir $G/abi -o $G/gen --exclude-manual runtime/src/overrides.c"
    $RECOMP --split 1000   # the C sources
    $RECOMP --header       # recomp_functions.h (a separate run: --header writes only the header)
    NEW=$($PY scripts/stub_seeds.py $G/gen/recomp_stubs_unresolved.c $G/stub_seeds.json)
    echo "pass $pass: $NEW new stub seeds"
    [ "$NEW" = 0 ] && break
done
rm -rf tools/recomp/output
$PY scripts/gen_imports.py $G/gta2_analysis.json $G/imports_table.c
