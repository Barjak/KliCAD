/*
 * KliCAD subsystem binding: Footprint Library SVG export.
 *
 * Exposes per-footprint / whole-library SVG export as
 *   klicad_native_fp_export_svg.run(library_path, output_dir, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli fp export svg` (see
 * kicad/cli/command_fp_export_svg.cpp).  Wraps JOB_FP_EXPORT_SVG and dispatches
 * via the live KIWAY (found by walking wxTopLevelWindows).  The library SVG
 * export is handled by the pcbnew kiface (FACE_PCB), so we make sure the PCB
 * editor frame is up before dispatch — same dance as bindings_export_gerbers.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_fp_export_svg.h>
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

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a live
// KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes with
// the matching helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_sch_bom.cpp
// (find_live_kiway_for_export_sch_bom), bindings_export_sch_netlist.cpp
// (find_live_kiway_for_export_sch_netlist), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), bindings_pcb_upgrade.cpp
// (find_live_kiway_for_pcb_upgrade), and bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_fp_export_svg()
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


py::object run_fp_export_svg( const std::string&                library_path,
                              const std::string&                output_dir,
                              const std::optional<std::string>& footprint,
                              const std::optional<std::string>& layers,
                              const std::string&                color_theme,
                              bool                              black_and_white,
                              bool                              sketch_pads_on_fab_layers,
                              bool                              hide_dnp_fps_on_fab_layers,
                              bool                              sketch_dnp_fps_on_fab_layers,
                              bool                              crossout_dnp_fps_on_fab_layers )
{
    KIWAY* kiway = find_live_kiway_for_fp_export_svg();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(Footprint SVG export needs the pcbnew kiface and "
                                  "project state to be loaded)" );

    // Footprint-library SVG export is dispatched through the pcbnew kiface
    // (KIWAY::FACE_PCB), matching the CLI in command_fp_export_svg.cpp.
    // Ensure the PCB editor frame is up so the kiface is loaded.
    // Player(..., true) creates the frame if missing.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_FP_EXPORT_SVG job;
    job.m_libraryPath = wxString::FromUTF8( library_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output_dir ) );

    if( footprint.has_value() )
        job.m_footprint = wxString::FromUTF8( *footprint );

    // JOB_EXPORT_PCB_PLOT (base) fields used by the SVG path.
    if( layers.has_value() )
        job.m_argLayers = wxString::FromUTF8( *layers );

    if( !color_theme.empty() )
        job.m_colorTheme = wxString::FromUTF8( color_theme );

    job.m_blackAndWhite           = black_and_white;
    job.m_sketchPadsOnFabLayers   = sketch_pads_on_fab_layers;
    if( job.m_sketchPadsOnFabLayers )
        job.m_plotPadNumbers = true;
    job.m_hideDNPFPsOnFabLayers     = hide_dnp_fps_on_fab_layers;
    job.m_sketchDNPFPsOnFabLayers   = sketch_dnp_fps_on_fab_layers;
    job.m_crossoutDNPFPsOnFabLayers = crossout_dnp_fps_on_fab_layers;

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


PYBIND11_EMBEDDED_MODULE( klicad_native_fp_export_svg, m )
{
    m.doc() = "KliCAD footprint-library SVG export binding (calls JOB_FP_EXPORT_SVG "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_fp_export_svg,
           py::arg( "library_path" ),
           py::arg( "output_dir" ),
           py::arg( "footprint" )                      = std::nullopt,
           py::arg( "layers" )                         = std::nullopt,
           py::arg( "color_theme" )                    = std::string(),
           py::arg( "black_and_white" )                = false,
           py::arg( "sketch_pads_on_fab_layers" )      = false,
           py::arg( "hide_dnp_fps_on_fab_layers" )     = false,
           py::arg( "sketch_dnp_fps_on_fab_layers" )   = true,
           py::arg( "crossout_dnp_fps_on_fab_layers" ) = true,
           R"DOC(Export SVG renders for a footprint library (or a single footprint within it).

Mirrors `kicad-cli fp export svg`. Library path should be a .pretty directory
(or whatever the footprint plugin resolves as a library). Outputs land in
output_dir, one .svg per footprint (or just the one named via `footprint`).

Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_FP_EXPORT_SVG dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files produced by the job (from JOB::GetOutputs())

Key kwargs (defaults match the CLI / JOB constructor):
  footprint                       Optional footprint name to restrict export to a
                                  single entry within the library (CLI --footprint).
                                  None or empty = export the whole library.
  layers                          Comma-separated layer names; None = use the
                                  job's default layer set (CLI --layers).
  color_theme                     Color theme name; empty = footprint editor's
                                  configured theme (CLI --theme / -t).
  black_and_white                 Render monochrome (CLI --black-and-white).
                                  Default False (matches JOB ctor).
  sketch_pads_on_fab_layers       CLI --sp. Default False. Forces plot pad
                                  numbers when enabled (mirrors CLI).
  hide_dnp_fps_on_fab_layers      CLI --hdnp. Default False.
  sketch_dnp_fps_on_fab_layers    CLI --sdnp. Default True (matches JOB ctor).
  crossout_dnp_fps_on_fab_layers  CLI --cdnp. Default True (matches JOB ctor).

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).

Deliberately not exposed (not configurable via the CLI for this job, and
not part of JOB_FP_EXPORT_SVG's intended surface):
  mirror, negative, scale, common_layers, drawing_sheet, variant,
  plotFootprintValues, plotRefDes, plotDrawingSheet, subtractSolderMaskFromSilk,
  drillShapeOption, useDrillOrigin, checkZonesBeforePlot, define-var overrides.
)DOC" );
}
