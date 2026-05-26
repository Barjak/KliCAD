/*
 * KliCAD subsystem binding: JOBSET (the umbrella job-runner).
 *
 * Exposes two operations on a .kicad_jobset file:
 *
 *   klicad_native_jobset.load(jobset_path)              -> dict
 *       Parses the jobset file and returns a description of its jobs and
 *       destinations.  Does NOT execute anything; cheap, no KIWAY needed.
 *
 *   klicad_native_jobset.run(jobset_path,
 *                           destinations=None,
 *                           stop_on_error=False)       -> dict
 *       Dispatches the jobset through JOBS_RUNNER, mirroring what
 *       command_jobset_run.cpp and panel_jobset.cpp do.  Requires a live
 *       KIWAY (KiCad GUI running), reuses the currently-loaded PROJECT.
 *
 * The jobset is the only binding in this directory whose payload can call
 * into both KIFACEs (PCB + SCH + special special_execute/special_copyfiles),
 * so we must spawn both editor frames defensively, the same way panel_jobset
 * does via EnsurePcbSchFramesOpen().
 *
 * For the per-subsystem binding pattern see bindings_drc.cpp.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/jobset.h>
#include <jobs/jobs_output.h>
#include <jobs_runner.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <pgm_base.h>
#include <project.h>
#include <reporter.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-TU naming convention; mirrors find_live_kiway_for_erc et al.
// Verified non-colliding via grep across common/api/bindings_*.cpp.
KIWAY* find_live_kiway_for_jobset()
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


// Translate the destination-type enum to a stable string for the dict.
std::string jobset_destination_type_to_string( JOBSET_DESTINATION_T t )
{
    switch( t )
    {
    case JOBSET_DESTINATION_T::FOLDER:  return "folder";
    case JOBSET_DESTINATION_T::ARCHIVE: return "archive";
    }
    return "unknown";
}


// Serialize one JOBSET_JOB to a Python dict.
py::dict jobset_job_to_dict( const JOBSET_JOB& aJob )
{
    py::dict d;
    d[ "uuid" ]        = aJob.m_id.ToStdString();
    d[ "type" ]        = aJob.m_type.ToStdString();
    d[ "description" ] = aJob.GetDescription().ToStdString();

    if( aJob.m_job )
    {
        d[ "default_description" ] = aJob.m_job->GetDefaultDescription().ToStdString();
        d[ "output_path" ]         = aJob.m_job->GetConfiguredOutputPath().ToStdString();
    }
    else
    {
        // Unknown job type — JOB_REGISTRY didn't recognise m_type at load time.
        d[ "default_description" ] = std::string();
        d[ "output_path" ]         = std::string();
        d[ "warning" ]             = std::string( "job class not registered for this type" );
    }

    return d;
}


// Serialize one JOBSET_DESTINATION to a Python dict.  Walks m_only to
// list which job ids this destination is restricted to (empty = all jobs).
py::dict jobset_destination_to_dict( const JOBSET_DESTINATION& aDest )
{
    py::dict d;
    d[ "uuid" ]        = aDest.m_id.ToStdString();
    d[ "type" ]        = jobset_destination_type_to_string( aDest.m_type );
    d[ "description" ] = aDest.GetDescription().ToStdString();

    if( aDest.m_outputHandler )
        d[ "output_path" ] = aDest.m_outputHandler->GetOutputPath().ToStdString();
    else
        d[ "output_path" ] = std::string();

    py::list included;
    for( const wxString& only : aDest.m_only )
        included.append( only.ToStdString() );
    d[ "included_jobs" ] = included;            // empty list == "all jobs"
    d[ "runs_all_jobs" ] = aDest.m_only.empty();

    return d;
}


// Make sure both the PCB editor and Schematic editor frames are alive so any
// PCB-side or SCH-side JOB in the jobset can dispatch successfully.  Mirrors
// PANEL_JOBSET::EnsurePcbSchFramesOpen.  We don't bother re-opening project
// files here: the live KIWAY's PROJECT (kiway->Prj()) is the one the user
// already has open, and the editors will pick it up.
void ensure_pcb_and_sch_frames( KIWAY* aKiway )
{
    // Player(..., true) creates the frame if it isn't already up; the editor
    // attaches to the current project on its own.
    aKiway->Player( FRAME_PCB_EDITOR, true );
    aKiway->Player( FRAME_SCH, true );
}


// Build the destination_results entry for one destination AFTER running.
// JOBS_RUNNER populates the JOBSET_DESTINATION's transient run-status fields
// (m_lastRunSuccess, m_lastRunSuccessMap, m_lastResolvedOutputPath); we
// shovel those into a dict.
py::dict destination_result_dict( const JOBSET_DESTINATION& aDest )
{
    py::dict d;
    d[ "uuid" ]        = aDest.m_id.ToStdString();
    d[ "description" ] = aDest.GetDescription().ToStdString();
    d[ "type" ]        = jobset_destination_type_to_string( aDest.m_type );

    if( aDest.m_lastRunSuccess.has_value() )
        d[ "ok" ] = aDest.m_lastRunSuccess.value();
    else
        d[ "ok" ] = py::none();                 // wasn't run

    if( aDest.m_lastResolvedOutputPath.has_value() )
        d[ "resolved_output_path" ] = aDest.m_lastResolvedOutputPath.value().ToStdString();
    else
        d[ "resolved_output_path" ] = py::none();

    // Per-job pass/fail map, keyed by job uuid.
    py::dict perJob;
    for( const auto& [jobId, okOpt] : aDest.m_lastRunSuccessMap )
    {
        if( okOpt.has_value() )
            perJob[ py::str( jobId.ToStdString() ) ] = okOpt.value();
        else
            perJob[ py::str( jobId.ToStdString() ) ] = py::none();
    }
    d[ "job_results" ] = perJob;

    // Per-job reporter text, keyed by job uuid (only populated when JOBS_RUNNER
    // was fed NULL_REPORTER as its primary, which is what we do — same as the
    // GUI's PANEL_JOBSET path).
    py::dict perJobMessages;
    for( const auto& [jobId, reporter] : aDest.m_lastRunReporters )
    {
        if( reporter )
            perJobMessages[ py::str( jobId.ToStdString() ) ] =
                    reporter->GetMessages().ToStdString();
        else
            perJobMessages[ py::str( jobId.ToStdString() ) ] = std::string();
    }
    d[ "job_messages" ] = perJobMessages;

    return d;
}


// -----------------------------------------------------------------------------
// load()
// -----------------------------------------------------------------------------

py::object load_jobset( const std::string& jobset_path )
{
    py::dict result;

    wxString path = wxString::FromUTF8( jobset_path );

    if( !wxFileName::FileExists( path ) )
    {
        result[ "ok" ]           = false;
        result[ "file" ]         = jobset_path;
        result[ "jobs" ]         = py::list();
        result[ "destinations" ] = py::list();
        result[ "messages" ]     = std::string( "jobset file does not exist: " )
                                   + jobset_path;
        return result;
    }

    // JOBSET parsing is pure JSON_SETTINGS work — no KIWAY required.
    JOBSET jobFile( path.ToStdString() );

    bool loadOk;
    try
    {
        loadOk = jobFile.LoadFromFile();
    }
    catch( const std::exception& e )
    {
        result[ "ok" ]           = false;
        result[ "file" ]         = jobset_path;
        result[ "jobs" ]         = py::list();
        result[ "destinations" ] = py::list();
        result[ "messages" ]     = std::string( "jobset load threw: " ) + e.what();
        return result;
    }

    py::list jobs;
    for( const JOBSET_JOB& job : jobFile.GetJobs() )
        jobs.append( jobset_job_to_dict( job ) );

    py::list dests;
    for( const JOBSET_DESTINATION& dest : jobFile.GetDestinations() )
        dests.append( jobset_destination_to_dict( dest ) );

    result[ "ok" ]           = loadOk;
    result[ "file" ]         = jobset_path;
    result[ "jobs" ]         = jobs;
    result[ "destinations" ] = dests;
    result[ "messages" ]     = loadOk ? std::string()
                                      : std::string( "JOBSET::LoadFromFile returned false "
                                                     "(file may be malformed or schema-mismatched)" );
    return result;
}


// -----------------------------------------------------------------------------
// run()
// -----------------------------------------------------------------------------

py::object run_jobset( const std::string&                     jobset_path,
                       const std::optional<std::vector<std::string>>& destinations,
                       bool                                   stop_on_error )
{
    KIWAY* kiway = find_live_kiway_for_jobset();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(jobset.run needs the live project + both kifaces; if you "
                                  "want a headless runner, use kicad-cli jobset run instead)" );

    wxString path = wxString::FromUTF8( jobset_path );
    if( !wxFileName::FileExists( path ) )
        throw std::runtime_error( "jobset file does not exist: " + jobset_path );

    // Reuse the live PROJECT — same as PANEL_JOBSET::OnGenerate does.  We do
    // NOT touch the SettingsManager here; the live project is already loaded
    // and switching projects mid-flight would invalidate any open editors.
    PROJECT& project = kiway->Prj();

    // Spawn both editor frames before dispatch.  A jobset may freely mix
    // PCB-side, SCH-side, and special jobs; either editor missing causes the
    // corresponding JOB to fail at load-board / load-schematic time.
    ensure_pcb_and_sch_frames( kiway );

    // chdir to the project directory so any relative paths inside the jobset
    // resolve the same way they do in the GUI flow.
    wxFileName projFn = project.GetProjectFullName();
    if( projFn.IsOk() && !projFn.GetPath().IsEmpty() )
        wxSetWorkingDirectory( projFn.GetPath() );

    JOBSET jobFile( path.ToStdString() );
    if( !jobFile.LoadFromFile() )
        throw std::runtime_error( "failed to load jobset file (parse error or schema mismatch): "
                                  + jobset_path );

    // Resolve the optional destinations filter.  JOBSET::FindDestination
    // matches by uuid OR by description (its idea of "name").  Pick that
    // same convention here and surface unresolved names as errors so the
    // caller never silently runs the wrong subset.
    std::vector<JOBSET_DESTINATION*> selected;
    bool                             runAll = !destinations.has_value();

    if( !runAll )
    {
        for( const std::string& key : destinations.value() )
        {
            wxString wkey = wxString::FromUTF8( key );
            JOBSET_DESTINATION* d = jobFile.FindDestination( wkey );

            if( !d )
                throw std::runtime_error( "no destination matched '" + key
                                          + "' (matched against destination uuid and "
                                            "description; ambiguous matches are rejected)" );

            selected.push_back( d );
        }
    }

    // Use WX_STRING_REPORTER as the top-level reporter so the caller gets a
    // single human-readable transcript.  JOBS_RUNNER will additionally
    // populate per-job reporters on each destination, but only when its
    // primary reporter IS NULL_REPORTER — we pass a real one, so we'll
    // collect everything in 'reporter' and skip the per-job map.
    WX_STRING_REPORTER reporter;

    bool overall_ok;
    {
        // Block other Python work for the duration of dispatch (long).
        py::gil_scoped_release nogil;

        JOBS_RUNNER runner( kiway, &jobFile, &project, reporter, nullptr );

        if( runAll )
        {
            overall_ok = runner.RunJobsAllDestinations( stop_on_error );
        }
        else
        {
            overall_ok = true;
            for( JOBSET_DESTINATION* d : selected )
            {
                bool ok = runner.RunJobsForDestination( d, stop_on_error );
                overall_ok = overall_ok && ok;

                if( !ok && stop_on_error )
                    break;
            }
        }
    }

    // Build the structured result.
    py::dict result;
    result[ "ok" ]        = overall_ok;
    result[ "exit_code" ] = overall_ok ? 0 : 1;       // mirrors CLI behaviour
    result[ "messages" ]  = reporter.GetMessages().ToStdString();

    py::list outputPaths;
    py::list destResults;

    auto includeDest = [&]( const JOBSET_DESTINATION& d )
    {
        if( runAll )
            return true;

        for( JOBSET_DESTINATION* sel : selected )
        {
            if( sel->m_id == d.m_id )
                return true;
        }
        return false;
    };

    for( const JOBSET_DESTINATION& dest : jobFile.GetDestinations() )
    {
        if( !includeDest( dest ) )
            continue;

        destResults.append( destination_result_dict( dest ) );

        if( dest.m_lastResolvedOutputPath.has_value() )
            outputPaths.append( dest.m_lastResolvedOutputPath.value().ToStdString() );
    }

    result[ "output_paths" ]        = outputPaths;
    result[ "destination_results" ] = destResults;

    return result;
}


} // anonymous namespace


PYBIND11_EMBEDDED_MODULE( klicad_native_jobset, m )
{
    m.doc() = "KliCAD JOBSET binding (calls JOBSET + JOBS_RUNNER under the hood). "
              "Exposes two operations: load() to introspect a .kicad_jobset file "
              "without executing anything, and run() to dispatch it through the "
              "live KIWAY. run() requires KiCad's GUI to be running.";

    m.def( "load", &load_jobset,
           py::arg( "jobset_path" ),
           R"DOC(Parse a .kicad_jobset file and return its structure (no execution).

Returns a dict with keys:
  - ok            bool        True iff JOBSET::LoadFromFile succeeded
  - file          str         the path that was loaded
  - jobs          list[dict]  per-job descriptions, in jobset order; each has:
                                * uuid                str   jobset-local id
                                * type                str   JOB_REGISTRY type name
                                * description          str   user-facing label
                                * default_description str   class-default label
                                * output_path         str   per-job configured output path
                                * warning             str   (optional) only if type is unknown
  - destinations  list[dict]  per-destination descriptions; each has:
                                * uuid           str       destination id
                                * type           str       'folder' | 'archive'
                                * description    str       user-facing label
                                * output_path    str       configured output path/file
                                * included_jobs  list[str] job-uuid allow-list (empty = all)
                                * runs_all_jobs  bool      True iff included_jobs is empty
  - messages      str         diagnostic text (empty on clean load)

Does not require a live KIWAY.
)DOC" );

    m.def( "run", &run_jobset,
           py::arg( "jobset_path" ),
           py::arg( "destinations" ) = py::none(),
           py::arg( "stop_on_error" ) = false,
           R"DOC(Run a .kicad_jobset file through JOBS_RUNNER.

Arguments:
  jobset_path    str                       path to the .kicad_jobset file
  destinations   list[str] | None          if None (default) all destinations run;
                                           otherwise each string is resolved via
                                           JOBSET::FindDestination, which matches
                                           against destination uuid OR description.
                                           Ambiguous or unresolved names raise
                                           RuntimeError — callers must use uuids
                                           when descriptions are not unique.
  stop_on_error  bool                      if True, abort the destination on the
                                           first failing job within it (mirrors
                                           kicad-cli jobset run --stop-on-error).

Returns a dict with keys:
  - ok                    bool        True iff every destination ran cleanly
  - exit_code             int         0 on success, 1 on any failure (CLI-style)
  - messages              str         full reporter transcript (status, info, errors)
  - output_paths          list[str]   resolved output paths for destinations that
                                      populated m_lastResolvedOutputPath (handy for
                                      callers that just want "where did stuff land")
  - destination_results   list[dict]  per-destination outcome; each has:
                                        * uuid                 str
                                        * description          str
                                        * type                 str
                                        * ok                   bool|None  (None = not run)
                                        * resolved_output_path str|None
                                        * job_results          dict[uuid -> bool|None]
                                        * job_messages         dict[uuid -> str]
                                      job_results/job_messages are typically empty
                                      because JOBS_RUNNER only populates them when its
                                      primary reporter is NULL_REPORTER; this binding
                                      uses a real WX_STRING_REPORTER so per-job output
                                      is captured in the top-level 'messages' field.

Raises RuntimeError if:
  - no live KIWAY is available (KiCad GUI not running)
  - the jobset file does not exist or fails to parse
  - any name in 'destinations' fails to resolve to exactly one destination
)DOC" );
}
