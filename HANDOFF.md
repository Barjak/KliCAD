# KliCAD handoff — 2026-05-17 (orientation only)

> ⚠ **Current state is in
> `~/projects/klicad-python/docs/plans/STATUS.md`** (last updated
> 2026-05-28).  Read that first if you're picking up active work.
> This file remains useful for project shape, architecture, binding
> patterns, and conventions — but the "Open crashes" / "Recent crashes"
> sections below are stale: every crash listed there has been fixed,
> and several new crash fixes landed in 2026-05-27/28 that aren't
> recorded here.  See STATUS.md "Crash fixes" subsection for the
> current list.  The "Phase B is preliminary / blocked by
> UnsavedChangesDialog" note below is also stale — the full
> multi-channel + ratsnest arc (R0–C.7) has landed on
> `loop/integration-7` and `to_schematic` round-trips cleanly.

You're picking up KliCAD development. This doc covers project shape, current
state, recent crashes (with full stack traces and repro scripts), conventions
to keep, and a prioritized work queue.

Mac dev session 2 added significant work that needs Linux validation —
see the **"Canonical circuit description"** section below for the new
`kipy.klicad.circuit` module. Phase A (Python DSL + SPICE deck + sim
verification) is tested and green. Phase B (`.to_kicad_sch()` schematic
generator) is preliminary — code is in but the Mac session was blocked
by an UnsavedChangesDialog before the end-to-end test could run.

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

## Schematic-export ground-naming gap (NEW — 2026-05-18)

Discovered while building a buck-converter demo with the canonical
circuit DSL.  The DSL places `power:GND` symbols for ground nets
*and* drops text labels on every pin including ground pins.  KiCad's
SPICE exporter uses label-text > power-port-identity precedence, so a
net with both gets named `"GND"` instead of mapped to SPICE node `0`.
ngspice then has no ground reference, the current-probe wrapper turns
the source into a near-short, and the sim aborts with a singular
matrix at ~1e-19s.

Three plausible fixes, ranked:

1. **Per-pin power ports**: place a `power:GND` symbol AT each ground
   pin instead of one symbol far away connected by labels.  Removes
   the label/power-port conflict entirely.  Cost: more symbols on the
   sheet; needs `_layout_positions` to be aware of which pins need
   power-port stubs.
2. **Skip labels on power/ground pins + route wires from each power
   pin to the single power-port symbol**.  Cleaner sheet but requires
   wire-routing (currently nonexistent — label-based connectivity is
   the deliberate "don't route" choice).
3. **Use the literal text `"0"` as the label for ground nets**.  May
   not be a valid KiCad label name.  Untested.

The bug shows up on the buck demo because the LED-oscillator demo's
ground happened to be tested with a different code path (canonical
deck via `load_netlist`, not `run_analysis(from_schematic=True)`) —
the from-schematic run on LED osc that I logged earlier as "3612
samples" must have happened *before* `to_kicad_sch` saved the labels
to disk, while the in-memory schematic state was still being authored.

## `export_sch_netlist` returns empty when not freshly-authored

The `kicad_native_export_sch_netlist.run(...)` binding returns
`{'ok': True, 'output_paths': [...]}` and writes a netlist file
that contains only `.title KiCad schematic\n.end\n` when the project's
schematic was loaded from disk in this KliCAD process rather than
authored via the API mid-session.  Confirmed: a schematic file with
22 placed symbols on disk produces an empty netlist via the API, but
the same schematic exports correctly when the API authored it earlier
in the same process.

Likely cause: the export job consults the in-memory `SCHEMATIC` from
the `SCH_EDIT_FRAME`, but the frame's load-from-disk path isn't fully
done by the time `kiway->Player(FRAME_SCH, true)` returns inside
`run_export_sch_netlist`.  Or the export job is reading
`Schematic()->Items()` before the schematic IO is plumbed in.

Workaround: keep the schematic editor on a fresh project that was
authored via `to_kicad_sch()` in this same session.  Don't try to
export a schematic that was only loaded.

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

- **Push DIRECTLY to the feature branch.** No PRs needed; the previous
  dev was pushing straight to `feature/always-on-api-server` (the main
  KliCAD work branch) and to `feature/klicad-bindings` on the Python
  side. PRs are optional, not required. The repo previously had an
  inherited-from-upstream lockdown bot that auto-closed PRs; it's been
  removed, but the direct-push pattern is still the path of least
  friction. If you do open a PR, you're welcome to self-merge.
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

