#!/bin/sh
# Regenerate generated/ from game_files/gta2.exe (pe_parser -> disasm -> func_id -> recomp -> imports).
set -eu
cd "$(dirname "$0")/.."
PY=.venv/bin/python
EXE=game_files/gta2.exe
G=generated
mkdir -p $G
$PY -m tools.pe_parser $EXE --json $G/gta2_analysis.json
SEEDS=""
[ -s $G/icall_seeds.json ] && SEEDS="--seed-functions $G/icall_seeds.json"
$PY -m tools.disasm $EXE --analysis-json $G/gta2_analysis.json -o $G/disasm --force $SEEDS
$PY -m tools.func_id $EXE --functions $G/disasm/functions.json --strings $G/disasm/strings.json \
    --xrefs $G/disasm/xrefs.json -o $G/func_id
rm -rf $G/gen && mkdir -p $G/gen
RECOMP="$PY -m tools.recomp $EXE --game-name GTA2 --all --gen-dir $G/gen --disasm-dir $G/disasm
    --func-id-dir $G/func_id --abi-dir $G/abi -o $G/gen --exclude-manual runtime/src/overrides.c"
$RECOMP --split 1000   # the C sources
$RECOMP --header       # recomp_functions.h (a separate run: --header writes only the header)
rm -rf tools/recomp/output
$PY scripts/gen_imports.py $G/gta2_analysis.json $G/imports_table.c
