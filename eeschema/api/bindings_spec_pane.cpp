/*
 * KliCAD eeschema spec-pane binding.  See
 * eeschema/api/klicad_kiface_register.cpp::klicad_register_eeschema_bindings()
 * for the registration site.
 *
 * The spec pane (widgets/sch_spec_pane.h) is a right-hand text editor
 * that displays the klicad-python spec authoring the current schematic
 * and re-runs it on every Enter / focus-loss.  This binding gives
 * external Python callers (and klicad-python itself, on first
 * to_schematic call) a way to point the pane at a spec file and show
 * it without needing a menu interaction.
 *
 * v1 surface area:
 *   - set_spec_path(abs_path) — load the file into the editor; empty
 *     path clears the pane.
 *   - get_spec_path() — return whatever the pane is bound to.
 *   - show(visible=True) — show / hide the pane in the AUI manager.
 *   - regen() — force a commit + regen now (same path the Enter and
 *     focus-loss handlers take).
 *
 * Cursor sync (canvas selection → editor caret; caret → flash) is
 * NOT in v1 — see widgets/sch_spec_pane.h.
 */

#include "klicad_kiface_register.h"

#include <pybind11/embed.h>

#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/string.h>
#include <wx/window.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <eda_base_frame.h>
#include <sch_edit_frame.h>
#include <widgets/sch_spec_pane.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;


namespace
{

// Each binding TU keeps its own copy of these helpers in its anonymous
// namespace (mirroring bindings_schematic_state.cpp and bindings_hierarchy.cpp);
// the helpers can't be shared because their respective TUs need internal
// linkage to avoid ODR collisions inside the same shared object.
KIWAY* find_live_kiway_for_spec_pane()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( holder->HasKiway() )
                return &holder->Kiway();
    }
    return nullptr;
}


SCH_EDIT_FRAME* find_sch_edit_frame_for_spec_pane()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( base && base->GetFrameType() == FRAME_SCH )
            return static_cast<SCH_EDIT_FRAME*>( base );
    }
    return nullptr;
}


SCH_EDIT_FRAME* require_sch_edit_frame()
{
    if( SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_spec_pane() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_spec_pane();

    if( !kiway )
        throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );

    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_spec_pane();

    if( !frame )
        throw std::runtime_error( "failed to obtain SCH_EDIT_FRAME after Player(FRAME_SCH)" );

    return frame;
}

SCH_SPEC_PANE* require_spec_pane()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCH_SPEC_PANE*  pane  = frame ? frame->GetSpecPane() : nullptr;

    if( !pane )
        throw std::runtime_error(
                "klicad_native_spec_pane: SCH_EDIT_FRAME has no spec pane "
                "(KliCAD built without spec-pane support)" );

    return pane;
}


py::dict spec_set_path( const std::string& aPath )
{
    SCH_SPEC_PANE*  pane  = require_spec_pane();
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();

    pane->SetSpecPath( wxString::FromUTF8( aPath.c_str() ) );

    // Loading a spec implies the user wants the pane visible.  Same
    // policy as upstream `File ▸ Open` — if you're loading content,
    // surface the view.
    wxAuiPaneInfo& info = frame->GetAuiManager().GetPane( pane );

    if( info.IsOk() )
    {
        info.Show( true );
        frame->GetAuiManager().Update();
    }

    py::dict d;
    d[ "ok" ]   = true;
    d[ "path" ] = aPath;
    return d;
}


py::dict spec_get_path()
{
    SCH_SPEC_PANE* pane = require_spec_pane();

    py::dict d;
    d[ "ok" ]   = true;
    d[ "path" ] = std::string( pane->GetSpecPath().utf8_str() );
    return d;
}


py::dict spec_show( bool aVisible )
{
    SCH_SPEC_PANE*  pane  = require_spec_pane();
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();

    wxAuiPaneInfo& info = frame->GetAuiManager().GetPane( pane );

    if( !info.IsOk() )
        throw std::runtime_error(
                "klicad_native_spec_pane.show: AUI pane not registered" );

    info.Show( aVisible );
    frame->GetAuiManager().Update();

    py::dict d;
    d[ "ok" ]      = true;
    d[ "visible" ] = aVisible;
    return d;
}


py::dict spec_regen()
{
    SCH_SPEC_PANE* pane = require_spec_pane();
    pane->CommitAndRegen();

    py::dict d;
    d[ "ok" ] = true;
    return d;
}

} // anon


void klicad_register_spec_pane_bindings( py::module_& m )
{
    m.def( "set_spec_path", &spec_set_path, py::arg( "path" ),
           R"DOC(Point the right-hand spec pane at a Python spec file.

Loads the file into the pane's editor and makes the pane visible.
Empty path clears the pane and leaves visibility alone.  Subsequent
edits in the pane trigger a regen subprocess against this path on
every Enter and on focus-loss.

Returns {ok, path}.  Raises RuntimeError if KliCAD wasn't built with
spec-pane support.
)DOC" );

    m.def( "get_spec_path", &spec_get_path,
           R"DOC(Return the spec path the pane is currently bound to.

Returns {ok, path}.  `path` is empty when no spec has been set.
)DOC" );

    m.def( "show", &spec_show, py::arg( "visible" ) = true,
           R"DOC(Show or hide the spec pane.

The pane defaults to hidden.  set_spec_path(...) shows it as a side
effect; call show(False) to dismiss it without clearing the spec.

Returns {ok, visible}.
)DOC" );

    m.def( "regen", &spec_regen,
           R"DOC(Force a spec commit + regen now.

Same code path as the Enter / focus-loss handlers; writes the editor
buffer back to disk and shells out to python3 to re-run the spec.
Used by callers that want to drive the regen explicitly (e.g. a
keyboard shortcut handler that doesn't go through the focus-loss
mechanism).

Returns {ok}.  No-op when the pane has no spec bound.
)DOC" );
}
