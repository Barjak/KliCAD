/*
 * KliCAD subsystem binding: BOARD state (direct creation).
 *
 * Exposes direct BOARD_ITEM creation as klicad_native_pcb_state.* —
 * placing tracks, vias, footprints and inspecting the running BOARD.
 * Fills the gap left by the typed-RPC CreateItems handler, which today
 * cannot round-trip a number of board-item kinds.
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer
 * runs after py::initialize_interpreter, and PyImport_AppendInittab
 * refuses post-init.
 *
 * Pattern:
 *   1. Walk wxTopLevelWindows for the live PCB_EDIT_FRAME (spawn via
 *      KIWAY::Player(FRAME_PCB_EDITOR, true) if missing).
 *   2. Wrap each mutation in a BOARD_COMMIT(frame) so the user can undo.
 *   3. Construct the BOARD_ITEM, set its properties (positions in nm =
 *      mm * 1e6 — KiCad internal unit for the PCB is also nm).
 *   4. commit.Add(item) + commit.Push("KliCAD: ...").
 *   5. Refresh the canvas via PCB_DRAW_PANEL_GAL::Refresh().
 *   6. Return { ok, kiid, error? } as a py::dict.
 *
 * Frame discovery: we cannot dynamic_cast<PCB_EDIT_FRAME*> across the
 * kiface boundary (typeinfo lives in _pcbnew.kiface.bundle), so we
 * identify via EDA_BASE_FRAME::GetFrameType() == FRAME_PCB_EDITOR and
 * then static_cast.  Same caveat as bindings_schematic_state.cpp.
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
#include <layer_ids.h>
#include <lseq.h>
#include <lset.h>
#include <math/vector2d.h>
#include <math/box2.h>

#include <board.h>
#include <board_commit.h>
#include <board_connected_item.h>
#include <board_item.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_base_frame.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_shape.h>
#include <pcb_track.h>
#include <zone.h>
#include <netinfo.h>

#include <project_pcb.h>
#include <footprint_library_adapter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Position scaling: mm in, nanometers out (KiCad internal unit for the
// PCB editor is nm — same scale as schematic).
constexpr double MM_TO_NM_PCB = 1e6;

inline int mm_to_nm_pcb( double aMm )
{
    return static_cast<int>( aMm * MM_TO_NM_PCB );
}

inline VECTOR2I vec_mm_to_nm_pcb( double x_mm, double y_mm )
{
    return VECTOR2I( mm_to_nm_pcb( x_mm ), mm_to_nm_pcb( y_mm ) );
}


// Per-TU helper name (unique vs find_live_kiway, find_live_kiway_for_erc,
// find_live_kiway_for_gui, find_live_kiway_for_schematic_state, ...).
KIWAY* find_live_kiway_for_pcb_state()
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
// boundary.
PCB_EDIT_FRAME* find_pcb_edit_frame_for_state()
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
PCB_EDIT_FRAME* require_pcb_edit_frame()
{
    if( PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_state() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_pcb_state();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(pcb_state needs to spawn the pcbnew frame)" );
    }

    kiway->Player( FRAME_PCB_EDITOR, true );

    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_state();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain PCB_EDIT_FRAME after "
            "KIWAY::Player(FRAME_PCB_EDITOR, true)" );
    }

    return frame;
}


// Refresh the PCB canvas after a mutation so the user sees the change.
void refresh_pcb_canvas( PCB_EDIT_FRAME* aFrame )
{
    if( aFrame && aFrame->GetCanvas() )
        aFrame->GetCanvas()->Refresh();
}


// Resolve a layer name like "F.Cu" / "B.Cu" / "Edge.Cuts" through
// BOARD::GetLayerID, which also accepts BOARD-specific user-rename
// strings.  Throws std::invalid_argument with the offending name on
// failure.
PCB_LAYER_ID resolve_pcb_layer( const BOARD* aBoard, const std::string& aName )
{
    wxString wxname = wxString::FromUTF8( aName.c_str() );
    PCB_LAYER_ID layer = aBoard->GetLayerID( wxname );

    if( layer == UNDEFINED_LAYER )
    {
        throw std::invalid_argument(
            "unknown PCB layer name '" + aName +
            "' (expected canonical names like 'F.Cu', 'B.Cu', 'F.SilkS', 'Edge.Cuts')" );
    }

    return layer;
}


// ──────────────────────────────────────────────────────────────────────────
// add_track
// ──────────────────────────────────────────────────────────────────────────
py::object pcb_state_add_track( double start_x_mm, double start_y_mm,
                                double end_x_mm,   double end_y_mm,
                                const std::string& layer_name,
                                double width_mm,
                                int net )
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    PCB_LAYER_ID layer = resolve_pcb_layer( board, layer_name );

    PCB_TRACK* track = new PCB_TRACK( board );
    track->SetStart( vec_mm_to_nm_pcb( start_x_mm, start_y_mm ) );
    track->SetEnd(   vec_mm_to_nm_pcb( end_x_mm,   end_y_mm   ) );
    track->SetLayer( layer );
    track->SetWidth( mm_to_nm_pcb( width_mm ) );

    if( net != 0 )
        track->SetNetCode( net, /*aNoAssert*/ true );

    {
        BOARD_COMMIT commit( frame );
        commit.Add( track );
        commit.Push( wxT( "KliCAD: add_track" ) );
    }

    refresh_pcb_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = track->m_Uuid.AsStdString();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_via
