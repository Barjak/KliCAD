# KliCAD

KliCAD is a local fork of [KiCad](https://kicad.org) master (10.99)
that swaps the typed-protobuf-RPC IPC paradigm for an embedded Python
interpreter exposed via pybind11.  One IPC command — `RunPython(code)`
— and one binding TU per subsystem.  Adding a new operation costs one
`m.def(...)` line.  No proto regen, no per-method C++ handler, no
downstream wrapper to ship.

This is a personal fork.  It is not an official KiCad release.  Use
upstream KiCad for production work.

## Why fork

Upstream KiCad's IPC pipeline requires per-method work in three
places: a proto schema entry, a C++ handler, and a `kipy` wrapper.
Adding "set pad shape" or "list violations as JSON" means touching
all three and shipping a new kipy release.  An audit of the open
surface area put full coverage at roughly 6–12 months of plumbing
for thirteen handlers.

KiCad upstream also removed its SWIG-based embedded-Python escape
hatch on the path to v11 — `master` (10.99) has no in-process Python
at all.  This fork puts an embedded interpreter back in, on the
pybind11 + RPC-bundle model that the FreeCAD remote-shell pattern
established, so that the wrapper "never needs to be updated again."

## What it looks like

External Python, talking to a running KliCAD instance:

```python
from kipy import KiCad

k = KiCad()
r = k.run_python("""
    import kicad_native_drc as drc
    result = drc.run('/path/to/board.kicad_pcb', severity='warning')
    result['report']['violations']
""")
print(r.result_repr)   # list of violations as a Python literal
print(r.stdout)        # anything the snippet print()'d
```

`KiCad.run_python(code) -> RunPythonResult` is the entire API.  The
result carries `ok`, `stdout`, `stderr`, `result_repr` (the repr of
the last expression), and `exception_traceback` if the snippet
raised.  Everything else is `import kicad_native_*` from inside the
snippet.

State persists across calls — `__main__` is shared, so variables
defined in one `run_python` survive into the next.

## Bindings

Each subsystem gets one `bindings_<name>.cpp` translation unit that
publishes a `kicad_native_<name>` module to the embedded interpreter.

**libkicommon-resident** (Pattern A, available immediately at
process start):

| Module | Coverage |
| --- | --- |
| `kicad_native` | smoke probes (`echo`, `version`) |
| `kicad_native_drc` | DRC run; structured violation report |
| `kicad_native_erc` | ERC run on schematics |
| `kicad_native_export_3d` | STEP / glb / brep / stl / vrml / ply / u3d / pdf |
| `kicad_native_export_drill` | Excellon + drill map |
| `kicad_native_export_gerbers` | multi-layer gerber set |
| `kicad_native_export_sch_bom` | CSV BOM with field-column selection |
| `kicad_native_export_sch_netlist` | 8 netlist formats |
| `kicad_native_export_sch_plot` | schematic plot (pdf/svg/dxf/ps) |
| `kicad_native_fp_export_svg` | per-footprint SVG |
| `kicad_native_fp_upgrade` | `.pretty` format upgrade |
| `kicad_native_gerber_diff` | gerber image diff |
| `kicad_native_gerber_export_png` | gerber → PNG raster |
| `kicad_native_gerber_info` | gerber metadata as JSON |
| `kicad_native_gui` | `show_frame(...)`, `dismiss_dialogs()`, frame inventory |
| `kicad_native_io_discovery` | enumerate the 12 SCH + 18 PCB plugin formats |
| `kicad_native_jobset` | `.kicad_jobset` load + run |
| `kicad_native_kiway_events` | KIWAY mail subscription with FIFO |
| `kicad_native_library_tables` | symbol + footprint lib-table CRUD |
| `kicad_native_pcb_calculator` | e-series, attenuator, trace-width, fusing |
| `kicad_native_pcb_import` | Altium / Eagle / CADSTAR / PADS / etc. import |
| `kicad_native_pcb_upgrade` | `.kicad_pcb` format upgrade |
| `kicad_native_project_manager` | load / save / archive projects |
| `kicad_native_render` | raytraced PNG/JPG board render |
| `kicad_native_sch_upgrade` | `.kicad_sch` format upgrade |
| `kicad_native_settings` | typed get/set on COMMON_SETTINGS by dotted path |
| `kicad_native_sym_export_svg` | per-symbol SVG |
| `kicad_native_sym_upgrade` | `.kicad_sym` format upgrade |
| `kicad_native_design_blocks` | design-block library CRUD |
| `kicad_native_local_history` | git-backed snapshot init / list / restore |
| `kicad_native_pcm` | installed-package enumeration (filesystem-walk) |

**Kiface-resident** (Pattern B, registers on first load of the
relevant editor):

| Module | Loads with | Coverage |
| --- | --- | --- |
| `kicad_native_sch_actions` | eeschema | 233 SCH `TOOL_ACTION`s |
| `kicad_native_schematic_state` | eeschema | direct `SCHEMATIC` CRUD (wire, junction, label, symbol) |
| `kicad_native_symbol_editor` | symbol_editor | `LIB_SYMBOL` CRUD |
| `kicad_native_simulator` | simulator | SPICE: tran / ac / dc / op / noise + vector readout |
| `kicad_native_annotation` | eeschema | annotate / clear / back-annotate from netlist |
| `kicad_native_pcb_actions` | pcbnew | ~363 PCB `TOOL_ACTION`s |
| `kicad_native_pcb_state` | pcbnew | direct `BOARD` CRUD (track, via, footprint) |
| `kicad_native_footprint_editor` | footprint_editor | `FOOTPRINT` CRUD |
| `kicad_native_stackup` | pcbnew | `BOARD_STACKUP` CRUD |
| `kicad_native_drc_rules` | pcbnew | custom DRC rules + length report |
| `kicad_native_3d_resolver` | pcbnew | `FILENAME_RESOLVER` search-path mgmt |
| `kicad_native_gerbview` | gerbview | load gerbers / Excellon / zip, layer mgmt |
| `kicad_native_pagelayout` | page_layout | drawing-sheet ops, `DS_DATA_MODEL` inspection |

A kiface-resident module isn't importable until the corresponding
editor frame has been spawned at least once.  Call
`kicad_native_gui.show_frame('pcb_editor')` first if you need
something from the pcbnew kiface.

## Architecture

- **Embedded interpreter** lives in `EMBEDDED_PYTHON`
  ([common/api/python_embedded.h](common/api/python_embedded.h)).
  Owned by `KICAD_API_SERVER`; init/finalize is tied to
  `Start()` / `Stop()`.  Runs in the main thread; GIL is acquired
  around each `RunPython` call.
- **IPC entry point** is `API_HANDLER_PYTHON`
  ([common/api/api_handler_python.h](common/api/api_handler_python.h)).
  Registers the single `RunPython` proto command, exec's the
  payload in shared `__main__`, and returns the structured result
  in one round trip.
- **Bindings** live in `bindings_<name>.cpp` files under
  [common/api](common/api),
  [eeschema/api](eeschema/api),
  [pcbnew/api](pcbnew/api),
  [gerbview/api](gerbview/api), and
  [pagelayout_editor/api](pagelayout_editor/api).  Pattern A files
  use `PYBIND11_EMBEDDED_MODULE` and register at static-init time.
  Pattern B files expose a
  `void klicad_register_<name>_bindings(py::module_&)` function and
  are wired into their kiface's
  `klicad_kiface_register.{h,cpp}`.
- **Proto change** is a single addition: `RunPython` +
  `RunPythonResponse` in
  [api/proto/common/commands/base_commands.proto](api/proto/common/commands/base_commands.proto).
- **kipy change** is a single method:
  `KiCad.run_python(code) -> RunPythonResult` in the matching
  [kicad-python fork](https://gitlab.com/kicad/code/kicad-python)
  branch.

For the binding contract and worked Pattern A / Pattern B examples,
see [common/api/BINDING_PATTERN.md](common/api/BINDING_PATTERN.md).

## Test suite

The kicad-python checkout ships a pytest suite
(`kicad-python/tests/`) that the fork relies on:

- `test_klicad_safety.py` — boot/protocol smoke, every binding
  module's importability, Pattern B lifecycle (module appears only
  after the kiface is loaded).
- `test_klicad_bindings.py` — per-module positive cases.
- `test_geometry.py` — kipy geometry coverage (upstream-ish).

Latest run: 141 passed, 2 xfailed (the 2 xfails are a known
`pcb_upgrade` save-rename constraint upstream).

## Build

This is a regular KiCad cmake build with two extra dependencies:

- Python 3.13 from the python.org Framework
  (`/Library/Frameworks/Python.framework/Versions/3.13`).  CMake's
  `find_package(Python3 REQUIRED COMPONENTS Development)` resolves
  to it.
- pybind11 ≥ 3.0 (Homebrew `pybind11@3.0.4`), header-only.

After modifying any `bindings_*.cpp`, rebuild `kicommon` first then
the rest (kifaces statically link `libcommon`):

```sh
cd ~/kicad-build/build
ninja kicommon
ninja
cmake --install .
```

On macOS the install step needs an ngspice symlink fixup that the
KiCad CMake doesn't reliably emit; the working pattern is in the
project memory file rather than here, since it's machine-specific.

## How to read the bindings

Two things help when adding a binding:

- Each `bindings_*.cpp` carries a docstring at the top that lists
  the module name, the functions it exposes, and any known coverage
  gaps (e.g. "S3D_CACHE is in 3d-viewer/, not bound here").  Start
  there.
- Pattern A vs Pattern B is a hard constraint, not a style choice.
  `PYBIND11_EMBEDDED_MODULE` calls `PyImport_AppendInittab`, which
  CPython rejects after `Py_Initialize` — so any binding that
  depends on a symbol unique to a kiface must be Pattern B.
  Similarly, any binding that depends on a symbol in the static
  `libcommon` (not the shared `libkicommon`) must be Pattern B;
  the bindings header
  [common/api/BINDING_PATTERN.md](common/api/BINDING_PATTERN.md)
  documents the line.

## Upstream

This fork tracks upstream KiCad master at
[gitlab.com/kicad/code/kicad](https://gitlab.com/kicad/code/kicad)
via the `upstream` remote.  All KliCAD work lives on
`feature/always-on-api-server` (the branch name predates the rename
to KliCAD).  Rebasing on upstream master happens manually and is
not on a schedule.

## License

KliCAD inherits KiCad's GPL-3.0-or-later license.  See [LICENSES/](LICENSES/).

The original upstream KiCad README is preserved at
[README.upstream.md](README.upstream.md) for reference.
