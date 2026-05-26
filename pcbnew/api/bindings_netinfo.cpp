/*
 * KliCAD subsystem binding: BOARD net inspection / analysis.
 *
 * Exposes read-only net + netclass queries as klicad_native_netinfo.* for
 * design checks (impedance, length matching, fanout) that need the live
 * wiring topology of the open board.
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here.  Frame discovery mirrors
 * bindings_pcb_state.cpp / bindings_drc_rules.cpp (GetFrameType() ==
 * FRAME_PCB_EDITOR + static_cast across the kiface boundary).
 *
 * Length aggregation: BOARD::GetTrackLength(seedTrack) returns the
 * delay-engine track+via + pad-to-die figures using the live
 * CONNECTIVITY_DATA + LENGTH_DELAY_CALCULATION (same path the
 * NETINFO_ITEM message panel uses).  We also report the raw per-segment
 * track sum (PCB_TRACK::GetLength) for callers that want a value
 * independent of T-junction optimisation.  Via "length" (drilled stub
 * for blind/buried) is not computed — needs stackup access we don't
 * pull in here; counts only.
 *
 * Netclass: BOARD_DESIGN_SETTINGS::m_NetSettings owns the project's
 * NET_SETTINGS.  GetNetclasses() yields user-defined classes by name;
 * GetDefaultNetclass() yields the implicit "Default".  Per-net effective
 * class comes from NET_SETTINGS::GetEffectiveNetClass(netname).
 *
 * Unconnected pads: ratsnest edges remaining after the connectivity
 * solve indicate not-yet-routed connections.  Non-empty
 * GetRatsnestForPad(pad) == unconnected.  GetNetCode() == 0 == no net.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <layer_ids.h>
#include <math/vector2d.h>

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <netclass.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_track.h>

#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>    // full CN_EDGE def — vector<CN_EDGE>::~vector needs it
#include <project/net_settings.h>

#include <wx/string.h>
#include <wx/window.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

constexpr double NM_TO_MM_NET = 1.0 / 1e6;


// Per-TU helpers — names disambiguated from other bindings_*.cpp.
KIWAY* find_live_kiway_for_netinfo()
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


PCB_EDIT_FRAME* find_pcb_edit_frame_for_netinfo()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( base && base->GetFrameType() == FRAME_PCB_EDITOR )
            return static_cast<PCB_EDIT_FRAME*>( base );
    }
    return nullptr;
}


BOARD* require_active_board_for_netinfo()
{
    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_netinfo();

    if( !frame )
    {
        KIWAY* kiway = find_live_kiway_for_netinfo();

        if( !kiway )
            throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );

        kiway->Player( FRAME_PCB_EDITOR, true );
        frame = find_pcb_edit_frame_for_netinfo();

        if( !frame )
            throw std::runtime_error(
                "failed to obtain PCB_EDIT_FRAME after KIWAY::Player(FRAME_PCB_EDITOR, true)" );
    }

    BOARD* board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    return board;
}


NETINFO_ITEM* require_net_by_name( BOARD* aBoard, const std::string& aName )
{
    wxString      wxname = wxString::FromUTF8( aName.c_str() );
    NETINFO_ITEM* net    = aBoard->GetNetInfo().GetNetItem( wxname );

    if( !net )
        throw std::invalid_argument( "unknown net name '" + aName + "'" );

    return net;
}


// Effective netclass name for a net: prefer NET_SETTINGS' resolved
// effective class (handles aggregate / pattern-matched assignments),
// fall back to the NETINFO_ITEM's bound NETCLASS, then "Default".
std::string effective_netclass_name( BOARD* aBoard, NETINFO_ITEM* aNet )
{
    BOARD_DESIGN_SETTINGS& bds = aBoard->GetDesignSettings();

    if( bds.m_NetSettings )
    {
        if( std::shared_ptr<NETCLASS> nc =
                bds.m_NetSettings->GetEffectiveNetClass( aNet->GetNetname() ) )
            return std::string( nc->GetHumanReadableName().utf8_str() );
    }

    if( NETCLASS* cached = aNet->GetNetClass() )
        return std::string( cached->GetHumanReadableName().utf8_str() );

    return std::string( "Default" );
}


// Per-net counts + segment-wise track-length sum + pad-to-die sum.
// Via stub length not included (needs stackup); via_count returned so
// callers can do their own accounting.
struct NetTotals
{
    int    track_count = 0;
    int    via_count   = 0;
    int    pad_count   = 0;
    double track_nm    = 0.0;
    double pad_die_nm  = 0.0;
};

NetTotals compute_net_totals( BOARD* aBoard, int aNetCode )
{
    NetTotals out;

    for( PCB_TRACK* t : aBoard->Tracks() )
    {
        if( t->GetNetCode() != aNetCode )
            continue;

        if( t->Type() == PCB_VIA_T )
        {
            ++out.via_count;
        }
        else
        {
            ++out.track_count;
            out.track_nm += t->GetLength();
        }
    }

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() == aNetCode )
            {
                ++out.pad_count;
                out.pad_die_nm += static_cast<double>( pad->GetPadToDieLength() );
            }
        }
    }

    return out;
}


// Find any PCB_TRACK (non-via) on the net — seed for BOARD::GetTrackLength.
PCB_TRACK* find_seed_track( BOARD* aBoard, int aNetCode )
{
    for( PCB_TRACK* t : aBoard->Tracks() )
    {
        if( t->GetNetCode() == aNetCode && t->Type() != PCB_VIA_T )
            return t;
    }

    return nullptr;
}


py::dict net_summary_dict( BOARD* aBoard, NETINFO_ITEM* aNet )
{
    NetTotals t = compute_net_totals( aBoard, aNet->GetNetCode() );

    py::dict d;
    d[ "name" ]            = std::string( aNet->GetNetname().utf8_str() );
    d[ "code" ]            = aNet->GetNetCode();
    d[ "class" ]           = effective_netclass_name( aBoard, aNet );
    d[ "num_pads" ]        = t.pad_count;
    d[ "num_tracks" ]      = t.track_count;
    d[ "num_vias" ]        = t.via_count;
    d[ "total_length_mm" ] = t.track_nm * NM_TO_MM_NET;
    d[ "pad_to_die_mm" ]   = t.pad_die_nm * NM_TO_MM_NET;
    return d;
}


py::dict position_mm_dict( const VECTOR2I& aPos )
{
    py::dict d;
    d[ "x_mm" ] = aPos.x * NM_TO_MM_NET;
    d[ "y_mm" ] = aPos.y * NM_TO_MM_NET;
    return d;
}


py::dict netclass_to_dict( const NETCLASS* aNc )
{
    auto opt_mm = []( bool has, int value_nm ) -> py::object
    {
        if( has )
            return py::float_( value_nm * NM_TO_MM_NET );
        return py::none();
    };

    py::dict d;
    d[ "name" ]               = std::string( aNc->GetHumanReadableName().utf8_str() );
    d[ "description" ]        = std::string( aNc->GetDescription().utf8_str() );
    d[ "priority" ]           = aNc->GetPriority();
    d[ "is_default" ]         = aNc->IsDefault();
    d[ "clearance_mm" ]       = opt_mm( aNc->HasClearance(),     aNc->GetClearance() );
    d[ "track_width_mm" ]     = opt_mm( aNc->HasTrackWidth(),    aNc->GetTrackWidth() );
    d[ "via_diameter_mm" ]    = opt_mm( aNc->HasViaDiameter(),   aNc->GetViaDiameter() );
    d[ "via_drill_mm" ]       = opt_mm( aNc->HasViaDrill(),      aNc->GetViaDrill() );
    d[ "uvia_diameter_mm" ]   = opt_mm( aNc->HasuViaDiameter(),  aNc->GetuViaDiameter() );
    d[ "uvia_drill_mm" ]      = opt_mm( aNc->HasuViaDrill(),     aNc->GetuViaDrill() );
    d[ "diff_pair_width_mm" ] = opt_mm( aNc->HasDiffPairWidth(), aNc->GetDiffPairWidth() );
    d[ "diff_pair_gap_mm" ]   = opt_mm( aNc->HasDiffPairGap(),   aNc->GetDiffPairGap() );
    return d;
}


// ─── list_nets / get_net ───────────────────────────────────────────────
py::list netinfo_list_nets()
{
    BOARD*   board = require_active_board_for_netinfo();
    py::list out;

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        if( net )
            out.append( net_summary_dict( board, net ) );
    }

    return out;
}


py::dict netinfo_get_net( const std::string& net_name )
{
    BOARD*        board = require_active_board_for_netinfo();
    NETINFO_ITEM* net   = require_net_by_name( board, net_name );
    return net_summary_dict( board, net );
}


// ─── get_net_pads ──────────────────────────────────────────────────────
py::list netinfo_get_net_pads( const std::string& net_name )
{
    BOARD*        board   = require_active_board_for_netinfo();
    NETINFO_ITEM* net     = require_net_by_name( board, net_name );
    int           netCode = net->GetNetCode();
    py::list      out;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() != netCode )
                continue;

            py::dict entry;
            entry[ "footprint_ref" ] = std::string( fp->GetReference().utf8_str() );
            entry[ "pad_number" ]    = std::string( pad->GetNumber().utf8_str() );
            entry[ "layer" ]         =
                    std::string( board->GetLayerName( pad->GetLayer() ).utf8_str() );
            entry[ "position_mm" ]   = position_mm_dict( pad->GetPosition() );
            out.append( entry );
        }
    }

    return out;
}


// ─── get_net_tracks (PCB_TRACE_T + PCB_ARC_T) ──────────────────────────
py::list netinfo_get_net_tracks( const std::string& net_name )
{
    BOARD*        board   = require_active_board_for_netinfo();
    NETINFO_ITEM* net     = require_net_by_name( board, net_name );
    int           netCode = net->GetNetCode();
    py::list      out;

    for( PCB_TRACK* t : board->Tracks() )
    {
        if( t->GetNetCode() != netCode || t->Type() == PCB_VIA_T )
            continue;

        py::dict entry;
        entry[ "start_mm" ]  = position_mm_dict( t->GetStart() );
        entry[ "end_mm" ]    = position_mm_dict( t->GetEnd() );
        entry[ "width_mm" ]  = t->GetWidth() * NM_TO_MM_NET;
        entry[ "length_mm" ] = t->GetLength() * NM_TO_MM_NET;
        entry[ "layer" ]     =
                std::string( board->GetLayerName( t->GetLayer() ).utf8_str() );
        entry[ "is_arc" ]    = ( t->Type() == PCB_ARC_T );
        out.append( entry );
    }

    return out;
}


// ─── get_net_vias ──────────────────────────────────────────────────────
py::list netinfo_get_net_vias( const std::string& net_name )
{
    BOARD*        board   = require_active_board_for_netinfo();
    NETINFO_ITEM* net     = require_net_by_name( board, net_name );
    int           netCode = net->GetNetCode();
    py::list      out;

    for( PCB_TRACK* t : board->Tracks() )
    {
        if( t->GetNetCode() != netCode || t->Type() != PCB_VIA_T )
            continue;

        PCB_VIA* via = static_cast<PCB_VIA*>( t );

        PCB_LAYER_ID top, bottom;
        via->LayerPair( &top, &bottom );

        py::list layers;
        layers.append( std::string( board->GetLayerName( top ).utf8_str() ) );
        layers.append( std::string( board->GetLayerName( bottom ).utf8_str() ) );

        const char* type_str = "unknown";

        switch( via->GetViaType() )
        {
        case VIATYPE::THROUGH:  type_str = "through"; break;
        case VIATYPE::BLIND:    type_str = "blind";   break;
        case VIATYPE::BURIED:   type_str = "buried";  break;
        case VIATYPE::MICROVIA: type_str = "micro";   break;
        default: break;
        }

        py::dict entry;
        entry[ "position_mm" ] = position_mm_dict( via->GetPosition() );
        entry[ "drill_mm" ]    = via->GetDrillValue() * NM_TO_MM_NET;
        entry[ "diameter_mm" ] = via->GetWidth() * NM_TO_MM_NET;
        entry[ "layers" ]      = layers;
        entry[ "type" ]        = std::string( type_str );
        out.append( entry );
    }

    return out;
}


// ─── get_net_length ────────────────────────────────────────────────────
//
// Returns both the raw per-segment track sum (include_vias=False) and
// the delay-engine track+via aggregate (include_vias=True, matches the
// NETINFO_ITEM panel "Net Length").  include_pads folds in pad-to-die
// internal-IC lengths.
//
py::dict netinfo_get_net_length( const std::string& net_name,
                                 bool include_vias,
                                 bool include_pads )
{
    BOARD*        board   = require_active_board_for_netinfo();
    NETINFO_ITEM* net     = require_net_by_name( board, net_name );
    int           netCode = net->GetNetCode();

    NetTotals  totals    = compute_net_totals( board, netCode );
    PCB_TRACK* seedTrack = find_seed_track( board, netCode );

    double calc_track_plus_via_nm = 0.0;
    double calc_pad_to_die_nm     = totals.pad_die_nm;

    if( seedTrack )
    {
        // tuple = (item_count, track+via length nm, pad-to-die nm,
        //          track+via delay, pad-to-die delay)
        auto [ cnt, len_tv, len_pd, td, pd ] = board->GetTrackLength( *seedTrack );
        (void) cnt; (void) td; (void) pd;
        calc_track_plus_via_nm = len_tv;
        calc_pad_to_die_nm     = len_pd;
    }

    double total_nm = include_vias ? calc_track_plus_via_nm : totals.track_nm;

    if( include_pads )
        total_nm += calc_pad_to_die_nm;

    py::dict out;
    out[ "name" ]                     = std::string( net->GetNetname().utf8_str() );
    out[ "code" ]                     = netCode;
    out[ "total_length_mm" ]          = total_nm * NM_TO_MM_NET;
    out[ "track_only_length_mm" ]     = totals.track_nm * NM_TO_MM_NET;
    out[ "track_plus_via_length_mm" ] = calc_track_plus_via_nm * NM_TO_MM_NET;
    out[ "pad_to_die_length_mm" ]     = calc_pad_to_die_nm * NM_TO_MM_NET;
    out[ "num_tracks" ]               = totals.track_count;
    out[ "num_vias" ]                 = totals.via_count;
    out[ "include_vias" ]             = include_vias;
    out[ "include_pads" ]             = include_pads;
    return out;
}


// ─── list_net_classes / get_net_class ──────────────────────────────────
py::list netinfo_list_net_classes()
{
    BOARD*                 board = require_active_board_for_netinfo();
    BOARD_DESIGN_SETTINGS& bds   = board->GetDesignSettings();
    py::list               out;

    if( !bds.m_NetSettings )
        return out;

    if( std::shared_ptr<NETCLASS> dflt = bds.m_NetSettings->GetDefaultNetclass() )
        out.append( netclass_to_dict( dflt.get() ) );

    for( const auto& [name, nc] : bds.m_NetSettings->GetNetclasses() )
    {
        if( !nc || nc->IsDefault() )
            continue;

        out.append( netclass_to_dict( nc.get() ) );
    }

    return out;
}


py::dict netinfo_get_net_class( const std::string& class_name )
{
    BOARD*                 board = require_active_board_for_netinfo();
    BOARD_DESIGN_SETTINGS& bds   = board->GetDesignSettings();

    if( !bds.m_NetSettings )
        throw std::runtime_error( "BOARD has no NET_SETTINGS attached" );

    wxString wxname = wxString::FromUTF8( class_name.c_str() );

    if( std::shared_ptr<NETCLASS> dflt = bds.m_NetSettings->GetDefaultNetclass() )
    {
        if( dflt->GetName() == wxname
            || ( dflt->IsDefault() && wxname == wxT( "Default" ) ) )
            return netclass_to_dict( dflt.get() );
    }

    const auto& classes = bds.m_NetSettings->GetNetclasses();
    auto        it      = classes.find( wxname );

    if( it == classes.end() || !it->second )
        throw std::invalid_argument( "unknown netclass '" + class_name + "'" );

    return netclass_to_dict( it->second.get() );
}


// ─── find_unconnected_pads ─────────────────────────────────────────────
//
// GetNetCode() == 0 → no-net (explicitly unconnected).  Otherwise,
// non-empty CONNECTIVITY_DATA::GetRatsnestForPad means at least one
// expected connection on the pad's net hasn't been satisfied by copper
// (placed but not yet routed).
//
py::list netinfo_find_unconnected_pads()
{
    BOARD*                             board = require_active_board_for_netinfo();
    std::shared_ptr<CONNECTIVITY_DATA> conn  = board->GetConnectivity();
    py::list                           out;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            int  netCode  = pad->GetNetCode();
            bool no_net   = ( netCode == 0 );
            bool has_rats = false;

            if( !no_net && conn )
                has_rats = !conn->GetRatsnestForPad( pad ).empty();

            if( !no_net && !has_rats )
                continue;

            NETINFO_ITEM* netinfo = board->GetNetInfo().GetNetItem( netCode );

            py::dict entry;
            entry[ "footprint_ref" ] = std::string( fp->GetReference().utf8_str() );
            entry[ "pad_number" ]    = std::string( pad->GetNumber().utf8_str() );
            entry[ "net_code" ]      = netCode;
            entry[ "net_name" ]      = netinfo
                    ? std::string( netinfo->GetNetname().utf8_str() )
                    : std::string();
            entry[ "position_mm" ]   = position_mm_dict( pad->GetPosition() );
            entry[ "reason" ]        = no_net ? std::string( "no_net" )
                                              : std::string( "pending_ratsnest" );
            out.append( entry );
        }
    }

    return out;
}


// ─── find_nets_with_no_pads ────────────────────────────────────────────
py::list netinfo_find_nets_with_no_pads()
{
    BOARD* board = require_active_board_for_netinfo();

    std::map<int, int> padCounts;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
            ++padCounts[ pad->GetNetCode() ];
    }

    py::list out;

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        if( !net || net->GetNetCode() == 0 )
            continue;

        if( padCounts[ net->GetNetCode() ] > 0 )
            continue;

        py::dict entry;
        entry[ "name" ]  = std::string( net->GetNetname().utf8_str() );
        entry[ "code" ]  = net->GetNetCode();
        entry[ "class" ] = effective_netclass_name( board, net );
        out.append( entry );
    }

    return out;
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h.
void klicad_register_netinfo_bindings( py::module_& m )
{
    m.doc() = "KliCAD BOARD net inspection / analysis binding — read-only "
              "queries over the live BOARD's NETINFO_LIST, NET_SETTINGS, "
              "and CONNECTIVITY_DATA.  All lengths in mm.";

    m.def( "list_nets", &netinfo_list_nets,
           "Every net with {name, code, class, num_pads, num_tracks, num_vias, "
           "total_length_mm (per-segment track sum), pad_to_die_mm}." );

    m.def( "get_net", &netinfo_get_net, py::arg( "name" ),
           "Same summary dict as list_nets() for one net.  ValueError if unknown." );

    m.def( "get_net_pads", &netinfo_get_net_pads, py::arg( "name" ),
           "Pads on the named net: [{footprint_ref, pad_number, layer, "
           "position_mm:{x_mm,y_mm}}, ...]." );

    m.def( "get_net_tracks", &netinfo_get_net_tracks, py::arg( "name" ),
           "PCB_TRACE_T + PCB_ARC_T on the named net (vias by get_net_vias): "
           "[{start_mm, end_mm, width_mm, length_mm, layer, is_arc}, ...]." );

    m.def( "get_net_vias", &netinfo_get_net_vias, py::arg( "name" ),
           "PCB_VIA_T on the named net: [{position_mm, drill_mm, diameter_mm, "
           "layers:[top,bottom], type:'through'|'blind'|'buried'|'micro'|"
           "'unknown'}, ...]." );

    m.def( "get_net_length", &netinfo_get_net_length,
           py::arg( "name" ),
           py::arg( "include_vias" ) = true,
           py::arg( "include_pads" ) = false,
           "Total length of the named net.  include_vias=True uses the "
           "delay-engine track+via aggregate (panel value); False uses the "
           "raw per-segment track sum.  include_pads adds pad-to-die.  "
           "Returns {name, code, total_length_mm, track_only_length_mm, "
           "track_plus_via_length_mm, pad_to_die_length_mm, num_tracks, "
           "num_vias, include_vias, include_pads}." );

    m.def( "list_net_classes", &netinfo_list_net_classes,
           "All netclasses on the project's NET_SETTINGS (default first)." );

    m.def( "get_net_class", &netinfo_get_net_class, py::arg( "name" ),
           "Netclass detail (use 'Default' for the implicit class).  Returns "
           "{name, description, priority, is_default, clearance_mm, "
           "track_width_mm, via_diameter_mm, via_drill_mm, uvia_diameter_mm, "
           "uvia_drill_mm, diff_pair_width_mm, diff_pair_gap_mm} — float or "
           "None per HasX().  ValueError if unknown." );

    m.def( "find_unconnected_pads", &netinfo_find_unconnected_pads,
           "Pads with GetNetCode()==0 or non-empty ratsnest (placed but not "
           "routed): [{footprint_ref, pad_number, net_code, net_name, "
           "position_mm, reason:'no_net'|'pending_ratsnest'}, ...].  Uses "
           "the in-memory CONNECTIVITY_DATA cache — trigger a recompute "
           "first if you just modified the board." );

    m.def( "find_nets_with_no_pads", &netinfo_find_nets_with_no_pads,
           "Orphan nets in NETINFO_LIST that no pad references (netcode 0 "
           "skipped): [{name, code, class}, ...]." );
}