// ──────────────────────────────────────────────────────────────────────────
py::object pcb_state_add_via( double x_mm, double y_mm,
                              double drill_mm, double diameter_mm,
                              const std::string& from_layer,
                              const std::string& to_layer,
                              int net )
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    PCB_LAYER_ID top    = resolve_pcb_layer( board, from_layer );
    PCB_LAYER_ID bottom = resolve_pcb_layer( board, to_layer );

    PCB_VIA* via = new PCB_VIA( board );

    VECTOR2I pos = vec_mm_to_nm_pcb( x_mm, y_mm );
    via->SetStart( pos );
    via->SetEnd(   pos );

    via->SetWidth( mm_to_nm_pcb( diameter_mm ) );
    via->SetDrill( mm_to_nm_pcb( drill_mm ) );

    // Determine via type from the layer pair.  THROUGH if outer-to-outer.
    // If one endpoint is an outer layer and the other is internal -> BLIND.
    // Both endpoints internal -> BURIED.  SetViaType internally calls
    // SanitizeLayers.
    bool top_is_outer    = ( top == F_Cu || top == B_Cu );
    bool bottom_is_outer = ( bottom == F_Cu || bottom == B_Cu );
    VIATYPE vtype;

    if( top_is_outer && bottom_is_outer )
        vtype = VIATYPE::THROUGH;
    else if( top_is_outer || bottom_is_outer )
        vtype = VIATYPE::BLIND;
    else
        vtype = VIATYPE::BURIED;

    via->SetViaType( vtype );
    via->SetLayerPair( top, bottom );

    if( net != 0 )
        via->SetNetCode( net, /*aNoAssert*/ true );

    {
        BOARD_COMMIT commit( frame );
        commit.Add( via );
        commit.Push( wxT( "KliCAD: add_via" ) );
    }

    refresh_pcb_canvas( frame );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "kiid" ] = via->m_Uuid.AsStdString();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_footprint
