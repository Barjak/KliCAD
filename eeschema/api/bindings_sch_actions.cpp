/*
 * KliCAD subsystem binding: schematic-editor TOOL_ACTION runner.
 *
 * Pybind11 analogue of the typed-RPC RunAction command implemented on
 * API_HANDLER_PCB — but for SCH_EDIT_FRAME, and reachable directly from
 * run_python without needing a proto schema.
 *
 *   from kipy import KiCad
 *   k = KiCad()
 *   r = k.run_python("""
 *   import klicad_native_sch_actions as sa
 *   print(sa.run_action('eeschema.InteractiveSelection.clearSelection'))
 *   """)
 *
 * CAVEAT (same as the RunAction proto warning): TOOL_ACTIONs are
 * specifically NOT an API.  Action names may change as code is refactored.
 * For low-level prototyping only.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <tool/action_manager.h>
#include <tool/tool_action.h>
#include <tool/tool_manager.h>

#include <sch_edit_frame.h>

#include <wx/toplevel.h>
#include <wx/window.h>

#include <map>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Per-TU helper name to avoid ODR clashes with the matching helpers in
// bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_gui.cpp (find_live_kiway_for_gui),
// and all the other bindings_*.cpp TUs.
KIWAY* find_live_kiway_for_sch_actions()
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


// Walk wxTopLevelWindows looking for an already-open schematic editor.
// Mirrors the pattern in bindings_gui.cpp (which walks the same list to
// find KIWAY_HOLDERs).
SCH_EDIT_FRAME* find_sch_edit_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( SCH_EDIT_FRAME* frame = dynamic_cast<SCH_EDIT_FRAME*>( w ) )
            return frame;
    }
    return nullptr;
}


// Locate (or spawn) the schematic editor and return it.  Throws on failure
// so callers can rely on a non-null pointer.
SCH_EDIT_FRAME* ensure_sch_edit_frame()
{
    if( SCH_EDIT_FRAME* frame = find_sch_edit_frame() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_sch_actions();
    if( !kiway )
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running?" );

    // Player(FRAME_SCH, true) creates the frame if missing.
    KIWAY_PLAYER* player = kiway->Player( FRAME_SCH, true );
    if( !player )
        throw std::runtime_error(
            "failed to spawn SCH_EDIT_FRAME (kiway->Player returned null)" );

    SCH_EDIT_FRAME* frame = dynamic_cast<SCH_EDIT_FRAME*>( player );
    if( !frame )
        throw std::runtime_error(
            "kiway->Player(FRAME_SCH) returned a player that is not a "
            "SCH_EDIT_FRAME" );

    return frame;
}


py::dict run_action( const std::string& name )
{
    SCH_EDIT_FRAME* frame  = ensure_sch_edit_frame();
    TOOL_MANAGER*   tmgr   = frame->GetToolManager();

    py::dict result;
    result[ "action" ] = name;

    if( !tmgr )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = "SCH_EDIT_FRAME has no TOOL_MANAGER";
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
    SCH_EDIT_FRAME* frame = ensure_sch_edit_frame();
    TOOL_MANAGER*   tmgr  = frame->GetToolManager();
    py::list out;

    if( !tmgr )
        return out;

    ACTION_MANAGER* amgr = tmgr->GetActionManager();
    if( !amgr )
        return out;

    // ACTION_MANAGER::GetActions() returns the full registered-action map.
    // Note: TOOL_ACTIONs are global (static initializers), so this list
    // includes PCB and other-subsystem actions as well — not just SCH ones.
    // Names are still useful for filtering with e.g. .startswith('eeschema.').
    for( const auto& [name, action] : amgr->GetActions() )
        out.append( name );

    return out;
}

} // anon


// Registered at kiface-load time by eeschema/api/klicad_kiface_register.cpp.
// PYBIND11_EMBEDDED_MODULE can't be used here: its static initializer would
// run when the eeschema kiface is dlopen'd, which happens AFTER
// KICAD_API_SERVER::Start has already called py::initialize_interpreter —
// PyImport_AppendInittab refuses post-init.  The register-on-load pattern
// adds the module to sys.modules at runtime via the Python C API instead.
void klicad_register_sch_actions_bindings( py::module_& m )
{
    m.doc() = "KliCAD schematic-editor TOOL_ACTION runner.  Pybind11 "
              "analogue of the typed-RPC RunAction command on "
              "API_HANDLER_PCB, targeting SCH_EDIT_FRAME instead.\n\n"
              "CAVEAT: TOOL_ACTIONs are specifically NOT an API.  Names "
              "may change as code is refactored.  For low-level "
              "prototyping only.";

    m.def( "run_action", &run_action,
           py::arg( "name" ),
           R"DOC(Run a schematic TOOL_ACTION by name on the SCH_EDIT_FRAME.

If the schematic editor isn't already open, it is spawned first via
kiway->Player(FRAME_SCH, true).  Then frame->GetToolManager()->RunAction(name, true)
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
schematic-side actions.  Filter client-side, e.g.::

    [n for n in sa.list_actions() if n.startswith('eeschema.')]

Spawns the SCH_EDIT_FRAME if it isn't already open.

CAVEAT: TOOL_ACTIONs are specifically NOT an API.  Names may change as
code is refactored.  For low-level prototyping only.
)DOC" );
}
