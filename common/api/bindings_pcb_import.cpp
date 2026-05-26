/*
 * KliCAD subsystem binding: PCB import (non-KiCad PCB formats -> .kicad_pcb).
 *
 * Exposes the PCB import operation as
 *   klicad_native_pcb_import.run(input_path, output_path, format='auto', ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli pcb import` (see
 * kicad/cli/command_pcb_import.cpp).  Wraps JOB_PCB_IMPORT and dispatches
 * via the live KIWAY (found by walking wxTopLevelWindows).
 *
 * Supports the same set of source formats as PCBNEW_JOBS_HANDLER::JobImport
 * (PADS ASCII, Altium Designer, Eagle, CADSTAR, Fabmaster, PCAD, SolidWorks
 * PCB) plus 'auto' for plugin-driven extension sniffing.  The JOB itself
 * does not record AddOutput, so we surface the output path back to the
 * caller manually.
 *
 * If the user opts into the JSON report and does not provide a report_file,
 * we route the report to a temp file, slurp it back, and embed both the
 * raw text and the parsed dict in the return value (matches bindings_drc.cpp).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_pcb_import.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/wfstream.h>
#include <wx/window.h>

#include <fstream>
#include <sstream>
#include <string>

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a live
// KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes with
// the matching helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_pcb_upgrade.cpp
// (find_live_kiway_for_pcb_upgrade), bindings_sch_upgrade.cpp
// (find_live_kiway_for_sch_upgrade), bindings_fp_upgrade.cpp
// (find_live_kiway_for_fp_upgrade), bindings_sym_upgrade.cpp
// (find_live_kiway_for_sym_upgrade), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_sch_bom.cpp
// (find_live_kiway_for_export_sch_bom), bindings_export_sch_netlist.cpp
// (find_live_kiway_for_export_sch_netlist), bindings_fp_export_svg.cpp
// (find_live_kiway_for_fp_export_svg), bindings_sym_export_svg.cpp
// (find_live_kiway_for_sym_export_svg), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), bindings_render.cpp
// (find_live_kiway_for_render), and bindings_jobset.cpp
// (find_live_kiway_for_jobset).
KIWAY* find_live_kiway_for_pcb_import()
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


std::string slurp_pcb_import_file( const wxString& aPath )
{
    std::ifstream f( aPath.ToStdString() );
    if( !f )
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}


JOB_PCB_IMPORT::FORMAT format_from_string( const std::string& s )
{
    // Mirror the CLI's accepted token set in command_pcb_import.cpp.
    if( s == "auto" )       return JOB_PCB_IMPORT::FORMAT::AUTO;
    if( s == "pads" )       return JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    if( s == "pads_ascii" ) return JOB_PCB_IMPORT::FORMAT::PADS_ASCII;
    if( s == "altium" )     return JOB_PCB_IMPORT::FORMAT::ALTIUM;
    if( s == "eagle" )      return JOB_PCB_IMPORT::FORMAT::EAGLE;
    if( s == "cadstar" )    return JOB_PCB_IMPORT::FORMAT::CADSTAR;
    if( s == "fabmaster" )  return JOB_PCB_IMPORT::FORMAT::FABMASTER;
    if( s == "pcad" )       return JOB_PCB_IMPORT::FORMAT::PCAD;
    if( s == "solidworks" ) return JOB_PCB_IMPORT::FORMAT::SOLIDWORKS;

    throw std::invalid_argument( "format must be one of: 'auto', 'pads' (alias "
                                 "'pads_ascii'), 'altium', 'eagle', 'cadstar', "
                                 "'fabmaster', 'pcad', 'solidworks' (got '"
                                 + s + "')" );
}


JOB_PCB_IMPORT::REPORT_FORMAT report_format_from_string( const std::string& s )
{
    if( s == "none" ) return JOB_PCB_IMPORT::REPORT_FORMAT::NONE;
    if( s == "json" ) return JOB_PCB_IMPORT::REPORT_FORMAT::JSON;
    if( s == "text" ) return JOB_PCB_IMPORT::REPORT_FORMAT::TEXT;

    throw std::invalid_argument( "report_format must be one of: 'none', 'json', "
                                 "'text' (got '" + s + "')" );
}


py::object run_pcb_import( const std::string& input_path,
                           const std::string& output_path,
                           const std::string& format,
                           const std::string& report_format,
                           const std::string& report_file )
{
    KIWAY* kiway = find_live_kiway_for_pcb_import();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(PCB import needs the pcbnew kiface to be loaded)" );

    // PCBNEW_JOBS_HANDLER::JobImport drives PCB_IO plugins through the pcbnew
    // kiface; make sure the PCB editor frame exists so the kiface and its
    // plugin registry are spun up.  Player(..., true) creates the frame if
    // missing.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_PCB_IMPORT job;
    job.m_inputFile = wxString::FromUTF8( input_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output_path ) );
    job.m_format = format_from_string( format );
    job.m_reportFormat = report_format_from_string( report_format );

    // If the caller asked for a report but didn't pin it to a file, route it to
    // a temp file so we can slurp it back into the return dict.  Matches the
    // bundle-paradigm rule from BINDING_PATTERN.md (return structured data,
    // never stream).
    wxString slurpedReportPath;
    bool     ownsReportTempFile = false;

    if( job.m_reportFormat != JOB_PCB_IMPORT::REPORT_FORMAT::NONE )
    {
        if( !report_file.empty() )
        {
            job.m_reportFile = wxString::FromUTF8( report_file );
            slurpedReportPath = job.m_reportFile;
        }
        else
        {
            wxFileName tmpFile;
            tmpFile.AssignTempFileName( wxS( "klicad_pcb_import_" ) );
            tmpFile.SetExt( job.m_reportFormat == JOB_PCB_IMPORT::REPORT_FORMAT::JSON
                                ? wxS( "json" )
                                : wxS( "txt" ) );
            job.m_reportFile   = tmpFile.GetFullPath();
            slurpedReportPath  = job.m_reportFile;
            ownsReportTempFile = true;
        }
    }

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // ProcessJob blocks on the main thread; release the GIL so any nested
        // Python callbacks (none today, but future bindings might) can re-acquire.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_PCB, &job, &reporter );
    }

    // JOB_PCB_IMPORT's handler doesn't call AddOutput, so GetOutputs() is
    // empty.  Surface the effective output path so callers can find the
    // .kicad_pcb we just wrote (mirrors what the CLI logs at INFO).
    py::list output_paths;

    for( const JOB_OUTPUT& out : job.GetOutputs() )
        output_paths.append( std::string( out.m_outputPath.ToUTF8() ) );

    if( exitCode == 0 && py::len( output_paths ) == 0 )
    {
        wxString effectiveOut = job.GetConfiguredOutputPath();

        if( effectiveOut.IsEmpty() )
        {
            wxFileName fn( job.m_inputFile );
            // FILEEXT::KiCadPcbFileExtension lives behind an include we don't
            // want to pull in here just for a constant; the handler hardcodes
            // the swap to a .kicad_pcb sibling, so do the same locally.
            fn.SetExt( wxS( "kicad_pcb" ) );
            effectiveOut = fn.GetFullPath();
        }

        output_paths.append( std::string( effectiveOut.ToUTF8() ) );
    }

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;

    // If a report was generated, slurp it back so the caller gets it in one
    // round trip.  JSON reports also get parsed into a dict for convenience.
    if( !slurpedReportPath.IsEmpty() )
    {
        std::string reportText = slurp_pcb_import_file( slurpedReportPath );
        result[ "report_path" ] = std::string( slurpedReportPath.ToUTF8() );
        result[ "report_text" ] = reportText;

        if( !reportText.empty()
            && job.m_reportFormat == JOB_PCB_IMPORT::REPORT_FORMAT::JSON )
        {
            try
            {
                py::object json_module = py::module_::import( "json" );
                result[ "report" ] = json_module.attr( "loads" )( reportText );
            }
            catch( const py::error_already_set& )
            {
                // Leave 'report' absent on parse failure; caller can fall back
                // to report_text.
            }
        }

        if( ownsReportTempFile )
            wxRemoveFile( slurpedReportPath );
    }

    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_pcb_import, m )
{
    m.doc() = "KliCAD PCB import binding (calls JOB_PCB_IMPORT under the hood). "
              "Converts a non-KiCad PCB file (Altium, Eagle, CADSTAR, Fabmaster, "
              "PADS ASCII, PCAD, SolidWorks PCB) to a .kicad_pcb. Returns a "
              "structured dict; never streams. Requires KiCad's GUI to be "
              "running (needs a live KIWAY).";

    m.def( "run", &run_pcb_import,
           py::arg( "input_path" ),
           py::arg( "output_path" ),
           py::arg( "format" )        = "auto",
           py::arg( "report_format" ) = "none",
           py::arg( "report_file" )   = "",
           R"DOC(Import a non-KiCad PCB file and write a .kicad_pcb.

Mirrors `kicad-cli pcb import`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_PCB_IMPORT dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files written by the job.  JOB_PCB_IMPORT does
                              not call AddOutput, so this falls back to the
                              effective output .kicad_pcb path on clean exit.
  - report_path   str         (only if report_format != 'none') path of the
                              report file; if the caller passed report_file
                              it's that path, otherwise a temp file that's
                              deleted before return.
  - report_text   str         (only if report_format != 'none') raw report
                              content slurped from report_path.
  - report        dict        (only if report_format == 'json' and parse
                              succeeded) parsed report; keys include
                              'source_file', 'source_format', 'output_file',
                              'layer_mapping', 'statistics', 'warnings',
                              'errors'.

Kwargs:
  format          Source format hint. One of:
                    'auto'        Detect from extension via PCB_IO plugin sniffing
                    'pads'        PADS ASCII (alias: 'pads_ascii')
                    'altium'      Altium Designer
                    'eagle'       Autodesk/CadSoft Eagle
                    'cadstar'     CADSTAR PCB Archive
                    'fabmaster'   Fabmaster
                    'pcad'        PCAD
                    'solidworks'  SolidWorks PCB
                  Default 'auto'. Raises ValueError on any other token.
  report_format   'none' (default), 'json', or 'text'. Raises ValueError on
                  any other token.
  report_file     If non-empty, the report is written to this path and not
                  cleaned up. If empty and report_format != 'none', the
                  binding routes the report through a temp file, slurps it
                  back into report_text/report, and deletes the temp file.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
Raises ValueError (via std::invalid_argument) on unknown format /
report_format strings.
)DOC" );
}
