/*
 * KliCAD subsystem binding: ERC.
 *
 * Exposes Electrical Rules Check as klicad_native_erc.run(schematic_path, ...).
 * Mirrors bindings_drc.cpp; see that file for the per-subsystem pattern.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_sch_erc.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <fstream>
#include <sstream>
#include <string>

namespace py = pybind11;

namespace
{

KIWAY* find_live_kiway_for_erc()
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


std::string slurp_file_erc( const wxString& aPath )
{
    std::ifstream f( aPath.ToStdString() );
    if( !f )
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}


JOB_RC::UNITS erc_units_from_string( const std::string& s )
{
    if( s == "mm" )    return JOB_RC::UNITS::MM;
    if( s == "in" )    return JOB_RC::UNITS::INCH;
    if( s == "inch" )  return JOB_RC::UNITS::INCH;
    if( s == "mils" )  return JOB_RC::UNITS::MILS;
    throw std::invalid_argument( "units must be one of: 'mm', 'in', 'mils' (got '" + s + "')" );
}


int erc_severity_from_string( const std::string& s )
{
    if( s == "error" )   return RPT_SEVERITY_ERROR;
    if( s == "warning" ) return RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING;
    if( s == "all" )     return RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING
                              | RPT_SEVERITY_EXCLUSION | RPT_SEVERITY_ACTION
                              | RPT_SEVERITY_INFO;
    throw std::invalid_argument( "severity must be one of: 'error', 'warning', 'all' (got '"
                                 + s + "')" );
}


py::object run_erc( const std::string& schematic_path,
                    const std::string& units,
                    const std::string& severity )
{
    KIWAY* kiway = find_live_kiway_for_erc();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(ERC needs the eeschema kiface and project state)" );

    // The schematic-side jobs handler likewise expects the schematic editor
    // frame to be live when running in GUI mode; create it if missing.
    kiway->Player( FRAME_SCH, true );

    wxFileName tmpFile;
    tmpFile.AssignTempFileName( wxS( "klicad_erc_" ) );
    tmpFile.SetExt( wxS( "json" ) );
    const wxString outPath = tmpFile.GetFullPath();

    JOB_SCH_ERC ercJob;
    ercJob.m_filename           = wxString::FromUTF8( schematic_path );
    ercJob.SetConfiguredOutputPath( outPath );
    ercJob.m_units              = erc_units_from_string( units );
    ercJob.m_severity           = erc_severity_from_string( severity );
    ercJob.m_format             = JOB_SCH_ERC::OUTPUT_FORMAT::JSON;
    ercJob.m_exitCodeViolations = false;

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, &ercJob, &reporter );
    }

    py::dict result;
    result[ "exit_code" ]   = exitCode;
    result[ "ok" ]          = ( exitCode == 0 );
    result[ "report_path" ] = outPath.ToStdString();
    result[ "messages" ]    = reporter.GetMessages().ToStdString();

    std::string json_text = slurp_file_erc( outPath );
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
        }
    }

    wxRemoveFile( outPath );
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_erc, m )
{
    m.doc() = "KliCAD ERC subsystem binding (calls JOB_SCH_ERC under the hood). "
              "Returns a structured dict; never streams. Requires KiCad's GUI to "
              "be running (needs a live KIWAY).";

    m.def( "run", &run_erc,
           py::arg( "schematic_path" ),
           py::arg( "units" )    = "mm",
           py::arg( "severity" ) = "warning",
           R"DOC(Run ERC on the given .kicad_sch file.

Returns a dict with the same shape as klicad_native_drc.run:
  ok, exit_code, messages, report_path, json_text, report (parsed dict)
)DOC" );
}