1. **Validate Phase B of `kipy.klicad.circuit`** (see next section). Code
   is in but live KliCAD round-trip wasn't tested on Mac (UnsavedChangesDialog
   blocked the main thread mid-test). On Linux there's no Gatekeeper, and
   you can scriptally dismiss-or-bypass dialogs more cleanly.
2. **Investigate crash #2** (SCHEMATIC::SetProject) — figure out what
   `OpenProjectFiles` does to the old SCHEMATIC and where the freed
   pointer comes from. Blocks programmatic project authoring; right now
   the workaround is "launch KliCAD with the project on cmd line, don't
   switch projects mid-session."
3. **Refactor IU scaling to use `schIUScale` / `pcbIUScale`** — kills the
   class of bugs that bit the previous dev. The Linux dev's commit
   `f3558c5b8e` already addressed the worst case (schematic IU rounding
   for net labels); `bindings_hierarchy.cpp:376` still has the wrong
   divisor.
4. **Add PCB authoring primitives**: `pcb_state.set_footprint_value`,
   `set_footprint_rotation`, `get_pad_position`. Modeled directly on the
   schematic_state primitives in commit `06ac615c21`.

## Canonical circuit description (`kipy.klicad.circuit`) — NEW

Single-source-of-truth pipeline for circuits. A Python `Circuit` object
describes the electrical reality (parts, nets, ICs, analyses); both the
SPICE deck and the KiCad schematic derive from it. No more parallel
hand-authored schematic + hand-authored deck that can drift.

Lives in [klicad-python:kipy/klicad/circuit/](https://github.com/Barjak/klicad-python/tree/feature/klicad-bindings/kipy/klicad/circuit)
on branch `feature/klicad-bindings`.

### Phase A (DONE, 8/8 tests green, commit `96351de`)
- `_part.py`: `Part` base + `R/C/L/D/LED/NPN/PNP/V/I` subclasses with
  SPICE-semantic pin names (`c/b/e`, `a/k`, `+/-`).
- `_analyses.py`: `Tran/Ac/Dc/Op/Noise` typed dataclasses + `Control`
  escape hatch for raw .control bodies.
- `_circuit.py`: `Circuit` container; validation at `.add()` time;
  `to_dict()`/`from_dict()` for round-trip; auto-detect power/ground
  nets by name.
- `_spice.py`: `to_spice_deck()` — generates a complete ngspice deck.
  Wraps analyses in `.control` block so they actually execute (raw
  `.tran` is parse-time-only). Rewrites GND→0 at emit.
- `models/standard.lib`: bundled starter library (2N3904, 2N3906,
  BC547, BC557, 1N4148, 1N4001, LED, 2N7000, generic fallbacks).
- `tests/test_circuit.py`: 8 tests covering build, validation, round-
  trip, deck well-formedness, and live ngspice oscillation verification.

### Phase B (DONE — verified end-to-end on Mac through GUI Play button)
- `_kicad_sch.py`: `to_kicad_sch(path)` — uses the pybind primitives
  (`add_symbol`, `set_symbol_value`, `set_symbol_field`,
  `get_symbol_pin_position`, `add_label`) to author into KliCAD's live
  schematic editor and save. Sets `Sim.Library` + `Sim.Name` on each
  model-bearing part.
- `examples/led_oscillator_canonical.py`: end-to-end demo. `--setup-only`
  writes project files; launch KliCAD on them; re-run without
  `--setup-only` to drive the full demo.

**Verified end-to-end flow on Mac (commits `b527a7e` + `4cbd476`):**

1. Python canonical → `to_spice_deck()` → ngspice runs → V(NL) swings 4.96V
   rail-to-rail, V(NR) 3.40V clamped by LED, 3469 samples.
2. Python canonical → `to_kicad_sch()` writes the live schematic with
   10 parts, 22 pin labels, Sim.Library + Sim.Name fields set.
3. **KiCad's OWN schematic→SPICE export** (the same code path the GUI
   Play button uses) produces a clean deck with `.include models.lib`,
   correct BJT C-B-E pin order, real model names (no more
   `Q1.unknown`). Identical sim result to (1).
