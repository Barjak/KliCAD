/*
 * KliCAD subsystem binding: schematic plot/export.
 *
 * Exposes JOB_EXPORT_SCH_PLOT (PDF / SVG / DXF / Postscript) as
 * klicad_native_export_sch_plot.run(schematic_path, output, format, ...).
 *
 * Mirrors bindings_erc.cpp / bindings_drc.cpp; see BINDING_PATTERN.md
 * for the per-subsystem recipe.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_export_sch_plot.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/window.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-subsystem unique helper names — must not collide with
// find_live_kiway (bindings_drc.cpp) or find_live_kiway_for_erc
// (bindings_erc.cpp).
KIWAY* find_live_kiway_for_export_sch_plot()
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


SCH_PLOT_FORMAT sch_plot_format_from_string( const std::string& s )
{
    if( s == "pdf" )                       return SCH_PLOT_FORMAT::PDF;
    if( s == "svg" )                       return SCH_PLOT_FORMAT::SVG;
    if( s == "dxf" )                       return SCH_PLOT_FORMAT::DXF;
    if( s == "ps" || s == "post"
        || s == "postscript" )             return SCH_PLOT_FORMAT::POST;
    if( s == "hpgl" )                      return SCH_PLOT_FORMAT::HPGL;
    throw std::invalid_argument( "format must be one of: 'pdf', 'svg', 'dxf', 'ps', 'hpgl' "
                                 "(got '" + s + "')" );
}


JOB_PAGE_SIZE sch_plot_page_size_from_string( const std::string& s )
{
    if( s == "auto" ) return JOB_PAGE_SIZE::PAGE_SIZE_AUTO;
    if( s == "A4" )   return JOB_PAGE_SIZE::PAGE_SIZE_A4;
    if( s == "A" )    return JOB_PAGE_SIZE::PAGE_SIZE_A;
    throw std::invalid_argument( "page_size must be one of: 'auto', 'A4', 'A' (got '"
                                 + s + "')" );
}


std::unique_ptr<JOB_EXPORT_SCH_PLOT> make_sch_plot_job( SCH_PLOT_FORMAT aFormat )
{
    switch( aFormat )
    {
    // PDF is the only format whose CLI uses a *file* output path (not a
    // directory); the others are inherently directory-output.  We preserve
    // that distinction here so SetConfiguredOutputPath() validates correctly.
    case SCH_PLOT_FORMAT::PDF:  return std::make_unique<JOB_EXPORT_SCH_PLOT_PDF>( false );
    case SCH_PLOT_FORMAT::DXF:  return std::make_unique<JOB_EXPORT_SCH_PLOT_DXF>();
    case SCH_PLOT_FORMAT::SVG:  return std::make_unique<JOB_EXPORT_SCH_PLOT_SVG>();
    case SCH_PLOT_FORMAT::POST: return std::make_unique<JOB_EXPORT_SCH_PLOT_PS>();
    case SCH_PLOT_FORMAT::HPGL:
        // The CLI command rejects HPGL as of KiCad 10.0 ("Plotting to HPGL is
        // no longer supported").  Refuse here too rather than silently making
        // a job whose dispatch will fail with an opaque error.
        throw std::invalid_argument( "format 'hpgl' is no longer supported as of KiCad 10.0" );
    }
    throw std::invalid_argument( "unhandled SCH_PLOT_FORMAT" );
}


py::object run_export_sch_plot( const std::string& schematic_path,
                                const std::string& output,
                                const std::string& format,
                                const std::string& page_size,
                                bool               black_and_white,
                                bool               plot_drawing_sheet,
                                bool               use_background_color,
                                const std::string& pages,
                                const std::string& drawing_sheet,
                                const std::string& default_font,
                                const std::string& color_theme,
                                bool               show_hop_over,
                                bool               pdf_property_popups,
                                bool               pdf_hierarchical_links,
                                bool               pdf_metadata )
{
    SCH_PLOT_FORMAT plotFormat = sch_plot_format_from_string( format );
    JOB_PAGE_SIZE   pageSize   = sch_plot_page_size_from_string( page_size );

    KIWAY* kiway = find_live_kiway_for_export_sch_plot();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(schematic plot needs the eeschema kiface and project state)" );

    // The schematic jobs handler expects the schematic editor frame to be
    // live when running in GUI mode with a project loaded.  Create it if
    // missing; Player(..., true) is a no-op if already up.
    kiway->Player( FRAME_SCH, true );

    std::unique_ptr<JOB_EXPORT_SCH_PLOT> plotJob = make_sch_plot_job( plotFormat );

    plotJob->m_filename         = wxString::FromUTF8( schematic_path );
    plotJob->m_plotFormat       = plotFormat;
    plotJob->SetConfiguredOutputPath( wxString::FromUTF8( output ) );
    plotJob->m_pageSizeSelect   = pageSize;
    plotJob->m_blackAndWhite    = black_and_white;
    plotJob->m_plotDrawingSheet = plot_drawing_sheet;
    plotJob->m_useBackgroundColor = use_background_color;
    plotJob->m_show_hop_over    = show_hop_over;
    plotJob->m_drawingSheet     = wxString::FromUTF8( drawing_sheet );
    plotJob->m_defaultFont      = wxString::FromUTF8( default_font );
    plotJob->m_theme            = wxString::FromUTF8( color_theme );
    plotJob->m_PDFPropertyPopups    = pdf_property_popups;
    plotJob->m_PDFHierarchicalLinks = pdf_hierarchical_links;
    plotJob->m_PDFMetadata          = pdf_metadata;

    // Parse comma-separated page list (matches the CLI --pages contract).
    // Empty string => plot all pages.
    std::vector<wxString> pageList;
    {
        wxString          pagesStr = wxString::FromUTF8( pages );
        wxStringTokenizer tokenizer( pagesStr, wxS( "," ), wxTOKEN_STRTOK );

        while( tokenizer.HasMoreTokens() )
            pageList.push_back( tokenizer.GetNextToken().Trim( true ).Trim( false ) );
    }
    plotJob->m_plotPages = pageList;
    plotJob->m_plotAll   = pageList.empty();

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the long C++ dispatch so any re-entrant
        // Python callbacks (none today, but future bindings might) can run.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, plotJob.get(), &reporter );
    }

    py::list outputPaths;
    for( const JOB_OUTPUT& out : plotJob->GetOutputs() )
        outputPaths.append( std::string( out.m_outputPath.ToUTF8() ) );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = outputPaths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_export_sch_plot, m )
{
    m.doc() = "KliCAD schematic plot/export binding (calls JOB_EXPORT_SCH_PLOT_* "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_sch_plot,
           py::arg( "schematic_path" ),
           py::arg( "output" ),
           py::arg( "format" )                 = "pdf",
           py::arg( "page_size" )              = "auto",
           py::arg( "black_and_white" )        = false,
           py::arg( "plot_drawing_sheet" )     = true,
           py::arg( "use_background_color" )   = true,
           py::arg( "pages" )                  = "",
           py::arg( "drawing_sheet" )          = "",
           py::arg( "default_font" )           = "",
           py::arg( "color_theme" )            = "",
           py::arg( "show_hop_over" )          = false,
           py::arg( "pdf_property_popups" )    = true,
           py::arg( "pdf_hierarchical_links" ) = true,
           py::arg( "pdf_metadata" )           = true,
           R"DOC(Plot a .kicad_sch file to PDF / SVG / DXF / Postscript.

Args:
  schematic_path:        path to .kicad_sch
  output:                output path.  For 'pdf' this is a file path; for
                         'svg' / 'dxf' / 'ps' it is a directory path (one
                         file per sheet is written into it).
  format:                'pdf' (default) | 'svg' | 'dxf' | 'ps' (aka
                         'post' / 'postscript').  'hpgl' is accepted but
                         immediately rejected — no longer supported in
                         KiCad 10.0+.
  page_size:             'auto' (default) | 'A4' | 'A'
  black_and_white:       monochrome output (default: False)
  plot_drawing_sheet:    include the drawing sheet / title block (default: True)
  use_background_color:  honor theme background color (default: True; only
                         meaningful for PDF / SVG / Postscript)
  pages:                 comma-separated list of page numbers to plot, or
                         '' (default) for all pages
  drawing_sheet:         optional path to a .kicad_wks override
  default_font:          optional default font name
  color_theme:           optional color theme name (default: schematic settings)
  show_hop_over:         draw hop-over at wire crossings (default: False)
  pdf_property_popups:   PDF: generate property popups (default: True)
  pdf_hierarchical_links: PDF: clickable hierarchical links (default: True)
  pdf_metadata:          PDF: emit AUTHOR/SUBJECT metadata (default: True)

Returns a dict:
  ok:           bool       True iff job exited cleanly (exit_code == 0)
  exit_code:    int        raw exit code from JOB dispatch
  messages:     str        reporter output (status / warnings from the run)
  output_paths: list[str]  files actually written, as reported by the JOB

Raises:
  ValueError       on unknown 'format' or 'page_size' (mapped from
                   std::invalid_argument by pybind11)
  RuntimeError     if no live KIWAY is available (KiCad GUI not running)
)DOC" );
}
