# KliCAD project notes — exported from the previous dev's memory

This document is the persistent memory the previous Mac-based developer
built up over the KliCAD work. It complements `HANDOFF.md` (which is the
"current state + what to do next" summary) with deeper context: paradigm
rationale, wave-by-wave history, conventions, restart/operating patterns,
and the pre-existing KiCad IPC API the fork builds on.

Read order if you're new:
1. `HANDOFF.md` — current state + open issues + suggested priorities
2. This file — context, history, conventions
3. `common/api/BINDING_PATTERN.md` — Pattern A vs Pattern B contract

Mac-specific sections are explicitly marked **(macOS-only)** so you can
skip them on Linux.

---

## Paradigm rationale

KiCad upstream's typed-IPC requires per-method work in three places
(proto schema + C++ handler + kipy wrapper). The previous-dev audit
found 13 handlers and roughly 6–12 months of work for full coverage.
KliCAD's embedded-Python approach: one `RunPython(code)` IPC command +
pybind11 bindings — each new operation costs one `m.def()` line. Mirrors
the FreeCAD remote-shell pattern, which KiCad upstream removed for v11
(no SWIG, no embedded Python in master 10.99 onwards). KliCAD re-adds it
as a local fork.

## Architecture (deeper than HANDOFF.md)

- **Embedded interpreter** — `EMBEDDED_PYTHON` in
  `common/api/python_embedded.{h,cpp}`. Owned by `KICAD_API_SERVER`;
  lifecycle tied to `Start()`/`Stop()`. Runs in KiCad's main thread.
- **IPC entry point** — `API_HANDLER_PYTHON` in
  `common/api/api_handler_python.{h,cpp}`. Registers the `RunPython`
  proto command. Acquires GIL, exec's user code in shared `__main__`,
  returns `{ok, stdout, stderr, result_repr, exception_traceback}` in one
  round trip (bundle paradigm). State persists in `__main__` across
  calls.
- **Native surface** — `PYBIND11_EMBEDDED_MODULE(kicad_native_<subsys>, ...)`
  in per-subsystem TUs under `common/api/bindings_<subsys>.cpp` (Pattern A)
  or `${kiface}/api/bindings_<subsys>.cpp` (Pattern B). Compiled into
  `libkicommon` or the kiface respectively; reachable from the embedded
  interp via `import kicad_native_<subsys>`.
- **Proto change** — `RunPython` + `RunPythonResponse` added to
  `api/proto/common/commands/base_commands.proto`. The only proto
  addition; no per-subsystem messages.
- **kipy entry** — `KiCad.run_python(code) -> RunPythonResult` added to
  the forked `kicad-python/kipy/kicad.py`. The only kipy addition; no
  per-subsystem wrappers.

## Pattern A vs Pattern B (see `common/api/BINDING_PATTERN.md`)

- **Pattern A**: libkicommon-resident, uses `PYBIND11_EMBEDDED_MODULE`.
  Static init runs at process start, before `py::initialize_interpreter`.
  Used for cross-cutting bindings that don't need kiface-specific
  symbols.
- **Pattern B**: kiface-resident, registered at `IFACE::OnKifaceStart`
  via `klicad_kiface_register.{h,cpp}`. `PYBIND11_EMBEDDED_MODULE` can't
  be used here (its static init runs post-py-init when the kiface is
  dlopen'd, which is forbidden by CPython — `PyImport_AppendInittab`
  refuses post-init). Instead each TU exposes a
  `void klicad_register_<name>_bindings(py::module_& m)` function and
  adds an entry to the kiface's register table. Module appears in
  `sys.modules` the first time the editor frame loads.

## Two structural gotchas

### libkicommon vs libcommon

Bindings that touch any symbol defined in `common/*.cpp` outside the
`KICOMMON_SRCS` list in `common/CMakeLists.txt` (i.e., in static
`libcommon`, not shared `libkicommon`) MUST be kiface-resident
(Pattern B). Linking libcommon-only symbols into libkicommon fails at
link. The `3d_resolver` binding hit this: `FILENAME_RESOLVER` is in
static libcommon, so the binding was moved to the pcbnew kiface.

### SCH_IO_MGR / PCB_IO_MGR are NOT libkicommon-reachable

