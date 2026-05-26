/*
 * KliCAD subsystem binding: symbol-library SVG export.
 *
 * Exposes JOB_SYM_EXPORT_SVG (the eeschema-side symbol library -> SVG
 * exporter that backs `kicad-cli sym export svg`) as
 * klicad_native_sym_export_svg.run(library_path, output_dir, ...).
 *
 * Mirrors bindings_erc.cpp / bindings_export_sch_plot.cpp; see
 * BINDING_PATTERN.md for the per-subsystem recipe.
 *
 * Dispatch target (verified from kicad/cli/command_sym_export_svg.cpp):
 *   KIWAY::FACE_SCH  -- handled by EESCHEMA_JOBS_HANDLER::JobSymExportSvg.
 * The frame we ensure is FRAME_SCH (same as ERC / sch plot bindings),
 * since the eeschema kiface is what owns the jobs handler.
 *
 * Note: JOB_SYM_EXPORT_SVG drives output via its own m_outputDirectory
 * field, NOT the JOB base class's configured-output-path mechanism (this
 * matches what the CLI does in command_sym_export_svg.cpp).  The handler
 * also does not populate JOB::m_outputs, so the returned "output_paths"
 * list will normally be empty -- it is included for contract consistency
 * with the other export bindings.  Per-file paths are reported through
 * the WX_STRING_REPORTER messages instead.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_sym_export_svg.h>
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

// Per-subsystem unique helper name -- must not collide with the matching
// helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_sch_bom.cpp
// (find_live_kiway_for_export_sch_bom), bindings_export_sch_netlist.cpp
// (find_live_kiway_for_export_sch_netlist), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), bindings_pcb_upgrade.cpp
// (find_live_kiway_for_pcb_upgrade), or bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_sym_export_svg()
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


py::object run_sym_export_svg( const std::string& library_path,
                               const std::string& output_dir,
                               const std::string& symbol,
                               bool               black_and_white,
                               bool               include_hidden_pins,
                               bool               include_hidden_fields,
                               const std::string& color_theme )
{
    KIWAY* kiway = find_live_kiway_for_sym_export_svg();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available -- is KiCad's GUI running? "
                                  "(symbol SVG export needs the eeschema kiface and "
                                  "project state)" );

    // EESCHEMA_JOBS_HANDLER expects the schematic editor frame to be live
    // when running in GUI mode with a project loaded.  Player(..., true)
    // is a no-op if the frame is already up.
    kiway->Player( FRAME_SCH, true );

    JOB_SYM_EXPORT_SVG svgJob;
    svgJob.m_libraryPath         = wxString::FromUTF8( library_path );
    svgJob.m_outputDirectory     = wxString::FromUTF8( output_dir );
    svgJob.m_symbol              = wxString::FromUTF8( symbol );
    svgJob.m_blackAndWhite       = black_and_white;
    svgJob.m_includeHiddenPins   = include_hidden_pins;
    svgJob.m_includeHiddenFields = include_hidden_fields;
    svgJob.m_colorTheme          = wxString::FromUTF8( color_theme );

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the long C++ dispatch so any re-entrant
        // Python callbacks (none today, but future bindings might) can run.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, &svgJob, &reporter );
    }

    py::list outputPaths;
    for( const JOB_OUTPUT& out : svgJob.GetOutputs() )
        outputPaths.append( std::string( out.m_outputPath.ToUTF8() ) );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = outputPaths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_sym_export_svg, m )
{
    m.doc() = "KliCAD symbol-library SVG export binding (calls JOB_SYM_EXPORT_SVG "
              "under the hood via the eeschema kiface). Returns a structured dict; "
              "never streams. Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_sym_export_svg,
           py::arg( "library_path" ),
           py::arg( "output_dir" ),
           py::arg( "symbol" )                = "",
           py::arg( "black_and_white" )       = false,
           py::arg( "include_hidden_pins" )   = false,
           py::arg( "include_hidden_fields" ) = false,
           py::arg( "color_theme" )           = "",
           R"DOC(Export symbol(s) from a .kicad_sym library to SVG.

Args:
  library_path:           path to a .kicad_sym symbol library file
  output_dir:             directory to write SVG file(s) into (created if
                          missing).  One file per (symbol, unit, body-style)
                          combination is written, named like
                          '<symbol>_unit<N>[_demorgan].svg'.
  symbol:                 optional single-symbol filter (matches the CLI's
                          --symbol flag).  '' (default) exports every
                          symbol in the library.
  black_and_white:        monochrome output (default: False)
  include_hidden_pins:    render pins flagged hidden (default: False)
  include_hidden_fields:  render fields flagged hidden (default: False)
  color_theme:            optional color theme name; '' (default) uses the
                          symbol editor settings

Returns a dict:
  ok:           bool       True iff job exited cleanly (exit_code == 0)
  exit_code:    int        raw exit code from JOB dispatch
  messages:     str        reporter output -- includes a 'Plotting symbol
                           ... to <path>' line per file actually written,
                           plus any warnings/errors
  output_paths: list[str]  files reported via JOB::GetOutputs().  Note: the
                           current eeschema handler does NOT populate this,
                           so the list is normally empty -- parse 'messages'
                           for per-file paths.

Raises:
  RuntimeError  if no live KIWAY is available (KiCad GUI not running)
)DOC" );
}