4. GUI Play button → `run_analysis(kind='tran', step='1ms', stop='3s', uic=True)`
   → 3561 samples → identical oscillation visible in the simulator
   plot canvas.

**Lessons baked into the code:**

1. `Sim.Name` is the model-name field KiCad 10 expects, NOT `Sim.Model`
   (KiCad 7 era).  Without it the netlist generator emits
   `<ref>.unknown`.  Same field absence also prevented Sim.Pins
   consultation, so pin order came out wrong (KiCad pin-num order
   instead of SPICE C-B-E).
2. Empty `(sym_lib_table)` is the right project sym-lib-table to write
   on KiCad 10 — let the global table resolve standard libs.  The
   env var is `${KICAD10_SYMBOL_DIR}` (versioned) and the libs are
   in exploded `.kicad_symdir/` layout; project-scoped tables with
   the old `${KICAD_SYMBOL_DIR}` / single-file paths break.
3. `to_kicad_sch()` refuses to run on a non-empty schematic (returns
   a clear error pointing at the workaround) — appending would
   double-place every symbol.  Real fix is Phase C diff/apply.
4. The `ModalAnnotate` dialog fires from `SCH_EDIT_FRAME::ReadyToNetlist`
   when CheckAnnotate finds something not-fully-annotated, even
   though refs are unique.  Blocks IPC because the modal loop holds
   the main thread; `click_dialog_button` CAN'T dismiss it (the IPC
   handler is queued behind the modal).  Workaround: pre-call
   `kicad_native_annotation.annotate(scope='all')` before any sim
   spawn; proper fix is a thread-local "embedded interp active"
   flag that switches `ModalAnnotate` to silent annotation.
5. Net names in KiCad-generated netlists have a leading `/`
   (hierarchical naming).  `get_vector('v(nl)')` won't match; use
   `get_vector('v(/nl)')`.

### Roadmap beyond Phase B (D / E / F / G)

The Mac session 3 working agreement: the canonical Python `Circuit` is
now the source of truth, and we own how it derives `.kicad_sch`. That
unlocks an auto-layout pipeline we couldn't approach when the .kicad_sch
was an artifact users hand-edited. Phases:

- **Phase C — incremental update** (`Circuit.from_kicad_sch()` +
  `Circuit.diff()` + `CircuitDiff.apply()`). Preserves position data
  across spec changes. Round-trip property test as oracle. Detailed
  below.
- **Phase D — hierarchical block decomposition.** Auto-detect natural
  subcircuits in a netgraph; emit each as its own sheet with hierarchical
  pins for the boundary nets. Algorithm: **Louvain community detection**
  on the netgraph (parts = nodes, nets approximated as binary edges).
  Threshold: pull out a block when `|external_nets| / |parts| < ~0.3`
  AND size ≥ ~3. Power/ground treated as implicit (excluded from the
  netgraph, available everywhere via labels) so they don't pull the
  whole circuit into one giant module.

  **TODO upgrade**: move from Louvain to **hypergraph partitioning**
  (KaHyPar, hMETIS, PaToH) — circuit nets are hyperedges, not binary
  edges, and real EDA tools use multilevel hypergraph partitioning for
  this exact problem. Louvain is the pragmatic starting point; the
  decision rule and threshold should port over.
- **Phase E — Sugiyama placement per sheet.** Replaces the current
  "horizontal row at y=ROW_Y" grid in `_kicad_sch.py`. Four passes:
  cycle removal → layer assignment → crossing minimization → coord
  assignment. Layers map naturally to signal-flow direction. Power
  rails at the top of each sheet, ground at the bottom, signals flow
  left-to-right.
- **Phase F — A\* routing for explicit wires** (OPTIONAL). Label-based
  wiring stays the default; this is opt-in via per-Circuit setting for
  cases where the user wants visible signal-flow runs (`R1 → C1` next
  to each other). A* on the Hanan grid with crossing penalty. Steiner
  trees for multi-pin nets.
- **Phase G — invariant-preserving transform system.** Once placement
  is non-trivial, refactor tools (move-symbol, straighten-wire,
  label↔wire conversion) need a correctness guard. Architecture: every
  transform is `(Schematic, params) → Either[Schematic, RejectReason]`
  that calls `from_kicad_sch()` before-and-after; rejects if the
  canonical slice changes. Cheap (O(n) canonical extraction); strong
  guarantee.

