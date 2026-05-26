/*
 * KliCAD subsystem binding: symbol library editor.
 *
 * Exposes the SYMBOL_EDIT_FRAME + LIB_SYMBOL_LIBRARY_MANAGER surface as
 * kicad_native_symbol_editor.* — programmatic symbol-library editing:
 *   - enumerate loaded libraries / symbols
 *   - inspect a symbol (lib_id, name, pin count, unit count, ref, ...)
 *   - load a symbol into the editor
 *   - create a new (root) symbol via NEW_SYMBOL_PROPERTIES
 *   - save current / save all / revert current
 *   - delete a symbol from its library buffer
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * eeschema/api/klicad_kiface_register.cpp::klicad_register_eeschema_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer runs
 * after py::initialize_interpreter, and PyImport_AppendInittab refuses
 * post-init.
 *
 * Frame discovery: walk wxTopLevelWindows for FRAME_SCH_SYMBOL_EDITOR.
 * We cannot dynamic_cast<SYMBOL_EDIT_FRAME*> across the kiface boundary
 * (RTTI symbol may live in _eeschema.kiface.bundle), so we identify via
 * EDA_BASE_FRAME::GetFrameType() == FRAME_SCH_SYMBOL_EDITOR and then
 * static_cast.  Same caveat as bindings_schematic_state.cpp.
 *
 * If no frame is open, kiway->Player(FRAME_SCH_SYMBOL_EDITOR, true)
 * spawns one (mirroring KIWAY_PLAYER spawning in bindings_sch_actions /
 * bindings_schematic_state).
 *
 * Returns structured py::dict / py::list only — no streaming, no protos.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <sch_field.h>
#include <sch_pin.h>
#include <symbol_editor/symbol_edit_frame.h>
#include <symbol_editor/lib_symbol_library_manager.h>

#include <wx/arrstr.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <list>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-TU helper name (unique vs find_live_kiway, find_live_kiway_for_erc,
// find_live_kiway_for_gui, find_live_kiway_for_sch_actions,
// find_live_kiway_for_schematic_state).
KIWAY* find_live_kiway_for_symbol_editor()
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


// Identify the SYMBOL_EDIT_FRAME via EDA_BASE_FRAME::GetFrameType().  Doing
// a direct dynamic_cast<SYMBOL_EDIT_FRAME*> from libkicommon would be
// unsafe — the typeinfo lives in the lazily-loaded _eeschema.kiface
// bundle.  But this TU is compiled INTO that kiface, so technically it
// could; we still use the frame-type identifier path for symmetry with
// bindings_schematic_state.cpp and to keep behaviour consistent if this
// code is ever moved.
SYMBOL_EDIT_FRAME* find_symbol_edit_frame_for_editor()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_SCH_SYMBOL_EDITOR )
            return static_cast<SYMBOL_EDIT_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live SYMBOL_EDIT_FRAME, spawning the symbol editor if missing.
// Throws std::runtime_error on any failure so callers don't have to
// null-check.
SYMBOL_EDIT_FRAME* require_symbol_edit_frame()
{
    if( SYMBOL_EDIT_FRAME* frame = find_symbol_edit_frame_for_editor() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_symbol_editor();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(symbol_editor needs to spawn FRAME_SCH_SYMBOL_EDITOR)" );
    }

    kiway->Player( FRAME_SCH_SYMBOL_EDITOR, true );

    SYMBOL_EDIT_FRAME* frame = find_symbol_edit_frame_for_editor();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain SYMBOL_EDIT_FRAME after "
            "KIWAY::Player(FRAME_SCH_SYMBOL_EDITOR, true)" );
    }

    return frame;
}


// Parse "LibName:SymbolName" into a LIB_ID, raising on failure.
LIB_ID parse_lib_id( const std::string& aLibIdStr )
{
    LIB_ID libId;

    // LIB_ID::Parse returns a negative value on success (it's the index of
    // the first illegal char on failure, or -1 / negative on success
    // depending on the version).  Following bindings_schematic_state's
    // convention: parse_result >= 0 means error.
    if( libId.Parse( aLibIdStr ) >= 0 )
    {
        throw std::invalid_argument(
            "lib_id parse failed; expected 'LibName:SymbolName' (got '"
            + aLibIdStr + "')" );
    }

    if( libId.GetLibNickname().empty() || libId.GetLibItemName().empty() )
    {
        throw std::invalid_argument(
            "lib_id missing library or symbol component; "
            "expected 'LibName:SymbolName' (got '" + aLibIdStr + "')" );
    }

    return libId;
}


// ──────────────────────────────────────────────────────────────────────────
// list_loaded_libraries
// ──────────────────────────────────────────────────────────────────────────
py::list sym_ed_list_loaded_libraries()
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    py::list out;

    wxArrayString names = mgr.GetLibraryNames();

    for( const wxString& name : names )
        out.append( std::string( name.ToUTF8() ) );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_symbols_in_library
// ──────────────────────────────────────────────────────────────────────────
py::list sym_ed_list_symbols_in_library( const std::string& aLibrary )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    wxString libName = wxString::FromUTF8( aLibrary.c_str() );

    if( !mgr.LibraryExists( libName, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is not loaded in the "
            "symbol editor's library manager" );
    }

    py::list out;
    wxArrayString symbolNames;
    mgr.GetSymbolNames( libName, symbolNames, SYMBOL_NAME_FILTER::ALL );

    for( const wxString& sym : symbolNames )
        out.append( std::string( sym.ToUTF8() ) );

    return out;
}


// Build a summary dict for the given LIB_SYMBOL.
py::dict make_symbol_info_dict( LIB_SYMBOL* aSymbol, const wxString& aLibrary,
                                const wxString& aName )
{
    py::dict d;

    d[ "library" ]    = std::string( aLibrary.ToUTF8() );
    d[ "name" ]       = std::string( aName.ToUTF8() );
    d[ "lib_id" ]     = std::string( ( aLibrary + wxT( ":" ) + aName ).ToUTF8() );

    if( !aSymbol )
    {
        d[ "found" ] = false;
        return d;
    }

    d[ "found" ]        = true;
    d[ "is_root" ]      = aSymbol->IsRoot();
    d[ "is_power" ]     = aSymbol->IsPower();
    d[ "is_multi_unit" ] = aSymbol->IsMultiUnit();
    d[ "unit_count" ]   = aSymbol->GetUnitCount();
    d[ "pin_count" ]    = aSymbol->GetPinCount();

    // Reference designator prefix (e.g. "R", "U").  GetReferenceField is
    // non-const on LIB_SYMBOL; const accessor returns the same SCH_FIELD&.
    d[ "reference" ]    =
        std::string( aSymbol->GetReferenceField().GetText().ToUTF8() );

    d[ "description" ]  = std::string( aSymbol->GetDescription().ToUTF8() );
    d[ "keywords" ]     = std::string( aSymbol->GetKeyWords().ToUTF8() );

    // Sim.* fields — the canonical place a symbol records its SPICE binding.
    // Absent fields appear as empty strings.  Lets clients see whether a
    // KiCad symbol is already mapped to a SPICE model and pull the existing
    // pin map / model name out, instead of inferring them from the .lib.
    py::dict sim_fields;
    auto read_field = [aSymbol]( const wxString& name ) -> std::string
    {
        const SCH_FIELD* f = aSymbol->GetField( name );
        return f ? std::string( f->GetText().ToUTF8() ) : std::string();
    };
    sim_fields[ "name" ]    = read_field( wxT( "Sim.Name" ) );
    sim_fields[ "type" ]    = read_field( wxT( "Sim.Type" ) );
    sim_fields[ "pins" ]    = read_field( wxT( "Sim.Pins" ) );
    sim_fields[ "library" ] = read_field( wxT( "Sim.Library" ) );
    sim_fields[ "params" ]  = read_field( wxT( "Sim.Params" ) );

    // Anything else under Sim.* (Sim.Params.<param> overrides, etc.)
    py::dict sim_extra;
    std::vector<SCH_FIELD*> all_fields;
    aSymbol->GetFields( all_fields, /*aVisibleOnly*/ false );
    for( SCH_FIELD* f : all_fields )
    {
        wxString fname = f->GetName();
        if( !fname.StartsWith( wxT( "Sim." ) ) )
            continue;
        if( fname == wxT( "Sim.Name" )    || fname == wxT( "Sim.Type" )
         || fname == wxT( "Sim.Pins" )    || fname == wxT( "Sim.Library" )
         || fname == wxT( "Sim.Params" ) )
            continue;
        sim_extra[ py::str( std::string( fname.ToUTF8() ) ) ] =
            std::string( f->GetText().ToUTF8() );
    }
    sim_fields[ "extra" ] = sim_extra;
    d[ "sim_fields" ] = sim_fields;

    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_symbol_info
