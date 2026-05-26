/*
 * KliCAD binding: schematic -> PCB sync (klicad_native_sync.*).
 *
 * Mirrors DIALOG_UPDATE_PCB::PerformUpdate: fetch NETLIST (KIWAY
 * MAIL_SCH_GET_NETLIST against eeschema, or from a file), run
 * BOARD_NETLIST_UPDATER, finalize BOARD.  Pattern B (kiface-resident);
 * EDA_BASE_FRAME::GetFrameType() + static_cast for frame discovery.
 * See bindings_pcb_state.cpp for the rationale on both points.
 * CAPTURING_REPORTER records (severity, text) so we can return
 * structured warnings/errors + heuristically classified add/remove/
 * modify slices (BOARD_NETLIST_UPDATER only exposes GetAddedFootprints
 * directly; other buckets are keyword-scraped from ACTION text).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <mail_type.h>

#include <eda_base_frame.h>
#include <ki_exception.h>
#include <reporter.h>
#include <richio.h>

#include <board.h>
#include <footprint.h>
#include <pcb_base_frame.h>
#include <pcb_edit_frame.h>
#include <pcb_draw_panel_gal.h>

#include <component_classes/component_class_manager.h>

#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-TU helpers (unique vs find_live_kiway_for_pcb_state / *_drc_rules / etc).
KIWAY* find_live_kiway_for_sync()
{
    for( wxWindow* w : wxTopLevelWindows )
        if( KIWAY_HOLDER* h = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( h->HasKiway() )
                return &h->Kiway();
    return nullptr;
}


PCB_EDIT_FRAME* find_pcb_edit_frame_for_sync()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w ) )
            if( base->GetFrameType() == FRAME_PCB_EDITOR )
                return static_cast<PCB_EDIT_FRAME*>( base );
    }
    return nullptr;
}


PCB_EDIT_FRAME* require_pcb_frame_for_sync()
{
    if( PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_sync() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_sync();

    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running?" );

    kiway->Player( FRAME_PCB_EDITOR, true );
    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_sync();

    if( !frame )
        throw std::runtime_error( "failed to obtain PCB_EDIT_FRAME after KIWAY::Player(FRAME_PCB_EDITOR, true)" );

    return frame;
}


// REPORTER that appends to a wxString blob AND records each
// (severity, message) pair — WX_STRING_REPORTER flattens severity away.
class CAPTURING_REPORTER : public REPORTER
{
public:
    struct Entry
    {
        int      severity;   // SEVERITY enum value
        wxString text;
    };

    CAPTURING_REPORTER() = default;
    ~CAPTURING_REPORTER() override = default;

    REPORTER& Report( const wxString& aText,
                      SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override
    {
        REPORTER::Report( aText, aSeverity );

        m_entries.push_back( { static_cast<int>( aSeverity ), aText } );

        if( !m_text.IsEmpty() )
            m_text << wxT( "\n" );

        m_text << aText;

        return *this;
    }

    void Clear() override
    {
        REPORTER::Clear();
        m_entries.clear();
        m_text.clear();
    }

    const wxString&           GetText() const    { return m_text; }
    const std::vector<Entry>& GetEntries() const { return m_entries; }

private:
    wxString           m_text;
    std::vector<Entry> m_entries;
};


// Persisted "last sync report" for get_last_sync_report().  Mutex-
// guarded just in case (we run on the main thread under the GIL).
struct LastSyncReport
{
    bool                     valid = false;
    bool                     ok = false;
    bool                     dry_run = false;
    std::string              text;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<std::string> info;
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> modified;
    int                      warning_count = 0;
    int                      error_count = 0;
    int                      added_count = 0;
    std::string              source;  // "schematic" or "file:<path>"
};

std::mutex      g_last_report_mutex;
LastSyncReport  g_last_report;


// Classify a RPT_SEVERITY_ACTION line by keyword (BOARD_NETLIST_UPDATER
// doesn't expose typed change lists).  Keywords come from
// board_netlist_updater.cpp: "Add..." "Remove..."/"Delet..."
// "Change..."/"Update..."/"Reconnect..."/"Rename..."/"Replace...".
enum class ChangeBucket
{
    NONE,
    ADDED,
    REMOVED,
    MODIFIED
};

ChangeBucket classify_action_line( const wxString& aLine )
{
    wxString lower = aLine.Lower();

    // Order matters: a "Remove pad ... and add ..." line shouldn't be
    // double-counted; we pick the first match.
    if( lower.Contains( wxT( "add" ) )
        && ( lower.Contains( wxT( "footprint" ) )
             || lower.Contains( wxT( "symbol" ) )
             || lower.Contains( wxT( "component" ) ) ) )
    {
        return ChangeBucket::ADDED;
    }

    if( lower.Contains( wxT( "remov" ) )
        || lower.Contains( wxT( "delet" ) ) )
    {
        return ChangeBucket::REMOVED;
    }

    if( lower.Contains( wxT( "change" ) )
        || lower.Contains( wxT( "update" ) )
        || lower.Contains( wxT( "reconnect" ) )
        || lower.Contains( wxT( "rename" ) )
        || lower.Contains( wxT( "replace" ) ) )
    {
        return ChangeBucket::MODIFIED;
    }

    return ChangeBucket::NONE;
}


// Build the structured-result dict.  Also writes-through to
// g_last_report so get_last_sync_report() can re-fetch.
py::dict build_report_dict( const CAPTURING_REPORTER& aReporter,
                            const std::vector<FOOTPRINT*>& aAddedFootprints,
                            bool aOk, bool aDryRun, const std::string& aSource )
{
    std::vector<std::string> warnings, errors, info, added, removed, modified;

    for( const CAPTURING_REPORTER::Entry& e : aReporter.GetEntries() )
    {
        std::string utf8( e.text.utf8_str() );

        switch( e.severity )
        {
        case RPT_SEVERITY_ERROR:   errors.push_back( utf8 );   break;
        case RPT_SEVERITY_WARNING: warnings.push_back( utf8 ); break;

        case RPT_SEVERITY_ACTION:
        case RPT_SEVERITY_INFO:
            if( e.severity == RPT_SEVERITY_INFO )
                info.push_back( utf8 );

            switch( classify_action_line( e.text ) )
            {
            case ChangeBucket::ADDED:    added.push_back( utf8 );    break;
            case ChangeBucket::REMOVED:  removed.push_back( utf8 );  break;
            case ChangeBucket::MODIFIED: modified.push_back( utf8 ); break;
            case ChangeBucket::NONE: break;
            }
            break;

        default: break;
        }
    }

    // Authoritative added-footprint list (ground truth — reporter scrape
    // above is best-effort).
    py::list added_fps;

    for( FOOTPRINT* fp : aAddedFootprints )
    {
        if( !fp )
            continue;

        py::dict entry;
        entry[ "reference" ] = std::string( fp->GetReference().utf8_str() );
        entry[ "value" ]     = std::string( fp->GetValue().utf8_str() );
        entry[ "lib_id" ]    = std::string( fp->GetFPID().Format().c_str() );
        entry[ "kiid" ]      = fp->m_Uuid.AsStdString();
        added_fps.append( entry );
    }

    py::dict changes;
    changes[ "added" ]            = py::cast( added );
    changes[ "removed" ]          = py::cast( removed );
    changes[ "modified" ]         = py::cast( modified );
    changes[ "added_footprints" ] = added_fps;

    std::string text_utf8( aReporter.GetText().utf8_str() );

    py::dict result;
    result[ "ok" ]            = aOk;
    result[ "dry_run" ]       = aDryRun;
    result[ "report" ]        = text_utf8;
    result[ "changes" ]       = changes;
    result[ "warnings" ]      = py::cast( warnings );
    result[ "errors" ]        = py::cast( errors );
    result[ "info" ]          = py::cast( info );
    result[ "warning_count" ] = static_cast<int>( warnings.size() );
    result[ "error_count" ]   = static_cast<int>( errors.size() );
    result[ "added_count" ]   = static_cast<int>( aAddedFootprints.size() );
    result[ "source" ]        = aSource;

    {
        std::lock_guard<std::mutex> lock( g_last_report_mutex );
        LastSyncReport snap;
        snap.valid         = true;
        snap.ok            = aOk;
        snap.dry_run       = aDryRun;
        snap.text          = text_utf8;
        snap.warnings      = warnings;
        snap.errors        = errors;
        snap.info          = info;
        snap.added         = added;
        snap.removed       = removed;
        snap.modified      = modified;
        snap.warning_count = static_cast<int>( warnings.size() );
        snap.error_count   = static_cast<int>( errors.size() );
        snap.added_count   = static_cast<int>( aAddedFootprints.size() );
        snap.source        = aSource;
        g_last_report      = std::move( snap );
    }

    return result;
}


// Sync options pulled from the caller's py::dict (unknown keys ignored).
struct SyncOptions
{
    bool replace_footprints  = false;
    bool delete_unused       = false;  // delete_singles in the option dict
    bool delete_unused_pads  = false;
    bool force_match         = false;  // ignore-timestamp / match-by-ref
    bool transfer_groups     = true;
    bool override_locks      = false;
    bool update_fields       = true;
    bool remove_extra_fields = false;
    bool dry_run             = false;
};

SyncOptions parse_sync_options( const py::dict& aOptions )
{
    SyncOptions out;
    auto get = [&]( const char* k, bool& s )
               { if( aOptions.contains( k ) ) s = aOptions[ k ].cast<bool>(); };

    get( "replace_footprints",  out.replace_footprints );
    get( "delete_singles",      out.delete_unused );
    get( "delete_unused",       out.delete_unused );      // alias
    get( "delete_unused_pads",  out.delete_unused_pads );
    get( "force_match",         out.force_match );
    get( "transfer_groups",     out.transfer_groups );
    get( "override_locks",      out.override_locks );
    get( "update_fields",       out.update_fields );
    get( "remove_extra_fields", out.remove_extra_fields );
    get( "dry_run",             out.dry_run );
    return out;
}


// Configure NETLIST + BOARD_NETLIST_UPDATER per parsed options.  Mirrors
// DIALOG_UPDATE_PCB::PerformUpdate.  FindByTimeStamp=true == match by
// KIID; force_match flips on the more aggressive by-refdes mode.
void apply_sync_options( NETLIST& aNetlist, BOARD_NETLIST_UPDATER& aUpdater,
                         const SyncOptions& aOpts )
{
    aNetlist.SetFindByTimeStamp( !aOpts.force_match );
    aNetlist.SetReplaceFootprints( aOpts.replace_footprints );
    aUpdater.SetIsDryRun( aOpts.dry_run );
    aUpdater.SetLookupByTimestamp( !aOpts.force_match );
    aUpdater.SetDeleteUnusedFootprints( aOpts.delete_unused );
    aUpdater.SetReplaceFootprints( aOpts.replace_footprints );
    aUpdater.SetTransferGroups( aOpts.transfer_groups );
    aUpdater.SetOverrideLocks( aOpts.override_locks );
    aUpdater.SetUpdateFields( aOpts.update_fields );
    aUpdater.SetRemoveExtraFields( aOpts.remove_extra_fields );
}


// Fetch the live schematic's netlist via KIWAY mail.  Like
// PCB_EDIT_FRAME::FetchNetlistFromSchematic but writes failures to the
// reporter instead of popping a wxDialog.
bool fetch_netlist_from_schematic( PCB_EDIT_FRAME* aFrame,
                                   NETLIST& aNetlist,
                                   REPORTER& aReporter )
{
    // Sentinel convention from PCB_EDIT_FRAME::FetchNetlistFromSchematic:
    // if eeschema returns the payload unchanged, the fetch failed.
    std::string annotateHint = "klicad_native_sync: schematic netlist fetch failed";
    std::string payload      = annotateHint;

    aFrame->Kiway().ExpressMail( FRAME_SCH, MAIL_SCH_GET_NETLIST, payload, aFrame );

    if( payload == annotateHint )
    {
        aReporter.Report( wxT( "Schematic netlist fetch failed: eeschema returned no payload. "
                               "Is eeschema's schematic open and fully annotated?" ),
                          RPT_SEVERITY_ERROR );
        return false;
    }

    try
    {
        // KICAD_NETLIST_READER takes ownership of the LINE_READER.
        auto reader = new STRING_LINE_READER( payload, wxT( "klicad_native_sync" ) );
        KICAD_NETLIST_READER netlistReader( reader, &aNetlist );
        netlistReader.LoadNetlist();
    }
    catch( const IO_ERROR& ioe )
    {
        aReporter.Report( wxT( "Failed to parse netlist payload: " ) + ioe.What(),
                          RPT_SEVERITY_ERROR );
        return false;
    }
    catch( const std::exception& ex )
    {
        aReporter.Report( wxString( wxT( "Netlist reader exception: " ) )
                              + wxString::FromUTF8( ex.what() ),
                          RPT_SEVERITY_ERROR );
        return false;
    }

    return true;
}


// File-based netlist reader.  Same write-to-reporter pattern.
bool fetch_netlist_from_file( PCB_EDIT_FRAME* aFrame,
                              const wxString& aPath,
                              NETLIST& aNetlist,
                              REPORTER& aReporter )
{
    wxFileName fn( aPath );

    if( !fn.IsOk() || !fn.FileExists() )
    {
        aReporter.Report( wxT( "Netlist file does not exist: " ) + aPath,
                          RPT_SEVERITY_ERROR );
        return false;
    }

    try
    {
        std::unique_ptr<NETLIST_READER> reader(
                NETLIST_READER::GetNetlistReader( &aNetlist, aPath, wxEmptyString ) );

        if( !reader.get() )
        {
            aReporter.Report( wxT( "Cannot open netlist file (unrecognized format): " ) + aPath,
                              RPT_SEVERITY_ERROR );
            return false;
        }

        reader->LoadNetlist();
        aFrame->LoadFootprints( aNetlist, aReporter );  // matches ReadNetlistFromFile
    }
    catch( const IO_ERROR& ioe )
    {
        aReporter.Report( wxT( "I/O error loading netlist file: " ) + ioe.What(),
                          RPT_SEVERITY_ERROR );
        return false;
    }
    catch( const std::exception& ex )
    {
        aReporter.Report( wxString( wxT( "Netlist file reader exception: " ) )
                              + wxString::FromUTF8( ex.what() ),
                          RPT_SEVERITY_ERROR );
        return false;
    }

    return true;
}


// Drive BOARD_NETLIST_UPDATER and finalize the board.  Returns the
// structured report dict.
py::dict run_netlist_update( PCB_EDIT_FRAME* aFrame,
                             NETLIST& aNetlist,
                             const SyncOptions& aOpts,
                             CAPTURING_REPORTER& aReporter,
                             const std::string& aSource )
{
    BOARD* board = aFrame->GetBoard();

    if( !board )
    {
        aReporter.Report( wxT( "PCB_EDIT_FRAME has no active BOARD" ),
                          RPT_SEVERITY_ERROR );
        return build_report_dict( aReporter,
                                  std::vector<FOOTPRINT*>{}, false,
                                  aOpts.dry_run, aSource );
    }

    aNetlist.SortByReference();

    BOARD_NETLIST_UPDATER updater( aFrame, board );
    updater.SetReporter( &aReporter );
    apply_sync_options( aNetlist, updater, aOpts );

    bool ok = true;

    try
    {
        // Release the GIL across long C++ work.
        py::gil_scoped_release nogil;
        ok = updater.UpdateNetlist( aNetlist );
    }
    catch( const std::exception& ex )
    {
        aReporter.Report(
            wxString( wxT( "BOARD_NETLIST_UPDATER threw std::exception: " ) )
                + wxString::FromUTF8( ex.what() ),
            RPT_SEVERITY_ERROR );
        ok = false;
    }
    catch( ... )
    {
        aReporter.Report( wxT( "BOARD_NETLIST_UPDATER threw unknown exception" ),
                          RPT_SEVERITY_ERROR );
        ok = false;
    }

    std::vector<FOOTPRINT*> added = updater.GetAddedFootprints();

    // Replicate the data-side of PCB_EDIT_FRAME::OnNetlistChanged
    // (re-sync nets + netclasses, rebuild component-class caches,
    // refresh canvas).  We skip the SpreadFootprints / drag-command UI
    // step — scripted callers place footprints via klicad_native_pcb_state.
    if( !aOpts.dry_run && ok )
    {
        try
        {
            board->SynchronizeNetsAndNetClasses( false );
            board->GetComponentClassManager().InvalidateComponentClasses();
            board->GetComponentClassManager().RebuildRequiredCaches();
        }
        catch( const std::exception& ex )
        {
            aReporter.Report(
                wxString( wxT( "Post-update finalize threw: " ) )
                    + wxString::FromUTF8( ex.what() ),
                RPT_SEVERITY_WARNING );
        }

        if( aFrame->GetCanvas() )
            aFrame->GetCanvas()->Refresh();
    }

    return build_report_dict( aReporter, added, ok, aOpts.dry_run, aSource );
}


// update_pcb_from_schematic
py::dict sync_update_pcb_from_schematic( const py::dict& options )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_sync();

    SyncOptions       opts = parse_sync_options( options );
    CAPTURING_REPORTER reporter;
    NETLIST            netlist;

    if( !fetch_netlist_from_schematic( frame, netlist, reporter ) )
    {
        return build_report_dict( reporter,
                                  std::vector<FOOTPRINT*>{}, false,
                                  opts.dry_run, std::string( "schematic" ) );
    }

    return run_netlist_update( frame, netlist, opts, reporter,
                               std::string( "schematic" ) );
}


// dry_run_update — forces dry_run = True without mutating caller's dict
py::dict sync_dry_run_update( const py::dict& options )
{
    py::dict effective;

    for( auto kv : options )
        effective[ kv.first ] = kv.second;

    effective[ "dry_run" ] = true;
    return sync_update_pcb_from_schematic( effective );
}


// import_netlist
py::dict sync_import_netlist( const std::string& path, const py::dict& options )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_sync();

    SyncOptions       opts = parse_sync_options( options );
    CAPTURING_REPORTER reporter;
    NETLIST            netlist;

    wxString wxpath = wxString::FromUTF8( path.c_str() );

    if( !fetch_netlist_from_file( frame, wxpath, netlist, reporter ) )
    {
        return build_report_dict( reporter,
                                  std::vector<FOOTPRINT*>{}, false,
                                  opts.dry_run,
                                  std::string( "file:" ) + path );
    }

    return run_netlist_update( frame, netlist, opts, reporter,
                               std::string( "file:" ) + path );
}


// get_last_sync_report
py::object sync_get_last_sync_report()
{
    std::lock_guard<std::mutex> lock( g_last_report_mutex );

    if( !g_last_report.valid )
        return py::none();

    py::dict changes;
    changes[ "added" ]    = py::cast( g_last_report.added );
    changes[ "removed" ]  = py::cast( g_last_report.removed );
    changes[ "modified" ] = py::cast( g_last_report.modified );

    py::dict result;
    result[ "ok" ]            = g_last_report.ok;
    result[ "dry_run" ]       = g_last_report.dry_run;
    result[ "report" ]        = g_last_report.text;
    result[ "changes" ]       = changes;
    result[ "warnings" ]      = py::cast( g_last_report.warnings );
    result[ "errors" ]        = py::cast( g_last_report.errors );
    result[ "info" ]          = py::cast( g_last_report.info );
    result[ "warning_count" ] = g_last_report.warning_count;
    result[ "error_count" ]   = g_last_report.error_count;
    result[ "added_count" ]   = g_last_report.added_count;
    result[ "source" ]        = g_last_report.source;
    return result;
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h.
void klicad_register_sync_bindings( py::module_& m )
{
    m.doc() = "KliCAD schematic -> PCB sync.  Pushes a fresh netlist into "
              "the active BOARD via BOARD_NETLIST_UPDATER (the GUI 'Update "
              "PCB from Schematic' dialog's backend).  Source: live eeschema "
              "(KIWAY mail MAIL_SCH_GET_NETLIST) or a netlist file.\n\n"
              "options dict (all bool, defaults): replace_footprints=False, "
              "delete_singles=False (alias delete_unused), delete_unused_pads"
              "=False (no upstream knob, ignored), force_match=False (match "
              "by refdes), transfer_groups=True, override_locks=False, "
              "update_fields=True, remove_extra_fields=False, dry_run=False.\n\n"
              "Returns dict: ok, dry_run, report (str), changes {added, "
              "removed, modified, added_footprints[{reference,value,lib_id,"
              "kiid}]}, warnings, errors, info, *_count, source ('schematic' "
              "or 'file:<path>').";

    m.def( "update_pcb_from_schematic", &sync_update_pcb_from_schematic,
           py::arg( "options" ) = py::dict(),
           "Push the live schematic netlist into the active BOARD." );

    m.def( "dry_run_update", &sync_dry_run_update,
           py::arg( "options" ) = py::dict(),
           "Shortcut for update_pcb_from_schematic(...) with dry_run=True." );

    m.def( "import_netlist", &sync_import_netlist,
           py::arg( "path" ), py::arg( "options" ) = py::dict(),
           "Like update_pcb_from_schematic but read the netlist from `path`." );

    m.def( "get_last_sync_report", &sync_get_last_sync_report,
           "Return the result dict from the most recent sync call, or None." );
}
