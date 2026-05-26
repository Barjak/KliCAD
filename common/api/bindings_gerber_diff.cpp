/*
 * KliCAD subsystem binding: Gerber Diff.
 *
 * Exposes the GerbView gerber-diff subsystem as
 *   klicad_native_gerber_diff.run(reference_gerber, comparison_gerber, ...).
 *
 * Mirrors bindings_drc.cpp / bindings_erc.cpp but targets KIWAY::FACE_GERBVIEW
 * via FRAME_GERBER.  Wraps JOB_GERBER_DIFF; see common/jobs/job_gerber_diff.h
 * for the underlying field set and kicad/cli/command_gerber_diff.cpp for the
 * canonical CLI mapping that this binding parallels.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_gerber_diff.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-subsystem helper name to avoid ODR clash with bindings_drc.cpp
// (find_live_kiway) and bindings_erc.cpp (find_live_kiway_for_erc).
KIWAY* find_live_kiway_for_gerber_diff()
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


JOB_GERBER_DIFF::OUTPUT_FORMAT gerber_diff_format_from_string( const std::string& s )
{
    if( s == "png" )  return JOB_GERBER_DIFF::OUTPUT_FORMAT::PNG;
    if( s == "text" ) return JOB_GERBER_DIFF::OUTPUT_FORMAT::TEXT;
    if( s == "json" ) return JOB_GERBER_DIFF::OUTPUT_FORMAT::JSON;
    throw std::invalid_argument( "output_format must be one of: 'png', 'text', 'json' (got '"
                                 + s + "')" );
}


py::object run_gerber_diff( const std::string& reference_gerber,
                            const std::string& comparison_gerber,
                            const std::string& output_path,
                            const std::string& output_format,
                            int                dpi,
                            bool               antialias,
                            bool               transparent_background,
                            bool               exit_code_only,
                            int                tolerance,
                            bool               strict,
                            bool               no_align )
{
    KIWAY* kiway = find_live_kiway_for_gerber_diff();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available - is KiCad's GUI running? "
                                  "(gerber_diff needs the GerbView kiface dispatched via KIWAY)" );

    // The GerbView jobs handler needs the GerbView frame instantiated so its
    // kiface is loaded before ProcessJob dispatches.  Player(..., true) creates
    // the frame if missing.
    kiway->Player( FRAME_GERBER, true );

    JOB_GERBER_DIFF diffJob;
    diffJob.m_inputFileA            = wxString::FromUTF8( reference_gerber );
    diffJob.m_inputFileB            = wxString::FromUTF8( comparison_gerber );
    diffJob.m_outputFormat          = gerber_diff_format_from_string( output_format );
    diffJob.m_dpi                   = dpi;
    diffJob.m_antialias             = antialias;
    diffJob.m_transparentBackground = transparent_background;
    diffJob.m_exitCodeOnly          = exit_code_only;
    diffJob.m_tolerance             = tolerance;
    diffJob.m_strict                = strict;
    diffJob.m_noAlign               = no_align;

    if( !output_path.empty() )
        diffJob.SetConfiguredOutputPath( wxString::FromUTF8( output_path ) );

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the (potentially long) ProcessJob call so any
        // re-entrant Python callbacks from other bindings can re-acquire it.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_GERBVIEW, &diffJob, &reporter );
    }

    py::dict result;
    result[ "ok" ]        = ( exitCode == 0 );
    result[ "exit_code" ] = exitCode;
    result[ "messages" ]  = reporter.GetMessages().ToStdString();

    // Mirror the GetOutputs() -> list[str] copy used by ExecuteBoardJob in
    // pcbnew/api/api_handler_pcb.cpp.
    std::vector<std::string> output_paths;
    for( const JOB_OUTPUT& out : diffJob.GetOutputs() )
        output_paths.emplace_back( out.m_outputPath.ToUTF8() );

    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_gerber_diff, m )
{
    m.doc() = "KliCAD Gerber Diff subsystem binding (calls JOB_GERBER_DIFF "
              "via KIWAY::FACE_GERBVIEW under the hood). Returns a structured "
              "dict; never streams. Requires KiCad's GUI to be running (needs a "
              "live KIWAY so the GerbView frame/kiface can be brought up).";

    m.def( "run", &run_gerber_diff,
           py::arg( "reference_gerber" ),
           py::arg( "comparison_gerber" ),
           py::arg( "output_path" )            = "",
           py::arg( "output_format" )          = "png",
           py::arg( "dpi" )                    = 300,
           py::arg( "antialias" )              = true,
           py::arg( "transparent_background" ) = true,
           py::arg( "exit_code_only" )         = false,
           py::arg( "tolerance" )              = 0,
           py::arg( "strict" )                 = false,
           py::arg( "no_align" )               = false,
           R"DOC(Compare two Gerber (or Excellon) files and report differences.

Positional args:
  reference_gerber    str   path to the reference (first) file
  comparison_gerber   str   path to the comparison (second) file

Keyword args:
  output_path             str    where to write the diff output; empty -> default
                                 (PNG: <reference>-diff.png; text/json: stdout)
  output_format           str    'png' (default) | 'text' | 'json'
  dpi                     int    resolution for PNG output (default 300)
  antialias               bool   anti-alias PNG output (default True)
  transparent_background  bool   transparent PNG background (default True)
  exit_code_only          bool   only set exit code, no output (0=identical, 1=different)
  tolerance               int    tolerance in IU for floating-point comparison (default 0)
  strict                  bool   fail on any parse warnings/errors (default False)
  no_align                bool   skip bounding-box origin alignment; catches absolute-
                                 placement regressions (default False)

Returns a dict with keys:
  ok            bool       True iff job exited cleanly (exit_code == 0)
  exit_code     int        raw exit code from JOB_GERBER_DIFF dispatch
  messages      str        WX_STRING_REPORTER output from the run
  output_paths  list[str]  files produced by the job (from JOB::GetOutputs())

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
