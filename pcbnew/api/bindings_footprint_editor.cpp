/*
 * KliCAD subsystem binding: footprint library editor.
 *
 * Exposes the FOOTPRINT_EDIT_FRAME + FOOTPRINT_LIBRARY_ADAPTER surface as
 * klicad_native_footprint_editor.* — programmatic footprint-library editing:
 *   - enumerate loaded libraries / footprints
 *   - inspect a footprint (lib_id, name, pad count, layer, value, description, ...)
 *   - list pads on a footprint
 *   - load a footprint into the editor
 *   - save current / save all / revert current
 *   - delete a footprint from a library
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer runs
 * after py::initialize_interpreter, and PyImport_AppendInittab refuses
 * post-init.  See klicad_kiface_register.h.
 *
 * Frame discovery: walk wxTopLevelWindows for FRAME_FOOTPRINT_EDITOR,
 * identify by EDA_BASE_FRAME::GetFrameType() and static_cast — symmetric
 * with bindings_symbol_editor.cpp.
 *
 * If no frame is open, kiway->Player(FRAME_FOOTPRINT_EDITOR, true) spawns
 * one.
 *
 * Returns structured py::dict / py::list only — no streaming, no protos.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <board.h>
#include <eda_base_frame.h>
#include <footprint.h>
#include <footprint_edit_frame.h>
#include <footprint_library_adapter.h>
#include <lib_id.h>
#include <pad.h>
#include <padstack.h>
#include <project.h>
#include <project_pcb.h>

#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-TU helper name (unique vs find_live_kiway, find_live_kiway_for_erc,
// find_live_kiway_for_gui, find_live_kiway_for_sch_actions,
// find_live_kiway_for_schematic_state, find_live_kiway_for_symbol_editor).
KIWAY* find_live_kiway_for_footprint_editor()
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


// Identify the FOOTPRINT_EDIT_FRAME via EDA_BASE_FRAME::GetFrameType().
// We use the frame-type identifier + static_cast (not dynamic_cast) for
// the same reason as bindings_symbol_editor.cpp — even though this TU is
// compiled into the pcbnew kiface where the typeinfo lives, we keep the
// pattern consistent with peer bindings.
FOOTPRINT_EDIT_FRAME* find_footprint_edit_frame_for_editor()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_FOOTPRINT_EDITOR )
            return static_cast<FOOTPRINT_EDIT_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live FOOTPRINT_EDIT_FRAME, spawning the footprint editor if
// missing.  Throws std::runtime_error on any failure so callers don't have
// to null-check.
FOOTPRINT_EDIT_FRAME* require_footprint_edit_frame()
{
    if( FOOTPRINT_EDIT_FRAME* frame = find_footprint_edit_frame_for_editor() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_footprint_editor();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(footprint_editor needs to spawn FRAME_FOOTPRINT_EDITOR)" );
    }

    kiway->Player( FRAME_FOOTPRINT_EDITOR, true );

    FOOTPRINT_EDIT_FRAME* frame = find_footprint_edit_frame_for_editor();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain FOOTPRINT_EDIT_FRAME after "
            "KIWAY::Player(FRAME_FOOTPRINT_EDITOR, true)" );
    }

    return frame;
}


// Parse "LibName:FootprintName" into a LIB_ID, raising on failure.
// (Distinct name from bindings_schematic_state's parse_lib_id /
// bindings_symbol_editor's parse_lib_id to avoid any ODR confusion across
// the kiface boundary, even though both are anon-namespaced.)
LIB_ID parse_lib_id_for_fp( const std::string& aLibIdStr )
{
    LIB_ID libId;

    // LIB_ID::Parse returns the index of the first illegal char on failure
    // (>= 0), negative on success — mirrors symbol_editor convention.
    if( libId.Parse( aLibIdStr ) >= 0 )
    {
        throw std::invalid_argument(
            "lib_id parse failed; expected 'LibName:FootprintName' (got '"
            + aLibIdStr + "')" );
    }

    if( libId.GetLibNickname().empty() || libId.GetLibItemName().empty() )
    {
        throw std::invalid_argument(
            "lib_id missing library or footprint component; "
            "expected 'LibName:FootprintName' (got '" + aLibIdStr + "')" );
    }

    return libId;
}


// Resolve the FOOTPRINT_LIBRARY_ADAPTER for the live project.  The
// footprint editor uses the project's library adapter (not its own per-
// editor library manager) — same instance used by board ops.
FOOTPRINT_LIBRARY_ADAPTER* require_fp_lib_adapter( FOOTPRINT_EDIT_FRAME* aFrame )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter =
        PROJECT_PCB::FootprintLibAdapter( &aFrame->Prj() );

    if( !adapter )
    {
        throw std::runtime_error(
            "PROJECT_PCB::FootprintLibAdapter returned null — project has "
            "no footprint library adapter (unexpected)" );
    }

    return adapter;
}


// Build a summary dict for a FOOTPRINT — used by get_footprint_info,
// get_current_footprint, load_footprint result.  Mirrors symbol_editor's
// make_symbol_info_dict shape (library/name/lib_id/found + per-item
// fields).
py::dict make_footprint_info_dict( FOOTPRINT* aFootprint, const wxString& aLibrary,
                                   const wxString& aName )
{
    py::dict d;

    d[ "library" ] = std::string( aLibrary.ToUTF8() );
    d[ "name" ]    = std::string( aName.ToUTF8() );
    d[ "lib_id" ]  = std::string( ( aLibrary + wxT( ":" ) + aName ).ToUTF8() );

    if( !aFootprint )
    {
        d[ "found" ] = false;
        return d;
    }

    d[ "found" ]       = true;
    d[ "pad_count" ]   = static_cast<int>( aFootprint->GetPadCount() );
    d[ "layer" ]       = std::string(
        BOARD::GetStandardLayerName( aFootprint->GetLayer() ).ToUTF8() );
    d[ "value" ]       = std::string( aFootprint->GetValue().ToUTF8() );
    d[ "reference" ]   = std::string( aFootprint->GetReference().ToUTF8() );
    d[ "description" ] = std::string( aFootprint->GetLibDescription().ToUTF8() );
    d[ "keywords" ]    = std::string( aFootprint->GetKeywords().ToUTF8() );

    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// list_loaded_libraries
// ──────────────────────────────────────────────────────────────────────────
py::list fp_ed_list_loaded_libraries()
{
    FOOTPRINT_EDIT_FRAME*      frame   = require_footprint_edit_frame();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = require_fp_lib_adapter( frame );

    py::list out;

    for( const wxString& name : adapter->GetLibraryNames() )
        out.append( std::string( name.ToUTF8() ) );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_footprints_in_library
// ──────────────────────────────────────────────────────────────────────────
py::list fp_ed_list_footprints_in_library( const std::string& aLibrary )
{
    FOOTPRINT_EDIT_FRAME*      frame   = require_footprint_edit_frame();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = require_fp_lib_adapter( frame );

    wxString libName = wxString::FromUTF8( aLibrary.c_str() );

    if( !adapter->HasLibrary( libName, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is not loaded in the "
            "footprint library adapter" );
    }

    py::list out;
    std::vector<wxString> names =
        adapter->GetFootprintNames( libName, /*aBestEfforts*/ true );

    for( const wxString& fp : names )
        out.append( std::string( fp.ToUTF8() ) );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// get_footprint_info
