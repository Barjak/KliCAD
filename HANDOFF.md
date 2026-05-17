# KliCAD handoff — 2026-05-17

You're picking up KliCAD development on Linux. This doc covers everything I
think you need: project shape, current state, recent crashes (with full
stack traces and the scripts that triggered them), conventions to keep, and
a prioritized work queue.

The previous developer (Mac) hit a fresh schematic-switch crash mid-demo
and stopped before forcing through. Don't repeat that — read the "Open
crashes" section before touching `open_schematic` or any project-switch
code path.

---

## What KliCAD is

A personal fork of KiCad master (10.99) that swaps the typed-protobuf-RPC
IPC paradigm for an embedded Python interpreter exposed via pybind11. **One
IPC command — `RunPython(code)` — and one binding TU per subsystem.**
Adding a new operation costs one `m.def(...)` line; no proto regen, no C++
handler, no kipy wrapper.

This deliberately diverges from upstream's roadmap (which removed embedded
Python on the path to v11). **Do not PR to gitlab.com/kicad/code/kicad.**

## Two repos

- `Barjak/KliCAD` — the C++ fork (this repo).
  - Branch you're picking up: `feature/always-on-api-server`
  - `upstream` remote: `gitlab.com/kicad/code/kicad.git` (do not push)
- `Barjak/klicad-python` — kipy fork + tests + `kipy.klicad` Python façade.
  - Branch: `feature/klicad-bindings`
  - `upstream`: `gitlab.com/kicad/code/kicad-python.git` (do not push)

Both repos have `origin = github.com/Barjak/...`. Push to origin after
every commit; don't batch.

## Bindings inventory (~50 modules)

**Pattern A** (libkicommon, available at process start):
`kicad_native`, `_design_blocks`, `_diff`, `_drc`, `_erc`, `_export_3d`,
`_export_drill`, `_export_gerbers`, `_export_sch_bom`, `_export_sch_netlist`,
`_export_sch_plot`, `_fp_export_svg`, `_fp_upgrade`, `_gerber_diff`,
`_gerber_export_png`, `_gerber_info`, `_gui`, `_io_discovery`, `_jobset`,
`_kiway_events`, `_library_tables`, `_local_history`, `_pcb_calculator`,
`_pcb_import`, `_pcb_upgrade`, `_pcm` (stub), `_bitmap2component` (stub),
`_project_manager`, `_render`, `_sch_upgrade`, `_settings`, `_sym_export_svg`,
`_sym_upgrade`.

**Pattern B** (kiface-resident, registered on first kiface load):
- eeschema: `_annotation`, `_hierarchy`, `_sch_actions`, `_schematic_state`,
  `_sim_advanced`, `_simulator`, `_symbol_editor`
- pcbnew: `_3d_resolver`, `_3d_viewer`, `_drc_rules`, `_footprint_editor`,
  `_netinfo`, `_pcb_actions`, `_pcb_state`, `_stackup`, `_sync`
- cvpcb: `_cvpcb`
- gerbview: `_gerbview`
- pl_editor: `_pagelayout`

Read `common/api/BINDING_PATTERN.md` before touching either pattern. It
documents why `PYBIND11_EMBEDDED_MODULE` can't be used in kifaces (post-init
inittab limitation) and the register-on-load workaround.

## Architecture

- **`common/api/python_embedded.{h,cpp}`** — `EMBEDDED_PYTHON` class.
  Owned by `KICAD_API_SERVER`; lifecycle tied to `Start/Stop`. Runs in
  KiCad's main thread; GIL acquired around each `RunPython` call.
- **`common/api/api_handler_python.{h,cpp}`** — registers the `RunPython`
  proto command, exec's payload in shared `__main__`, returns
  `{ok, stdout, stderr, result_repr, exception_traceback}` in one round trip.
