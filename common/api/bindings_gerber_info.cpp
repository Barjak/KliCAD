/*
 * KliCAD subsystem binding: Gerber Info.
 *
 * Exposes the GerbView gerber-info subsystem as
 *   klicad_native_gerber_info.run(gerber_path, ...).
 *
 * Mirrors bindings_gerber_diff.cpp (the closest peer: same GerbView face
 * KIWAY::FACE_GERBVIEW dispatched via FRAME_GERBER).  Wraps JOB_GERBER_INFO;
 * see common/jobs/job_gerber_info.h for the underlying field set and
 * kicad/cli/command_gerber_info.cpp for the canonical CLI mapping that this
 * binding parallels.
 *
 * Unlike DRC, JOB_GERBER_INFO does NOT write a sidecar file: its handler
 * (GERBVIEW_JOBS_HANDLER::JobGerberInfo in gerbview/gerbview_jobs_handler.cpp)
 * emits the entire report — text or pretty-printed JSON — through the
 * supplied REPORTER.  We capture that with a WX_STRING_REPORTER and, when
 * the user asks for JSON, parse the captured text into the returned dict
 * under a "report" key (matching the convenience surface bindings_drc.cpp
 * exposes for its on-disk JSON report).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_gerber_info.h>
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

// Per-subsystem helper name to avoid ODR clash with the matching helpers
// in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and all the other bindings_*.cpp TUs
// that follow the find_live_kiway_for_<subsys> convention.
KIWAY* find_live_kiway_for_gerber_info()
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


JOB_GERBER_INFO::OUTPUT_FORMAT gerber_info_format_from_string( const std::string& s )
{
    if( s == "text" ) return JOB_GERBER_INFO::OUTPUT_FORMAT::TEXT;
    if( s == "json" ) return JOB_GERBER_INFO::OUTPUT_FORMAT::JSON;
    throw std::invalid_argument( "output_format must be one of: 'text', 'json' (got '"
                                 + s + "')" );
}


JOB_GERBER_INFO::UNITS gerber_info_units_from_string( const std::string& s )
{
    if( s == "mm" )   return JOB_GERBER_INFO::UNITS::MM;
    if( s == "in" )   return JOB_GERBER_INFO::UNITS::INCH;
    if( s == "inch" ) return JOB_GERBER_INFO::UNITS::INCH;
    if( s == "mils" ) return JOB_GERBER_INFO::UNITS::MILS;
    throw std::invalid_argument( "units must be one of: 'mm', 'in', 'mils' (got '"
                                 + s + "')" );
}


py::object run_gerber_info( const std::string& gerber_path,
                            const std::string& output_format,
                            const std::string& units,
                            bool               calculate_area,
                            bool               strict )
{
    KIWAY* kiway = find_live_kiway_for_gerber_info();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available - is KiCad's GUI running? "
                                  "(gerber_info needs the GerbView kiface dispatched via KIWAY)" );

    // The GerbView jobs handler needs the GerbView frame instantiated so its
    // kiface is loaded before ProcessJob dispatches.  Player(..., true) creates
    // the frame if missing.
    kiway->Player( FRAME_GERBER, true );

    JOB_GERBER_INFO infoJob;
    infoJob.m_inputFile     = wxString::FromUTF8( gerber_path );
    infoJob.m_outputFormat  = gerber_info_format_from_string( output_format );
    infoJob.m_units         = gerber_info_units_from_string( units );
    infoJob.m_calculateArea = calculate_area;
    infoJob.m_strict        = strict;

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the (potentially long) ProcessJob call so any
        // re-entrant Python callbacks from other bindings can re-acquire it.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_GERBVIEW, &infoJob, &reporter );
    }

    std::string messages = reporter.GetMessages().ToStdString();

    py::dict result;
    result[ "ok" ]        = ( exitCode == 0 );
    result[ "exit_code" ] = exitCode;
    result[ "messages" ]  = messages;

    // JOB_GERBER_INFO does not produce on-disk outputs; surface an empty list
    // for shape parity with the other binding TUs (gerber_diff, exports, etc.).
    std::vector<std::string> output_paths;
    for( const JOB_OUTPUT& out : infoJob.GetOutputs() )
        output_paths.emplace_back( out.m_outputPath.ToUTF8() );
    result[ "output_paths" ] = output_paths;

    // The handler funnels the report (either pretty-printed JSON or human-
    // readable text) through the REPORTER.  Mirror bindings_drc.cpp by
    // parsing the JSON variant into a Python dict under "report" for
    // ergonomic access; on text mode the reporter content is the report.
    if( exitCode == 0
            && infoJob.m_outputFormat == JOB_GERBER_INFO::OUTPUT_FORMAT::JSON
            && !messages.empty() )
    {
        try
        {
            py::object json_module = py::module_::import( "json" );
            result[ "report" ] = json_module.attr( "loads" )( messages );
        }
        catch( const py::error_already_set& )
        {
            // Leave 'report' absent on parse failure; caller can fall back to
            // 'messages'.
        }
    }

    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_gerber_info, m )
{
    m.doc() = "KliCAD Gerber Info subsystem binding (calls JOB_GERBER_INFO "
              "via KIWAY::FACE_GERBVIEW under the hood). Returns a structured "
              "dict; never streams. Requires KiCad's GUI to be running (needs a "
              "live KIWAY so the GerbView frame/kiface can be brought up).";

    m.def( "run", &run_gerber_info,
           py::arg( "gerber_path" ),
           py::arg( "output_format" ) = "json",
           py::arg( "units" )         = "mm",
           py::arg( "calculate_area" ) = false,
           py::arg( "strict" )        = false,
           R"DOC(Inspect a Gerber or Excellon file and report its metadata.

Positional args:
  gerber_path     str   path to the Gerber/Excellon file to inspect

Keyword args:
  output_format   str    'json' (default) | 'text'
  units           str    'mm' (default) | 'in' | 'mils'
  calculate_area  bool   include estimated copper area (default False)
  strict          bool   fail on any parse warnings/errors (default False)

Returns a dict with keys:
  ok            bool       True iff job exited cleanly (exit_code == 0)
  exit_code     int        raw exit code from JOB_GERBER_INFO dispatch
  messages      str        REPORTER output — for output_format='text' this is
                           the full human-readable report; for 'json' this is
                           the pretty-printed JSON document
  output_paths  list[str]  files produced by the job (always empty for
                           JOB_GERBER_INFO; included for shape parity with
                           other binding TUs)
  report        dict       parsed JSON report (only present when
                           output_format='json', exit_code==0, and the JSON
                           parsed successfully). Keys include 'filename',
                           'type', 'item_count', 'units', 'bounding_box'
                           (origin_x, origin_y, width, height), 'aperture_count',
                           and 'copper_area' (only when calculate_area=True).

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
