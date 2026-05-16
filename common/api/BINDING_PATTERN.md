# KliCAD subsystem binding pattern

Reference doc for agents binding KiCad subsystems into the `kicad_native_*`
pybind11 modules. Each agent owns one subsystem (one new `bindings_*.cpp`
TU in this directory). The pattern below is the contract.

## Architecture in one paragraph

KliCAD runs an in-process Python interpreter inside KiCad's main thread,
managed by `EMBEDDED_PYTHON` (see `python_embedded.h`). The `RunPython`
IPC command runs scripted code in a shared `__main__` namespace and
returns `{ok, stdout, stderr, result_repr, exception_traceback}` in one
round trip — the **bundle paradigm**. Each KiCad subsystem is exposed
through one `PYBIND11_EMBEDDED_MODULE(kicad_native_<name>, m)` in its
own TU, compiled into `libkicommon`. Users import these modules from
inside `run_python` calls and operate on KiCad state directly.

## What's already there

- `common/api/kicad_native_module.cpp` — `kicad_native` (smoke probes:
  `echo`, `version`).
- `common/api/bindings_drc.cpp` — `kicad_native_drc.run(board_path, ...)`.
  **Canonical reference**: read this file before writing yours.
- `common/api/bindings_erc.cpp` — `kicad_native_erc.run(schematic_path, ...)`.

## The recipe

### 1. Create one new TU per subsystem

`common/api/bindings_<subsystem>.cpp`. Use snake_case for `<subsystem>`.

### 2. Skeleton

```cpp
#include <pybind11/embed.h>
#include <pybind11/stl.h>

// Subsystem-specific KiCad headers
#include <whatever_you_need.h>

// If you call any JOB_*:
#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

namespace py = pybind11;

namespace
{

// Convention: helpers anonymous-namespaced.  Function names per-subsystem
// to avoid ODR clashes with other bindings_*.cpp.

KIWAY* find_live_kiway_for_<subsys>()  // copy from bindings_drc.cpp
{
    for( wxWindow* w : wxTopLevelWindows )
        if( KIWAY_HOLDER* h = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( h->HasKiway() )
                return &h->Kiway();
    return nullptr;
}

py::object my_operation( /* typed args */ )
{
    KIWAY* kiway = find_live_kiway_for_<subsys>();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );

    // Make sure any prerequisite editor frame is up; JOB_* dispatchers
    // typically refuse to load files when the user has a project open
    // but the relevant editor isn't.  Player(FRAME_*, true) creates it.
    kiway->Player( FRAME_PCB_EDITOR, true );  // or FRAME_SCH, FRAME_SCH_VIEWER, etc.

    // Build your JOB or call your subsystem class directly.
    // ...

    // Release the GIL across long C++ work so any Python callbacks
    // (yours, or other bindings called re-entrantly) can re-acquire it.
    int exitCode;
    {
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_PCB, &myJob, &reporter );
    }

    py::dict result;
    result[ "ok" ]        = ( exitCode == 0 );
    result[ "exit_code" ] = exitCode;
    // ... add subsystem-specific fields
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( kicad_native_<subsys>, m )
{
    m.doc() = "KliCAD <subsystem> binding (calls <C++ class> under the hood). "
              "Returns structured dict; never streams.";

    m.def( "my_operation", &my_operation,
           py::arg( "first" ),
           py::arg( "second" ) = "default",
           "One-line docstring." );
}
```

### 3. Add to `common/CMakeLists.txt`

In the `if( KICAD_IPC_API )` block where `KICOMMON_SRCS` is extended,
insert your TU alphabetically:

```cmake
        api/bindings_<subsystem>.cpp
```

### 4. Build, install, restart KiCad. Test.

```python
from kipy import KiCad
k = KiCad()
r = k.run_python("""
import kicad_native_<subsystem> as sub
sub.my_operation(...)
""")
print(r.ok, r.stdout, r.result_repr)
```

## Rules

1. **Return structured data only.** Always `py::dict` or `py::list` — never
   write to stdout/stderr to communicate results (those are captured but
   meant for human-readable diagnostics, not data).

2. **One round trip per call.** No streaming, no progress callbacks. If
   you need progress on long operations, that's a per-handler push channel
   added separately; do not invent your own bidirectional protocol here.

3. **Fail explicitly with `std::runtime_error`.** pybind11 maps this to
   Python `RuntimeError` with your message. The `RunPython` handler
   captures the full traceback for the caller.

4. **Release the GIL across long C++ work.** Wrap with
   `py::gil_scoped_release` so other operations (current or future) can
   make progress.

