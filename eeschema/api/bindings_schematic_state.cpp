/*
 * KliCAD subsystem binding: SCHEMATIC state (direct creation).
 *
 * Exposes direct SCH_ITEM creation as kicad_native_schematic_state.* —
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
#include <math/vector2d.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_commit.h>
#include <sch_sheet_path.h>

#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Position scaling: mm in, nanometers out (KiCad internal unit for
// schematics is nm).
constexpr double MM_TO_NM = 1e6;

inline VECTOR2I mm_to_nm( double x_mm, double y_mm )
{
    return VECTOR2I( static_cast<int>( x_mm * MM_TO_NM ),
                     static_cast<int>( y_mm * MM_TO_NM ) );
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

    VECTOR2I start = mm_to_nm( start_x_mm, start_y_mm );
    VECTOR2I end   = mm_to_nm( end_x_mm,   end_y_mm   );

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

    VECTOR2I pos = mm_to_nm( x_mm, y_mm );

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

    VECTOR2I pos = mm_to_nm( x_mm, y_mm );
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

    VECTOR2I pos = mm_to_nm( x_mm, y_mm );

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

    std::vector<wxString> files = { wxString::FromUTF8( aPath.c_str() ) };
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

Returns {ok: bool, kiid: str, error?: str}.  Uses SCH_EDIT_FRAME::AddJunction
under the hood for parity with the interactive tool.
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

    m.def( "open_schematic", &sch_state_open_schematic, py::arg( "path" ),
           R"DOC(Load a .kicad_sch file into the open SCH_EDIT_FRAME.

Wraps SCH_EDIT_FRAME::OpenProjectFiles().  After this returns ok=True
the SCHEMATIC on the editor is the just-loaded one and subsequent
calls (get_items_summary, kicad_native_hierarchy.*, etc.) operate
against it.

Returns: {ok, path, sheet_count}.

Raises:
    ValueError on empty path.
    RuntimeError if SCH editor can't be spawned.
)DOC" );

    m.def( "save_schematic", &sch_state_save_schematic,
           R"DOC(Save the active SCHEMATIC in place.

Wraps SCH_EDIT_FRAME::SaveProject(aSaveAs=False).  For "save as",
drive the EditorControl.saveAs tool action via kicad_native_sch_actions.

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
}