- **`common/api/kicad_native_module.cpp`** — smoke probes (`echo`, `version`).
- **`${KIFACE}/api/klicad_kiface_register.{h,cpp}`** — Pattern B
  registration per kiface. Each binding TU exposes
  `void klicad_register_<name>_bindings(py::module_&)`, listed in the
  register file's `entries` vector. `IFACE::OnKifaceStart` calls
  `klicad_register_${kiface}_bindings()` under `#if defined(KICAD_IPC_API)`.

## Open crashes — READ BEFORE TOUCHING `open_schematic`/`open_board`

### Crash #1: BOARD::ClearProject same-path re-open (FIXED 2026-05-16)

```
0  BOARD::ClearProject() + 29
1  PCB_EDIT_FRAME::SetBoard(BOARD*, bool, PROGRESS_REPORTER*) + 140
2  PCB_EDIT_FRAME::OpenProjectFiles(...) + 5811
3  pcb_state_open_board(...) + 410
```

Root cause: `OpenProjectFiles` skips its up-front `ClearProject +
UnloadProject + LoadProject` cleanup when the requested path matches the
currently-loaded project. Then `BOARD_LOADER::Load` eagerly calls
`SetProject(&Prj())` on the new board, reassigning
`project.m_BoardSettings` from the old board's settings to the new
board's. `SetBoard` then calls `oldBoard->ClearProject()` which tries to
`ReleaseNestedSettings(project.m_BoardSettings)` — stale pointer → SIGSEGV.

Fix: same-path no-op in `pcb_state.open_board` and
`schematic_state.open_schematic`. Commit `2ac4ac7d2b`.

### Crash #2: SCHEMATIC::SetProject on schematic-switch (OPEN — INVESTIGATE)

```
0  SCHEMATIC::SetProject(PROJECT*) + 36
1  SCH_EDIT_FRAME::SetSchematic(SCHEMATIC*) + 48
2  SCH_EDIT_FRAME::OpenProjectFiles(...) + 11542
3  sch_state_open_schematic(...) + 1220
```

Triggered by: calling `sch_state.open_schematic('/tmp/new_project/x.kicad_sch')`
when eeschema is already loaded with a DIFFERENT schematic (e.g. the
switch fixture). The crash is at SetProject offset 36 with a high-address
freed-pointer pattern (`0x0000000280000090`).

Likely sister of crash #1 but on the schematic side. Same kind of
dangling-state issue when switching projects mid-session. The same-path
no-op guard doesn't catch this case because the paths legitimately differ.

**Suggested investigation path:**
1. Read `eeschema/sch_edit_frame.cpp::OpenProjectFiles` around line ~11500.
   What does it do to the *current* `SCHEMATIC` before constructing the
   new one?
2. Compare against `pcbnew/files.cpp::OpenProjectFiles` (which has the
   `setProject` shortcut + explicit ClearProject path).
3. Find what `SCHEMATIC::SetProject(nullptr)` does — does it null
   `m_project` cleanly, or release something that's about to be freed
   anyway?
4. Look at `SCHEMATIC::~SCHEMATIC` to see what it tries to release.
5. The previous-developer fix on the PCB side was a same-path no-op, but
   that's defensive. The schematic side needs a real fix because users
   legitimately want to switch projects.

**Repro script** (run with KliCAD freshly launched on the switch fixture):

```python
from kipy.klicad import KliCAD
import os, shutil
k = KliCAD()
PRJ = "/tmp/klicad-crash2-repro"
shutil.rmtree(PRJ, ignore_errors=True)
os.makedirs(PRJ)
with open(f"{PRJ}/x.kicad_pro", 'w') as f:
    f.write('{"meta":{"filename":"x.kicad_pro","version":3}}\n')
with open(f"{PRJ}/x.kicad_sch", 'w') as f:
    f.write('(kicad_sch (version 20250114) (generator "klicad")\n'
            ' (generator_version "10.99") (uuid "00000000-0000-0000-0000-000000000001")\n'
            ' (paper "A4") (lib_symbols) (sheet_instances (path "/" (page "1"))))\n')
k.run_python(f"""
import kicad_native_project_manager as pm
import kicad_native_schematic_state as ss
pm.load_project({PRJ + '/x.kicad_pro'!r})
ss.open_schematic({PRJ + '/x.kicad_sch'!r})   # <-- crashes here
""")
```