5. **Walk `wxTopLevelWindows` to find a live `KIWAY`.** Do not assume
   the project manager is the active frame. The first KIWAY_HOLDER with
   `HasKiway() == true` is fine — they all share one KIWAY per process.

6. **Spawn editor frames if your JOB needs them.** `kiway->Player(
   FRAME_*, true )` creates the frame if missing. Use `FRAME_PCB_EDITOR`
   for board ops, `FRAME_SCH` for schematic ops, etc. See `frame_type.h`
   for the full list.

7. **Use `JOB_*` classes when they exist.** Don't reimplement DRC/ERC/
   export/etc. logic — wrap the JOB and dispatch via
   `kiway->ProcessJob(KIWAY::FACE_<face>, &job, &reporter)`. The
   `WX_STRING_REPORTER` captures status messages for inclusion in the
   returned dict.

8. **Single-name TUs.** Each binding TU defines exactly one
   `PYBIND11_EMBEDDED_MODULE`. Module name = `kicad_native_<subsys>`.

9. **No proto schemas.** That's the entire point of this paradigm. If
   you find yourself thinking "I should add a `.proto` message", you've
   slipped out of the KliCAD pattern.

10. **Don't touch other subsystems' TUs.** Cross-subsystem coordination
    is the user's job in their Python script (which can `import
    kicad_native_a; import kicad_native_b; …`).

## Priority list of subsystems to bind

From `kicad_api_audit.md` Section 4, ordered for parallel agent work:

**Tier 1** (each agent: 1 TU, mostly mechanical):
- `bindings_drc.cpp` — **done** (PCB DRC via JOB_PCB_DRC)
- `bindings_erc.cpp` — **done** (schematic ERC via JOB_SCH_ERC)
- `bindings_export_gerbers.cpp` — JOB_EXPORT_PCB_GERBERS
- `bindings_export_drill.cpp` — JOB_EXPORT_PCB_DRILL
- `bindings_export_3d.cpp` — JOB_EXPORT_PCB_3D
- `bindings_export_render.cpp` — JOB_PCB_RENDER
- `bindings_export_sch_pdf.cpp` — JOB_EXPORT_SCH_PDF
- `bindings_export_sch_bom.cpp` — JOB_EXPORT_SCH_BOM
- `bindings_export_sch_netlist.cpp` — JOB_EXPORT_SCH_NETLIST
- `bindings_gerber_diff.cpp` — JOB_GERBER_DIFF
- `bindings_pcb_upgrade.cpp` — JOB_PCB_UPGRADE
- `bindings_sch_upgrade.cpp` — JOB_SCH_UPGRADE
- `bindings_fp_upgrade.cpp` — JOB_FP_UPGRADE
- `bindings_sym_upgrade.cpp` — JOB_SYM_UPGRADE
- `bindings_fp_svg.cpp` — JOB_FP_EXPORT_SVG
- `bindings_sym_svg.cpp` — JOB_SYM_EXPORT_SVG
- `bindings_jobset.cpp` — JOBSET + JOB_REGISTRY (the umbrella runner)

**Tier 2** (gaps in existing handlers; same pattern, just direct C++):
- `bindings_board_state.cpp` — BOARD read/inspect/save without JOB
  indirection (mirrors what kipy already does typed-RPC; this gives
  the run_python path equivalent access)
- `bindings_schematic_state.cpp` — SCHEMATIC equivalent

**Tier 3** (frame-coupled but stateless: project manager, calculator,
  bitmap converter, cvpcb).

**Tier 4** (library editors: `bindings_symbol_editor.cpp`,
  `bindings_footprint_editor.cpp`, plus `bindings_library_tables.cpp`
  for the read/write surface independent of the editors).

**Tier 5** (viewers: gerber, page layout, 3D).

**Tier 6** (the simulator — state machine, ngspice integration,
  `bindings_simulator.cpp`; deserves a dedicated agent and probably
  more than one TU as it grows).

## What NOT to do

- Don't add `m.def_submodule(...)` — flat module names per TU.
- Don't import other `kicad_native_*` modules from your TU's body.
  Composition happens user-side in Python, not C++.
- Don't add Python files alongside your TU. C++ only.
- Don't write to `kicad/api/proto/`. Stay out of the proto tree.
- Don't add fields to `kipy/kicad.py`. The `run_python` method on
  `KiCad` is the only kipy entry point you ever call.
- Don't introduce threading. The IPC server already dispatches your
  call on the main thread under the GIL.
