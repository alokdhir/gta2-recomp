"""Collect unresolved-stub addresses from recomp_stubs_unresolved.c into a
--seed-functions JSON file (merged with what is already there). Prints how
many addresses are new."""
import json
import os
import re
import sys

stubs, out = sys.argv[1], sys.argv[2]
seen = set()
if os.path.exists(out):
    seen = {int(e["start"], 16) for e in json.load(open(out))}
found = set()
if os.path.exists(stubs):
    for m in re.finditer(r"^void sub_([0-9A-Fa-f]{8})\(void\) \{ RECOMP_UNRESOLVED_STUB", open(stubs).read(), re.M):
        found.add(int(m.group(1), 16))
new = found - seen
json.dump([{"start": f"0x{a:08X}"} for a in sorted(seen | found)], open(out, "w"), indent=1)
print(len(new))
