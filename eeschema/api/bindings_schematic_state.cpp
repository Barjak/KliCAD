/*
 * KliCAD subsystem binding: SCHEMATIC state (direct creation).
 *
 * Exposes direct SCH_ITEM creation as klicad_native_schematic_state.* —
 * placing symbols, drawing wires, adding labels and junctions.  Fills the
 * gap left by the typed-RPC CreateItems handler, which today refuses to
 * round-trip schematic items.
 *
 * Pattern (follows BINDING_PATTERN.md):
 *   1. Walk wxTopLevelWindows for the live SCH_EDIT_FRAME (spawn via
 *      KIWAY::Player(FRAME_SCH, true) if missing).
 *   2. Wrap each mutation in a SCH_COMMIT(frame) so the user can undo.
 *   3. Construct the SCH_ITEM, set its properties (positions in nm =
 *      mm * 1e6), Append it to the current screen, and Commit/Push.
 *   4. Refresh the canvas via EDA_DRAW_FRAME::RefreshCanvas().
 *   5. Return { ok, kiid, error? } as a py::dict.
 *
 * NOT a JOB_*-backed binding — the create path is direct C++ against
 * SCH_SCREEN / SCH_SYMBOL / SCH_LINE / SCH_LABEL / SCH_JUNCTION.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <eda_draw_frame.h>
#include <eda_item.h>
#include <kiid.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <layer_ids.h>
#include <base_units.h>
#include <math/vector2d.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_sheet_path.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_field.h>
#include <sch_pin.h>
#include <sch_commit.h>
#include <sch_sheet_path.h>
#include <template_fieldnames.h>

#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Position scaling: mm <-> schematic IU.  Use schIUScale (from base_units.h),
// NOT a hand-rolled constant — schIUScale.mmToIU() rounds half-up, while
// `(int)(x_mm * 10000)` truncates.  The truncation form silently placed
// labels 1 IU off the pin for any mm value whose binary-float product with
// 1e4 fell on the "wrong side" of a hundredth (e.g. 194.92 -> 1949199.999...
// -> truncated to 1949199 IU instead of 1949200), making the netlist
// generator leave that pin dangling.  See test_smoke_oscillator demo.
//
// History: an even earlier version used 1e6 (PCB scale) here and placed
// everything 100x out of canvas.  Fixed to 1e4 in commit 06ac615c21.  This
// commit fixes the residual rounding error.

inline VECTOR2I mm_to_iu( double x_mm, double y_mm )
{
    return VECTOR2I( schIUScale.mmToIU( x_mm ), schIUScale.mmToIU( y_mm ) );
}


// Per-TU helper name (unique vs existing bindings: see find_live_kiway,
// find_live_kiway_for_erc, find_live_kiway_for_gui, ...).
KIWAY* find_live_kiway_for_schematic_state()
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


// Walk wxTopLevelWindows for an SCH_EDIT_FRAME (FRAME_SCH).  We can't
// dynamic_cast<SCH_EDIT_FRAME*> safely across kiface boundaries — the
// typeinfo lives in _eeschema.kiface.bundle and may not be visible to
// libkicommon at link time.  Instead, use EDA_BASE_FRAME::GetFrameType()
// to identify FRAME_SCH, then static_cast (safe given the identifier).
SCH_EDIT_FRAME* find_sch_edit_frame_for_state()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_SCH )
            return static_cast<SCH_EDIT_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live SCH_EDIT_FRAME, spawning eeschema if necessary.  Throws on
// any failure so the caller doesn't have to null-check.
SCH_EDIT_FRAME* require_sch_edit_frame()
{
    if( SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_state() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_schematic_state();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(schematic_state needs to spawn the eeschema frame)" );
    }

    // Spawn eeschema; this creates the SCH_EDIT_FRAME and an empty/default
    // schematic if no project is open.
    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_state();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain SCH_EDIT_FRAME after KIWAY::Player(FRAME_SCH, true)" );
    }

    return frame;
}


// Refresh the schematic canvas after a mutation so the user sees the change
// immediately.  EDA_DRAW_FRAME::RefreshCanvas() is the high-level hook.
void refresh_sch_canvas( SCH_EDIT_FRAME* aFrame )
{
    if( aFrame && aFrame->GetCanvas() )
        aFrame->GetCanvas()->Refresh();
}


// ──────────────────────────────────────────────────────────────────────────
// add_wire
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_wire( double start_x_mm, double start_y_mm,
                               double end_x_mm,   double end_y_mm )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCH_SCREEN*     screen = frame->GetScreen();

    if( !screen )
        throw std::runtime_error( "SCH_EDIT_FRAME has no active SCH_SCREEN" );

    VECTOR2I start = mm_to_iu( start_x_mm, start_y_mm );
    VECTOR2I end   = mm_to_iu( end_x_mm,   end_y_mm   );

    SCH_LINE* wire = new SCH_LINE( start, LAYER_WIRE );
    wire->SetEndPoint( end );

    {
        SCH_COMMIT commit( frame );
        frame->AddToScreen( wire, screen );
        commit.Added( wire, screen );
        commit.Push( wxT( "KliCAD: add_wire" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = wire->m_Uuid.AsStdString();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_junction
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_junction( double x_mm, double y_mm )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCH_SCREEN*     screen = frame->GetScreen();

    if( !screen )
        throw std::runtime_error( "SCH_EDIT_FRAME has no active SCH_SCREEN" );

    VECTOR2I pos = mm_to_iu( x_mm, y_mm );

    // SCH_EDIT_FRAME::AddJunction is declared in the header but has no
    // corresponding definition in this build — construct directly instead.
    SCH_JUNCTION* j = new SCH_JUNCTION( pos );
    {
        SCH_COMMIT commit( frame );
        frame->AddToScreen( j, screen );
        commit.Added( j, screen );
        commit.Push( wxT( "KliCAD: add_junction" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = j->m_Uuid.AsStdString();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_label
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_label( double x_mm, double y_mm,
                                const std::string& text,
                                const std::string& kind )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCH_SCREEN*     screen = frame->GetScreen();

    if( !screen )
        throw std::runtime_error( "SCH_EDIT_FRAME has no active SCH_SCREEN" );

    VECTOR2I pos = mm_to_iu( x_mm, y_mm );
    wxString wxText = wxString::FromUTF8( text.c_str() );

    SCH_LABEL_BASE* label = nullptr;

    if( kind == "local" || kind.empty() )
    {
        label = new SCH_LABEL( pos, wxText );
    }
    else if( kind == "global" )
    {
        label = new SCH_GLOBALLABEL( pos, wxText );
    }
    else if( kind == "hierarchical" || kind == "hier" )
    {
        label = new SCH_HIERLABEL( pos, wxText );
    }
    else
    {
        throw std::invalid_argument(
            "kind must be one of: 'local', 'global', 'hierarchical' (got '"
            + kind + "')" );
    }

    {
        SCH_COMMIT commit( frame );
        frame->AddToScreen( label, screen );
        commit.Added( label, screen );
        commit.Push( wxT( "KliCAD: add_label" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = label->m_Uuid.AsStdString();
    result[ "kind" ] = kind.empty() ? std::string( "local" ) : kind;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_symbol
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_symbol( const std::string& lib_id_str,
                                 const std::string& ref_des,
                                 double x_mm, double y_mm,
                                 int unit )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCH_SCREEN*     screen = frame->GetScreen();

    if( !screen )
        throw std::runtime_error( "SCH_EDIT_FRAME has no active SCH_SCREEN" );

    LIB_ID libId;

    if( libId.Parse( lib_id_str ) >= 0 )
    {
        throw std::invalid_argument(
            "lib_id parse failed; expected 'LibName:SymbolName' (got '"
            + lib_id_str + "')" );
    }

    // Resolve via SYMBOL_LIBRARY_ADAPTER from PROJECT_SCH; this is the
    // post-refactor replacement for the old SchSymbolLibTable accessor.
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &frame->Prj() );

    if( !adapter )
    {
        throw std::runtime_error(
            "PROJECT_SCH::SymbolLibAdapter returned nullptr — is a project loaded?" );
    }

    LIB_SYMBOL* libSymbol = nullptr;

    try
    {
        libSymbol = adapter->LoadSymbol( libId );
    }
    catch( const std::exception& ex )
    {
        throw std::runtime_error(
            std::string( "LoadSymbol failed for '" ) + lib_id_str + "': " + ex.what() );
    }
    catch( ... )
    {
        throw std::runtime_error(
            std::string( "LoadSymbol failed for '" ) + lib_id_str
            + "': unknown exception (IO_ERROR?)" );
    }

    if( !libSymbol )
    {
        py::dict result;
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "lib_id '" ) + lib_id_str
                            + "' not found in project symbol library table";
        return result;
    }

    VECTOR2I pos = mm_to_iu( x_mm, y_mm );

    // SCH_SYMBOL ctor wraps the lib_symbol (clones internally via SetLibSymbol).
    // Pass the current sheet so the instance bookkeeping is set up correctly.
    SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, libId, &frame->GetCurrentSheet(),
                                          unit, /*aBodyStyle*/ 0, pos,
                                          &frame->Schematic() );

    if( !ref_des.empty() )
        symbol->SetRef( &frame->GetCurrentSheet(), wxString::FromUTF8( ref_des.c_str() ) );

    {
        SCH_COMMIT commit( frame );
        frame->AddToScreen( symbol, screen );
        commit.Added( symbol, screen );
        commit.Push( wxT( "KliCAD: add_symbol" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]      = true;
    result[ "kiid" ]    = symbol->m_Uuid.AsStdString();
    result[ "lib_id" ]  = lib_id_str;
    result[ "ref_des" ] = ref_des;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// Helpers for the new symbol-edit primitives.  Find a placed SCH_SYMBOL by
// its KIID anywhere in the schematic hierarchy (walks every sheet's screen).
// ──────────────────────────────────────────────────────────────────────────
SCH_SYMBOL* find_symbol_by_kiid( SCHEMATIC& aSch, const wxString& aKiidStr )
{
    KIID needle;
    try
    {
        needle = KIID( aKiidStr );
    }
    catch( ... )
    {
        return nullptr;
    }

    for( const SCH_SHEET_PATH& path : aSch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            if( item->m_Uuid == needle )
                return static_cast<SCH_SYMBOL*>( item );
        }
    }
    return nullptr;
}


// Resolve which sheet path contains a given placed SCH_SYMBOL.  Needed for
// the per-instance setters (SetRef / SetValueFieldText accept an instance).
SCH_SHEET_PATH find_sheet_for_symbol( SCHEMATIC& aSch, SCH_SYMBOL* aSym )
{
    for( const SCH_SHEET_PATH& path : aSch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            if( item == aSym )
                return path;
        }
    }
    return aSch.CurrentSheet();
}