Despite what `bindings_io_discovery.cpp`'s docstring may suggest
(io_discovery hand-mirrors the format enums rather than calling the
managers' Load), `pcb_io_mgr.cpp` is in static pcbcommon (pcbnew kiface
only), and `sch_io_mgr.cpp` is eeschema kiface only. Pattern A bindings
that need to parse `.kicad_pcb` / `.kicad_sch` must either go Pattern B
or do their own s-expr parsing (see `bindings_diff.cpp` for the latter).

## Wave-by-wave binding rollout

**Wave 1 — Tier 1 baseline** (20 Pattern A modules + cross-cutting GUI):

- `kicad_native` — smoke probes (echo, version)
- `kicad_native_drc` — DRC via `JOB_PCB_DRC`; structured violations
- `kicad_native_erc` — ERC via `JOB_SCH_ERC`
- `kicad_native_export_3d` — STEP/glb/brep/stl/vrml/ply/u3d/pdf
- `kicad_native_export_drill` — Excellon + drill map
- `kicad_native_export_gerbers` — multi-file gerber set
- `kicad_native_export_sch_bom` — CSV BOM with custom field columns
- `kicad_native_export_sch_netlist` — 8 formats
  (kicad/xml/orcad/cadstar/pads/spice/spice-model/allegro)
- `kicad_native_export_sch_plot` — schematic plot (pdf/svg/dxf/ps)
- `kicad_native_fp_export_svg` — per-footprint SVG
- `kicad_native_fp_upgrade` — in-place `.pretty` format upgrade
- `kicad_native_gerber_diff` — gerber image diff
- `kicad_native_gerber_export_png` — gerber → PNG raster
- `kicad_native_gerber_info` — gerber metadata as JSON
- `kicad_native_jobset` — load() + run() for `.kicad_jobset` files
- `kicad_native_pcb_import` — Altium/Eagle/CADSTAR/PADS/etc. → KiCad
- `kicad_native_pcb_upgrade` — in-place `.kicad_pcb` format upgrade
- `kicad_native_render` — raytraced PNG/JPG board render
- `kicad_native_sch_upgrade` — in-place `.kicad_sch` format upgrade
- `kicad_native_sym_export_svg` — per-symbol SVG
- `kicad_native_sym_upgrade` — in-place `.kicad_sym` format upgrade

Plus `kicad_native_gui` — `show_frame(name)` / `list_open_frames()` /
`dismiss_dialogs()`. Launch any KiCad frame programmatically by string
name. Pre-spawns required parent frame for context-dependent ones
(`FRAME_SIMULATOR` needs SCH parent). `dismiss_dialogs()` is the
in-process replacement for AppleScript on macOS — calls `EndModal/Close`
on every wxDialog.

**Wave 2** (Tier 2-4-6 deliveries):

- `kicad_native_settings` (Pattern A) — typed get/set on COMMON_SETTINGS
  via dotted JSON paths
- `kicad_native_project_manager` (Pattern A) — load/save/archive via
  SETTINGS_MANAGER + PROJECT_ARCHIVER
- `kicad_native_library_tables` (Pattern A) — symbol+footprint
  library-table CRUD
- `kicad_native_sch_actions` (eeschema Pattern B) — SCH_EDIT_FRAME
  tool-action runner; 233 actions reachable
- `kicad_native_schematic_state` (eeschema Pattern B) — SCHEMATIC
  CRUD: add_wire / add_junction / add_label / add_symbol / etc.
- `kicad_native_symbol_editor` (eeschema Pattern B) — LIB_SYMBOL CRUD
  via SYMBOL_EDIT_FRAME + LIB_SYMBOL_LIBRARY_MANAGER
- `kicad_native_simulator` (eeschema Pattern B) — SPICE binding:
  run_analysis (tran/ac/dc/op/noise), get_vector with x-axis,
  list_vectors. Workbooks + tuners deferred to wave 5
- `kicad_native_pcb_state` (pcbnew Pattern B) — BOARD CRUD:
  add_track / add_via / add_footprint, get_items_summary, list_layers
- `kicad_native_footprint_editor` (pcbnew Pattern B) — FOOTPRINT CRUD
  via FOOTPRINT_EDIT_FRAME + FOOTPRINT_LIBRARY_ADAPTER

**Wave 3:**

- `kicad_native_pcb_actions` (pcbnew Pattern B) — PCB tool-action
  runner; ~363 pcbnew actions
