/*
 * KliCAD subsystem binding: PCB Drill export.
 *
 * Exposes drill file export as
 *   klicad_native_export_drill.run(board_path, output_dir, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli pcb export drill` (see
 * kicad/cli/command_pcb_export_drill.cpp).  Wraps JOB_EXPORT_PCB_DRILL and
 * dispatches via the live KIWAY (found by walking wxTopLevelWindows).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_export_pcb_drill.h>
#include <jobs/job.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <string>

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a live
// KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes with
// helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot).
KIWAY* find_live_kiway_for_export_drill()
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


JOB_EXPORT_PCB_DRILL::DRILL_FORMAT drill_format_from_string( const std::string& s )
{
    if( s == "excellon" ) return JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON;
    if( s == "gerber" )   return JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::GERBER;
    throw std::invalid_argument( "format must be one of: 'excellon', 'gerber' (got '"
                                 + s + "')" );
}


JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN drill_origin_from_string( const std::string& s )
{
    if( s == "absolute" ) return JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::ABS;
    if( s == "abs" )      return JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::ABS;
    if( s == "plot" )     return JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::PLOT;
    throw std::invalid_argument( "drill_origin must be one of: 'absolute', 'plot' (got '"
                                 + s + "')" );
}


JOB_EXPORT_PCB_DRILL::DRILL_UNITS drill_units_from_string( const std::string& s )
{
    if( s == "mm" )   return JOB_EXPORT_PCB_DRILL::DRILL_UNITS::MM;
    if( s == "in" )   return JOB_EXPORT_PCB_DRILL::DRILL_UNITS::INCH;
    if( s == "inch" ) return JOB_EXPORT_PCB_DRILL::DRILL_UNITS::INCH;
    throw std::invalid_argument( "units must be one of: 'mm', 'in' (got '" + s + "')" );
}


JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT drill_zeros_format_from_string( const std::string& s )
{
    if( s == "decimal" )          return JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::DECIMAL;
    if( s == "suppressleading" )  return JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::SUPPRESS_LEADING;
    if( s == "suppresstrailing" ) return JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::SUPPRESS_TRAILING;
    if( s == "keep" )             return JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::KEEP_ZEROS;
    throw std::invalid_argument( "zeros_format must be one of: 'decimal', 'suppressleading', "
                                 "'suppresstrailing', 'keep' (got '" + s + "')" );
}


JOB_EXPORT_PCB_DRILL::MAP_FORMAT drill_map_format_from_string( const std::string& s )
{
    if( s == "pdf" )      return JOB_EXPORT_PCB_DRILL::MAP_FORMAT::PDF;
    if( s == "ps" )       return JOB_EXPORT_PCB_DRILL::MAP_FORMAT::POSTSCRIPT;
    if( s == "gerberx2" ) return JOB_EXPORT_PCB_DRILL::MAP_FORMAT::GERBER_X2;
    if( s == "dxf" )      return JOB_EXPORT_PCB_DRILL::MAP_FORMAT::DXF;
    if( s == "svg" )      return JOB_EXPORT_PCB_DRILL::MAP_FORMAT::SVG;
    throw std::invalid_argument( "map_format must be one of: 'pdf', 'ps', 'gerberx2', "
                                 "'dxf', 'svg' (got '" + s + "')" );
}


py::object run_export_drill( const std::string& board_path,
                             const std::string& output_dir,
                             const std::string& format,
                             const std::string& drill_origin,
                             const std::string& units,
                             const std::string& zeros_format,
                             const std::string& oval_format,
                             const std::string& map_format,
                             int                gerber_precision,
                             bool               excellon_mirror_y,
                             bool               excellon_min_header,
                             bool               excellon_separate_th,
                             bool               generate_map,
                             bool               generate_report,
                             bool               generate_tenting,
                             const std::string& report_path )
{
    if( gerber_precision != 5 && gerber_precision != 6 )
        throw std::invalid_argument( "gerber_precision must be 5 or 6" );

    if( oval_format != "alternate" && oval_format != "route" )
        throw std::invalid_argument( "oval_format must be 'alternate' or 'route' (got '"
                                     + oval_format + "')" );

    KIWAY* kiway = find_live_kiway_for_export_drill();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(Drill export needs the pcbnew kiface and project state "
                                  "to be loaded)" );

    // The PCB editor frame must exist for board-loading paths used by
    // JOB_EXPORT_PCB_DRILL dispatch.  Player(..., true) creates it if absent.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_EXPORT_PCB_DRILL job;
    job.m_filename = wxString::FromUTF8( board_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output_dir ) );

    job.m_format       = drill_format_from_string( format );
    job.m_drillOrigin  = drill_origin_from_string( drill_origin );
    job.m_drillUnits   = drill_units_from_string( units );
    job.m_zeroFormat   = drill_zeros_format_from_string( zeros_format );
    job.m_mapFormat    = drill_map_format_from_string( map_format );
    job.m_gerberPrecision = gerber_precision;

    job.m_excellonMirrorY        = excellon_mirror_y;
    job.m_excellonMinimalHeader  = excellon_min_header;
    // CLI exposes --excellon-separate-th; the JOB field is the inverse (combine).
    job.m_excellonCombinePTHNPTH = !excellon_separate_th;
    // CLI exposes oval_format string; the JOB field is a bool.
    job.m_excellonOvalDrillRoute = ( oval_format == "route" );

    job.m_generateMap     = generate_map;
    job.m_generateReport  = generate_report;
    job.m_generateTenting = generate_tenting;

    // Report path is only meaningful with generate_report; matches CLI semantics.
    if( generate_report )
        job.m_reportPath = wxString::FromUTF8( report_path );
    else
        job.m_reportPath = wxString();

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // ProcessJob blocks on the main thread; release the GIL so any nested
        // Python callbacks (none today, but future bindings might) can re-acquire.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_PCB, &job, &reporter );
    }

    py::list output_paths;
    for( const JOB_OUTPUT& out : job.GetOutputs() )
        output_paths.append( std::string( out.m_outputPath.ToUTF8() ) );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_export_drill, m )
{
    m.doc() = "KliCAD PCB drill export binding (calls JOB_EXPORT_PCB_DRILL "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_drill,
           py::arg( "board_path" ),
           py::arg( "output_dir" ),
           py::arg( "format" )               = "excellon",
           py::arg( "drill_origin" )         = "absolute",
           py::arg( "units" )                = "mm",
           py::arg( "zeros_format" )         = "decimal",
           py::arg( "oval_format" )          = "alternate",
           py::arg( "map_format" )           = "pdf",
           py::arg( "gerber_precision" )     = 6,
           py::arg( "excellon_mirror_y" )    = false,
           py::arg( "excellon_min_header" )  = false,
           py::arg( "excellon_separate_th" ) = false,
           py::arg( "generate_map" )         = false,
           py::arg( "generate_report" )      = false,
           py::arg( "generate_tenting" )     = false,
           py::arg( "report_path" )          = std::string(),
           R"DOC(Export drill files for the given .kicad_pcb into output_dir.

Mirrors `kicad-cli pcb export drill`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_EXPORT_PCB_DRILL dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files produced by the job (from JOB::GetOutputs())

Key kwargs (defaults match KiCad CLI behavior):
  format             'excellon' (default) | 'gerber'
  drill_origin       'absolute' (default) | 'plot' (also accepts 'abs')
  units              'mm' (default) | 'in' (also accepts 'inch')
  zeros_format       'decimal' (default) | 'suppressleading' | 'suppresstrailing' | 'keep'
  oval_format        'alternate' (default) | 'route'
  map_format         'pdf' (default) | 'ps' | 'gerberx2' | 'dxf' | 'svg'
  gerber_precision   5 or 6 (default 6)
  excellon_mirror_y      Mirror Y axis in Excellon output.
  excellon_min_header    Emit a minimal Excellon header.
  excellon_separate_th   Generate independent files for NPTH and PTH holes
                         (inverse of JOB::m_excellonCombinePTHNPTH).
  generate_map           Generate map / summary of drill hits.
  generate_report        Generate report of drill hits.
  generate_tenting       Generate a file specifically for tenting.
  report_path            Report output file path (only used when
                         generate_report=True; silently ignored otherwise,
                         matching CLI semantics).

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running),
or ValueError on invalid format/origin/units/zeros_format/oval_format/
map_format strings or a gerber_precision other than 5 or 6.
)DOC" );
}
