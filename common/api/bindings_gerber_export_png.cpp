/*
 * KliCAD subsystem binding: Gerber Export PNG.
 *
 * Exposes the GerbView "convert to PNG" subsystem as
 *   klicad_native_gerber_export_png.run(gerber_paths, output_dir, ...).
 *
 * Mirrors bindings_gerber_diff.cpp (peer GerbView-faced binding) and
 * bindings_export_drill.cpp (peer multi-file/output_dir binding).  Wraps
 * JOB_GERBER_EXPORT_PNG; see common/jobs/job_gerber_export_png.h for the
 * underlying field set and kicad/cli/command_gerber_convert_png.cpp for the
 * canonical CLI mapping this binding parallels.
 *
 * The underlying JOB takes a single input file at a time; this binding
 * accepts a list[str] of input gerbers and iterates ProcessJob once per
 * input, accumulating output_paths across all calls into a single result.
 * The first non-zero exit code wins (further inputs are still processed).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_gerber_export_png.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-subsystem helper name to avoid ODR clash with the matching helpers in
// bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and the rest of the bindings_*.cpp TUs.
KIWAY* find_live_kiway_for_gerber_export_png()
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


JOB_GERBER_EXPORT_PNG::UNITS gerber_png_units_from_string( const std::string& s )
{
    if( s == "mm" )   return JOB_GERBER_EXPORT_PNG::UNITS::MM;
    if( s == "in" )   return JOB_GERBER_EXPORT_PNG::UNITS::INCH;
    if( s == "inch" ) return JOB_GERBER_EXPORT_PNG::UNITS::INCH;
    if( s == "mils" ) return JOB_GERBER_EXPORT_PNG::UNITS::MILS;
    throw std::invalid_argument( "units must be one of: 'mm', 'inch', 'mils' (got '"
                                 + s + "')" );
}


py::object run_gerber_export_png( const std::vector<std::string>& gerber_paths,
                                  const std::string&              output_dir,
                                  int                             dpi,
                                  int                             width,
                                  int                             height,
                                  bool                            antialias,
                                  bool                            transparent_background,
                                  bool                            strict,
                                  const std::string&              units,
                                  double                          origin_x,
                                  double                          origin_y,
                                  double                          window_width,
                                  double                          window_height,
                                  const std::string&              foreground_color,
                                  const std::string&              background_color )
{
    if( gerber_paths.empty() )
        throw std::invalid_argument( "gerber_paths must be a non-empty list[str] of input gerber/excellon files" );

    // CLI guards: window dimensions must both be > 0 or both == 0 to disable
    // viewport override.  Mirror that here.
    if( ( window_width > 0.0 ) != ( window_height > 0.0 ) )
        throw std::invalid_argument( "window_width and window_height must be specified together "
                                     "(both > 0 to enable viewport mode, or both 0 to disable)" );

    KIWAY* kiway = find_live_kiway_for_gerber_export_png();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available - is KiCad's GUI running? "
                                  "(gerber_export_png needs the GerbView kiface dispatched via KIWAY)" );

    // The GerbView jobs handler needs the GerbView frame instantiated so its
    // kiface is loaded before ProcessJob dispatches.  Player(..., true) creates
    // the frame if missing.
    kiway->Player( FRAME_GERBER, true );

    const wxString outDir = wxString::FromUTF8( output_dir );
    const JOB_GERBER_EXPORT_PNG::UNITS unitsEnum = gerber_png_units_from_string( units );

    WX_STRING_REPORTER reporter;
    std::vector<std::string> all_output_paths;
    int firstNonZeroExitCode = 0;

    for( const std::string& inputPath : gerber_paths )
    {
        JOB_GERBER_EXPORT_PNG pngJob;
        pngJob.m_inputFile             = wxString::FromUTF8( inputPath );
        pngJob.m_dpi                   = dpi;
        pngJob.m_width                 = width;
        pngJob.m_height                = height;
        pngJob.m_antialias             = antialias;
        pngJob.m_transparentBackground = transparent_background;
        pngJob.m_strict                = strict;
        pngJob.m_units                 = unitsEnum;
        pngJob.m_originX               = origin_x;
        pngJob.m_originY               = origin_y;
        pngJob.m_windowWidth           = window_width;
        pngJob.m_windowHeight          = window_height;
        pngJob.m_foregroundColor       = wxString::FromUTF8( foreground_color );
        pngJob.m_backgroundColor       = wxString::FromUTF8( background_color );

        // Build the per-input output path: <output_dir>/<input_basename>.png
        // (mirrors the CLI's default when --output is omitted, but rerooted
        // into output_dir so multi-file calls don't collide and don't pollute
        // the caller's source tree).
        wxFileName inputFn( wxString::FromUTF8( inputPath ) );
        wxFileName outFn;
        outFn.AssignDir( outDir );
        outFn.SetName( inputFn.GetName() );
        outFn.SetExt( wxS( "png" ) );
        pngJob.SetConfiguredOutputPath( outFn.GetFullPath() );

        int exitCode;
        {
            // Release the GIL across the (potentially long) ProcessJob call so
            // any re-entrant Python callbacks from other bindings can re-acquire
            // it.  Cairo rasterization can take a while for dense gerbers.
            py::gil_scoped_release nogil;
            exitCode = kiway->ProcessJob( KIWAY::FACE_GERBVIEW, &pngJob, &reporter );
        }

        if( exitCode != 0 && firstNonZeroExitCode == 0 )
            firstNonZeroExitCode = exitCode;

        // Mirror the GetOutputs() -> list[str] copy used by bindings_gerber_diff.cpp.
        for( const JOB_OUTPUT& out : pngJob.GetOutputs() )
            all_output_paths.emplace_back( out.m_outputPath.ToUTF8() );
    }

    py::dict result;
    result[ "ok" ]           = ( firstNonZeroExitCode == 0 );
    result[ "exit_code" ]    = firstNonZeroExitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = all_output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_gerber_export_png, m )
{
    m.doc() = "KliCAD Gerber Export PNG subsystem binding (calls "
              "JOB_GERBER_EXPORT_PNG via KIWAY::FACE_GERBVIEW under the hood). "
              "Accepts a list of input gerber/excellon files and rasterizes each "
              "into a PNG inside output_dir. Returns a structured dict; never "
              "streams. Requires KiCad's GUI to be running (needs a live KIWAY "
              "so the GerbView frame/kiface can be brought up).";

    m.def( "run", &run_gerber_export_png,
           py::arg( "gerber_paths" ),
           py::arg( "output_dir" ),
           py::arg( "dpi" )                    = 300,
           py::arg( "width" )                  = 0,
           py::arg( "height" )                 = 0,
           py::arg( "antialias" )              = true,
           py::arg( "transparent_background" ) = true,
           py::arg( "strict" )                 = false,
           py::arg( "units" )                  = "mm",
           py::arg( "origin_x" )               = 0.0,
           py::arg( "origin_y" )               = 0.0,
           py::arg( "window_width" )           = 0.0,
           py::arg( "window_height" )          = 0.0,
           py::arg( "foreground_color" )       = "",
           py::arg( "background_color" )       = "",
           R"DOC(Rasterize one or more Gerber/Excellon files to PNG images.

Positional args:
  gerber_paths   list[str]   input gerber or excellon files (one PNG per input)
  output_dir     str         directory to write <basename>.png files into

Keyword args:
  dpi                     int    resolution in DPI when width/height not set (default 300)
  width                   int    output width in pixels; overrides DPI when >0 (default 0)
  height                  int    output height in pixels; overrides DPI when >0 (default 0)
  antialias               bool   anti-alias PNG output (default True)
  transparent_background  bool   transparent PNG background (default True)
  strict                  bool   fail on any parse warnings/errors (default False)
  units                   str    units for viewport params: 'mm' (default) | 'inch' | 'mils'
  origin_x                float  viewport origin X in `units` (default 0.0)
  origin_y                float  viewport origin Y in `units` (default 0.0)
  window_width            float  viewport width in `units`; enables viewport mode when >0
                                 (must be paired with window_height; default 0.0)
  window_height           float  viewport height in `units`; enables viewport mode when >0
                                 (must be paired with window_width; default 0.0)
  foreground_color        str    foreground hex color, e.g. '#FFFFFF' (default '': layer default)
  background_color        str    background hex color, e.g. '#000000' (default '': transparent or layer default)

Returns a dict with keys:
  ok            bool       True iff every per-input dispatch exited cleanly
  exit_code     int        first non-zero exit code across inputs (0 if all clean)
  messages      str        accumulated WX_STRING_REPORTER output across all inputs
  output_paths  list[str]  every PNG produced, in input order (from JOB::GetOutputs())

Raises:
  ValueError    if gerber_paths is empty, or window_width/window_height pairing is invalid,
                or units is not one of the accepted strings.
  RuntimeError  if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