// ──────────────────────────────────────────────────────────────────────────
py::object pcb_state_add_footprint( const std::string& lib_id_str,
                                    const std::string& ref_des,
                                    double x_mm, double y_mm,
                                    double rotation_deg,
                                    const std::string& layer_name )
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    LIB_ID libId;

    if( libId.Parse( lib_id_str ) >= 0 )
    {
        throw std::invalid_argument(
            "lib_id parse failed; expected 'LibName:FootprintName' (got '"
            + lib_id_str + "')" );
    }

    FOOTPRINT_LIBRARY_ADAPTER* adapter =
        PROJECT_PCB::FootprintLibAdapter( &frame->Prj() );

    if( !adapter )
    {
        py::dict result;
        result[ "ok" ]    = false;
        result[ "error" ] = std::string(
            "PROJECT_PCB::FootprintLibAdapter returned nullptr — is a project loaded?" );
        return result;
    }

    FOOTPRINT* fp = nullptr;

    try
    {
        // aKeepUUID = false: board-editor semantics (fresh UUIDs).
        fp = adapter->LoadFootprint( libId, /*aKeepUUID*/ false );
    }
    catch( const std::exception& ex )
    {
        throw std::runtime_error(
            std::string( "LoadFootprint failed for '" ) + lib_id_str + "': " + ex.what() );
    }
    catch( ... )
    {
        throw std::runtime_error(
            std::string( "LoadFootprint failed for '" ) + lib_id_str
            + "': unknown exception (IO_ERROR?)" );
    }

    if( !fp )
    {
        py::dict result;
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "lib_id '" ) + lib_id_str
                            + "' not found in project footprint library table";
        return result;
    }

    fp->SetParent( board );
    fp->SetPosition( vec_mm_to_nm_pcb( x_mm, y_mm ) );

    if( rotation_deg != 0.0 )
        fp->SetOrientationDegrees( rotation_deg );

    // Side selection: any non-front layer name flips the footprint.
    PCB_LAYER_ID layer = resolve_pcb_layer( board, layer_name );

    if( layer == B_Cu )
        fp->SetLayerAndFlip( B_Cu );
    else
        fp->SetLayer( layer );  // typically F_Cu; copper layer of the footprint

    if( !ref_des.empty() )
        fp->SetReference( wxString::FromUTF8( ref_des.c_str() ) );

    {
        BOARD_COMMIT commit( frame );
        commit.Add( fp );
        commit.Push( wxT( "KliCAD: add_footprint" ) );
    }

    refresh_pcb_canvas( frame );

    py::dict result;
    result[ "ok" ]      = true;
    result[ "kiid" ]    = fp->m_Uuid.AsStdString();
    result[ "lib_id" ]  = lib_id_str;
    result[ "ref_des" ] = ref_des;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_items_summary
// ──────────────────────────────────────────────────────────────────────────
py::dict pcb_state_get_items_summary()
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    int tracks_n      = 0;
    int vias_n        = 0;
    int arcs_n        = 0;
    int footprints_n  = 0;
    int pads_n        = 0;
    int zones_n       = 0;
    int shapes_n      = 0;
    int texts_n       = 0;
    int dimensions_n  = 0;

    if( board )
    {
        for( PCB_TRACK* t : board->Tracks() )
        {
            switch( t->Type() )
            {
            case PCB_TRACE_T: ++tracks_n; break;
            case PCB_VIA_T:   ++vias_n;   break;
            case PCB_ARC_T:   ++arcs_n;   break;
            default: break;
            }
        }

        for( FOOTPRINT* fp : board->Footprints() )
        {
            ++footprints_n;
            pads_n += static_cast<int>( fp->Pads().size() );
        }

        zones_n = static_cast<int>( board->Zones().size() );

        for( BOARD_ITEM* d : board->Drawings() )
        {
            switch( d->Type() )
            {
            case PCB_SHAPE_T:        ++shapes_n;     break;
            case PCB_TEXT_T:         ++texts_n;      break;
            case PCB_DIMENSION_T:
            case PCB_DIM_ALIGNED_T:
            case PCB_DIM_LEADER_T:
            case PCB_DIM_CENTER_T:
            case PCB_DIM_RADIAL_T:
            case PCB_DIM_ORTHOGONAL_T:
                ++dimensions_n;
                break;
            default: break;
            }
        }
    }

    py::dict d;
    d[ "tracks" ]     = tracks_n;
    d[ "vias" ]       = vias_n;
    d[ "arcs" ]       = arcs_n;
    d[ "footprints" ] = footprints_n;
    d[ "pads" ]       = pads_n;
    d[ "zones" ]      = zones_n;
    d[ "shapes" ]     = shapes_n;
    d[ "texts" ]      = texts_n;
    d[ "dimensions" ] = dimensions_n;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// list_layers
