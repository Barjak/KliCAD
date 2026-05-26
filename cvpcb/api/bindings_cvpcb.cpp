/*
 * KliCAD subsystem binding: CVPCB state (Pattern B, kiface-resident).
 *
 * Exposes CVPCB_MAINFRAME component-to-footprint association as
 * klicad_native_cvpcb.* — load netlist, enumerate components, list footprint
 * candidates, assign/clear, auto-associate, and save back-annotation.
 *
 * Per BINDING_PATTERN.md Pattern B: spawn FRAME_CVPCB via KIWAY::Player if
 * missing; identify via GetFrameType() + static_cast (cross-kiface
 * dynamic_cast unsafe).  Direct C++ against CVPCB_MAINFRAME + NETLIST.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>

#include <cvpcb_mainframe.h>
#include <cvpcb_association.h>
#include <netlist_reader/pcb_netlist.h>
#include <netlist_reader/netlist_reader.h>
#include <footprint_info.h>
#include <lib_id.h>
#include <richio.h>

#include <wx/string.h>
#include <wx/filename.h>
#include <wx/window.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

KIWAY* find_live_kiway_for_cvpcb()
{
    for( wxWindow* w : wxTopLevelWindows )
        if( KIWAY_HOLDER* h = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( h->HasKiway() )
                return &h->Kiway();
    return nullptr;
}


// Cross-kiface dynamic_cast<CVPCB_MAINFRAME*> is unsafe (typeinfo in the
// kiface bundle).  Use GetFrameType() + static_cast instead.
CVPCB_MAINFRAME* find_cvpcb_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( base && base->GetFrameType() == FRAME_CVPCB )
            return static_cast<CVPCB_MAINFRAME*>( base );
    }
    return nullptr;
}


// Resolve a live CVPCB_MAINFRAME, spawning cvpcb if necessary.
CVPCB_MAINFRAME* require_cvpcb_frame()
{
    if( CVPCB_MAINFRAME* frame = find_cvpcb_frame() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_cvpcb();

    if( !kiway )
        throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );

    kiway->Player( FRAME_CVPCB, true );

    if( CVPCB_MAINFRAME* frame = find_cvpcb_frame() )
        return frame;

    throw std::runtime_error( "failed to obtain CVPCB_MAINFRAME after KIWAY::Player" );
}


// CVPCB_MAINFRAME exposes m_netlist only to its CV::IFACE friend; the
// public readNetListAndFpFiles is protected.  Rather than synthesize a
// fake kiway mail, we load the netlist into a TU-local NETLIST cache via
// NETLIST_READER and write through the public AssociateFootprint() entry-
// point so the UI stays in sync.  Cache is keyed off the frame pointer —
// if the frame is destroyed/recreated, the cache is invalidated.

struct CvpcbState
{
    CVPCB_MAINFRAME*               frame = nullptr;
    std::unique_ptr<NETLIST>       netlist;       // mirror, owned by us
    std::map<wxString, unsigned>   ref_to_index;  // reference -> netlist index

    void rebuild_index()
    {
        ref_to_index.clear();

        if( !netlist )
            return;

        for( unsigned i = 0; i < netlist->GetCount(); ++i )
            ref_to_index[ netlist->GetComponent( i )->GetReference() ] = i;
    }
};

CvpcbState& state()
{
    static CvpcbState s;
    return s;
}


void invalidate_state_if_frame_changed( CVPCB_MAINFRAME* aFrame )
{
    if( state().frame != aFrame )
    {
        state().frame = aFrame;
        state().netlist.reset();
        state().ref_to_index.clear();
    }
}


// Format-autodetecting netlist load.  Throws std::runtime_error on failure.
void load_netlist_file_into( const wxString& aPath, NETLIST& aNetlist )
{
    if( !wxFileName::FileExists( aPath ) )
        throw std::runtime_error( std::string( "netlist file does not exist: " )
                                  + aPath.utf8_string() );

    std::unique_ptr<NETLIST_READER> reader(
            NETLIST_READER::GetNetlistReader( &aNetlist, aPath, wxEmptyString ) );

    if( !reader )
        throw std::runtime_error( std::string( "unable to determine netlist format for " )
                                  + aPath.utf8_string() );

    try
    {
        reader->LoadNetlist();
    }
    catch( const IO_ERROR& ioe )
    {
        throw std::runtime_error( std::string( "netlist load error: " )
                                  + ioe.What().utf8_string() );
    }
}


// Find the netlist index of a component reference, throwing if not loaded
// or not found.
unsigned require_index_for_ref( const std::string& aRef )
{
    if( !state().netlist )
        throw std::runtime_error( "no netlist loaded — call load_netlist(path) first" );

    wxString ref = wxString::FromUTF8( aRef.c_str() );
    auto it = state().ref_to_index.find( ref );

    if( it == state().ref_to_index.end() )
        throw std::runtime_error( std::string( "no component with reference: " ) + aRef );

    return it->second;
}


// load_netlist
py::object cvpcb_load_netlist( const std::string& path )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    wxString wxp = wxString::FromUTF8( path.c_str() );

    auto netlist = std::make_unique<NETLIST>();
    load_netlist_file_into( wxp, *netlist );

    // Populate the frame's footprint list so candidate queries can resolve.
    frame->LoadFootprintFiles();

    // Cache the loaded netlist; list_components() reads from here.  The
    // frame's m_netlist remains independent (only populated when a real
    // schematic is open).
    state().netlist = std::move( netlist );
    state().rebuild_index();

    py::dict result;
    result[ "ok" ]              = true;
    result[ "component_count" ] = static_cast<int>( state().netlist->GetCount() );
    result[ "path" ]            = path;
    return result;
}


// list_components
py::list cvpcb_list_components()
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    py::list result;

    if( !state().netlist )
        return result;

    for( unsigned i = 0; i < state().netlist->GetCount(); ++i )
    {
        COMPONENT* c = state().netlist->GetComponent( i );

        if( !c )
            continue;

        // candidate_count: -1 means "no filter set, anything allowed";
        // otherwise count of loaded footprints matching the filter list.
        int                  candidate_count = -1;
        const wxArrayString& filters         = c->GetFootprintFilters();

        if( filters.GetCount() > 0 && frame->m_FootprintsList )
        {
            candidate_count = 0;

            for( const std::unique_ptr<FOOTPRINT_INFO>& fp : frame->m_FootprintsList->GetList() )
            {
                wxString name = fp->GetFootprintName();

                for( size_t f = 0; f < filters.GetCount(); ++f )
                    if( name.Matches( filters[f] ) ) { ++candidate_count; break; }
            }
        }

        py::dict d;
        d[ "ref" ]               = std::string( c->GetReference().utf8_str() );
        d[ "value" ]             = std::string( c->GetValue().utf8_str() );
        d[ "current_footprint" ] = std::string( c->GetFPID().Format().c_str() );
        d[ "candidate_count" ]   = candidate_count;
        result.append( d );
    }

    return result;
}


// list_footprint_candidates
py::list cvpcb_list_footprint_candidates( const std::string& aRef, const std::string& aFilter )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    py::list result;

    if( !state().netlist )
        throw std::runtime_error( "no netlist loaded — call load_netlist(path) first" );

    if( !frame->m_FootprintsList )
        return result;

    unsigned   idx = require_index_for_ref( aRef );
    COMPONENT* c   = state().netlist->GetComponent( idx );

    if( !c )
        return result;

    const wxArrayString& fpFilters = c->GetFootprintFilters();
    wxString             extraFilter = wxString::FromUTF8( aFilter.c_str() );

    for( const std::unique_ptr<FOOTPRINT_INFO>& fp : frame->m_FootprintsList->GetList() )
    {
        wxString name  = fp->GetFootprintName();
        wxString libid = fp->GetLIB_ID().Format().wx_str();

        // Apply symbol's footprint filters (empty list = anything allowed).
        if( fpFilters.GetCount() > 0 )
        {
            bool matched = false;

            for( size_t f = 0; f < fpFilters.GetCount() && !matched; ++f )
                if( name.Matches( fpFilters[f] ) ) matched = true;

            if( !matched )
                continue;
        }

        // Extra substring filter on LIB_ID.
        if( !extraFilter.IsEmpty() && libid.Find( extraFilter ) == wxNOT_FOUND )
            continue;

        py::dict d;
        d[ "lib_id" ]      = std::string( libid.utf8_str() );
        d[ "description" ] = std::string( fp->GetDesc().utf8_str() );
        result.append( d );
    }

    return result;
}


// assign_footprint
py::object cvpcb_assign_footprint( const std::string& aRef, const std::string& aLibId )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    unsigned idx = require_index_for_ref( aRef );

    LIB_ID   fpid;
    wxString libIdStr = wxString::FromUTF8( aLibId.c_str() );

    if( !libIdStr.IsEmpty() && fpid.Parse( libIdStr ) >= 0 )
        throw std::runtime_error( std::string( "invalid LIB_ID: " ) + aLibId );

    // Mutate our cache and mirror to the frame.  AssociateFootprint
    // early-returns on an empty frame m_netlist — our cache remains the
    // source of truth that save_associations writes from.
    state().netlist->GetComponent( idx )->SetFPID( fpid );
    frame->AssociateFootprint( CVPCB_ASSOCIATION( idx, fpid ), true, true );

    py::dict result;
    result[ "ok" ]     = true;
    result[ "ref" ]    = aRef;
    result[ "lib_id" ] = aLibId;
    return result;
}


// bulk_assign
py::object cvpcb_bulk_assign( const std::map<std::string, std::string>& aAssignments )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    if( !state().netlist )
        throw std::runtime_error( "no netlist loaded — call load_netlist(path) first" );

    py::list errors;
    int      assigned = 0;
    bool     firstAssoc = true;

    for( const auto& [ref, libId] : aAssignments )
    {
        auto it = state().ref_to_index.find( wxString::FromUTF8( ref.c_str() ) );

        if( it == state().ref_to_index.end() )
        {
            errors.append( std::string( "no component with reference: " ) + ref );
            continue;
        }

        LIB_ID   fpid;
        wxString libIdStr = wxString::FromUTF8( libId.c_str() );

        if( !libIdStr.IsEmpty() && fpid.Parse( libIdStr ) >= 0 )
        {
            errors.append( std::string( "invalid LIB_ID for " ) + ref + ": " + libId );
            continue;
        }

        state().netlist->GetComponent( it->second )->SetFPID( fpid );

        // Chain into one undo entry (firstAssoc on first call only).
        frame->AssociateFootprint( CVPCB_ASSOCIATION( it->second, fpid ), firstAssoc, true );
        firstAssoc = false;
        ++assigned;
    }

    py::dict result;
    result[ "ok" ]        = errors.size() == 0;
    result[ "assigned" ]  = assigned;
    result[ "errors" ]    = errors;
    return result;
}


// clear_assignment
py::object cvpcb_clear_assignment( const std::string& aRef )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    unsigned idx = require_index_for_ref( aRef );

    LIB_ID empty;
    state().netlist->GetComponent( idx )->SetFPID( empty );
    frame->AssociateFootprint( CVPCB_ASSOCIATION( idx, empty ), true, true );

    py::dict result;
    result[ "ok" ]  = true;
    result[ "ref" ] = aRef;
    return result;
}


// auto_associate
// LIMITATION: AutomaticFootprintMatching operates on the frame's internal
// m_netlist (only populated when cvpcb opened against a live project), not
// on load_netlist()'s cache.  Useful in the GUI-driven case only.
py::object cvpcb_auto_associate()
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    frame->AutomaticFootprintMatching();

    py::dict result;
    result[ "ok" ]   = true;
    result[ "note" ] = "operates on frame's internal m_netlist, not "
                       "load_netlist()'s cache";
    return result;
}


// save_associations
py::object cvpcb_save_associations( const std::string& aPath )
{
    CVPCB_MAINFRAME* frame = require_cvpcb_frame();
    invalidate_state_if_frame_changed( frame );

    if( !state().netlist )
        throw std::runtime_error( "no netlist loaded — call load_netlist(path) first" );

    wxString wxp = wxString::FromUTF8( aPath.c_str() );

    // Format cached netlist in cvpcb back-annotation form (.cmp-style).
    STRING_FORMATTER sf;

    try
    {
        state().netlist->FormatCvpcbNetlist( &sf );
    }
    catch( const IO_ERROR& ioe )
    {
        throw std::runtime_error( std::string( "FormatCvpcbNetlist failed: " )
                                  + ioe.What().utf8_string() );
    }

    const std::string& payload = sf.GetString();
    FILE*              fp      = wxFopen( wxp, wxT( "wb" ) );

    if( !fp )
        throw std::runtime_error( std::string( "could not open for writing: " ) + aPath );

    size_t written = std::fwrite( payload.data(), 1, payload.size(), fp );
    std::fclose( fp );

    if( written != payload.size() )
        throw std::runtime_error( std::string( "short write to " ) + aPath );

    py::dict result;
    result[ "ok" ]            = true;
    result[ "path" ]          = aPath;
    result[ "bytes_written" ] = static_cast<int>( written );
    return result;
}


} // anon


// Registered at kiface-load time (see klicad_kiface_register.h).
void klicad_register_cvpcb_bindings_impl( py::module_& m )
{
    m.doc() = "KliCAD CVPCB_MAINFRAME binding — programmatic component-to-"
              "footprint association.  Maintains a TU-local NETLIST cache "
              "from load_netlist(path); assignments mutate the cache and "
              "(if non-empty) the frame's internal netlist.";

    m.def( "load_netlist", &cvpcb_load_netlist, py::arg( "path" ),
           "Load .net/.xml netlist; returns {ok, component_count, path}." );
    m.def( "list_components", &cvpcb_list_components,
           "[{ref, value, current_footprint, candidate_count}, ...].  "
           "candidate_count=-1 means no filter set." );
    m.def( "list_footprint_candidates", &cvpcb_list_footprint_candidates,
           py::arg( "ref" ), py::arg( "filter" ) = std::string(),
           "Footprints matching symbol's filters; optional LIB_ID substring "
           "filter.  [{lib_id, description}, ...]." );
    m.def( "assign_footprint", &cvpcb_assign_footprint,
           py::arg( "ref" ), py::arg( "lib_id" ),
           "Set the footprint for one component." );
    m.def( "bulk_assign", &cvpcb_bulk_assign, py::arg( "assignments" ),
           "Batch {ref: lib_id} assignment, one undo step." );
    m.def( "clear_assignment", &cvpcb_clear_assignment, py::arg( "ref" ),
           "Clear the footprint assignment for one component." );
    m.def( "auto_associate", &cvpcb_auto_associate,
           "Run cvpcb's .equ-driven matcher (operates on frame m_netlist, "
           "not load_netlist()'s cache)." );
    m.def( "save_associations", &cvpcb_save_associations, py::arg( "path" ),
           "Write cached netlist to a .cmp-style back-annotation file." );
}