- `kicad_native_pcb_calculator` (Pattern A) — pure-math subset:
  e_series_closest, resistor_combination, attenuator
  (pi/tee/bridged_tee/split), trace_width_for_current, fusing_current.
  Math reimplemented inline since `pcb_calculator/` headers aren't
  libkicommon-reachable.
- `kicad_native_gerbview` (gerbview Pattern B) — file loading + layer
  management; introduces gerbview-side `klicad_kiface_register.{h,cpp}`
- `kicad_native_pagelayout` (pl_editor Pattern B) — drawing-sheet file
  ops + DS_DATA_MODEL inspection; introduces pl_editor-side register
- `kicad_native_kiway_events` (Pattern A) — KIWAY mail event
  subscription with thread-safe FIFO + observer wxEvtHandler
- `kicad_native_io_discovery` (Pattern A) — enumerate 12 SCH + 18 PCB
  upstream file-format plugins with extension lookup

**Wave 4:**

- `kicad_native_pcm` (Pattern A) — filesystem-walk variant: lists
  installed packages by scanning user's `3rdparty/packages3d` cache;
  avoids the `kicad/pcm/dialogs/*` deps that blocked the earlier
  attempt
- `kicad_native_design_blocks` (Pattern A) — design-block library
  CRUD via DESIGN_BLOCK_IO
- `kicad_native_local_history` (Pattern A) — git-backed snapshot
  init/list/get/restore against the PROJECT-resident history dir
- `kicad_native_annotation` (eeschema Pattern B) — annotate /
  clear_annotation / back_annotate_from_netlist
- `kicad_native_stackup` (pcbnew Pattern B) — BOARD_STACKUP CRUD
- `kicad_native_drc_rules` (pcbnew Pattern B) — custom DRC-rule file
  get/set/validate + get_length_report
- `kicad_native_3d_resolver` (pcbnew Pattern B) — FILENAME_RESOLVER
  search-path mgmt and alias resolution. Originally targeted at
  libkicommon but FILENAME_RESOLVER lives in static libcommon, so
  moved to pcbnew kiface

**Wave 5:**

- `kicad_native_diff` (Pattern A) — UUID-keyed semantic diff of
  `.kicad_sch` / `.kicad_pcb` files via a self-contained s-expression
  parser (works without PROJECT context; importable at process start)
- `kicad_native_bitmap2component` (Pattern A) — **metadata only**;
  `convert()` is NotImplementedError pending potrace +
  BITMAPCONV_INFO link surgery into libkicommon
- `kicad_native_hierarchy` (eeschema Pattern B) — multi-sheet nav:
  list_sheets, get/set_current_sheet, push/pop_sheet, walk_hierarchy,
  count_instances, list_sheet_pins, update_page_numbers
- `kicad_native_sim_advanced` (eeschema Pattern B) — SPICE depth:
  workbook save/load, parameter_sweep, tuners
  (add/list/set_value; **`remove_tuner` stubbed** because
  `SIMULATOR_FRAME_UI::RemoveTuner` is private), measure, fft, sim
  parameter get/set
- `kicad_native_3d_viewer` (pcbnew Pattern B) — EDA_3D_VIEWER_FRAME
  control: take_snapshot, set_view_preset, set_render_mode, etc.
  **`take_snapshot` resizes the live canvas** (upstream's headless
  `captureScreenshot(wxSize)` is private)
- `kicad_native_netinfo` (pcbnew Pattern B) — BOARD net analysis.
  *include `connectivity/connectivity_algo.h`* — forward-decl in
  `connectivity_data.h` is insufficient for `vector<CN_EDGE>::~vector`
- `kicad_native_sync` (pcbnew Pattern B) — schematic→PCB update.
  Uses `KIWAY::ExpressMail(FRAME_SCH, MAIL_SCH_GET_NETLIST, ...)` +
  `BOARD_NETLIST_UPDATER`