// ──────────────────────────────────────────────────────────────────────────
py::list pcb_state_list_layers()
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    py::list out;

    if( !board )
        return out;

    LSEQ seq = board->GetEnabledLayers().Seq();

    for( PCB_LAYER_ID layer : seq )
    {
        wxString name = board->GetLayerName( layer );
        out.append( std::string( name.utf8_str() ) );
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// open_board
// ──────────────────────────────────────────────────────────────────────────
py::dict pcb_state_open_board( const std::string& aPath )
{
    if( aPath.empty() )
        throw std::invalid_argument( "open_board: path is empty" );

    PCB_EDIT_FRAME* frame    = require_pcb_edit_frame();
    BOARD*          oldBoard = frame->GetBoard();
    wxString        wxPath   = wxString::FromUTF8( aPath.c_str() );

    // If the editor already has this exact board open, no-op.  Calling
    // PCB_EDIT_FRAME::OpenProjectFiles a second time with the same path
    // is NOT safe: setProject=false skips the up-front ClearProject /
    // UnloadProject cleanup, then BOARD_LOADER::Load eagerly calls
    // SetProject on the freshly-loaded board which reassigns
    // project.m_BoardSettings to point at the new board's settings.
    // The subsequent SetBoard() calls oldBoard->ClearProject() which
    // tries to ReleaseNestedSettings on the stale pointer → SIGSEGV in
    // BOARD::ClearProject.  No "force reload" arg yet; if you need one,
    // close+reopen via show_frame or use a different open_board path.
    if( oldBoard && oldBoard->GetFileName() == wxPath )
    {
        py::dict d;
        d[ "ok" ]       = true;
        d[ "path" ]     = aPath;
        d[ "filename" ] = std::string( oldBoard->GetFileName().utf8_str() );
        d[ "note" ]     = std::string(
            "board already open at this path; no reload performed "
            "(force-reload not supported — see binding source for why)" );
        return d;
    }

    std::vector<wxString> files = { wxPath };
    bool ok = frame->OpenProjectFiles( files, /*aCtl*/ 0 );

    refresh_pcb_canvas( frame );

    py::dict d;
    d[ "ok" ] = ok;
    d[ "path" ] = aPath;

    if( BOARD* board = frame->GetBoard() )
        d[ "filename" ] = std::string( board->GetFileName().utf8_str() );
    else
        d[ "filename" ] = std::string();

    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// save_board
// ──────────────────────────────────────────────────────────────────────────
py::dict pcb_state_save_board( const std::string& aPath )
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "save_board: no BOARD is currently open" );

    // Empty path means "save in place".
    wxString target = aPath.empty()
                          ? board->GetFileName()
                          : wxString::FromUTF8( aPath.c_str() );

    if( target.IsEmpty() )
    {
        throw std::runtime_error(
            "save_board: no target path supplied and BOARD has no filename" );
    }

    bool ok = frame->SavePcbFile( target,
                                  /*addToHistory*/ false,
                                  /*aChangeProject*/ false );

    py::dict d;
    d[ "ok" ]   = ok;
    d[ "path" ] = std::string( target.utf8_str() );
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// get_board_info
// ──────────────────────────────────────────────────────────────────────────
py::dict pcb_state_get_board_info()
{
    PCB_EDIT_FRAME* frame = require_pcb_edit_frame();
    BOARD*          board = frame->GetBoard();

    py::dict d;

    if( !board )
    {
        d[ "filename" ]           = std::string();
        d[ "copper_layer_count" ] = 0;
        return d;
    }

    d[ "filename" ] = std::string( board->GetFileName().utf8_str() );
    d[ "copper_layer_count" ] = board->GetCopperLayerCount();

    // Bounding box, in mm, of the board edges (Edge.Cuts).  If the board
    // has no edges, ComputeBoundingBox returns the full content bbox.
    BOX2I bbox = board->ComputeBoundingBox( /*aBoardEdgesOnly*/ true,
                                            /*aPhysicalLayersOnly*/ true );

    py::dict outline;
    outline[ "x_mm" ]      = bbox.GetX()      / MM_TO_NM_PCB;
    outline[ "y_mm" ]      = bbox.GetY()      / MM_TO_NM_PCB;
    outline[ "width_mm" ]  = bbox.GetWidth()  / MM_TO_NM_PCB;
    outline[ "height_mm" ] = bbox.GetHeight() / MM_TO_NM_PCB;
    d[ "board_outline_bbox" ] = outline;

    return d;
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h for why
// PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_pcb_state_bindings( py::module_& m )
{
    m.doc() = "KliCAD direct BOARD state binding — programmatic creation "
              "of tracks, vias and footprints against the running "
              "PCB_EDIT_FRAME.  Each mutation wraps in a BOARD_COMMIT for "
              "undo support.  Positions are in millimeters.";

    m.def( "add_track", &pcb_state_add_track,
           py::arg( "start_x_mm" ), py::arg( "start_y_mm" ),
           py::arg( "end_x_mm" ),   py::arg( "end_y_mm" ),
           py::arg( "layer" )    = std::string( "F.Cu" ),
           py::arg( "width_mm" ) = 0.2,
           py::arg( "net" )      = 0,
           R"DOC(Add a PCB_TRACK on the given copper layer between two points (mm).

layer: canonical layer name (default 'F.Cu').  Resolved via BOARD::GetLayerID.
width_mm: track width in mm (default 0.2).
net: integer netcode (default 0 = unconnected); resolved via NETINFO_LIST.

Returns {ok: bool, kiid: str}.
)DOC" );

    m.def( "add_via", &pcb_state_add_via,
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "drill_mm" )    = 0.3,
           py::arg( "diameter_mm" ) = 0.6,
           py::arg( "from_layer" )  = std::string( "F.Cu" ),
           py::arg( "to_layer" )    = std::string( "B.Cu" ),
           py::arg( "net" )         = 0,
           R"DOC(Add a PCB_VIA at (x_mm, y_mm).

drill_mm:    drill hole diameter (default 0.3).
diameter_mm: copper diameter (default 0.6).
from_layer / to_layer: canonical layer names (default 'F.Cu' / 'B.Cu' =
    through via).  Anything else is treated as BLIND_BURIED.
net: integer netcode.

Returns {ok: bool, kiid: str}.
)DOC" );

    m.def( "add_footprint", &pcb_state_add_footprint,
           py::arg( "lib_id" ),
           py::arg( "ref_des" ),
           py::arg( "x_mm" ), py::arg( "y_mm" ),
           py::arg( "rotation_deg" ) = 0.0,
           py::arg( "layer" )        = std::string( "F.Cu" ),
           R"DOC(Add a FOOTPRINT at (x_mm, y_mm).

lib_id: 'LibName:FootprintName'.  Resolved via PROJECT_PCB::FootprintLibAdapter
        + LIB_ID::Parse + FOOTPRINT_LIBRARY_ADAPTER::LoadFootprint.
ref_des: reference designator (e.g. 'U1').  Pass empty string to skip.
rotation_deg: footprint orientation in degrees (default 0).
layer: 'F.Cu' (default) or 'B.Cu' to place on the back side
       (SetLayerAndFlip).

Returns {ok: bool, kiid: str, lib_id: str, ref_des: str, error?: str}.
Returns ok=False with an error field if no project is loaded, or if
lib_id resolves to no footprint.  Raises RuntimeError on parse failure
or underlying IO_ERROR.
)DOC" );

    m.def( "get_items_summary", &pcb_state_get_items_summary,
           R"DOC(Return per-type item counts on the active BOARD.

Keys: tracks (PCB_TRACE_T), vias (PCB_VIA_T), arcs (PCB_ARC_T),
footprints, pads (across all footprints), zones, shapes (PCB_SHAPE_T),
texts (PCB_TEXT_T), dimensions (all PCB_DIM_* variants).
)DOC" );

    m.def( "list_layers", &pcb_state_list_layers,
           R"DOC(Return canonical layer names for every enabled layer on the active BOARD.

Order follows LSET::Seq().  Names match BOARD::GetLayerName, which
prefers any user-renamed copper layer name.
)DOC" );

    m.def( "open_board", &pcb_state_open_board, py::arg( "path" ),
           R"DOC(Load a .kicad_pcb file into the open PCB_EDIT_FRAME.

Wraps PCB_EDIT_FRAME::OpenProjectFiles().  After this returns ok=True
the BOARD on the editor is the just-loaded one and subsequent calls
(list_layers, get_board_info, klicad_native_netinfo.*, etc.) operate
against it.

Returns: {ok, path, filename}.

Raises:
    ValueError on empty path.
    RuntimeError if PCB editor can't be spawned.
)DOC" );

    m.def( "save_board", &pcb_state_save_board,
           py::arg( "path" ) = std::string(),
           R"DOC(Save the active BOARD.  Empty path means "save in place".

Wraps PCB_EDIT_FRAME::SavePcbFile() without project rename and without
adding to file history (so headless saves don't pollute the recent-
files list).  For "save as" with project rename, drive the
EditorControl.saveAs tool action through klicad_native_pcb_actions.

Returns: {ok, path}.

Raises:
    RuntimeError if no board is open or empty path + no current filename.
)DOC" );

    m.def( "get_board_info", &pcb_state_get_board_info,
           R"DOC(Return high-level metadata about the active BOARD.

Keys: filename (utf-8), copper_layer_count, board_outline_bbox
({x_mm, y_mm, width_mm, height_mm} from ComputeBoundingBox(edges-only,
physical-only)).
)DOC" );
}
