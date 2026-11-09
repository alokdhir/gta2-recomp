"""Win32 PE32 front-end for the vendored xboxrecomp pipeline.

Writes the same ``<stem>_analysis.json`` that ``tools.xbe_parser --json`` writes,
so tools.disasm / tools.recomp can consume a plain Windows executable.

PE imports are reported as ``kernel_imports``: each Import Address Table slot
becomes one entry whose ``thunk_addr`` is the slot's VA (the game calls them as
``call dword ptr [slot]``). ``name`` is ``DLL!Symbol`` (or ``DLL!#ordinal``)
and ``ordinal`` is a dense index the host runtime uses to bind its own
implementation. ``kernel_thunk_addr`` is the start of the IAT.

Usage: python -I -m tools.pe_parser game_files/gta2.exe --json out.json
"""

import argparse
import json
import struct
import sys
from pathlib import Path

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000
IMAGE_SCN_CNT_CODE = 0x00000020


def parse(path):
    d = Path(path).read_bytes()
    if d[:2] != b"MZ":
        raise ValueError(f"{path}: not an MZ/PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: missing PE signature")
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    if machine != 0x14C:
        raise ValueError(f"{path}: machine {machine:#x} is not i386")
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x10B:
        raise ValueError(f"{path}: not PE32")
    entry_rva = struct.unpack_from("<I", d, opt + 16)[0]
    base = struct.unpack_from("<I", d, opt + 28)[0]
    image_size = struct.unpack_from("<I", d, opt + 56)[0]
    ndirs = struct.unpack_from("<I", d, opt + 92)[0]
    dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(ndirs)]

    sections = []
    sh = opt + opt_size
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", d, sh + 40 * i)
        chars = struct.unpack_from("<I", d, sh + 40 * i + 36)[0]
        sections.append(dict(
            name=name.rstrip(b"\0").decode("ascii", "replace"),
            rva=va, vsize=vsize or rsize, roff=roff, rsize=rsize, chars=chars))

    def r2o(rva):
        for s in sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rsize"]):
                return rva - s["rva"] + s["roff"]
        raise ValueError(f"RVA {rva:#x} not in any section")

    def cstr(off):
        return d[off:d.index(b"\0", off)].decode("ascii")

    imports = []
    imp_rva = dirs[1][0] if ndirs > 1 else 0
    p = r2o(imp_rva) if imp_rva else None
    while p is not None:
        oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", d, p)
        p += 20
        if not name_rva:
            break
        dll = cstr(r2o(name_rva))
        lookup = r2o(oft or ft)
        slot = ft
        while True:
            e = struct.unpack_from("<I", d, lookup)[0]
            if not e:
                break
            sym = f"#{e & 0xFFFF}" if e & 0x80000000 else cstr(r2o(e & 0x7FFFFFFF) + 2)
            imports.append(dict(ordinal=len(imports), name=f"{dll}!{sym}",
                                thunk_addr=f"0x{base + slot:08X}"))
            lookup += 4
            slot += 4

    iat_rva = dirs[12][0] if ndirs > 12 and dirs[12][0] else (
        int(imports[0]["thunk_addr"], 16) - base if imports else 0)

    def flagstr(c):
        return "|".join(n for n, b in (("CODE", IMAGE_SCN_CNT_CODE),
                                        ("EXEC", IMAGE_SCN_MEM_EXECUTE),
                                        ("WRITE", IMAGE_SCN_MEM_WRITE)) if c & b)

    return {
        "format": "pe32",
        "base_address": f"0x{base:08X}",
        "image_size": image_size,
        "entry_point": f"0x{base + entry_rva:08X}",
        "kernel_thunk_addr": f"0x{base + iat_rva:08X}",
        "sections": [dict(
            name=s["name"],
            virtual_addr=f"0x{base + s['rva']:08X}",
            virtual_size=s["vsize"],
            raw_addr=f"0x{s['roff']:08X}",
            raw_size=s["rsize"],
            flags=flagstr(s["chars"]),
            writable=bool(s["chars"] & IMAGE_SCN_MEM_WRITE),
            executable=bool(s["chars"] & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)),
        ) for s in sections],
        "kernel_imports": imports,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.pe_parser")
    ap.add_argument("exe")
    ap.add_argument("--json", help="output path (default <exe stem>_analysis.json beside the exe)")
    a = ap.parse_args(argv)
    info = parse(a.exe)
    out = Path(a.json) if a.json else Path(a.exe).with_name(Path(a.exe).stem + "_analysis.json")
    out.write_text(json.dumps(info, indent=1))
    print(f"{a.exe}: base {info['base_address']} entry {info['entry_point']} "
          f"{len(info['sections'])} sections, {len(info['kernel_imports'])} imports -> {out}")


if __name__ == "__main__":
    sys.exit(main())