## Open bugs (not crashes)

### IU scale fragility

Schematic IU is `SCH_IU_PER_MM = 1e4` (100nm per IU). PCB IU is
`PCB_IU_PER_MM = 1e6` (1nm per IU). The two are non-fungible.

**Three known places to fix** — all should be refactored to use the
canonical `schIUScale` / `pcbIUScale` accessors from
`include/base_units.h` (`pcbIUScale.mmToIU(mm)`,
`schIUScale.IUToMillimeter(iu)`) instead of hardcoded constants:

1. `pcbnew/api/bindings_pcb_state.cpp` — uses `MM_TO_NM_PCB = 1e6`
   (correct value, wrong way to express it).
2. `eeschema/api/bindings_schematic_state.cpp` — uses `MM_TO_IU = 1e4`
   (correct value, wrong way; just renamed from the old buggy
   `MM_TO_NM = 1e6` in commit `06ac615c21`).
3. **`eeschema/api/bindings_hierarchy.cpp:376`** — has
   `IU_TO_MM = 1.0 / 1e6` which is the PCB scale, but the file is on the
   schematic side. **This is a real bug**: every pin/sheet position
   returned by the hierarchy binding is reported 100× too small. Probably
   nobody noticed because hierarchy.list_sheets results use the values
   for display only.

Also check `pcbnew/api/bindings_drc_rules.cpp` and `bindings_netinfo.cpp`
(both use `1e6` correctly for PCB) — refactor for symmetry.

## Known stubs / gaps (smaller than the crashes)

- `bitmap2component.convert()` — stub. Needs potrace + bitmap2component.cpp
  link-pulled into libkicommon. ~3-line CMakeLists edit but adds a runtime
  dep. The metadata helpers (`list_supported_formats`,
  `list_layer_choices`) work.
- `sim_advanced.remove_tuner` — raises `RuntimeError`. Upstream's
  `SIMULATOR_FRAME_UI::RemoveTuner` is private. Needs a public forwarder.
- `cvpcb.auto_associate` — no-op for headless sessions; operates on
  CVPCB_MAINFRAME's private `m_netlist`. Fix by routing through a
  `MAIL_ASSIGN_FOOTPRINTS` KIWAY round-trip.
- `run_action(name)` doesn't accept `args` dict — KiCad's TOOL_ACTION
  parameter system uses strongly-typed `ki::any` retrieval; most useful
  actions take pointer types that don't safely map to Python. Documented
  as punted; reopen if user needs it.
- **No PCB-side authoring equivalents** of the wave-just-landed sch
  primitives. Need `pcb_state.set_footprint_value`,
  `set_footprint_rotation`, `get_pad_position` for end-to-end programmatic
  PCB authoring. ~80 lines, modeled on `bindings_schematic_state.cpp`.
- 3D viewer kiface — works via the spawn-path fix (commit `d7651ea2e3`)
  but only after PCB_EDIT_FRAME is up. Can't be spawned standalone via
  `gui.show_frame('3d_viewer')` because upstream's IFACE::CreateKiWindow
  has no case for FRAME_PCB_DISPLAY3D.

## Test suite

Run KiCad first (sock at `/tmp/kicad/api.sock`), then:

```
cd kicad-python
pytest tests/                          # full: 172 passed last run
pytest tests/test_gui_smoke.py -v -s   # visual walkthrough w/ screenshots
```

Screenshots dropped to `$TMPDIR/klicad-gui-smoke/`. Last full run before
the schematic-switch crash: 172 passed, 0 xfailed.

The `loaded_switch_project` session fixture in `tests/test_gui_smoke.py`
loads `tests/fixtures/switch_project/` (now bundled in the repo, formerly
hardcoded to a Mac-only path).

## macOS-specific gotchas (probably not your problem on Linux)