- `kicad_native_cvpcb` (cvpcb Pattern B) — component→footprint
  association. Adds new `cvpcb/api/klicad_kiface_register` infra
  (mirrors gerbview wave-3 pattern). **`auto_associate` is headless-
  only no-op** (operates on the frame's private `m_netlist`, not the
  binding's NETLIST cache); fix by routing through
  MAIL_ASSIGN_FOOTPRINTS round-trip.

**Bug fix in wave 5:** Footprint editor 0-libs. Fixed by calling
`AsyncLoad+BlockUntilLoaded` at the top of
`FOOTPRINT_EDIT_FRAME::initLibraryTree()`, mirroring
`panel_footprint_chooser.cpp`'s pattern.

**Python façade in wave 5:** `kipy.klicad.KliCAD` — 13 proxies
(DRC/ERC/Schematic/Board/Simulator/Library [with nested
SymbolEditor/FootprintEditor] /Export/GUI/Project/Gerber/Jobset).
~125 wrapped methods; users skip the run_python boilerplate.

**Post-wave-5 fixes:**

- `pcb_state.open_board(path)` / `save_board(path='')` and
  `schematic_state.open_schematic(path)` / `save_schematic()` —
  end-to-end blocker: `load_project` loads the project file but
  doesn't push the `.kicad_pcb` / `.kicad_sch` into the editor frame.
  These wrap `PCB_EDIT_FRAME::OpenProjectFiles` + `SavePcbFile` and
  the sch equivalents.
- **3D viewer kiface real root cause**: pcbnew's
  `IFACE::CreateKiWindow` (pcbnew.cpp:253) has NO case for
  `FRAME_PCB_DISPLAY3D`. Upstream KiCad spawns the 3D viewer as a
  child of PCB_EDIT_FRAME via
  `PCB_BASE_FRAME::CreateAndShow3D_Frame` (pcb_base_frame.cpp:678),
  NOT through the kiway frame factory. `KIWAY::Player(FRAME_PCB_DISPLAY3D, true)`
  always returns nullptr. Fixed by routing through
  CreateAndShow3D_Frame in
  `bindings_3d_viewer.cpp::require_3d_viewer_frame()`.
  `gui.show_frame('3d_viewer')` can't fix this in libkicommon
  (PCB_BASE_FRAME lives in pcbnew kiface), so it returns a clear
  redirect to `kicad_native_3d_viewer`.
- Four sch-state authoring primitives: `set_symbol_value`,
  `set_symbol_field`, `set_symbol_rotation`,
  `get_symbol_pin_position`. Also fixed pre-existing IU scale bug
  in the schematic binding (was using PCB scale = 1e6 instead of
  SCH scale = 1e4 = 100nm/IU). See HANDOFF.md for the open IU
  refactor task.
- **run_action arg dispatcher**: punted. KiCad's TOOL_ACTION
  parameter system uses `ki::any` with type-specific retrieval at
  the receiving end ("type must match exactly"). Most actions take
  pointer types (`EDA_ITEM*`, `FRAME_T`, etc.) that don't have safe
  Python equivalents.

## Total binding count after wave 5: 31 Pattern A + 19 Pattern B = 50.

---

# Conventions

## Push to GitHub after every commit

After every `git commit`, push to the GitHub remote without being
asked. Do not batch multiple commits before pushing — push each commit
(or each logical group landed in a single response) right after
creating it.

**Why:** The user wants the GitHub copy to be the authoritative mirror
at all times; sitting on local commits creates a drift window where
the GitHub fork looks stale.

**How to apply:**

- For the KliCAD kicad fork, push to `origin` (`github.com/Barjak/KliCAD`)
  on the current branch.
- For the kicad-python fork, push to `origin`
  (`github.com/Barjak/klicad-python`).
- Both repos have the same remote layout: `origin` = user's GitHub,
  `upstream` = the original gitlab.
- For other repos whose `origin` is *not* a GitHub remote, don't
  silently push to the non-GitHub remote. Surface and ask.
- The "ask before destructive actions" rule still wins. Don't
  force-push, don't push to `main`/`master`, don't push commits that
  include files the user hasn't seen, without confirming.

## Never PR upstream KiCad

KliCAD is a personal fork. Never open a pull/merge request against
upstream KiCad (`gitlab.com/kicad/code/kicad`) on the user's behalf.

**Why:** The fork's design choices (embedded Python + per-subsystem
pybind11 bindings) deliberately diverge from upstream's typed-IPC
roadmap, which removed embedded Python on the path to v11. Upstream
wouldn't accept the change. The user wants the fork to stay private
to their GitHub mirror.

**How to apply:**