**Concrete first step for Phase D**: implement `Circuit.partition() →
list[Block]` on top of `networkx.algorithms.community.louvain_communities`
or `python-igraph`'s equivalent. No schematic generation yet — just
expose the decomposition so we can eyeball whether the heuristic picks
sensible blocks for test designs (LED osc → 1 block; instrumentation
amp → ~3; MCU board → 8+). Verify on real designs before wiring
to_kicad_sch into the sheet-emit path.

### Phase C (NEXT — design sketched, not implemented)

Single-source-of-truth pipeline now works, but `to_kicad_sch()` is
full-rebuild only.  Iterating on a Circuit object should NOT tear down
+ rebuild — would lose schematic position data, manual cosmetic
touches, ratsnest assignments, sheet layout.  Incremental update
preserves the non-canonical data while propagating canonical changes.

Pieces needed:

1. **`Circuit.from_kicad_sch(path)`** — reverse converter, extracts
   the canonical slice (refs, values, models, pin→net mappings, ICs,
   analyses). Skips positions, fonts, rotations, sheet layout,
   ratsnest, user-added text. Probably direct s-exp parsing like
   `bindings_diff.cpp` — no KiCad needed, faster than pybind.
2. **`Circuit.diff(other) -> CircuitDiff`** — operation list keyed by
   ref designator. Ops: `AddPart`, `RemovePart`, `SetValue`,
   `SetModel`, `RewireConnection`, `SetIC`, `AddAnalysis`,
   `RemoveAnalysis`.
3. **`CircuitDiff.apply(target_sch, kicad=)`** — executes ops against
   live schematic. Some need new bindings: `delete_symbol`,
   `find_items_near(x, y)` for finding labels by position to rewire.
4. **Round-trip property test**: assert `from_kicad_sch(incremental_apply)
   == from_kicad_sch(full_rebuild)` produces the same canonical slice.
   Positions won't match (incremental preserves, full-rebuild grids
   fresh) but the canonical slice will. Strong correctness oracle
   without graph-isomorphism-NP machinery — we control the labeling.

Naming convention going forward:
- `to_kicad_sch_full(path)` — tear-down + rebuild (current
  implementation; oracle for property tests).
- `to_kicad_sch_incremental(path)` — diff existing + apply; default
  for users.

### Suggested validation flow on Linux
```bash
# Pull both repos to latest
(cd kicad-build/kicad        && git pull origin feature/always-on-api-server)
(cd kicad-build/kicad-python && git pull origin feature/klicad-bindings)

# Phase A pure tests (no KiCad needed):
cd kicad-build/kicad-python
PYTHONPATH=. python -m pytest tests/test_circuit.py::test_build_circuit_no_errors \
   tests/test_circuit.py::test_roundtrip_to_dict_from_dict \
   tests/test_circuit.py::test_spice_deck_well_formed -v

# Then with KliCAD running:
kicad &
PYTHONPATH=. python -m pytest tests/test_circuit.py -v

# Phase B live round-trip:
PYTHONPATH=. python examples/led_oscillator_canonical.py --setup-only
kicad /tmp/klicad-led-osc-canonical/led-osc.kicad_pro &
# wait for socket, then:
PYTHONPATH=. python examples/led_oscillator_canonical.py
# Then click PLAY in the simulator window — expect oscillation matching
# the Python-side run.
```

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

## Deeper context: `docs/PROJECT_NOTES.md`

The previous dev's persistent memory has been exported to
`docs/PROJECT_NOTES.md` in this repo. It covers:

- Paradigm rationale + architecture (deeper than the summary here)
- Wave-by-wave binding rollout history (wave 1 through wave 5 + post-5)
- The two structural gotchas (libkicommon-vs-libcommon, SCH_IO_MGR not
  libkicommon-reachable)
- Conventions (push-after-commit, never-PR-upstream)
- "How to use" — external Python client patterns + façade examples
- The pre-existing KiCad IPC API the fork builds on (always-on
  patches, socket details, two CLI surfaces)
- Build setup (Mac-specific bits clearly labeled)
- Operating patterns (restart policy, dialog dismissal, autosave)

This HANDOFF.md is the status doc; PROJECT_NOTES.md is the reference.

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
