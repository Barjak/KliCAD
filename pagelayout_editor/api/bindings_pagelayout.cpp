/*
 * KliCAD subsystem binding: PAGE LAYOUT (drawing-sheet) editor.
 *
 * Exposes file-level and model-level operations on the PL_EDITOR_FRAME as
 * klicad_native_pagelayout.* — loading/saving .kicad_wks files, inserting
 * additional drawing sheets, starting fresh, and reading basic state.
 *
 * Pattern (follows BINDING_PATTERN.md, Pattern B):
 *   1. Walk wxTopLevelWindows for the live PL_EDITOR_FRAME (spawn via
 *      KIWAY::Player(FRAME_PL_EDITOR, true) if missing).
 *   2. Delegate to PL_EDITOR_FRAME methods directly — they handle the
 *      DS_DATA_MODEL and refresh the canvas.
 *   3. Return { ok, error?, ... } as a py::dict.
 *
 * Not a JOB_*-backed binding — the drawing sheet editor does not yet have
 * a JOB dispatcher.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>

#include <drawing_sheet/ds_data_item.h>
#include <drawing_sheet/ds_data_model.h>

#include "../pl_editor_frame.h"

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Per-TU helper name (unique vs existing bindings).
KIWAY* find_live_kiway_for_pagelayout()
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


// Walk wxTopLevelWindows for a PL_EDITOR_FRAME (FRAME_PL_EDITOR).  We can't
// dynamic_cast<PL_EDITOR_FRAME*> safely across kiface boundaries — the
// typeinfo lives in _pl_editor.kiface.bundle and may not be visible to
// libkicommon at link time.  Instead, use EDA_BASE_FRAME::GetFrameType()
// to identify FRAME_PL_EDITOR, then static_cast (safe given the identifier).
PL_EDITOR_FRAME* find_pl_editor_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_PL_EDITOR )
            return static_cast<PL_EDITOR_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live PL_EDITOR_FRAME, spawning the drawing sheet editor if
// necessary.  Throws on any failure so the caller doesn't have to null-check.
PL_EDITOR_FRAME* require_pl_editor_frame()
{
    if( PL_EDITOR_FRAME* frame = find_pl_editor_frame() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_pagelayout();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(pagelayout needs to spawn the pl_editor frame)" );
    }

    // Spawn pl_editor; this creates the PL_EDITOR_FRAME and a default empty
    // drawing sheet if none is loaded.
    kiway->Player( FRAME_PL_EDITOR, true );

    PL_EDITOR_FRAME* frame = find_pl_editor_frame();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain PL_EDITOR_FRAME after KIWAY::Player(FRAME_PL_EDITOR, true)" );
    }

    return frame;
}


// ──────────────────────────────────────────────────────────────────────────
// load_drawing_sheet
// ──────────────────────────────────────────────────────────────────────────
py::object pagelayout_load_drawing_sheet( const std::string& path )
{
    PL_EDITOR_FRAME* frame = require_pl_editor_frame();

    wxString wxPath = wxString::FromUTF8( path.c_str() );

    py::dict result;

    bool ok = frame->LoadDrawingSheetFile( wxPath );

    result[ "ok" ] = ok;

    if( !ok )
    {
        result[ "error" ] = std::string( "LoadDrawingSheetFile failed for '" )
                            + path + "'";
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// save_drawing_sheet
// ──────────────────────────────────────────────────────────────────────────
py::object pagelayout_save_drawing_sheet( const std::string& path )
{
    PL_EDITOR_FRAME* frame = require_pl_editor_frame();

    wxString wxPath;

    if( path.empty() )
    {
        wxPath = frame->GetCurrentFileName();

        if( wxPath.IsEmpty() )
        {
            py::dict result;
            result[ "ok" ]    = false;
            result[ "error" ] = std::string(
                "no current filename set — pass an explicit path or "
                "use load_drawing_sheet first" );
            return result;
        }
    }
    else
    {
        wxPath = wxString::FromUTF8( path.c_str() );
    }

    py::dict result;

    bool ok = frame->SaveDrawingSheetFile( wxPath );

    result[ "ok" ] = ok;

    if( !ok )
    {
        result[ "error" ] = std::string( "SaveDrawingSheetFile failed for '" )
                            + std::string( wxPath.utf8_str() ) + "'";
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// new_drawing_sheet
// ──────────────────────────────────────────────────────────────────────────
py::object pagelayout_new_drawing_sheet()
{
    PL_EDITOR_FRAME* frame = require_pl_editor_frame();

    frame->OnNewDrawingSheet();

    py::dict result;
    result[ "ok" ] = true;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// insert_drawing_sheet
// ──────────────────────────────────────────────────────────────────────────
py::object pagelayout_insert_drawing_sheet( const std::string& path )
{
    PL_EDITOR_FRAME* frame = require_pl_editor_frame();

    wxString wxPath = wxString::FromUTF8( path.c_str() );

    py::dict result;

    bool ok = frame->InsertDrawingSheetFile( wxPath );

    result[ "ok" ] = ok;

    if( !ok )
    {
        result[ "error" ] = std::string( "InsertDrawingSheetFile failed for '" )
                            + path + "'";
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_current_filename
// ──────────────────────────────────────────────────────────────────────────
std::string pagelayout_get_current_filename()
{
    PL_EDITOR_FRAME* frame = require_pl_editor_frame();

    wxString name = frame->GetCurrentFileName();
    return std::string( name.utf8_str() );
}


// ──────────────────────────────────────────────────────────────────────────
// get_item_count
// ──────────────────────────────────────────────────────────────────────────
int pagelayout_get_item_count()
{
    // Touch the frame so we still raise the same "GUI not running" error
    // when nothing is up, even though DS_DATA_MODEL is a process-global
    // singleton.
    (void) require_pl_editor_frame();

    DS_DATA_MODEL& model = DS_DATA_MODEL::GetTheInstance();
    return static_cast<int>( model.GetItems().size() );
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h for why
// PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_pagelayout_bindings( py::module_& m )
{
    m.doc() = "KliCAD drawing-sheet (page layout) editor binding — "
              "load/save .kicad_wks files and inspect basic state of the "
              "running PL_EDITOR_FRAME.  Spawns the editor on first call.";

    m.def( "load_drawing_sheet", &pagelayout_load_drawing_sheet,
           py::arg( "path" ),
           R"DOC(Load a .kicad_wks drawing sheet file into the editor.

Returns {ok: bool, error?: str}.  Spawns the pl_editor frame if it isn't
already open.
)DOC" );

    m.def( "save_drawing_sheet", &pagelayout_save_drawing_sheet,
           py::arg( "path" ) = std::string( "" ),
           R"DOC(Save the current drawing sheet to a .kicad_wks file.

If path is empty (default), saves to the current filename (as set by the
most recent load_drawing_sheet or interactive load).  Returns
{ok: bool, error?: str}.
)DOC" );

    m.def( "new_drawing_sheet", &pagelayout_new_drawing_sheet,
           R"DOC(Reset the editor to a fresh, empty drawing sheet.

Returns {ok: bool}.  Equivalent to PL_EDITOR_FRAME::OnNewDrawingSheet.
)DOC" );

    m.def( "insert_drawing_sheet", &pagelayout_insert_drawing_sheet,
           py::arg( "path" ),
           R"DOC(Load a .kicad_wks file and append its items to the current sheet.

Returns {ok: bool, error?: str}.
)DOC" );

    m.def( "get_current_filename", &pagelayout_get_current_filename,
           R"DOC(Return the current drawing-sheet filename (UTF-8 string).

Empty string if no file has been loaded yet (default/new sheet).
)DOC" );

    m.def( "get_item_count", &pagelayout_get_item_count,
           R"DOC(Return the number of DS_DATA_ITEMs in the model.

Counts the underlying model items (text, segments, rects, polypolygons,
bitmaps), not the rendered DS_DRAW_ITEMs.
)DOC" );
}
