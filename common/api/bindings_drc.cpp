/*
 * KliCAD subsystem binding: DRC.
 *
 * Exposes Design Rules Check as kicad_native.drc.run(board_path, ...).
 *
 * Pattern (every subsystem binding TU mirrors this shape):
 *   1. Pure-Python entry: kicad_native.drc.run(path, options...) -> dict
 *   2. Internally constructs the matching JOB_* class
 *   3. Dispatches via the live KIWAY (found via toplevel-window walk)
 *   4. Writes JSON to a temp file, parses, returns dict — never raw streams
 *   5. Single round trip; no progress callbacks (cf. KliCAD bundle paradigm)
 *
 * Agents binding other JOB_*-backed subsystems should copy this file and
 * substitute the JOB class, KIWAY::FACE_T, and any extra config fields.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_pcb_drc.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/wfstream.h>
#include <wx/window.h>

#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace py = pybind11;

namespace
{

// Walk the live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with
// a live KIWAY, and return that KIWAY*.  Returns nullptr if no GUI frame is
// up (headless kicad-cli api-server case — DRC needs a Kiway, so caller
// should raise).
KIWAY* find_live_kiway()
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


std::string slurp_file( const wxString& aPath )
{
    std::ifstream f( aPath.ToStdString() );
    if( !f )
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}


// Resolve a path to its canonical real form (follows symlinks).  KliCAD
// targets macOS + Linux only, so POSIX realpath is fine.  Used to compare
// the requested board path against the editor's loaded board without being
// fooled by the macOS /tmp -> /private/tmp symlink (which would otherwise
// make us re-open an identical board and trip the BOARD::ClearProject
// same-path crash).
wxString canonical_path( const wxString& aPath )
{
    if( aPath.IsEmpty() )
        return wxEmptyString;

    char resolved[PATH_MAX];

    if( realpath( aPath.fn_str(), resolved ) )
        return wxString::FromUTF8( resolved );

    // realpath fails when the file doesn't exist — fall back to a plain
    // absolute path so the comparison still does something sensible.
    wxFileName fn( aPath );
    fn.MakeAbsolute();
    return fn.GetFullPath();
}


JOB_RC::UNITS units_from_string( const std::string& s )
{
    if( s == "mm" )    return JOB_RC::UNITS::MM;
    if( s == "in" )    return JOB_RC::UNITS::INCH;
    if( s == "inch" )  return JOB_RC::UNITS::INCH;
    if( s == "mils" )  return JOB_RC::UNITS::MILS;
    throw std::invalid_argument( "units must be one of: 'mm', 'in', 'mils' (got '" + s + "')" );
}


int severity_from_string( const std::string& s )
{
    // RPT_SEVERITY_* values from widgets/report_severity.h.  Bitmask.
    // For DRC reporting we include ERROR | WARNING by default; user can pass
    // 'all' to include EXCLUSION + ACTION + INFO too.
    if( s == "error" )   return RPT_SEVERITY_ERROR;
    if( s == "warning" ) return RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING;
    if( s == "all" )     return RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING
                              | RPT_SEVERITY_EXCLUSION | RPT_SEVERITY_ACTION
                              | RPT_SEVERITY_INFO;
    throw std::invalid_argument( "severity must be one of: 'error', 'warning', 'all' (got '"
                                 + s + "')" );
}


py::object run_drc( const std::string& board_path,
                    const std::string& units,
                    const std::string& severity,
                    bool all_track_errors,
                    bool schematic_parity,
                    bool refill_zones )
{
    KIWAY* kiway = find_live_kiway();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(DRC needs the pcbnew kiface and project state to be loaded)" );

    // PCBNEW_JOBS_HANDLER::getBoard, in GUI mode with a project open, returns
    // editFrame->GetBoard() and IGNORES the job's m_filename.  So a DRC job
    // nominally targeting `board_path` would actually run against whatever
    // board the PCB editor currently holds — including the empty board of a
    // freshly-spawned editor frame, which yields a bogus "no edges found on
    // Edge.Cuts" / invalid_outline.  To make drc.run(board_path) honour its
    // argument, load the requested board into the editor ourselves before
    // dispatching the job.
    KIWAY_PLAYER* pcbFrame = kiway->Player( FRAME_PCB_EDITOR, true );

    if( !pcbFrame )
    {
        throw std::runtime_error(
            "DRC: could not obtain the PCB editor frame "
            "(KIWAY::Player(FRAME_PCB_EDITOR) returned null)" );
    }

    // Only (re)load when the editor isn't already showing the requested
    // board.  Re-opening the identical path would hit the BOARD::ClearProject
    // same-path crash — see the matching guard in pcb_state.open_board.
    // canonical_path() resolves symlinks so /tmp vs /private/tmp doesn't
    // fool us into a needless (and crash-prone) reload.
    const wxString wantedBoard = canonical_path( wxString::FromUTF8( board_path ) );
    const wxString loadedBoard = canonical_path( pcbFrame->GetCurrentFileName() );

    if( wantedBoard != loadedBoard )
    {
        if( !pcbFrame->OpenProjectFiles( { wxString::FromUTF8( board_path ) }, 0 ) )
        {
            throw std::runtime_error(
                "DRC: failed to load board '" + board_path + "' into the PCB "
                "editor (KIWAY_PLAYER::OpenProjectFiles returned false)" );
        }
    }

    // Temp file for the JSON report — DRC writes to disk and we slurp it back.
    wxFileName tmpFile;
    tmpFile.AssignTempFileName( wxS( "klicad_drc_" ) );
    tmpFile.SetExt( wxS( "json" ) );
    const wxString outPath = tmpFile.GetFullPath();

    JOB_PCB_DRC drcJob;
    drcJob.m_filename             = wxString::FromUTF8( board_path );
    drcJob.SetConfiguredOutputPath( outPath );
    drcJob.m_units                = units_from_string( units );
    drcJob.m_severity             = severity_from_string( severity );
    drcJob.m_format               = JOB_PCB_DRC::OUTPUT_FORMAT::JSON;
    drcJob.m_reportAllTrackErrors = all_track_errors;
    drcJob.m_parity               = schematic_parity;
    drcJob.m_refillZones          = refill_zones;
    drcJob.m_saveBoard            = false;
    drcJob.m_exitCodeViolations   = false;

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // ProcessJob blocks on the main thread and may dispatch into wx event
        // handling — release the GIL so any nested Python callbacks (none today,
        // but future bindings might) can re-acquire it.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_PCB, &drcJob, &reporter );
    }

    py::dict result;
    result[ "exit_code" ]   = exitCode;
    result[ "ok" ]          = ( exitCode == 0 );
    result[ "report_path" ] = outPath.ToStdString();
    result[ "messages" ]    = reporter.GetMessages().ToStdString();

    // Slurp the JSON report and embed it as parsed Python dict for convenience.
    std::string json_text = slurp_file( outPath );
    result[ "json_text" ] = json_text;

    if( !json_text.empty() )
    {
        try
        {
            py::object json_module = py::module_::import( "json" );
            result[ "report" ] = json_module.attr( "loads" )( json_text );
        }
        catch( const py::error_already_set& )
        {
            // Leave 'report' absent on parse failure; caller can fall back to
            // json_text.
        }
    }

    wxRemoveFile( outPath );
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( kicad_native_drc, m )
{
    m.doc() = "KliCAD DRC subsystem binding (calls JOB_PCB_DRC under the hood). "
              "Returns a structured dict; never streams. Requires KiCad's GUI to "
              "be running (needs a live KIWAY).";

    m.def( "run", &run_drc,
           py::arg( "board_path" ),
           py::arg( "units" )             = "mm",
           py::arg( "severity" )          = "warning",
           py::arg( "all_track_errors" )  = false,
           py::arg( "schematic_parity" )  = false,
           py::arg( "refill_zones" )      = false,
           R"DOC(Run DRC on the .kicad_pcb file at board_path.

board_path is honoured: the binding loads that board into the PCB editor
before dispatching the job.  This is necessary because PCBNEW_JOBS_HANDLER::
getBoard() ignores the job's m_filename in GUI mode and uses whatever board
the editor currently holds — so without the explicit load, drc.run() would
silently check the wrong board (or an empty one, yielding a spurious
invalid_outline).  If the editor already shows board_path, no reload happens
(re-opening the same path would trip the BOARD::ClearProject same-path crash).

Returns a dict with keys:
  - ok           bool       True iff job exited cleanly (NOT iff zero violations)
  - exit_code    int        raw exit code from JOB_PCB_DRC dispatch
  - messages     str        reporter output (status, info, warnings from the run itself)
  - report_path  str        temp file the JSON was written to (deleted before return)
  - json_text    str        raw JSON report content
  - report       dict       parsed JSON (only present if parse succeeded); has keys like
                            'violations', 'unconnected_items', 'schematic_parity', ...

severity: 'error' | 'warning' (default; includes errors) | 'all'
units:    'mm' (default) | 'in' | 'mils'

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running),
or if board_path can't be loaded into the PCB editor.
)DOC" );
}