- Pushes go to `origin` (`github.com/Barjak/KliCAD`). Never push or PR
  to the `upstream` remote (gitlab) or any other KiCad channel.
- Applies to kicad-python too.
- Cherry-picking individual fixes upstream-ward is also out of scope
  unless the user asks for it explicitly.

---

# How to use (external Python client)

```python
from kipy import KiCad

k = KiCad()                   # auto-finds /tmp/kicad/api.sock
r = k.run_python("""
import kicad_native_drc as drc
result = drc.run('/path/to/board.kicad_pcb', severity='warning')
result['report']['violations']
""")
print(r.result_repr)
```

That's the entire API. Everything else is `import kicad_native_*` from
inside the run_python body.

Or via the façade:

```python
from kipy.klicad import KliCAD

k = KliCAD()
violations = k.drc.run('/path/to/board.kicad_pcb')
```

---

# The pre-existing KiCad IPC API (what KliCAD builds on)

The fork is on branch `feature/always-on-api-server` because before
the pybind11 work there were three patches that make the IPC API
server start unconditionally on every launch:

1. `common/api/api_server.cpp` — constructor skips the
   `m_Api.enable_server` check and always calls `Start()`.
2. `common/eda_base_frame.cpp` — `CommonSettingsChanged` no longer
   stops the server when the (unused) preference is false; only
   re-starts if `Running()` returns false.
3. `eeschema/CMakeLists.txt` — install resolved `libngspice.0.dylib`
   (REALPATH) plus a local versionless symlink, so `install(DIRECTORY)`
   doesn't drag in broken `../Cellar/...` symlinks that trip
   RefixupMacOS (macOS-only fix).

## Socket + auth

- **Socket:** `/tmp/kicad/api.sock` (`wxFileName.AppendDir("kicad").SetFullName("api.sock")`
  in `api_server.cpp::Start()`). Lock file `api.lock` in same dir.
  Unix-domain, not TCP.
- **Auth:** `KICAD_API_TOKEN` env var exists but server only rejects
  *mismatches* — empty token passes
  (`api_server.cpp:254`: `if( !empty() && mismatch )`). External
  clients don't need the token; kipy uses env var if set, otherwise
  omits.
- **Wire:** NNG req/rep transport, protobuf payloads. Schemas in
  `api/proto/`.

## What's accessible vs what isn't

The project-manager process owns the API server. Each editor frame
(PCB, schematic) registers its own handler when opened
(`pcb_edit_frame.cpp:530`, `sch_edit_frame.cpp:460`). So:

- Project manager alone → only `API_HANDLER_COMMON` operations.
- PCB editor open → board handler available.
- Schematic editor open → schematic handler available.
- **All KliCAD `kicad_native_*` bindings** are accessible the moment
  the embedded interpreter is up (Pattern A) or the moment the kiface
  is loaded (Pattern B).

## Two CLI surfaces

1. **`kicad-cli`** — one-shot batch tool. Subcommands: `pcb {drc,
   export, render, upgrade, import}`, `sch {erc, export, upgrade}`,
   `sym`, `fp`, `gerber`, `jobset`. Loads a file, does the thing,
   exits. No live attach.
2. **`kicad-cli api-server [PROJECT_OR_FILE] [--socket PATH]`** —
   headless API server, no GUI. Pre-loads a project if given. Use
   when you want API access without the project-manager GUI running.

## Caveats

- **API server is on whenever any KiCad process is running.**
  Localhost-only Unix socket; not exposed off-machine.
- **One socket per machine.** If two KiCad processes try to launch
  concurrently, the second uses `api-<pid>.sock` — kipy's default
  discovery won't find it; pass `socket_path=` explicitly.

---

# Build setup

## **(macOS-only)** Previous-dev setup details

The previous Mac-based dev's setup, summarized for reference. Most of
this won't transfer to Linux but the configure flags are largely
portable.

### wxWidgets 3.3 (not 3.2.x)

KiCad master uses `wxTreeCtrl::SetStateImages(wxVector<wxBitmapBundle>)`
which only exists in wxWidgets 3.3+. Stock 3.2.5 fails with
`error: use of undeclared identifier 'SetStateImages'` in
`kicad/project_tree.cpp:158`. On Linux you may need to build wx 3.3
from source if your distro only packages 3.2.