These are macOS pain points; calling them out so you know what's
platform-specific:

- **Gatekeeper**: macOS holds the first launch of any freshly-signed
  binary pending a user "Open" click. From `sample`, this looks identical
  to `LIBRARY_MANAGER::LoadGlobalTables → open$NOCANCEL` — don't be
  fooled.
- **ngspice symlinks**: `cmake --install` reliably copies
  `libngspice.0.dylib` but doesn't always create the unversioned symlink
  `libngspice.dylib` the simulator dlopen's. Manual fix scripted in
  memory.
- **stale `/tmp/kicad/api.sock`**: If KiCad crashes the socket file
  lingers. Always `rm -f` before relaunch.

On Linux: just `cmake --install`, run. wxWidgets 3.3 has to be available;
pybind11 ≥ 3.0 header-only; Python 3.13 dev headers.

## Conventions

- **Push to GitHub after every commit.** No batching. Both repos have
  `origin = Barjak/...` GitHub. Don't push to `upstream` (gitlab).
- **Per-binding commits.** Each new binding gets its own commit with a
  clear message: what it binds, why, any documented caveats. Look at
  `git log --oneline` to see the style.
- **Co-author trailer**: commits include
  `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`
  — feel free to use your own model identifier.
- **Test before committing**: `pytest tests/test_klicad_safety.py` is the
  fast (~10s) sanity. If you've touched a Pattern B binding, also
  `tests/test_klicad_bindings.py`.

## Suggested priority order

1. **Investigate crash #2** (SCHEMATIC::SetProject) — figure out what
   `OpenProjectFiles` does to the old SCHEMATIC and where the freed
   pointer comes from. This blocks programmatic project authoring.
2. **Refactor IU scaling to use `schIUScale` / `pcbIUScale`** — kills the
   class of bugs that just bit the previous dev. Includes fixing
   `bindings_hierarchy.cpp:376`.
3. **Add PCB authoring primitives**: `pcb_state.set_footprint_value`,
   `set_footprint_rotation`, `get_pad_position`. Modeled directly on the
   new schematic_state primitives in commit `06ac615c21`.
4. **Resume the LED-oscillator demo** (see "Demo in progress" below) on
   a stable foundation.

## Demo in progress

The previous dev was attempting to programmatically:
1. Create a fresh project at `/tmp/klicad-led-osc/`
2. Author a 2-transistor astable multivibrator (Q1+Q2 cross-coupled,
   R1/R2/R3/R4, C1/C2, LED) with two solder pads for +5V/GND input
3. Run a SPICE transient against it

Approach used: place every component with `add_symbol`, set values with
`set_symbol_value`, identify pin world-coordinates with
`get_symbol_pin_position`, drop net labels at each pin instead of routing
wires (so KiCad's netlist generator connects by name). For simulation,
push a hand-authored SPICE deck directly to ngspice via
`simulator.load_netlist` — bypasses the schematic-to-SPICE pipeline (which
would need every component's `Sim_Device`/`Sim_Model`/`Sim_Pins` fields
set, which `set_symbol_field` can do but adds complexity).

This works as a design; the schematic-switch crash blocked it because
the demo started by loading a fresh project. Once crash #2 is fixed (or
worked around by doing the demo against the existing fixture project),
the rest should just run.

## Memory file (Claude Code's persistent memory across sessions)

The previous dev kept a memory file at
`~/.claude/projects/-Users-shopnew/memory/klicad_fork.md`. It has
overlapping content with this handoff (architecture notes, build setup,
known gotchas, wave-by-wave inventory). Worth a read but not the source
of truth for status — this HANDOFF.md is.

## Quick-start (Linux)

```
# Build
cmake -B build -GNinja -DKICAD_IPC_API=ON
ninja -C build
sudo ninja -C build install

# Run
kicad &  # wait for /tmp/kicad/api.sock to appear

# Test the bindings work
cd kicad-python
pytest tests/test_klicad_safety.py

# If the safety suite passes you're in business.
```

Good luck.
