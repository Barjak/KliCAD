/*
 * KliCAD subsystem binding: PCB Gerbers export.
 *
 * Exposes Gerber file export as
 *   klicad_native_export_gerbers.run(board_path, output_dir, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli pcb export gerbers` (see
 * kicad/cli/command_pcb_export_gerbers.cpp and the parent
 * command_pcb_export_gerber.cpp).  Wraps JOB_EXPORT_PCB_GERBERS and dispatches
 * via the live KIWAY (found by walking wxTopLevelWindows).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_gerber.h>
#include <jobs/job_export_pcb_plot.h>
#include <jobs/job.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <optional>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a live
// KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes with
// the matching helper in bindings_drc.cpp (find_live_kiway) and bindings_erc.cpp
// (find_live_kiway_for_erc).
KIWAY* find_live_kiway_for_export_gerbers()
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


py::object run_export_gerbers( const std::string&                board_path,
                               const std::string&                output_dir,
                               const std::optional<std::string>& layers,
                               const std::optional<std::string>& common_layers,
                               bool                              use_board_plot_params,
                               bool                              exclude_value,
                               bool                              exclude_refdes,
                               bool                              include_border_title,
                               bool                              sketch_pads_on_fab_layers,
                               bool                              hide_dnp_fps_on_fab_layers,
                               bool                              sketch_dnp_fps_on_fab_layers,
                               bool                              crossout_dnp_fps_on_fab_layers,
                               bool                              no_x2,
                               bool                              no_netlist,
                               bool                              subtract_soldermask,
                               bool                              disable_aperture_macros,
                               bool                              use_drill_file_origin,
                               int                               precision,
                               bool                              no_protel_extension,
                               bool                              check_zones,
                               bool                              create_jobs_file,
                               const std::string&                drawing_sheet,
                               const std::string&                variant )
{
    if( precision != 5 && precision != 6 )
        throw std::invalid_argument( "precision must be 5 or 6" );

    KIWAY* kiway = find_live_kiway_for_export_gerbers();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(Gerber export needs the pcbnew kiface and project state "
                                  "to be loaded)" );

    // The PCB editor frame must exist for board-loading paths used by
    // JOB_EXPORT_PCB_GERBERS dispatch.  Player(..., true) creates it if absent.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_EXPORT_PCB_GERBERS job;
    job.m_filename = wxString::FromUTF8( board_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output_dir ) );

    // JOB_EXPORT_PCB_GERBERS-specific fields
    job.m_useBoardPlotParams = use_board_plot_params;
    job.m_createJobsFile     = create_jobs_file;

    // JOB_EXPORT_PCB_GERBER (parent) fields
    job.m_includeNetlistAttributes = !no_netlist;
    job.m_useX2Format              = !no_x2;
    job.m_disableApertureMacros    = disable_aperture_macros;
    job.m_useProtelFileExtension   = !no_protel_extension;
    job.m_precision                = precision;

    // JOB_EXPORT_PCB_PLOT (grandparent) fields
    if( layers.has_value() )
        job.m_argLayers = wxString::FromUTF8( *layers );
    if( common_layers.has_value() )
        job.m_argCommonLayers = wxString::FromUTF8( *common_layers );

    if( !drawing_sheet.empty() )
        job.m_drawingSheet = wxString::FromUTF8( drawing_sheet );
    if( !variant.empty() )
        job.m_variant = wxString::FromUTF8( variant );

    job.m_plotFootprintValues       = !exclude_value;
    job.m_plotRefDes                = !exclude_refdes;
    job.m_plotDrawingSheet          = include_border_title;
    job.m_sketchPadsOnFabLayers     = sketch_pads_on_fab_layers;
    if( job.m_sketchPadsOnFabLayers )
        job.m_plotPadNumbers = true;
    job.m_hideDNPFPsOnFabLayers     = hide_dnp_fps_on_fab_layers;
    job.m_sketchDNPFPsOnFabLayers   = sketch_dnp_fps_on_fab_layers;
    job.m_crossoutDNPFPsOnFabLayers = crossout_dnp_fps_on_fab_layers;
    job.m_subtractSolderMaskFromSilk = subtract_soldermask;
    job.m_useDrillOrigin            = use_drill_file_origin;
    job.m_checkZonesBeforePlot      = check_zones;

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


PYBIND11_EMBEDDED_MODULE( klicad_native_export_gerbers, m )
{
    m.doc() = "KliCAD PCB Gerbers export binding (calls JOB_EXPORT_PCB_GERBERS "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_gerbers,
           py::arg( "board_path" ),
           py::arg( "output_dir" ),
           py::arg( "layers" )                         = std::nullopt,
           py::arg( "common_layers" )                  = std::nullopt,
           py::arg( "use_board_plot_params" )          = false,
           py::arg( "exclude_value" )                  = false,
           py::arg( "exclude_refdes" )                 = false,
           py::arg( "include_border_title" )           = false,
           py::arg( "sketch_pads_on_fab_layers" )      = false,
           py::arg( "hide_dnp_fps_on_fab_layers" )     = false,
           py::arg( "sketch_dnp_fps_on_fab_layers" )   = false,
           py::arg( "crossout_dnp_fps_on_fab_layers" ) = false,
           py::arg( "no_x2" )                          = false,
           py::arg( "no_netlist" )                     = false,
           py::arg( "subtract_soldermask" )            = false,
           py::arg( "disable_aperture_macros" )        = false,
           py::arg( "use_drill_file_origin" )          = false,
           py::arg( "precision" )                      = 6,
           py::arg( "no_protel_extension" )            = false,
           py::arg( "check_zones" )                    = false,
           py::arg( "create_jobs_file" )               = false,
           py::arg( "drawing_sheet" )                  = std::string(),
           py::arg( "variant" )                        = std::string(),
           R"DOC(Export Gerber files for the given .kicad_pcb into output_dir.

Mirrors `kicad-cli pcb export gerbers`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_EXPORT_PCB_GERBERS dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files produced by the job (from JOB::GetOutputs())

Key kwargs (defaults match KiCad CLI behavior unless noted):
  layers / common_layers      Comma-separated layer names; None = use plot
                              settings on the board (or pass an explicit list).
  use_board_plot_params       Use the Gerber plot settings stored in the board
                              file (mirrors --board-plot-params).
  precision                   Gerber coordinate precision: 5 or 6 (default 6).
  no_x2                       Disable extended X2 format (default: X2 enabled).
  no_netlist                  Disable netlist attribute emission.
  no_protel_extension         Use KiCad's .gbr extension instead of Protel
                              per-layer extensions.
  create_jobs_file            Emit a .gbrjob alongside the Gerbers.
  drawing_sheet               Path to a .kicad_wks drawing sheet (optional).
  variant                     Variant name to apply (optional).
  exclude_value/refdes,
  include_border_title,
  sketch_pads_on_fab_layers,
  hide/sketch/crossout_dnp_fps_on_fab_layers,
  subtract_soldermask,
  disable_aperture_macros,
  use_drill_file_origin,
  check_zones                 Same semantics as the matching kicad-cli flags.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running),
or ValueError if precision is not 5 or 6.
)DOC" );
}
