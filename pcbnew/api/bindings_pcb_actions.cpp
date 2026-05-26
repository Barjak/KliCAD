/*
 * KliCAD subsystem binding: PCB-editor TOOL_ACTION runner.
 *
 * Pybind11 analogue of the typed-RPC RunAction command implemented on
 * API_HANDLER_PCB — but reachable directly from run_python without
 * needing a proto schema.  Mirror of eeschema/api/bindings_sch_actions.cpp,
 * targeting PCB_EDIT_FRAME instead of SCH_EDIT_FRAME.
 *
 *   from kipy import KiCad
 *   k = KiCad()
 *   r = k.run_python("""
 *   import klicad_native_pcb_actions as pa
 *   print(pa.run_action('pcbnew.InteractiveSelection.clearSelection'))
 *   """)
 *
 * CAVEAT (same as the RunAction proto warning): TOOL_ACTIONs are
 * specifically NOT an API.  Action names may change as code is refactored.
 * For low-level prototyping only.
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer
 * runs after py::initialize_interpreter, and PyImport_AppendInittab
 * refuses post-init.
 *
 * Frame discovery: we cannot dynamic_cast<PCB_EDIT_FRAME*> across the
 * kiface boundary (typeinfo lives in _pcbnew.kiface.bundle), so we
 * identify via EDA_BASE_FRAME::GetFrameType() == FRAME_PCB_EDITOR and
 * then static_cast.  Same caveat as bindings_pcb_state.cpp.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>

#include <tool/action_manager.h>
#include <tool/tool_action.h>
#include <tool/tool_manager.h>

#include <pcb_edit_frame.h>

#include <wx/toplevel.h>
#include <wx/window.h>

#include <map>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Per-TU helper name to avoid ODR clashes with the matching helpers in
// bindings_pcb_state.cpp (find_live_kiway_for_pcb_state,
// find_pcb_edit_frame_for_state, require_pcb_edit_frame),
// bindings_footprint_editor.cpp, common/api/bindings_*.cpp etc.
KIWAY* find_live_kiway_for_pcb_actions()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
        {
            if( holder->HasKiway() )
                return &holder->Kiway();
        }
    }
    return nullptr;
}


// Walk wxTopLevelWindows for a PCB_EDIT_FRAME (FRAME_PCB_EDITOR).  Use
// EDA_BASE_FRAME::GetFrameType() to identify and static_cast — we
// cannot dynamic_cast<PCB_EDIT_FRAME*> safely across the kiface
// boundary.  Mirrors find_pcb_edit_frame_for_state in bindings_pcb_state.cpp.
PCB_EDIT_FRAME* find_pcb_edit_frame_for_actions()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_PCB_EDITOR )
            return static_cast<PCB_EDIT_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live PCB_EDIT_FRAME, spawning pcbnew if necessary.  Throws
// on any failure so the caller doesn't have to null-check.
PCB_EDIT_FRAME* require_pcb_edit_frame_for_actions()
{
    if( PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_actions() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_pcb_actions();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(pcb_actions needs to spawn the pcbnew frame)" );
    }

    // Player(FRAME_PCB_EDITOR, true) creates the frame if missing.
    KIWAY_PLAYER* player = kiway->Player( FRAME_PCB_EDITOR, true );

    if( !player )
    {
        throw std::runtime_error(
            "failed to spawn PCB_EDIT_FRAME (kiway->Player returned null)" );
    }

    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_actions();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain PCB_EDIT_FRAME after "
            "KIWAY::Player(FRAME_PCB_EDITOR, true)" );
    }

    return frame;
}


py::dict run_action( const std::string& name )
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame_for_actions();
    TOOL_MANAGER*   tmgr  = frame->GetToolManager();

    py::dict result;
    result[ "action" ] = name;

    if( !tmgr )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = "PCB_EDIT_FRAME has no TOOL_MANAGER";
        return result;
    }

    // Mirrors API_HANDLER_PCB::handleRunAction: RunAction returns true on
    // dispatch, false on unknown/invalid action name.
    bool dispatched = tmgr->RunAction( name, true );

    result[ "ok" ] = dispatched;
    if( !dispatched )
        result[ "error" ] = "TOOL_MANAGER::RunAction returned false "
                            "(unknown action or no tool accepted it)";
    return result;
}


py::list list_actions()
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame_for_actions();
    TOOL_MANAGER*   tmgr  = frame->GetToolManager();
    py::list out;

    if( !tmgr )
        return out;

    ACTION_MANAGER* amgr = tmgr->GetActionManager();
    if( !amgr )
        return out;

    // ACTION_MANAGER::GetActions() returns the full registered-action map.
    // Note: TOOL_ACTIONs are global (static initializers), so this list
    // includes SCH and other-subsystem actions as well — not just PCB ones.
    // Names are still useful for filtering with e.g. .startswith('pcbnew.').
    for( const auto& [name, action] : amgr->GetActions() )
        out.append( name );

    return out;
}

} // anon


// Registered at kiface-load time by pcbnew/api/klicad_kiface_register.cpp.
// PYBIND11_EMBEDDED_MODULE can't be used here: its static initializer would
// run when the pcbnew kiface is dlopen'd, which happens AFTER
// KICAD_API_SERVER::Start has already called py::initialize_interpreter —
// PyImport_AppendInittab refuses post-init.  The register-on-load pattern
// adds the module to sys.modules at runtime via the Python C API instead.
void klicad_register_pcb_actions_bindings( py::module_& m )
{
    m.doc() = "KliCAD PCB-editor TOOL_ACTION runner.  Pybind11 "
              "analogue of the typed-RPC RunAction command on "
              "API_HANDLER_PCB, targeting PCB_EDIT_FRAME.\n\n"
              "CAVEAT: TOOL_ACTIONs are specifically NOT an API.  Names "
              "may change as code is refactored.  For low-level "
              "prototyping only.";

    m.def( "run_action", &run_action,
           py::arg( "name" ),
           R"DOC(Run a PCB TOOL_ACTION by name on the PCB_EDIT_FRAME.

If the PCB editor isn't already open, it is spawned first via
kiway->Player(FRAME_PCB_EDITOR, true).  Then frame->GetToolManager()->RunAction(name, true)
is called.

Returns a dict with keys:
  - ok (bool):       True iff the tool manager dispatched the action
  - action (str):    echo of the requested action name
  - error (str):     present only when ok is False

CAVEAT: TOOL_ACTIONs are specifically NOT an API.  Action names may
change as code is refactored.  For low-level prototyping only.
)DOC" );

    m.def( "list_actions", &list_actions,
           R"DOC(Return the list of all registered TOOL_ACTION names.

Sourced from frame->GetToolManager()->GetActionManager()->GetActions().
Note that TOOL_ACTION objects are global (static initialisers), so the
returned list contains every action registered in the process — not just
PCB-side actions.  Filter client-side, e.g.::

    [n for n in pa.list_actions() if n.startswith('pcbnew.')]

Spawns the PCB_EDIT_FRAME if it isn't already open.

CAVEAT: TOOL_ACTIONs are specifically NOT an API.  Names may change as
code is refactored.  For low-level prototyping only.
)DOC" );
}