The Mac build had:
- Source: `~/kicad-build/wxwidgets-src/` (cloned at v3.3.2 tag)
- Build: `~/kicad-build/wx-build/`
- Install: `~/kicad-build/wx-install/`
- Configured shared, with `wxUSE_WEBVIEW=ON` (required by KiCad's
  `find_package(wxWidgets ... webview REQUIRED)`)
- **macOS-only**: `wxUSE_ZLIB=sys -DwxUSE_REGEX=sys -DwxUSE_EXPAT=sys`
  because the bundled zlib/regex/expat collide with macOS Sequoia
  SDK's `<stdio.h>` (`#define fdopen(...) NULL`) and won't compile

### Cmake configure flags (largely portable)

```
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_INSTALL_PREFIX=...
-DCMAKE_PREFIX_PATH="$WX_INSTALL_DIR;/usr/local"
-DwxWidgets_CONFIG_EXECUTABLE=$WX_INSTALL_DIR/bin/wx-config
-DKICAD_BUILD_QA_TESTS=OFF -DKICAD_USE_PCH=ON -DKICAD_BUILD_I18N=ON
-DKICAD_INSTALL_DEMOS=ON -DKICAD_USE_SENTRY=OFF -DKICAD_UPDATE_CHECK=OFF
-DKICAD_SIGNAL_INTEGRITY=ON -DKICAD_IPC_API=ON -DKICAD_IDF_TOOLS=ON
-DPython_EXECUTABLE=...
-DNGSPICE_LIBRARY=/usr/lib/x86_64-linux-gnu/libngspice.so   # adapt for Linux
-DNGSPICE_INCLUDE_DIR=/usr/include
-DNGSPICE_DLL=/usr/lib/x86_64-linux-gnu/libngspice.so
```

`KICAD_IPC_API=ON` is required for the embedded-Python bindings to
build. Don't turn it off.

### Mac-specific dependencies

- pybind11 was Homebrew's `pybind11@3.0.4`; header-only,
  `find_package(pybind11 REQUIRED CONFIG)` picks it up.
- Python interpreter was Python.org Framework 3.13 at
  `/Library/Frameworks/Python.framework/Versions/3.13`. CMake's
  `find_package(Python3 REQUIRED COMPONENTS Development)` resolves to
  this. Same interpreter used by external kipy client.
- Brew deps: `ngspice libgit2 glm nng unixodbc autoconf automake
  libtool texinfo`

### **(macOS-only)** `cmake --install` is required, not just `ninja`

The build-tree `KiCad.app` is incomplete — it's missing
`Contents/SharedSupport/resources/images.tar.gz`, wx libs in
`Contents/Frameworks/`, schemas, templates. Launching the build-tree
app produces "can't open file 'images.tar.gz' (error 2)" and missing
icons. **After every `ninja`, run `cmake --install`** to populate the
install bundle, which:

1. Copies `images.tar.gz` to `Contents/SharedSupport/resources/`
2. Copies all wx dylibs into `Contents/Frameworks/`
3. Rewrites `LC_RPATH` from absolute path to `@executable_path/../Frameworks`
4. Installs translation .mo files, schemas, templates, plugins

The Desktop launcher must point at the install bundle, NOT the
build-tree app.

On Linux: `ninja install` (with sudo if installing to a system path)
should be enough — there's no app-bundle relocation step.

### **(macOS-only)** ngspice symlink fix