// ──────────────────────────────────────────────────────────────────────────
py::dict sym_ed_get_symbol_info( const std::string& aLibIdStr )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    LIB_ID libId = parse_lib_id( aLibIdStr );

    wxString lib  = wxString::FromUTF8( libId.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( libId.GetLibItemName().c_str() );

    LIB_SYMBOL* sym = nullptr;

    if( mgr.LibraryExists( lib, /*aCheckEnabled*/ false ) )
        sym = mgr.GetSymbol( name, lib );

    return make_symbol_info_dict( sym, lib, name );
}


// ──────────────────────────────────────────────────────────────────────────
// list_pins
// ──────────────────────────────────────────────────────────────────────────
py::list sym_ed_list_pins( const std::string& aLibIdStr )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    LIB_ID libId = parse_lib_id( aLibIdStr );

    wxString lib  = wxString::FromUTF8( libId.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( libId.GetLibItemName().c_str() );

    if( !mgr.LibraryExists( lib, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + std::string( lib.ToUTF8() )
            + "' is not loaded in the symbol editor's library manager" );
    }

    LIB_SYMBOL* sym = mgr.GetSymbol( name, lib );

    if( !sym )
    {
        throw std::runtime_error(
            std::string( "symbol '" ) + aLibIdStr + "' not found" );
    }

    py::list out;

    for( SCH_PIN* pin : sym->GetPins() )
    {
        if( !pin )
            continue;

        py::dict p;
        p[ "name" ]   = std::string( pin->GetName().ToUTF8() );
        p[ "number" ] = std::string( pin->GetNumber().ToUTF8() );
        p[ "length" ] = pin->GetLength();
        p[ "electrical_type" ] =
            std::string( pin->GetElectricalTypeName().ToUTF8() );
        p[ "x_nm" ]   = pin->GetPosition().x;
        p[ "y_nm" ]   = pin->GetPosition().y;
        out.append( p );
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// load_symbol
// ──────────────────────────────────────────────────────────────────────────
py::dict sym_ed_load_symbol( const std::string& aLibIdStr )
{
    SYMBOL_EDIT_FRAME* frame = require_symbol_edit_frame();

    LIB_ID libId = parse_lib_id( aLibIdStr );

    // SYMBOL_EDIT_FRAME::LoadSymbol(const LIB_ID&, int aUnit, int aBodyStyle)
    bool ok = frame->LoadSymbol( libId, /*aUnit*/ 1, /*aBodyStyle*/ 1 );

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "lib_id" ] = aLibIdStr;

    if( !ok )
        result[ "error" ] =
            std::string( "SYMBOL_EDIT_FRAME::LoadSymbol returned false (lib_id "
                         "not found or library not loaded)" );

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_current_symbol
// ──────────────────────────────────────────────────────────────────────────
py::object sym_ed_get_current_symbol()
{
    SYMBOL_EDIT_FRAME* frame = require_symbol_edit_frame();
    LIB_SYMBOL*        sym   = frame->GetCurSymbol();

    if( !sym )
        return py::none();

    wxString lib  = sym->GetLibId().GetLibNickname().wx_str();
    wxString name = sym->GetName();

    return make_symbol_info_dict( sym, lib, name );
}


// ──────────────────────────────────────────────────────────────────────────
// create_symbol
// ──────────────────────────────────────────────────────────────────────────
//
// Wraps LIB_SYMBOL_LIBRARY_MANAGER::CreateNewSymbol(library, NEW_SYMBOL_PROPERTIES).
// Most NEW_SYMBOL_PROPERTIES fields get sensible defaults (mirrors what the
// New-Symbol dialog uses for a blank root symbol).  Caller supplies library
// name, symbol name, optional reference designator prefix, unit count, and
// whether this is a power symbol.
py::dict sym_ed_create_symbol( const std::string& aLibrary,
                               const std::string& aName,
                               const std::string& aReference,
                               int                aUnitCount,
                               bool               aPowerSymbol )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    wxString lib = wxString::FromUTF8( aLibrary.c_str() );

    if( !mgr.LibraryExists( lib, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is not loaded; "
            "open it in the symbol editor (or pick an existing one from "
            "list_loaded_libraries) first" );
    }

    if( mgr.IsLibraryReadOnly( lib ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is read-only" );
    }

    wxString name = wxString::FromUTF8( aName.c_str() );

    if( mgr.SymbolNameInUse( name, lib ) )
    {
        py::dict result;
        result[ "ok" ]    = false;
        result[ "error" ] = "symbol '" + aName + "' already exists in '"
                            + aLibrary + "'";
        return result;
    }

    NEW_SYMBOL_PROPERTIES props;
    props.name                   = name;
    props.parentSymbolName       = wxEmptyString;  // root symbol
    props.reference              = wxString::FromUTF8(
        aReference.empty() ? "U" : aReference.c_str() );
    props.unitCount              = ( aUnitCount > 0 ) ? aUnitCount : 1;
    props.pinNameInside          = true;
    props.pinTextPosition        = 50;             // nm offset, dialog default
    props.powerSymbol            = aPowerSymbol;
    props.showPinNumber          = !aPowerSymbol;
    props.showPinName            = true;
    props.unitsInterchangeable   = true;
    props.includeInBom           = !aPowerSymbol;
    props.includeOnBoard         = !aPowerSymbol;
    props.alternateBodyStyle     = false;
    props.keepFootprint          = false;
    props.keepDatasheet          = false;
    props.transferUserFields     = false;
    props.keepContentUserFields  = false;

    bool ok = mgr.CreateNewSymbol( lib, props );

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "lib_id" ] = aLibrary + ":" + aName;

    if( !ok )
    {
        result[ "error" ] =
            std::string( "LIB_SYMBOL_LIBRARY_MANAGER::CreateNewSymbol "
                         "returned false (see KiCad log for details)" );
    }

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// save_current
// ──────────────────────────────────────────────────────────────────────────
py::dict sym_ed_save_current()
{
    SYMBOL_EDIT_FRAME* frame = require_symbol_edit_frame();

    // SYMBOL_EDIT_FRAME::Save() saves the selected symbol/library — same
    // entry point as the toolbar Save button.  Any IO errors surface as
    // wxLog messages; we can't trivially capture them here, so callers
    // should check by re-querying via is_content_modified afterward.
    frame->Save();

    py::dict result;
    result[ "ok" ] = true;
    result[ "content_modified" ] = frame->IsContentModified();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// save_all
// ──────────────────────────────────────────────────────────────────────────
py::dict sym_ed_save_all()
{
    SYMBOL_EDIT_FRAME* frame = require_symbol_edit_frame();

    frame->SaveAll();

    py::dict result;
    result[ "ok" ] = true;
    result[ "content_modified" ] = frame->IsContentModified();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// revert_current
// ──────────────────────────────────────────────────────────────────────────
py::dict sym_ed_revert_current()
{
    SYMBOL_EDIT_FRAME* frame = require_symbol_edit_frame();

    // Pass aConfirm=false to skip the modal — programmatic callers don't
    // want to drive the GUI's confirmation dialog.
    frame->Revert( /*aConfirm*/ false );

    py::dict result;
    result[ "ok" ] = true;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// delete_symbol
// ──────────────────────────────────────────────────────────────────────────
//
// LIB_SYMBOL_LIBRARY_MANAGER::RemoveSymbol marks the symbol for deletion in
// its library buffer; you still need save_current/save_all afterward to
// flush the change to disk.
py::dict sym_ed_delete_symbol( const std::string& aLibIdStr )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    LIB_ID libId = parse_lib_id( aLibIdStr );

    wxString lib  = wxString::FromUTF8( libId.GetLibNickname().c_str() );
    wxString name = wxString::FromUTF8( libId.GetLibItemName().c_str() );

    if( !mgr.LibraryExists( lib, /*aCheckEnabled*/ false ) )
    {
        throw std::runtime_error(
            std::string( "library '" ) + std::string( lib.ToUTF8() )
            + "' is not loaded" );
    }

    if( !mgr.SymbolExists( name, lib ) )
    {
        py::dict result;
        result[ "ok" ]    = false;
        result[ "error" ] = "symbol '" + aLibIdStr + "' not found in library";
        return result;
    }

    bool ok = mgr.RemoveSymbol( name, lib );

    // Refresh the library tree so the deletion is visible in the GUI.
    if( ok )
        frame->RefreshLibraryTree();

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "lib_id" ] = aLibIdStr;

    if( !ok )
        result[ "error" ] =
            std::string( "LIB_SYMBOL_LIBRARY_MANAGER::RemoveSymbol returned "
                         "false" );

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// is_content_modified
// ──────────────────────────────────────────────────────────────────────────
bool sym_ed_is_content_modified()
{
    return require_symbol_edit_frame()->IsContentModified();
}


// ──────────────────────────────────────────────────────────────────────────
// is_library_modified
// ──────────────────────────────────────────────────────────────────────────
bool sym_ed_is_library_modified( const std::string& aLibrary )
{
    SYMBOL_EDIT_FRAME*          frame = require_symbol_edit_frame();
    LIB_SYMBOL_LIBRARY_MANAGER& mgr   = frame->GetLibManager();

    wxString lib = wxString::FromUTF8( aLibrary.c_str() );

    if( !mgr.LibraryExists( lib, /*aCheckEnabled*/ false ) )
        throw std::runtime_error(
            std::string( "library '" ) + aLibrary + "' is not loaded" );

    return mgr.IsLibraryModified( lib );
}

} // anon


// Registered at kiface-load time by eeschema/api/klicad_kiface_register.cpp.
// PYBIND11_EMBEDDED_MODULE can't be used here — see klicad_kiface_register.h
// for the explanation.
void klicad_register_symbol_editor_bindings( py::module_& m )
{
    m.doc() = "KliCAD symbol library editor binding.  Drives the running "
              "SYMBOL_EDIT_FRAME and its LIB_SYMBOL_LIBRARY_MANAGER for "
              "programmatic symbol-library editing: enumerate libraries / "
              "symbols, inspect a symbol's pins and metadata, create new "
              "root symbols, load a symbol into the editor canvas, save / "
              "save-all / revert, and delete symbols.\n\n"
              "Spawns FRAME_SCH_SYMBOL_EDITOR via KIWAY::Player if the "
              "editor isn't already open.  Changes that mutate library "
              "state (create_symbol, delete_symbol) only update the "
              "library manager's in-memory buffer — call save_current or "
              "save_all to flush to disk.";

    m.def( "list_loaded_libraries", &sym_ed_list_loaded_libraries,
           R"DOC(Return a list of library nicknames currently loaded in the
SYMBOL_EDIT_FRAME's library manager.  Sourced from
LIB_SYMBOL_LIBRARY_MANAGER::GetLibraryNames().

Spawns the symbol editor if it isn't already open.
)DOC" );

    m.def( "list_symbols_in_library", &sym_ed_list_symbols_in_library,
           py::arg( "library" ),
           R"DOC(Return a list of symbol names in the given library nickname.

library: the library nickname (NOT the full path).  Must be in the list
         returned by list_loaded_libraries().

Sourced from LIB_SYMBOL_LIBRARY_MANAGER::GetSymbolNames(..., SYMBOL_NAME_FILTER::ALL),
so derived (alias) symbols are included alongside root symbols.

Raises RuntimeError if the library isn't loaded.
)DOC" );

    m.def( "get_symbol_info", &sym_ed_get_symbol_info,
           py::arg( "lib_id" ),
           R"DOC(Return a dict describing the symbol at 'LibName:SymbolName'.

Keys: library, name, lib_id, found (bool).  When found is True, also:
unit_count, pin_count, is_root, is_power, is_multi_unit, reference (RefDes
prefix), description, keywords.

Raises ValueError on lib_id parse failure.  Returns found=False if the
library exists but the symbol doesn't.
)DOC" );

    m.def( "list_pins", &sym_ed_list_pins,
           py::arg( "lib_id" ),
           R"DOC(Return a list of pin dicts for the symbol at 'LibName:SymbolName'.

Each dict has: name, number, length, electrical_type, x_nm, y_nm.
Positions are in KiCad internal units (nanometers).

Raises RuntimeError if the library isn't loaded or the symbol isn't
found.  Raises ValueError on lib_id parse failure.
)DOC" );

    m.def( "load_symbol", &sym_ed_load_symbol,
           py::arg( "lib_id" ),
           R"DOC(Load 'LibName:SymbolName' into the symbol editor canvas
(equivalent to clicking the symbol in the library tree).  Always loads
unit 1, body style 1.

Returns {ok: bool, lib_id: str, error?: str}.
)DOC" );

    m.def( "get_current_symbol", &sym_ed_get_current_symbol,
           R"DOC(Return the info dict for the symbol currently loaded in the
editor canvas, or None if no symbol is loaded.

Same keys as get_symbol_info().
)DOC" );

    m.def( "create_symbol", &sym_ed_create_symbol,
           py::arg( "library" ),
           py::arg( "name" ),
           py::arg( "reference" ) = std::string( "U" ),
           py::arg( "unit_count" ) = 1,
           py::arg( "power_symbol" ) = false,
           R"DOC(Create a new root LIB_SYMBOL in the given library buffer.

library: library nickname (must be loaded and writable).
name:    new symbol name (must not already exist in the library).
reference: RefDes prefix (e.g. 'R', 'U', 'D').  Defaults to 'U'.
unit_count: number of units per package.  Defaults to 1.
power_symbol: if True, create as a power symbol (hides pin numbers,
              excludes from BoM/board).  Defaults to False.

The new symbol exists only in the library manager's in-memory buffer
until you call save_current() or save_all().

Returns {ok: bool, lib_id: str, error?: str}.  Raises RuntimeError if
the library isn't loaded or is read-only.
)DOC" );

    m.def( "save_current", &sym_ed_save_current,
           R"DOC(Save the currently selected symbol / library to disk
(SYMBOL_EDIT_FRAME::Save).

Returns {ok: bool, content_modified: bool}.  content_modified will be
True if other unsaved changes remain.
)DOC" );

    m.def( "save_all", &sym_ed_save_all,
           R"DOC(Save every modified symbol and library to disk
(SYMBOL_EDIT_FRAME::SaveAll).

Returns {ok: bool, content_modified: bool}.
)DOC" );

    m.def( "revert_current", &sym_ed_revert_current,
           R"DOC(Discard unsaved changes to the currently selected symbol /
library (SYMBOL_EDIT_FRAME::Revert).  Skips the GUI confirmation dialog.

Returns {ok: bool}.
)DOC" );

    m.def( "delete_symbol", &sym_ed_delete_symbol,
           py::arg( "lib_id" ),
           R"DOC(Delete the symbol at 'LibName:SymbolName' from its library
buffer (LIB_SYMBOL_LIBRARY_MANAGER::RemoveSymbol).

The deletion only takes effect in memory — call save_current() or
save_all() to flush to disk.

Returns {ok: bool, lib_id: str, error?: str}.
)DOC" );

    m.def( "is_content_modified", &sym_ed_is_content_modified,
           R"DOC(Return True if any symbol or library has unsaved changes
(SYMBOL_EDIT_FRAME::IsContentModified).
)DOC" );

    m.def( "is_library_modified", &sym_ed_is_library_modified,
           py::arg( "library" ),
           R"DOC(Return True if the given library nickname has unsaved
changes in the library manager buffer.

Raises RuntimeError if the library isn't loaded.
)DOC" );
}