// ──────────────────────────────────────────────────────────────────────────
// set_symbol_value
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_set_symbol_value( const std::string& kiid_str,
                                     const std::string& new_value )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    SCH_SYMBOL* sym = find_symbol_by_kiid( sch, wxString::FromUTF8( kiid_str.c_str() ) );

    if( !sym )
        throw std::runtime_error( "set_symbol_value: no symbol with kiid '" + kiid_str + "'" );

    SCH_SHEET_PATH path = find_sheet_for_symbol( sch, sym );
    SCH_SCREEN*    screen = path.LastScreen();

    {
        SCH_COMMIT commit( frame );
        commit.Modify( sym, screen );
        sym->SetValueFieldText( wxString::FromUTF8( new_value.c_str() ), &path );
        commit.Push( wxT( "KliCAD: set_symbol_value" ) );
    }

    if( frame->GetCanvas() )
        frame->GetCanvas()->Refresh();

    py::dict d;
    d[ "ok" ]    = true;
    d[ "kiid" ]  = kiid_str;
    d[ "value" ] = new_value;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// set_symbol_field — generic field setter.  Creates a USER field if the
// named field doesn't exist.  Used for SPICE annotations (Sim_Device,
// Sim_Model, Sim_Pins, etc.) as well as Footprint, Datasheet, custom.
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_set_symbol_field( const std::string& kiid_str,
                                     const std::string& field_name,
                                     const std::string& new_value )
{
    if( field_name.empty() )
        throw std::invalid_argument( "set_symbol_field: field_name is empty" );

    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    SCH_SYMBOL* sym = find_symbol_by_kiid( sch, wxString::FromUTF8( kiid_str.c_str() ) );

    if( !sym )
        throw std::runtime_error( "set_symbol_field: no symbol with kiid '" + kiid_str + "'" );

    SCH_SHEET_PATH path = find_sheet_for_symbol( sch, sym );
    SCH_SCREEN*    screen = path.LastScreen();

    wxString fname = wxString::FromUTF8( field_name.c_str() );
    wxString fval  = wxString::FromUTF8( new_value.c_str() );

    bool created = false;

    {
        SCH_COMMIT commit( frame );
        commit.Modify( sym, screen );

        SCH_FIELD* field = sym->GetField( fname );

        if( field )
        {
            field->SetText( fval );
        }
        else
        {
            // Create a USER field on this symbol.  SCH_FIELD ctor takes
            // (parent, type, name).  USER fields participate in netlist
            // generation just like mandatory ones.
            SCH_FIELD newField( sym, FIELD_T::USER, fname );
            newField.SetText( fval );
            sym->AddField( newField );
            created = true;
        }

        commit.Push( wxT( "KliCAD: set_symbol_field" ) );
    }

    if( frame->GetCanvas() )
        frame->GetCanvas()->Refresh();

    py::dict d;
    d[ "ok" ]      = true;
    d[ "kiid" ]    = kiid_str;
    d[ "field" ]   = field_name;
    d[ "value" ]   = new_value;
    d[ "created" ] = created;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// set_symbol_rotation — absolute rotation in degrees (0/90/180/270).
// Mirror flags are left untouched.
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_set_symbol_rotation( const std::string& kiid_str, int degrees )
{
    int normalized = ( ( degrees % 360 ) + 360 ) % 360;

    int orient_flag;
    switch( normalized )
    {
    case 0:   orient_flag = SYM_ORIENT_0;   break;
    case 90:  orient_flag = SYM_ORIENT_90;  break;
    case 180: orient_flag = SYM_ORIENT_180; break;
    case 270: orient_flag = SYM_ORIENT_270; break;
    default:
        throw std::invalid_argument(
            "set_symbol_rotation: degrees must be a multiple of 90 "
            "(got " + std::to_string( degrees ) + ")" );
    }

    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    SCH_SYMBOL* sym = find_symbol_by_kiid( sch, wxString::FromUTF8( kiid_str.c_str() ) );

    if( !sym )
        throw std::runtime_error( "set_symbol_rotation: no symbol with kiid '" + kiid_str + "'" );

    SCH_SHEET_PATH path = find_sheet_for_symbol( sch, sym );
    SCH_SCREEN*    screen = path.LastScreen();

    // Preserve mirror flags from the current orientation while replacing
    // the rotation component.
    int mirror = sym->GetOrientation() & ( SYM_MIRROR_X | SYM_MIRROR_Y );

    {
        SCH_COMMIT commit( frame );
        commit.Modify( sym, screen );
        sym->SetOrientation( orient_flag | mirror );
        commit.Push( wxT( "KliCAD: set_symbol_rotation" ) );
    }

    if( frame->GetCanvas() )
        frame->GetCanvas()->Refresh();

    py::dict d;
    d[ "ok" ]       = true;
    d[ "kiid" ]     = kiid_str;
    d[ "rotation" ] = normalized;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_symbol_pin_position — return world-coordinate (mm) position of a pin
// on a placed symbol, identified by pin number OR pin name.  Returns the
// list of available pins on failure so callers can diagnose.
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_get_symbol_pin_position( const std::string& kiid_str,
                                            const std::string& pin_id )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    SCH_SYMBOL* sym = find_symbol_by_kiid( sch, wxString::FromUTF8( kiid_str.c_str() ) );

    if( !sym )
        throw std::runtime_error( "get_symbol_pin_position: no symbol with kiid '" + kiid_str + "'" );

    wxString needle = wxString::FromUTF8( pin_id.c_str() );
    SCH_SHEET_PATH path = find_sheet_for_symbol( sch, sym );

    std::vector<SCH_PIN*> pins = sym->GetPins( &path );

    // First pass: exact match on pin number.
    for( SCH_PIN* pin : pins )
    {
        if( pin->GetNumber() == needle )
        {
            VECTOR2I pos = pin->GetPosition();

            py::dict d;
            d[ "ok" ]         = true;
            d[ "kiid" ]       = kiid_str;
            d[ "pin_number" ] = std::string( pin->GetNumber().utf8_str() );
            d[ "pin_name" ]   = std::string( pin->GetName().utf8_str() );
            d[ "x_mm" ]       = schIUScale.IUTomm( pos.x );
            d[ "y_mm" ]       = schIUScale.IUTomm( pos.y );
            return d;
        }
    }

    // Second pass: match on pin name.
    for( SCH_PIN* pin : pins )
    {
        if( pin->GetName() == needle )
        {
            VECTOR2I pos = pin->GetPosition();

            py::dict d;
            d[ "ok" ]         = true;
            d[ "kiid" ]       = kiid_str;
            d[ "pin_number" ] = std::string( pin->GetNumber().utf8_str() );
            d[ "pin_name" ]   = std::string( pin->GetName().utf8_str() );
            d[ "x_mm" ]       = schIUScale.IUTomm( pos.x );
            d[ "y_mm" ]       = schIUScale.IUTomm( pos.y );
            return d;
        }
    }

    // Not found — return the available pins for diagnosis.
    py::list available;
    for( SCH_PIN* pin : pins )
    {
        py::dict p;
        p[ "number" ] = std::string( pin->GetNumber().utf8_str() );
        p[ "name" ]   = std::string( pin->GetName().utf8_str() );
        available.append( p );
    }

    py::dict d;
    d[ "ok" ]             = false;
    d[ "error" ]          = std::string( "pin '" ) + pin_id + "' not found on symbol";
    d[ "available_pins" ] = available;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_symbol_bbox — return the body-only bounding box of a placed symbol
// in world mm.  "Body-only" = excludes pin extents and text fields, so
// the result is suitable as an obstacle rectangle for wire routing
// (wires can legitimately approach via pin endpoints; the pin lines
// themselves shouldn't count as keep-outs).
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_get_symbol_bbox( const std::string& kiid_str )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    SCH_SYMBOL* sym = find_symbol_by_kiid( sch, wxString::FromUTF8( kiid_str.c_str() ) );

    if( !sym )
        throw std::runtime_error( "get_symbol_bbox: no symbol with kiid '" + kiid_str + "'" );

    BOX2I box = sym->GetBodyBoundingBox();

    py::dict d;
    d[ "ok" ]      = true;
    d[ "kiid" ]    = kiid_str;
    d[ "x_mm" ]    = schIUScale.IUTomm( box.GetLeft() );
    d[ "y_mm" ]    = schIUScale.IUTomm( box.GetTop() );
    d[ "w_mm" ]    = schIUScale.IUTomm( box.GetWidth() );
    d[ "h_mm" ]    = schIUScale.IUTomm( box.GetHeight() );
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_sheet_count
// ──────────────────────────────────────────────────────────────────────────
int sch_state_get_sheet_count()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    if( !sch.IsValid() )
        return 0;

    return static_cast<int>( sch.Hierarchy().size() );
}


// ──────────────────────────────────────────────────────────────────────────
// open_schematic
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_open_schematic( const std::string& aPath )
{
    if( aPath.empty() )
        throw std::invalid_argument( "open_schematic: path is empty" );

    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();
    wxString        wxPath = wxString::FromUTF8( aPath.c_str() );

    // Same defensive shortcut as pcb_state.open_board.  Re-opening the
    // currently-loaded schematic via OpenProjectFiles has the same
    // dangling-nested-settings risk on the PCB side; play it safe and
    // no-op when the path matches.
    if( sch.IsValid() && sch.GetFileName() == wxPath )
    {
        py::dict d;
        d[ "ok" ]          = true;
        d[ "path" ]        = aPath;
        d[ "sheet_count" ] = sch_state_get_sheet_count();
        d[ "note" ]        = std::string(
            "schematic already open at this path; no reload performed" );
        return d;
    }

    std::vector<wxString> files = { wxPath };
    bool ok = frame->OpenProjectFiles( files, /*aCtl*/ 0 );

    if( frame->GetCanvas() )
        frame->GetCanvas()->Refresh();

    py::dict d;
    d[ "ok" ]    = ok;
    d[ "path" ]  = aPath;
    d[ "sheet_count" ] = sch_state_get_sheet_count();
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// save_schematic
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_save_schematic()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    if( !sch.IsValid() )
        throw std::runtime_error( "save_schematic: no SCHEMATIC is currently open" );

    bool ok = frame->SaveProject( /*aSaveAs*/ false );

    py::dict d;
    d[ "ok" ] = ok;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_items_summary
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_get_items_summary()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    int symbols    = 0;
    int wires      = 0;
    int junctions  = 0;
    int labels     = 0;     // local SCH_LABEL only
    int glabels    = 0;
    int hlabels    = 0;
    int lines      = 0;     // total line-items (any layer)
    int sheets     = 0;

    if( sch.IsValid() )
    {
        for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
        {
            SCH_SCREEN* screen = path.LastScreen();

            if( !screen )
                continue;

            for( SCH_ITEM* item : screen->Items() )
            {
                switch( item->Type() )
                {
                case SCH_SYMBOL_T:        ++symbols;   break;
                case SCH_JUNCTION_T:      ++junctions; break;
                case SCH_LABEL_T:         ++labels;    break;
                case SCH_GLOBAL_LABEL_T:  ++glabels;   break;
                case SCH_HIER_LABEL_T:    ++hlabels;   break;
                case SCH_LINE_T:
                {
                    ++lines;
                    if( item->GetLayer() == LAYER_WIRE )
                        ++wires;
                    break;
                }
                case SCH_SHEET_T:         ++sheets;    break;
                default: break;
                }
            }
        }
    }

    py::dict d;
    d[ "symbols" ]    = symbols;
    d[ "wires" ]      = wires;
    d[ "lines" ]      = lines;
    d[ "junctions" ]  = junctions;
    d[ "labels" ]     = labels;
    d[ "glabels" ]    = glabels;
    d[ "hlabels" ]    = hlabels;
    d[ "sheets" ]     = sheets;
    d[ "sheet_count" ] = sch_state_get_sheet_count();
    return d;
}

// ──────────────────────────────────────────────────────────────────────────
// list_symbols — enumerate placed symbols (for diff/apply idempotency)
// ──────────────────────────────────────────────────────────────────────────
py::list sch_state_list_symbols( const std::string& sheet_path = std::string() )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();
    py::list        out;

    if( !sch.IsValid() )
        return out;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        // Per-sheet scoping: when sheet_path is non-empty, only return
        // rows for items on that exact path.  "" = whole hierarchy.
        std::string this_path = std::string( path.Path().AsString().utf8_str() );
        if( !sheet_path.empty() && this_path != sheet_path )
            continue;

        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() != SCH_SYMBOL_T )
                continue;

            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            VECTOR2I    pos = sym->GetPosition();
            int         orient = sym->GetOrientation();
            py::dict    d;
            d[ "kiid" ]        = sym->m_Uuid.AsStdString();
            d[ "ref" ]         = std::string( sym->GetRef( &path ).utf8_string() );
            d[ "lib_id" ]      = std::string( sym->GetLibId().Format().wx_str().utf8_string() );
            d[ "sheet_path" ]  = this_path;
            d[ "x_mm" ]        = schIUScale.IUTomm( pos.x );
            d[ "y_mm" ]        = schIUScale.IUTomm( pos.y );
            d[ "orientation" ] = orient;
            d[ "mirror_x" ]    = static_cast<bool>( orient & SYMBOL_ORIENTATION_T::SYM_MIRROR_X );
            d[ "mirror_y" ]    = static_cast<bool>( orient & SYMBOL_ORIENTATION_T::SYM_MIRROR_Y );

            // Value + Footprint as their own keys (mandatory fields).
            const SCH_FIELD* valueF     = sym->GetField( FIELD_T::VALUE );
            const SCH_FIELD* footprintF = sym->GetField( FIELD_T::FOOTPRINT );
            d[ "value" ]     = valueF
                                   ? std::string( valueF->GetText().utf8_string() )
                                   : std::string();
            d[ "footprint" ] = footprintF
                                   ? std::string( footprintF->GetText().utf8_string() )
                                   : std::string();

            // Every field — mandatory + user — keyed by field name.  Sim.*
            // fields and any custom user fields land here so diff/apply
            // tests can verify they're preserved (or updated) as expected.
            py::dict fields;
            for( const SCH_FIELD& f : sym->GetFields() )
            {
                fields[ py::str( std::string( f.GetName().utf8_string() ) ) ]
                    = std::string( f.GetText().utf8_string() );
            }
            d[ "fields" ] = fields;

            out.append( d );
        }
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_labels — enumerate local/global/hierarchical labels.
//
// klicad-python's per-sheet diff uses this to preserve hier-label
// positions across diff iterations (so user-positioned port anchors
// survive).  Returns rows {kiid, name, kind, x_mm, y_mm, sheet_path}.
// ──────────────────────────────────────────────────────────────────────────
py::list sch_state_list_labels( const std::string& sheet_path = std::string() )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();
    py::list        out;

    if( !sch.IsValid() )
        return out;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        std::string this_path = std::string( path.Path().AsString().utf8_str() );
        if( !sheet_path.empty() && this_path != sheet_path )
            continue;

        SCH_SCREEN* screen = path.LastScreen();
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items() )
        {
            const char* kind = nullptr;
            switch( item->Type() )
            {
            case SCH_LABEL_T:        kind = "local";        break;
            case SCH_GLOBAL_LABEL_T: kind = "global";       break;
            case SCH_HIER_LABEL_T:   kind = "hierarchical"; break;
            default:                 continue;
            }

            SCH_LABEL_BASE* lbl = static_cast<SCH_LABEL_BASE*>( item );
            VECTOR2I        pos = lbl->GetPosition();

            py::dict d;
            d[ "kiid" ]       = lbl->m_Uuid.AsStdString();
            d[ "name" ]       = std::string( lbl->GetText().utf8_string() );
            d[ "kind" ]       = std::string( kind );
            d[ "sheet_path" ] = this_path;
            d[ "x_mm" ]       = schIUScale.IUTomm( pos.x );
            d[ "y_mm" ]       = schIUScale.IUTomm( pos.y );
            out.append( d );
        }
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// delete_by_kiid — remove a single SCH_ITEM by its UUID
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_delete_by_kiid( const std::string& kiid_str )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCHEMATIC&      sch    = frame->Schematic();
    KIID            target( wxString::FromUTF8( kiid_str.c_str() ) );
    SCH_ITEM*       found        = nullptr;
    SCH_SCREEN*     found_screen = nullptr;
    SCH_SHEET*      pin_parent   = nullptr;     // non-null when found is a sheet pin

    if( sch.IsValid() )
    {
        for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
        {
            SCH_SCREEN* screen = path.LastScreen();

            if( !screen )
                continue;

            // First check direct screen items (symbols, wires, labels,
            // junctions, sheet instances).
            for( SCH_ITEM* item : screen->Items() )
            {
                if( item->m_Uuid == target )
                {
                    found        = item;
                    found_screen = screen;
                    break;
                }
            }

            if( found )
                break;

            // SCH_SHEET_PINs aren't direct screen children — they're owned
            // by their parent SCH_SHEET.  Walk every sheet on this screen
            // and check its pins.  Required so per-pin sheet diff can
            // delete individual pins.
            for( SCH_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );
                for( SCH_SHEET_PIN* pin : sheet->GetPins() )
                {
                    if( pin && pin->m_Uuid == target )
                    {
                        found        = pin;
                        found_screen = screen;
                        pin_parent   = sheet;
                        break;
                    }
                }
                if( found )
                    break;
            }

            if( found )
                break;
        }
    }

    py::dict result;

    if( !found )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "no item with kiid " ) + kiid_str;
        return result;
    }

    {
        SCH_COMMIT commit( frame );

        if( pin_parent )
        {
            // SCH_SHEET_PIN — modify the parent sheet and remove the pin
            // from its pin list.  RemoveFromScreen wouldn't work since
            // pins aren't on the screen directly.
            commit.Modify( pin_parent, found_screen );
            pin_parent->RemovePin( static_cast<SCH_SHEET_PIN*>( found ) );
        }
        else
        {
            commit.Removed( found, found_screen );
            frame->RemoveFromScreen( found, found_screen );
        }

        commit.Push( wxT( "KliCAD: delete_by_kiid" ) );
    }

    refresh_sch_canvas( frame );

    result[ "ok" ]   = true;
    result[ "kiid" ] = kiid_str;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// clear_routing — bulk-delete wires + labels + junctions + power-flag symbols