`cmake --install` reliably copies `libngspice.0.dylib` to
`Contents/Frameworks/` but the install code block that should also
create the unversioned `libngspice.dylib` symlink (which the
simulator's dlopen needs) isn't firing. Manual fix after each install:

```sh
cd ~/kicad-build/install/KiCad.app/Contents/Frameworks
ln -sf libngspice.0.dylib libngspice.dylib
cd ../PlugIns/sim
ln -sf libngspice.0.dylib libngspice.dylib
```

Plus the codemodels subdir at `Contents/PlugIns/sim/ngspice` is
another broken `../Cellar/...` symlink the previous dev fixed manually
with `ln -sf /usr/local/Cellar/libngspice/.../lib/ngspice ngspice`.

On Linux this is moot — the dylib naming convention is different.

## Workflow

- After modifying any `bindings_*.cpp`, rebuild kicommon first
  (`ninja kicommon`), then full ninja (kifaces statically link
  libcommon), then install.
- `kicad_native` and friends only load inside the embedded interp.
  Never `import kicad_native_*` from your external Python — they
  don't exist as pip-installable modules.
- **Stale `/tmp/kicad/api.sock`**: if KiCad crashes, the socket file
  lingers as an orphan. The "wait for socket" launch loop will
  falsely report ready on the stale file. Always
  `rm -f /tmp/kicad/api.sock /tmp/kicad/api.lock` before relaunching
  after a crash.

---

# Operating patterns

## Restart policy

**(macOS-only)** Authorized for autonomous graceful restart after
rebuild: `osascript -e 'tell application "KiCad" to quit'` → wait →
`open -n …`. SIGKILL only on explicit user request. Pause if a dialog
blocks the quit (osascript returns -128) — that means KiCad is
showing something the user needs to address.

On Linux: `pkill -TERM kicad` followed by relaunch. No Gatekeeper
holds; no codesigning rituals.

## Disable autosave

To prevent the periodic "Board outline is malformed / File
'.kicad-save-NNNN-N' couldn't be removed" modal that blocks
AppleScript quit (and adds noise to any test run): Preferences →
Common → Editing options → Auto save → 0 (disabled). Or set
programmatically via the `settings` binding:

```python
k.run_python("import kicad_native_settings as s; s.set('system.local_history_debounce', 0)")
```

## Dialog dismissal

In-process: from any test or script,
`kicad_native_gui.dismiss_dialogs()` calls EndModal on every wxDialog.
The `tests/conftest.py::auto_dismiss_dialogs` fixture clears stray
popups after each test that uses it.

The one case this can't handle is dialogs already up *before* a test
starts — modal dialogs hold the main thread, blocking the IPC request
that would call dismiss. Fall back to either manual user click, or
(macOS) grant Accessibility permission so osascript System Events can
keystroke Return.

## **(macOS-only)** Gatekeeper

KiCad bundle is ad-hoc signed (`codesign --force --sign -`). macOS
shows a "first run" approval dialog the very first time the user
launches the freshly-re-signed binary; subsequent launches of the same
bundle ID hit a cached decision and start clean.

If KiCad gets force-killed while a Gatekeeper popup is pending and
relaunched, the kernel queues another pending popup that blocks the
new instance's main thread at the first `open()` syscall — which from
`sample` looks identical to a code-path hang in
`LIBRARY_MANAGER::LoadGlobalTables → wxDir::Open → open$NOCANCEL`.
Don't be fooled: that's the Gatekeeper hold, not a real library scan
bug. Rapid `pkill -9` + relaunch loops accumulate queued dialogs
invisibly.

---

# kicad-python (kipy) fork details

- Regenerated `kipy/proto/` from the patched `.proto` files. **Must
  use protoc 29** (from Homebrew protobuf@29 on Mac; pin to 29.x on
  Linux) to match Python protobuf 5.x runtime; system protoc 33 emits
  gencode the runtime rejects.
- `RunPythonResult` dataclass + `KiCad.run_python(code)` method
  added to `kipy/kicad.py`.
- `kipy/klicad/` Python façade — 13 proxies wrapping ~125 of the most
  useful binding methods.

External Python client setup: `PYTHONPATH=/path/to/kicad-python
python3 your_script.py`. **Don't `pip install -e .` the fork** — the
setup.py's protoc regen fails on the mypy plugin (harmless but blocks
install).

---

# Standard library setup

The previous-dev workstation has the upstream KiCad library repos
cloned to `~/kicad-build/libs/`:

- `kicad-symbols` (285M, 222 libs)
- `kicad-footprints` (197M, 155 libs)
- `kicad-packages3D` (3.7G, 3D STEP models)
- `kicad-templates` (8.5M, starter templates)

Global lib tables installed from the upstream repos to
`~/Library/Preferences/kicad/10.99/{sym,fp}-lib-table` (macOS) or
`~/.config/kicad/10.99/...` (Linux); canonical KiCad URIs use
`${KICAD10_SYMBOL_DIR}` / `${KICAD10_FOOTPRINT_DIR}` etc., which are
wired via the `settings` binding to point at the cloned dirs.

Don't bundle these — they're big and live outside the repo. Clone
them yourself on Linux if you need to exercise demos that depend on
standard parts.
