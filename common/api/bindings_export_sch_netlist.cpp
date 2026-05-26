/*
 * KliCAD subsystem binding: schematic netlist export.
 *
 * Exposes JOB_EXPORT_SCH_NETLIST (KiCad sexpr / XML / OrcadPCB2 / CadStar /
 * PADS / Spice / Spice model / Allegro) as
 * klicad_native_export_sch_netlist.run(schematic_path, output, format, ...).
 *
 * Mirrors bindings_export_sch_plot.cpp (schematic-side, multi-format
 * string-to-enum) and bindings_erc.cpp (schematic-side FACE_SCH dispatch);
 * see BINDING_PATTERN.md for the per-subsystem recipe.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_export_sch_netlist.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-subsystem unique helper name — must not collide with the matching
// helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), and bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_export_sch_netlist()
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


JOB_EXPORT_SCH_NETLIST::FORMAT sch_netlist_format_from_string( const std::string& s )
{
    // Accept every spelling the CLI / JSON serializer recognizes, plus a
    // couple of common aliases ('xml' for 'kicadxml', 'orcad' / 'orcadpcb2'
    // both, 'sexpr' for 'kicadsexpr').  The default is the KiCad s-expression
    // netlist ('kicad' / 'kicadsexpr' / 'sexpr').
    if( s == "kicad"      || s == "kicadsexpr" || s == "sexpr" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::KICADSEXPR;
    if( s == "kicadxml"   || s == "xml" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::KICADXML;
    if( s == "orcadpcb2"  || s == "orcad" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::ORCADPCB2;
    if( s == "cadstar" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::CADSTAR;
    if( s == "pads" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::PADS;
    if( s == "spice" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::SPICE;
    if( s == "spicemodel" || s == "spice-model" || s == "spice_model" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::SPICEMODEL;
    if( s == "allegro" )
        return JOB_EXPORT_SCH_NETLIST::FORMAT::ALLEGRO;

    throw std::invalid_argument( "format must be one of: 'kicad' (aka 'kicadsexpr' / 'sexpr'), "
                                 "'kicadxml' (aka 'xml'), 'orcadpcb2' (aka 'orcad'), 'cadstar', "
                                 "'pads', 'spice', 'spicemodel' (aka 'spice-model'), 'allegro' "
                                 "(got '" + s + "')" );
}


py::object run_export_sch_netlist( const std::string& schematic_path,
                                   const std::string& output,
                                   const std::string& format,
                                   bool               spice_save_all_voltages,
                                   bool               spice_save_all_currents,
                                   bool               spice_save_all_dissipations,
                                   bool               spice_save_all_events,
                                   const std::string& variants )
{
    JOB_EXPORT_SCH_NETLIST::FORMAT netFormat = sch_netlist_format_from_string( format );

    KIWAY* kiway = find_live_kiway_for_export_sch_netlist();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(schematic netlist export needs the eeschema kiface and "
                                  "project state)" );

    // The schematic jobs handler expects the schematic editor frame to be
    // live when running in GUI mode with a project loaded.  Create it if
    // missing; Player(..., true) is a no-op if already up.
    kiway->Player( FRAME_SCH, true );

    JOB_EXPORT_SCH_NETLIST netJob;
    netJob.m_filename = wxString::FromUTF8( schematic_path );
    netJob.SetConfiguredOutputPath( wxString::FromUTF8( output ) );
    netJob.format = netFormat;
    netJob.m_spiceSaveAllVoltages     = spice_save_all_voltages;
    netJob.m_spiceSaveAllCurrents     = spice_save_all_currents;
    netJob.m_spiceSaveAllDissipations = spice_save_all_dissipations;
    netJob.m_spiceSaveAllEvents       = spice_save_all_events;

    // Parse comma-separated variant list (matches the CLI --variants contract).
    // Empty string => default variant only.
    {
        std::vector<wxString> variantList;
        wxString              variantsStr = wxString::FromUTF8( variants );
        wxStringTokenizer     tokenizer( variantsStr, wxS( "," ), wxTOKEN_STRTOK );

        while( tokenizer.HasMoreTokens() )
            variantList.push_back( tokenizer.GetNextToken().Trim( true ).Trim( false ) );

        netJob.m_variantNames = variantList;
    }

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the long C++ dispatch so any re-entrant
        // Python callbacks (none today, but future bindings might) can run.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, &netJob, &reporter );
    }

    py::list outputPaths;
    for( const JOB_OUTPUT& out : netJob.GetOutputs() )
        outputPaths.append( std::string( out.m_outputPath.ToUTF8() ) );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = outputPaths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_export_sch_netlist, m )
{
    m.doc() = "KliCAD schematic netlist export binding (calls JOB_EXPORT_SCH_NETLIST "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_sch_netlist,
           py::arg( "schematic_path" ),
           py::arg( "output" ),
           py::arg( "format" )                      = "kicad",
           py::arg( "spice_save_all_voltages" )     = false,
           py::arg( "spice_save_all_currents" )     = false,
           py::arg( "spice_save_all_dissipations" ) = false,
           py::arg( "spice_save_all_events" )       = false,
           py::arg( "variants" )                    = "",
           R"DOC(Export a netlist from a .kicad_sch file.

Args:
  schematic_path:                path to .kicad_sch
  output:                        output file path (netlist export writes a
                                 single file regardless of format)
  format:                        netlist format (default 'kicad'). Accepted:
                                   'kicad'     / 'kicadsexpr' / 'sexpr'  -> KiCad S-expression
                                   'kicadxml'  / 'xml'                   -> KiCad XML
                                   'orcadpcb2' / 'orcad'                 -> OrcadPCB2
                                   'cadstar'                             -> CadStar
                                   'pads'                                -> PADS
                                   'spice'                               -> Spice
                                   'spicemodel'/ 'spice-model'           -> Spice model
                                   'allegro'                             -> Allegro
  spice_save_all_voltages:       Spice formats: save all node voltages    (default False)
  spice_save_all_currents:       Spice formats: save all branch currents  (default False)
  spice_save_all_dissipations:   Spice formats: save all power dissipations (default False)
  spice_save_all_events:         Spice formats: save all digital events   (default False)
  variants:                      comma-separated list of variant names to
                                 export, or '' (default) for the default
                                 variant only

Returns a dict:
  ok:           bool       True iff job exited cleanly (exit_code == 0)
  exit_code:    int        raw exit code from JOB dispatch
  messages:     str        reporter output (status / warnings from the run)
  output_paths: list[str]  files actually written, as reported by the JOB

Raises:
  ValueError       on unknown 'format' (mapped from std::invalid_argument
                   by pybind11)
  RuntimeError     if no live KIWAY is available (KiCad GUI not running)
)DOC" );
}