// (preserves real component symbols).
//
// Used by to_schematic(mode="diff") between iterations: real component
// symbols + their positions/rotations/custom fields survive, but every
// wire / label / junction / power-flag (#PWR_* refs) is removed so the
// router + power placement can re-emit them clean against the current
// Circuit topology — no accumulation across runs.
// ──────────────────────────────────────────────────────────────────────────
py::dict sch_state_clear_routing( const std::string& sheet_path = std::string(),
                                   bool keep_hier_labels = false )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();
    int             n     = 0;

    if( sch.IsValid() )
    {
        for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
        {
            std::string this_path = std::string( path.Path().AsString().utf8_str() );
            if( !sheet_path.empty() && this_path != sheet_path )
                continue;

            SCH_SCREEN* screen = path.LastScreen();

            if( !screen )
                continue;

            std::vector<SCH_ITEM*> to_remove;

            for( SCH_ITEM* item : screen->Items() )
            {
                KICAD_T t = item->Type();

                if( t == SCH_JUNCTION_T || t == SCH_LABEL_T
                    || t == SCH_GLOBAL_LABEL_T )
                {
                    to_remove.push_back( item );
                }
                else if( t == SCH_HIER_LABEL_T && !keep_hier_labels )
                {
                    // Optionally spared so port anchors can survive
                    // diff iterations (klicad-python H3 contract).
                    to_remove.push_back( item );
                }
                else if( t == SCH_LINE_T && item->GetLayer() == LAYER_WIRE )
                {
                    to_remove.push_back( item );
                }
                else if( t == SCH_SYMBOL_T )
                {
                    // Power-flag symbols use refs prefixed with '#' — the
                    // KiCad convention for non-netlist refs.  They're
                    // routing decoration (one per net), not parts, and
                    // get re-emitted every pass by _place_power_symbols.
                    SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
                    wxString    ref = sym->GetRef( &path );

                    if( !ref.IsEmpty() && ref[0] == wxT( '#' ) )
                        to_remove.push_back( item );
                }
            }

            if( to_remove.empty() )
                continue;

            SCH_COMMIT commit( frame );

            for( SCH_ITEM* item : to_remove )
            {
                commit.Removed( item, screen );
                frame->RemoveFromScreen( item, screen );
                ++n;
            }

            commit.Push( wxT( "KliCAD: clear_routing" ) );
        }
    }

    refresh_sch_canvas( frame );

    py::dict d;
    d[ "ok" ]      = true;
    d[ "removed" ] = n;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// add_sheet — place a SCH_SHEET on the current sheet (used by H2 hierarchy).
