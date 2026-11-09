"""Win32 PE front-end: see __main__.py. ``as_xbe_file`` lets the vendored
pipeline's XBEParser hand back a PE in the shape its callers expect."""
from types import SimpleNamespace


def as_xbe_file(path):
    from .__main__ import parse
    info = parse(path)
    h = lambda s: int(s, 16)
    secs = []
    for s in info["sections"]:
        flags = (0x1 if s["writable"] else 0) | (0x4 if s["executable"] else 0)
        secs.append(SimpleNamespace(
            name=s["name"], virtual_addr=h(s["virtual_addr"]), virtual_size=s["virtual_size"],
            raw_addr=h(s["raw_addr"]), raw_size=s["raw_size"], flags=flags))
    header = SimpleNamespace(
        base_address=h(info["base_address"]), image_size=info["image_size"],
        entry_point=h(info["entry_point"]), kernel_thunk_addr=h(info["kernel_thunk_addr"]))
    imports = [SimpleNamespace(ordinal=i["ordinal"], name=i["name"], thunk_addr=h(i["thunk_addr"]))
               for i in info["kernel_imports"]]
    return SimpleNamespace(header=header, sections=secs, kernel_imports=imports,
                           title_name=None, format="pe32")