// ──────────────────────────────────────────────────────────────────────────
py::dict fp_ed_get_footprint_info( const std::string& aLibIdStr )
{
    FOOTPRINT_EDIT_FRAME*      frame   = require_footprint_edit_frame();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = require_fp_lib_adapter( frame );

    LIB_ID libId = parse_lib_id_for_fp( aLibIdStr );

    wxString lib  = wxString::FromUTF8( libId.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( libId.GetLibItemName().c_str() );

    if( !adapter->HasLibrary( lib, /*aCheckEnabled*/ false ) )
    {
        // Match symbol_editor behaviour: return found=false rather than
        // throwing, so callers can probe inexpensively.
        return make_footprint_info_dict( nullptr, lib, name );
    }

    // LoadFootprint returns nullptr if the footprint isn't found; we own
    // the returned pointer.  Use std::unique_ptr<FOOTPRINT> for RAII.
    std::unique_ptr<FOOTPRINT> fp{ adapter->LoadFootprint( lib, name,
                                                           /*aKeepUUID*/ true ) };

    return make_footprint_info_dict( fp.get(), lib, name );
}


// ──────────────────────────────────────────────────────────────────────────
// list_pads
// ──────────────────────────────────────────────────────────────────────────
py::list fp_ed_list_pads( const std::string& aLibIdStr )
{
    FOOTPRINT_EDIT_FRAME*      frame   = require_footprint_edit_frame();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = require_fp_lib_adapter( frame );

    LIB_ID libId = parse_lib_id_for_fp( aLibIdStr );

    wxString lib  = wxString::FromUTF8( libId.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( libId.GetLibItemName().c_str() );

    if( !adapter->HasLibrary( lib, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + std::string( lib.ToUTF8() )
            + "' is not loaded in the footprint library adapter" );
    }

    std::unique_ptr<FOOTPRINT> fp{ adapter->LoadFootprint( lib, name,
                                                           /*aKeepUUID*/ true ) };

    if( !fp )
    {
        throw std::runtime_error(
            std::string( "footprint '" ) + aLibIdStr + "' not found" );
    }

    py::list out;

    for( PAD* pad : fp->Pads() )
    {
        if( !pad )
            continue;

        py::dict p;
        p[ "number" ] = std::string( pad->GetNumber().ToUTF8() );
        p[ "name" ]   = std::string( pad->GetPinFunction().ToUTF8() );

        VECTOR2I pos = pad->GetPosition();
        p[ "x_nm" ]  = pos.x;
        p[ "y_nm" ]  = pos.y;

        p[ "layer" ] = std::string(
            BOARD::GetStandardLayerName( pad->GetLayer() ).ToUTF8() );

        // ShowPadShape uses the primary layer's PAD_SHAPE; padstacks can
        // vary per-layer but the primary entry mirrors what the legacy
        // single-shape API reported.
        p[ "shape" ] = std::string(
            pad->ShowPadShape( pad->GetLayer() ).ToUTF8() );

        // Drill size: report the primary (X) drill in mm.  0 if no hole.
        // pcbnew stores drill internally in nanometers; convert here so
        // callers don't need to know about IU.
        int drillNm = pad->GetDrillSizeX();
        p[ "drill_mm" ] = static_cast<double>( drillNm ) / 1e6;

        out.append( p );
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// load_footprint
// ──────────────────────────────────────────────────────────────────────────
py::dict fp_ed_load_footprint( const std::string& aLibIdStr )
{
    FOOTPRINT_EDIT_FRAME* frame = require_footprint_edit_frame();

    LIB_ID libId = parse_lib_id_for_fp( aLibIdStr );

    // LoadFootprintFromLibrary returns void and shows a wxLog error if
    // it can't load — we can't easily detect failure here, so we check
    // by re-querying GetLoadedFPID afterward.
    frame->LoadFootprintFromLibrary( libId );

    py::dict result;
    LIB_ID   loaded = frame->GetLoadedFPID();
    bool     ok     = ( loaded == libId );

    result[ "ok" ]     = ok;
    result[ "lib_id" ] = aLibIdStr;

    if( !ok )
    {
        result[ "error" ] =
            std::string( "FOOTPRINT_EDIT_FRAME::LoadFootprintFromLibrary "
                         "did not load the requested footprint (currently "
                         "loaded: '" )
            + loaded.Format().c_str() + "')";
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_current_footprint
// ──────────────────────────────────────────────────────────────────────────
py::object fp_ed_get_current_footprint()
{
    FOOTPRINT_EDIT_FRAME* frame = require_footprint_edit_frame();
    FOOTPRINT*            fp    = frame->GetBoard() ? frame->GetBoard()->GetFirstFootprint()
                                                    : nullptr;

    if( !fp )
        return py::none();

    LIB_ID   fpid = fp->GetFPID();
    wxString lib  = wxString::FromUTF8( fpid.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( fpid.GetLibItemName().c_str() );

    return make_footprint_info_dict( fp, lib, name );
}


// ──────────────────────────────────────────────────────────────────────────
// save_current
// ──────────────────────────────────────────────────────────────────────────
py::dict fp_ed_save_current()
{
    FOOTPRINT_EDIT_FRAME* frame = require_footprint_edit_frame();

    BOARD*     board = frame->GetBoard();
    FOOTPRINT* fp    = board ? board->GetFirstFootprint() : nullptr;

    py::dict result;

    if( !fp )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = "no footprint currently loaded in the editor";
        return result;
    }

    bool ok = frame->SaveFootprint( fp );

    if( ok )
    {
        // Mirror what FOOTPRINT_EDITOR_CONTROL::Save does after a
        // successful write — clear the modified flag and refresh the
        // library tree so the GUI reflects the on-disk state.
        frame->ClearModify();
        frame->UpdateTitle();
        frame->RefreshLibraryTree();
    }

    result[ "ok" ]              = ok;
    result[ "content_modified" ] = frame->IsContentModified();

    if( !ok )
        result[ "error" ] =
            std::string( "FOOTPRINT_EDIT_FRAME::SaveFootprint returned "
                         "false (library read-only or write failed)" );

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// save_all
// ──────────────────────────────────────────────────────────────────────────
//
// The footprint editor edits one footprint at a time (no multi-buffer
// library manager like SYMBOL_EDIT_FRAME), so "save all" is equivalent to
// "save current".  Kept for API symmetry with klicad_native_symbol_editor.
py::dict fp_ed_save_all()
{
    return fp_ed_save_current();
}


// ──────────────────────────────────────────────────────────────────────────
// revert_current
// ──────────────────────────────────────────────────────────────────────────
py::dict fp_ed_revert_current()
{
    FOOTPRINT_EDIT_FRAME* frame = require_footprint_edit_frame();

    // FOOTPRINT_EDIT_FRAME::RevertFootprint() pops a confirmation dialog
    // unconditionally — there's no aConfirm parameter, unlike
    // SYMBOL_EDIT_FRAME::Revert.  Callers should be aware that this will
    // surface a modal GUI dialog.
    bool ok = frame->RevertFootprint();

    py::dict result;
    result[ "ok" ] = ok;

    if( !ok )
        result[ "error" ] =
            std::string( "FOOTPRINT_EDIT_FRAME::RevertFootprint returned "
                         "false (user cancelled or nothing to revert)" );

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// delete_footprint
// ──────────────────────────────────────────────────────────────────────────
py::dict fp_ed_delete_footprint( const std::string& aLibIdStr )
{
    FOOTPRINT_EDIT_FRAME* frame = require_footprint_edit_frame();

    LIB_ID libId = parse_lib_id_for_fp( aLibIdStr );

    // DeleteFootprintFromLibrary takes (LIB_ID, bool aConfirm).  Pass
    // aConfirm=false so we don't pop a GUI confirmation dialog from a
    // programmatic call.
    bool ok = frame->DeleteFootprintFromLibrary( libId, /*aConfirm*/ false );

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "lib_id" ] = aLibIdStr;

    if( !ok )
    {
        result[ "error" ] =
            std::string( "FOOTPRINT_EDIT_FRAME::DeleteFootprintFromLibrary "
                         "returned false (library read-only, footprint not "
                         "found, or write failed)" );
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// is_content_modified
// ──────────────────────────────────────────────────────────────────────────
bool fp_ed_is_content_modified()
{
    return require_footprint_edit_frame()->IsContentModified();
}


// ──────────────────────────────────────────────────────────────────────────
// is_library_modified
// ──────────────────────────────────────────────────────────────────────────
//
// The footprint editor doesn't track per-library modification state the
// way SYMBOL_EDIT_FRAME's LIB_SYMBOL_LIBRARY_MANAGER does — it edits one
// footprint at a time and writes straight through to the library on Save.
// The closest equivalent (mirroring FP_TREE_SYNCHRONIZING_ADAPTER's bold-
// font logic) is: "the loaded footprint's library is modified iff the
// currently-loaded footprint has unsaved changes and isn't a board copy".
// For any other library nickname this function returns false.
bool fp_ed_is_library_modified( const std::string& aLibrary )
{
    FOOTPRINT_EDIT_FRAME*      frame   = require_footprint_edit_frame();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = require_fp_lib_adapter( frame );

    wxString lib = wxString::FromUTF8( aLibrary.c_str() );

    if( !adapter->HasLibrary( lib, /*aCheckEnabled*/ false ) )
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is not loaded" );

    wxString loadedLib = frame->GetLoadedFPID().GetLibNickname().wx_str();

    if( loadedLib != lib )
        return false;

    return frame->IsContentModified() && !frame->IsCurrentFPFromBoard();
}

} // anon


// Registered at kiface-load time by pcbnew/api/klicad_kiface_register.cpp.
// PYBIND11_EMBEDDED_MODULE can't be used here — see klicad_kiface_register.h
// for the explanation.
void klicad_register_footprint_editor_bindings( py::module_& m )
{
    m.doc() = "KliCAD footprint library editor binding.  Drives the running "
              "FOOTPRINT_EDIT_FRAME and its FOOTPRINT_LIBRARY_ADAPTER for "
              "programmatic footprint-library editing: enumerate libraries "
              "/ footprints, inspect a footprint's pads and metadata, load "
              "a footprint into the editor canvas, save / save-all / "
              "revert, and delete footprints.\n\n"
              "Spawns FRAME_FOOTPRINT_EDITOR via KIWAY::Player if the editor "
              "isn't already open.  Unlike the symbol editor, the footprint "
              "editor writes straight to disk on save (no per-library in-"
              "memory buffer) — save_current and save_all are therefore "
              "equivalent.";

    m.def( "list_loaded_libraries", &fp_ed_list_loaded_libraries,
           R"DOC(Return a list of library nicknames currently loaded in the
project's footprint library adapter.  Sourced from
FOOTPRINT_LIBRARY_ADAPTER::GetLibraryNames().

Spawns the footprint editor if it isn't already open.
)DOC" );

    m.def( "list_footprints_in_library", &fp_ed_list_footprints_in_library,
           py::arg( "library" ),
           R"DOC(Return a list of footprint names in the given library nickname.

library: the library nickname (NOT the full path).  Must be in the list
         returned by list_loaded_libraries().

Sourced from FOOTPRINT_LIBRARY_ADAPTER::GetFootprintNames(..., aBestEfforts=true)
so transient enumeration errors degrade to a partial / empty list rather
than raising.

Raises RuntimeError if the library isn't loaded.
)DOC" );

    m.def( "get_footprint_info", &fp_ed_get_footprint_info,
           py::arg( "lib_id" ),
           R"DOC(Return a dict describing the footprint at 'LibName:FootprintName'.

Keys: library, name, lib_id, found (bool).  When found is True, also:
pad_count, layer (canonical layer name, e.g. 'F.Cu'), value, reference,
description, keywords.

Raises ValueError on lib_id parse failure.  Returns found=False if the
library isn't loaded or the footprint doesn't exist.
)DOC" );

    m.def( "list_pads", &fp_ed_list_pads,
           py::arg( "lib_id" ),
           R"DOC(Return a list of pad dicts for the footprint at 'LibName:FootprintName'.

Each dict has: number, name (pin function / net hint), x_nm, y_nm (pad
center, in KiCad internal units = nanometers), layer (primary layer
name), shape ('Circle', 'Oval', 'Rectangle', 'Trapezoid', 'Rounded
rectangle', 'Chamfered rectangle', 'Custom shape'), drill_mm (primary
drill diameter in millimeters; 0.0 for SMD pads).

Raises RuntimeError if the library isn't loaded or the footprint isn't
found.  Raises ValueError on lib_id parse failure.
)DOC" );

    m.def( "load_footprint", &fp_ed_load_footprint,
           py::arg( "lib_id" ),
           R"DOC(Load 'LibName:FootprintName' into the footprint editor canvas
(equivalent to double-clicking the footprint in the library tree).

Returns {ok: bool, lib_id: str, error?: str}.  ok is true iff
GetLoadedFPID() matches the requested lib_id after the load attempt.
)DOC" );

    m.def( "get_current_footprint", &fp_ed_get_current_footprint,
           R"DOC(Return the info dict for the footprint currently loaded in the
editor canvas, or None if no footprint is loaded.

Same keys as get_footprint_info().
)DOC" );

    m.def( "save_current", &fp_ed_save_current,
           R"DOC(Save the currently loaded footprint to its library on disk
(FOOTPRINT_EDIT_FRAME::SaveFootprint on the editor's first footprint).
Also clears the modified flag, updates the title bar, and refreshes the
library tree — same post-save housekeeping the toolbar Save button does.

Returns {ok: bool, content_modified: bool, error?: str}.
)DOC" );

    m.def( "save_all", &fp_ed_save_all,
           R"DOC(Equivalent to save_current() — the footprint editor edits one
footprint at a time (no multi-buffer library manager), so there is no
distinct save-all operation.  Kept for API symmetry with
klicad_native_symbol_editor.save_all().

Returns {ok: bool, content_modified: bool, error?: str}.
)DOC" );

    m.def( "revert_current", &fp_ed_revert_current,
           R"DOC(Reload the currently loaded footprint from disk, discarding
unsaved changes (FOOTPRINT_EDIT_FRAME::RevertFootprint).

NOTE: this function does NOT suppress the GUI confirmation dialog —
RevertFootprint always shows one.  Callers driving the editor headlessly
should be aware that a modal dialog will surface.

Returns {ok: bool, error?: str}.  ok=false if the user cancelled the
confirmation or there was nothing to revert.
)DOC" );

    m.def( "delete_footprint", &fp_ed_delete_footprint,
           py::arg( "lib_id" ),
           R"DOC(Delete the footprint at 'LibName:FootprintName' from its
library on disk (FOOTPRINT_EDIT_FRAME::DeleteFootprintFromLibrary,
aConfirm=false).

Unlike the symbol editor, this writes through to the library
immediately — there is no in-memory buffer to flush afterward.

Returns {ok: bool, lib_id: str, error?: str}.
)DOC" );

    m.def( "is_content_modified", &fp_ed_is_content_modified,
           R"DOC(Return True if the currently loaded footprint has unsaved
changes (FOOTPRINT_EDIT_FRAME::IsContentModified).
)DOC" );

    m.def( "is_library_modified", &fp_ed_is_library_modified,
           py::arg( "library" ),
           R"DOC(Return True if the given library has unsaved changes.

Caveat: the footprint editor does not track per-library modification
state the way the symbol editor does (it writes straight through to disk
on save).  The best approximation — and what this function returns — is:
True iff the given library matches the currently-loaded footprint's
library AND the loaded footprint has unsaved changes AND it isn't a copy
loaded from a board.  For any other library nickname this returns False
even if you have unsaved changes in a different one (you can't — the
editor only holds one footprint).

Raises RuntimeError if the library isn't loaded.
)DOC" );
}
