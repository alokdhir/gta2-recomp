# Local changes to the vendored xboxrecomp pipeline

Upstream: see xboxrecomp/README.md. Changes made so the pipeline accepts a Win32 PE32:

- `tools/pe_parser/` (new): parses PE32 headers + import table and writes the same
  `<stem>_analysis.json` as `tools.xbe_parser --json`. Each IAT slot is reported as a
  "kernel import" (`name` = `DLL!Symbol`, `ordinal` = dense index, `thunk_addr` = slot VA).
  `as_xbe_file()` returns the PE in the shape `XBEParser.parse()` callers expect.
- `xbe_parser/xbe_parser.py` `XBEParser.parse()`: MZ input is delegated to `pe_parser`.
- `disasm/loader.py` `load_image()`: accepts MZ as well as XBEH magic.
- `recomp/lifter.py` + `translator.py` (implicit-register table): added `pushaw`/`popaw` (16-bit pusha/popa; gta2.exe uses it in its rdtsc timing helper at 0x4C3850).