//
// Creates a fresh empty SCH_SCREEN for the new sheet so subsequent
// set_current_sheet + add_symbol calls have somewhere to land.  The
// Python side is expected to have already laid down the corresponding
// child .kicad_sch stub on disk; save_schematic will overwrite it
// with the in-memory SCH_SCREEN's content.
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_sheet(
        const std::string& name,
        const std::string& filename,
        double x_mm, double y_mm,
        double w_mm, double h_mm )
{
    SCH_EDIT_FRAME* frame         = require_sch_edit_frame();
    SCHEMATIC&      sch           = frame->Schematic();
    SCH_SCREEN*     parent_screen = frame->GetScreen();

    if( !parent_screen )
        throw std::runtime_error( "add_sheet: SCH_EDIT_FRAME has no active screen" );

    VECTOR2I pos = mm_to_iu( x_mm, y_mm );
    VECTOR2I size( schIUScale.mmToIU( w_mm ), schIUScale.mmToIU( h_mm ) );

    SCH_SHEET* sheet = new SCH_SHEET( &frame->Schematic(), pos );
    sheet->SetSize( size );
    sheet->SetName( wxString::FromUTF8( name.c_str() ) );
    wxString fn = wxString::FromUTF8( filename.c_str() );
    sheet->SetFileName( fn );

    // Screen-sharing for multi-instance Sub-Circuits: if another SCH_SHEET
    // in the hierarchy already points at the same .kicad_sch filename,
    // share its SCH_SCREEN.  Edits to body content show up in every
    // instance.  This is KiCad's "complex hierarchy" semantic; the
    // alternative (each instance gets its own SCH_SCREEN) would race the
    // single backing file on save.
    SCH_SCREEN* shared_screen = nullptr;
    for( const SCH_SHEET_PATH& other_path : sch.Hierarchy() )
    {
        SCH_SCREEN* sc = other_path.LastScreen();
        if( !sc )
            continue;
        for( SCH_ITEM* item : sc->Items().OfType( SCH_SHEET_T ) )
        {
            SCH_SHEET* other = static_cast<SCH_SHEET*>( item );
            if( other->GetFileName() == fn )
            {
                shared_screen = other->GetScreen();
                break;
            }
        }
        if( shared_screen )
            break;
    }

    if( shared_screen )
    {
        sheet->SetScreen( shared_screen );
    }
    else
    {
        SCH_SCREEN* new_screen = new SCH_SCREEN( &frame->Schematic() );
        // Critical: SaveProject iterates SCH_SCREEN_S and writes each to
        // screen->GetFileName().  Without setting the screen's filename,
        // it stays empty and the save silently skips the child .kicad_sch.
        // The Python side is expected to have laid down a stub at
        // `<project_dir>/<filename>` first.
        wxFileName resolved( frame->Prj().AbsolutePath( fn ) );
        new_screen->SetFileName( resolved.GetFullPath() );
        sheet->SetScreen( new_screen );
    }

    {
        SCH_COMMIT commit( frame );
        frame->AddToScreen( sheet, parent_screen );
        commit.Added( sheet, parent_screen );
        commit.Push( wxT( "KliCAD: add_sheet" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]        = true;
    result[ "kiid" ]      = sheet->m_Uuid.AsStdString();
    result[ "name" ]      = name;
    result[ "file_name" ] = filename;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_sheet_pin — add a SCH_SHEET_PIN to an existing SCH_SHEET by uuid.
//
// `side` ∈ {"left","right","top","bottom"}.
// `shape` ∈ {"input","output","bidi"/"bidirectional","tristate"/"tri_state",
//            "passive"/"unspecified"}.  KliCAD treats all shapes equivalently
// in the netlist; the shape affects only the rendering glyph.
// ──────────────────────────────────────────────────────────────────────────
py::object sch_state_add_sheet_pin(
        const std::string& sheet_kiid_str,
        const std::string& name,
        const std::string& side,
        double x_mm, double y_mm,
        const std::string& shape = std::string( "bidi" ) )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    SCHEMATIC&      sch    = frame->Schematic();
    KIID            target( wxString::FromUTF8( sheet_kiid_str.c_str() ) );

    SCH_SHEET*  parent_sheet  = nullptr;
    SCH_SCREEN* parent_screen = nullptr;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();
        if( !screen )
            continue;
        for( SCH_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
        {
            if( item->m_Uuid == target )
            {
                parent_sheet  = static_cast<SCH_SHEET*>( item );
                parent_screen = screen;
                break;
            }
        }
        if( parent_sheet )
            break;
    }

    if( !parent_sheet )
        throw std::runtime_error(
            std::string( "add_sheet_pin: no SCH_SHEET with uuid '" )
            + sheet_kiid_str + "'" );

    SHEET_SIDE side_enum;
    if( side == "left" )        side_enum = SHEET_SIDE::LEFT;
    else if( side == "right" )  side_enum = SHEET_SIDE::RIGHT;
    else if( side == "top" )    side_enum = SHEET_SIDE::TOP;
    else if( side == "bottom" ) side_enum = SHEET_SIDE::BOTTOM;
    else
        throw std::invalid_argument(
            "add_sheet_pin: side must be one of 'left'/'right'/'top'/'bottom' "
            "(got '" + side + "')" );

    LABEL_FLAG_SHAPE shape_enum;
    if( shape == "input" )
        shape_enum = LABEL_FLAG_SHAPE::L_INPUT;
    else if( shape == "output" )
        shape_enum = LABEL_FLAG_SHAPE::L_OUTPUT;
    else if( shape == "bidi" || shape == "bidirectional" )
        shape_enum = LABEL_FLAG_SHAPE::L_BIDI;
    else if( shape == "tristate" || shape == "tri_state" )
        shape_enum = LABEL_FLAG_SHAPE::L_TRISTATE;
    else if( shape == "passive" || shape == "unspecified" )
        shape_enum = LABEL_FLAG_SHAPE::L_UNSPECIFIED;
    else
        throw std::invalid_argument(
            "add_sheet_pin: shape must be one of 'input'/'output'/'bidi'/"
            "'tristate'/'passive' (got '" + shape + "')" );

    VECTOR2I pos = mm_to_iu( x_mm, y_mm );

    SCH_SHEET_PIN* pin = new SCH_SHEET_PIN( parent_sheet, pos,
                                            wxString::FromUTF8( name.c_str() ) );
    pin->SetSide( side_enum );
    pin->SetShape( shape_enum );

    {
        SCH_COMMIT commit( frame );
        commit.Modify( parent_sheet, parent_screen );
        parent_sheet->AddPin( pin );
        commit.Push( wxT( "KliCAD: add_sheet_pin" ) );
    }

    refresh_sch_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = pin->m_Uuid.AsStdString();
    result[ "name" ] = name;
    return result;
}


} // anon


// Registered at kiface-load time — see header in bindings_sch_actions.cpp for
// why PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_schematic_state_bindings( py::module_& m )
{
    m.doc() = "KliCAD direct SCHEMATIC state binding — programmatic creation "
              "of wires, junctions, labels, and symbols against the running "
              "SCH_EDIT_FRAME.  Each call wraps changes in a SCH_COMMIT for "
              "undo support.  Positions are in millimeters.";

    m.def( "add_wire", &sch_state_add_wire,
           py::arg( "start_x_mm" ), py::arg( "start_y_mm" ),
           py::arg( "end_x_mm" ),   py::arg( "end_y_mm" ),
           R"DOC(Add a wire (SCH_LINE on LAYER_WIRE) between two points (mm).

Returns {ok: bool, kiid: str (uuid)}.
Spawns the schematic editor if it isn't already open.
)DOC" );

    m.def( "add_junction", &sch_state_add_junction,
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           R"DOC(Add a SCH_JUNCTION at (x_mm, y_mm).

Returns {ok: bool, kiid: str, error?: str}.

CAVEAT: KiCad's connection-graph cleanup runs at save time and silently
DROPS junctions deemed "unneeded" — i.e. anywhere that doesn't actually
have 3+ wire endpoints meeting, or doesn't have a wire passing over a
pin mid-segment.  A router that depends on its junctions surviving must
also ensure the local topology gives KiCad a reason to keep them.  In
particular: two collinear wire segments meeting at a terminal pin are
merged into one wire at save time, and ANY junction at the merge point
disappears — losing the pin's connection.  Place a small lateral
"kink" or split a routed wire at the pin so two non-collinear endpoints
meet there before relying on add_junction.
)DOC" );

    m.def( "add_label", &sch_state_add_label,
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "text" ),
           py::arg( "kind" ) = std::string( "local" ),
           R"DOC(Add a label at (x_mm, y_mm) with the given text.

kind: 'local'        -> SCH_LABEL       (default)
      'global'       -> SCH_GLOBALLABEL
      'hierarchical' -> SCH_HIERLABEL   ('hier' also accepted)

Returns {ok: bool, kiid: str, kind: str}.
)DOC" );

    m.def( "add_symbol", &sch_state_add_symbol,
           py::arg( "lib_id" ),
           py::arg( "ref_des" ),
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "unit" ) = 1,
           R"DOC(Add a SCH_SYMBOL at (x_mm, y_mm).

lib_id: 'LibName:SymbolName'.  Resolved via PROJECT_SCH::SymbolLibAdapter
        + LIB_ID::Parse + SYMBOL_LIBRARY_ADAPTER::LoadSymbol.
ref_des: reference designator (e.g. 'R1').  Pass empty string to skip.
unit: 1-based unit index for multi-unit packages.

Returns {ok: bool, kiid: str, lib_id: str, ref_des: str, error?: str}.
Raises RuntimeError if lib_id parse fails, if no project is loaded, or
if the underlying LoadSymbol throws IO_ERROR.  Returns ok=False with an
error field if the lib_id parses but resolves to no symbol.
)DOC" );

    m.def( "set_symbol_value", &sch_state_set_symbol_value,
           py::arg( "kiid" ), py::arg( "value" ),
           R"DOC(Set the visible 'Value' field of a placed symbol.

This is the post-add_symbol companion: after `add_symbol('Device:R', 'R1', ...)`
the resistor's value reads as 'R'; call `set_symbol_value(kiid, '1k')` to make
it '1k'.  Wrapped in an SCH_COMMIT so undo works.

Returns {ok: bool, kiid: str, value: str}.
Raises RuntimeError if no symbol with that kiid is in the hierarchy.
)DOC" );

    m.def( "set_symbol_field", &sch_state_set_symbol_field,
           py::arg( "kiid" ), py::arg( "field_name" ), py::arg( "value" ),
           R"DOC(Set a named field on a placed symbol.  Creates a USER field if absent.

Used both for built-in fields ('Footprint', 'Datasheet', 'Description') and
for SPICE annotations the netlist generator picks up:

    set_symbol_field(kiid, 'Sim_Device', 'BJT')
    set_symbol_field(kiid, 'Sim_Model',  '2N3904')
    set_symbol_field(kiid, 'Sim_Pins',   '1=C 2=B 3=E')

USER fields participate in netlist generation just like mandatory ones.

Returns {ok: bool, kiid: str, field: str, value: str, created: bool}.
Raises ValueError on empty field name; RuntimeError if symbol kiid not found.
)DOC" );

    m.def( "set_symbol_rotation", &sch_state_set_symbol_rotation,
           py::arg( "kiid" ), py::arg( "degrees" ),
           R"DOC(Set absolute rotation of a placed symbol.

degrees: 0, 90, 180, or 270 (multiples of 90 only).  Mirror flags on the
symbol are preserved.  Wrapped in an SCH_COMMIT.

Returns {ok: bool, kiid: str, rotation: int}.
Raises ValueError if degrees isn't a multiple of 90;
RuntimeError if symbol kiid not found.
)DOC" );

    m.def( "get_symbol_pin_position", &sch_state_get_symbol_pin_position,
           py::arg( "kiid" ), py::arg( "pin_id" ),
           R"DOC(Return the world-coordinate (mm) position of a pin on a placed symbol.

pin_id: matched against pin NUMBER first, then pin NAME.  E.g. for a 2N3904
        in 'Transistor_BJT:2N3904' (pin numbering 1=B, 2=C, 3=E) you can use
        either '1' or 'B'.

The position accounts for the symbol's current placement + rotation.  Useful
for dropping net labels at pin coordinates instead of routing wires:

    p = get_symbol_pin_position(r1_kiid, '2')
    add_label(p['x_mm'], p['y_mm'], 'NET_OUT')

Returns on success:
    {ok: True, kiid, pin_number, pin_name, x_mm, y_mm}

Returns on miss (does not raise):
    {ok: False, error, available_pins: [{number, name}, ...]}
)DOC" );

    m.def( "get_symbol_bbox", &sch_state_get_symbol_bbox,
           py::arg( "kiid" ),
           R"DOC(Return the body-only bounding box of a placed symbol in mm.

The box excludes pin extents and text-field footprints, so it represents
the obstacle a wire router should avoid passing THROUGH (wires can still
approach via the pin endpoints, which lie at or just outside this box).

Rotation: GetBodyBoundingBox() walks the symbol's body geometry through
SCH_SYMBOL::doGetBoundingBox which applies the placement orient/mirror
transforms to each child item before computing the bound — so the
returned axis-aligned box reflects the symbol's CURRENT orientation.

Returns: {ok: True, kiid, x_mm, y_mm, w_mm, h_mm}
         where (x_mm, y_mm) is the top-left corner.
)DOC" );

    m.def( "open_schematic", &sch_state_open_schematic, py::arg( "path" ),
           R"DOC(Load a .kicad_sch file into the open SCH_EDIT_FRAME.

Wraps SCH_EDIT_FRAME::OpenProjectFiles().  After this returns ok=True
the SCHEMATIC on the editor is the just-loaded one and subsequent
calls (get_items_summary, klicad_native_hierarchy.*, etc.) operate
against it.

Returns: {ok, path, sheet_count}.

Raises:
    ValueError on empty path.
    RuntimeError if SCH editor can't be spawned.
)DOC" );

    m.def( "save_schematic", &sch_state_save_schematic,
           R"DOC(Save the active SCHEMATIC in place.

Wraps SCH_EDIT_FRAME::SaveProject(aSaveAs=False).  For "save as",
drive the EditorControl.saveAs tool action via klicad_native_sch_actions.

Returns: {ok}.

Raises:
    RuntimeError if no schematic is open.
)DOC" );

    m.def( "get_sheet_count", &sch_state_get_sheet_count,
           R"DOC(Return the number of sheets in the schematic hierarchy.

0 if no schematic is currently loaded (newly-spawned blank frame).
)DOC" );

    m.def( "get_items_summary", &sch_state_get_items_summary,
           R"DOC(Return per-type item counts across every sheet in the hierarchy.

Keys: symbols, wires (SCH_LINE on LAYER_WIRE), lines (all SCH_LINEs),
junctions, labels (local), glabels, hlabels, sheets, sheet_count.
)DOC" );

    m.def( "list_symbols", &sch_state_list_symbols,
           py::arg( "sheet_path" ) = std::string(),
           R"DOC(Enumerate placed symbols.

sheet_path: "" = walk whole hierarchy (default); "/uuid1/uuid2" = scope
to a single sheet path (as returned by klicad_native_hierarchy.list_sheets
or get_current_sheet's path_string).

Each row carries: kiid, ref, lib_id, sheet_path, x_mm, y_mm, orientation,
mirror_x, mirror_y, value, footprint, fields (dict of name->text).

Used by klicad-python's to_schematic(mode='diff') to compute the
add/remove/keep ref-set and verify field state per sheet.
)DOC" );

    m.def( "list_labels", &sch_state_list_labels,
           py::arg( "sheet_path" ) = std::string(),
           R"DOC(Enumerate SCH_LABEL / SCH_GLOBAL_LABEL / SCH_HIER_LABEL items.

sheet_path: "" = walk whole hierarchy (default); "/uuid" = scope to one
sheet.  Each row: kiid, name, kind ∈ {'local','global','hierarchical'},
sheet_path, x_mm, y_mm.

Used by klicad-python's port-anchor diff so user-positioned hier-labels
survive iterations.
)DOC" );

    m.def( "delete_by_kiid", &sch_state_delete_by_kiid, py::arg( "kiid" ),
           R"DOC(Remove a single SCH_ITEM (symbol, wire, label, junction, sheet,
sheet-pin) by UUID.

Returns {ok: bool, kiid: str} on success, {ok: False, error: str} if no
item matches.  Wrapped in an SCH_COMMIT so the deletion is undoable.
)DOC" );

    m.def( "clear_routing", &sch_state_clear_routing,
           py::arg( "sheet_path" )       = std::string(),
           py::arg( "keep_hier_labels" ) = false,
           R"DOC(Bulk-delete every wire, label, junction, and #PWR_* power-flag
symbol; preserve real component symbols.

sheet_path:       "" = walk whole hierarchy (default); "/uuid" = scope
                  to one sheet.  Used between diff iterations so the
                  router and power-symbol placement re-emit clean each
                  pass without #PWR accumulation.
keep_hier_labels: when True, SCH_HIER_LABELs are spared.  Used by
                  to_schematic(mode='diff') on child sheets so port
                  anchors keep their (possibly user-positioned)
                  coordinates across iterations.

Returns {ok: True, removed: int}.
)DOC" );

    m.def( "add_sheet", &sch_state_add_sheet,
           py::arg( "name" ), py::arg( "filename" ),
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "w_mm" ), py::arg( "h_mm" ),
           R"DOC(Place a SCH_SHEET on the current sheet.

A fresh empty SCH_SCREEN is created and attached so subsequent
set_current_sheet + add_symbol calls populate the child sheet's
content.  The Python side is responsible for laying down the
corresponding child .kicad_sch stub on disk; save_schematic
overwrites it with the in-memory screen.

Returns {ok, kiid, name, file_name}.
)DOC" );

    m.def( "add_sheet_pin", &sch_state_add_sheet_pin,
           py::arg( "sheet_kiid" ), py::arg( "name" ),
           py::arg( "side" ),
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "shape" ) = std::string( "bidi" ),
           R"DOC(Add a SCH_SHEET_PIN to an existing SCH_SHEET (by uuid).

side  ∈ {'left','right','top','bottom'}.
shape ∈ {'input','output','bidi'/'bidirectional','tristate'/'tri_state',
         'passive'/'unspecified'}.

Bus-syntax names like 'DATA[0..7]' are accepted verbatim — KliCAD's
label/bus parser detects them automatically and renders as a bus pin.

Returns {ok, kiid, name}.
)DOC" );
}
