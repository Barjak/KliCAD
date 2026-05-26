/*
 * KliCAD subsystem binding: PCB raytraced render (image export).
 *
 * Exposes 3D PCB rendering as
 *   klicad_native_render.run(board_path, output, format='png', side='top', ...) -> dict
 *
 * Mirrors `kicad-cli pcb render` (see kicad/cli/command_pcb_render.cpp) and
 * wraps JOB_PCB_RENDER, dispatching via the live KIWAY (found by walking
 * wxTopLevelWindows).  Returns a structured dict; never streams.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_pcb_render.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <math/vector3.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a
// live KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes
// with other bindings_*.cpp TUs (e.g. find_live_kiway in bindings_drc.cpp).
KIWAY* find_live_kiway_for_render()
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


JOB_PCB_RENDER::FORMAT render_format_from_string( const std::string& s )
{
    if( s == "png" )                            return JOB_PCB_RENDER::FORMAT::PNG;
    if( s == "jpg" || s == "jpeg" )             return JOB_PCB_RENDER::FORMAT::JPEG;
    throw std::invalid_argument( "format must be one of: 'png', 'jpg', 'jpeg' (got '"
                                 + s + "')" );
}


JOB_PCB_RENDER::SIDE render_side_from_string( const std::string& s )
{
    if( s == "top" )    return JOB_PCB_RENDER::SIDE::TOP;
    if( s == "bottom" ) return JOB_PCB_RENDER::SIDE::BOTTOM;
    if( s == "left" )   return JOB_PCB_RENDER::SIDE::LEFT;
    if( s == "right" )  return JOB_PCB_RENDER::SIDE::RIGHT;
    if( s == "front" )  return JOB_PCB_RENDER::SIDE::FRONT;
    if( s == "back" )   return JOB_PCB_RENDER::SIDE::BACK;
    throw std::invalid_argument( "side must be one of: 'top', 'bottom', 'left', 'right', "
                                 "'front', 'back' (got '" + s + "')" );
}


JOB_PCB_RENDER::BG_STYLE render_bg_from_string( const std::string& s )
{
    if( s == "default" )     return JOB_PCB_RENDER::BG_STYLE::DEFAULT;
    if( s == "transparent" ) return JOB_PCB_RENDER::BG_STYLE::TRANSPARENT;
    if( s == "opaque" )      return JOB_PCB_RENDER::BG_STYLE::OPAQUE;
    throw std::invalid_argument( "background must be one of: 'default', 'transparent', 'opaque' "
                                 "(got '" + s + "')" );
}


JOB_PCB_RENDER::QUALITY render_quality_from_string( const std::string& s )
{
    if( s == "basic" )         return JOB_PCB_RENDER::QUALITY::BASIC;
    if( s == "high" )          return JOB_PCB_RENDER::QUALITY::HIGH;
    if( s == "user" )          return JOB_PCB_RENDER::QUALITY::USER;
    if( s == "job_settings" )  return JOB_PCB_RENDER::QUALITY::JOB_SETTINGS;
    throw std::invalid_argument( "quality must be one of: 'basic', 'high', 'user', "
                                 "'job_settings' (got '" + s + "')" );
}


// Parse a 3-tuple/list of doubles, or a Python None for "leave default".
// Empty list also means "leave default" for caller convenience.
bool render_vec3_from_pyobj( const py::object& obj, VECTOR3D& out )
{
    if( obj.is_none() )
        return true;

    py::sequence seq;
    try
    {
        seq = obj.cast<py::sequence>();
    }
    catch( const py::cast_error& )
    {
        throw std::invalid_argument( "vector kwarg must be a 3-element sequence (x, y, z) "
                                     "or None" );
    }

    if( py::len( seq ) == 0 )
        return true;

    if( py::len( seq ) != 3 )
        throw std::invalid_argument( "vector kwarg must have exactly 3 elements (x, y, z)" );

    out.x = seq[0].cast<double>();
    out.y = seq[1].cast<double>();
    out.z = seq[2].cast<double>();
    return true;
}


py::object run_render( const std::string& board_path,
                       const std::string& output,
                       const std::string& format,
                       const std::string& side,
                       const std::string& background,
                       const std::string& quality,
                       const std::string& preset,
                       bool               use_board_stackup_colors,
                       int                width,
                       int                height,
                       double             zoom,
                       bool               perspective,
                       bool               floor,
                       bool               anti_alias,
                       bool               post_process,
                       bool               procedural_textures,
                       const py::object&  rotation,
                       const py::object&  pan,
                       const py::object&  pivot,
                       const py::object&  light_top_intensity,
                       const py::object&  light_bottom_intensity,
                       const py::object&  light_side_intensity,
                       const py::object&  light_camera_intensity,
                       int                light_side_elevation,
                       const std::string& variant )
{
    KIWAY* kiway = find_live_kiway_for_render();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(PCB render needs the pcbnew kiface and project state "
                                  "to be loaded)" );

    // The PCB editor frame must exist for the board-loading path used by
    // JOB_PCB_RENDER dispatch.  Player(..., true) creates the frame if absent.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_PCB_RENDER job;
    job.m_filename = wxString::FromUTF8( board_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output ) );

    job.m_format   = render_format_from_string( format );
    job.m_side     = render_side_from_string( side );
    job.m_bgStyle  = render_bg_from_string( background );
    job.m_quality  = render_quality_from_string( quality );

    job.m_appearancePreset      = preset;
    job.m_useBoardStackupColors = use_board_stackup_colors;

    job.m_width        = width;
    job.m_height       = height;
    job.m_zoom         = zoom;
    job.m_perspective  = perspective;
    job.m_floor        = floor;
    job.m_antiAlias    = anti_alias;
    job.m_postProcess  = post_process;
    job.m_proceduralTextures = procedural_textures;

    render_vec3_from_pyobj( rotation, job.m_rotation );
    render_vec3_from_pyobj( pan,      job.m_pan );
    render_vec3_from_pyobj( pivot,    job.m_pivot );

    render_vec3_from_pyobj( light_top_intensity,    job.m_lightTopIntensity );
    render_vec3_from_pyobj( light_bottom_intensity, job.m_lightBottomIntensity );
    render_vec3_from_pyobj( light_side_intensity,   job.m_lightSideIntensity );
    render_vec3_from_pyobj( light_camera_intensity, job.m_lightCameraIntensity );

    job.m_lightSideElevation = light_side_elevation;

    if( !variant.empty() )
        job.m_variant = wxString::FromUTF8( variant );

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // ProcessJob blocks on the main thread and may dispatch into wx event
        // handling; release the GIL so any nested Python callbacks (none today,
        // but future bindings might) can re-acquire it.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_PCB, &job, &reporter );
    }

    py::list output_paths;
    for( const JOB_OUTPUT& out : job.GetOutputs() )
        output_paths.append( std::string( out.m_outputPath.ToUTF8() ) );

    // Fallback: if the job produced no JOB_OUTPUT entries (some renderer
    // paths only write the file without registering it), surface the
    // configured output path so callers always see where the image went.
    if( py::len( output_paths ) == 0 )
        output_paths.append( output );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_render, m )
{
    m.doc() = "KliCAD PCB render binding (calls JOB_PCB_RENDER under the hood). "
              "Renders the 3D board view to a PNG or JPEG image. Returns a "
              "structured dict; never streams. Requires KiCad's GUI to be "
              "running (needs a live KIWAY).";

    m.def( "run", &run_render,
           py::arg( "board_path" ),
           py::arg( "output" ),
           py::arg( "format" )                   = "png",
           py::arg( "side" )                     = "top",
           py::arg( "background" )               = "default",
           py::arg( "quality" )                  = "basic",
           py::arg( "preset" )                   = std::string(),
           py::arg( "use_board_stackup_colors" ) = true,
           py::arg( "width" )                    = 1600,
           py::arg( "height" )                   = 900,
           py::arg( "zoom" )                     = 1.0,
           py::arg( "perspective" )              = false,
           py::arg( "floor" )                    = false,
           py::arg( "anti_alias" )               = true,
           py::arg( "post_process" )             = false,
           py::arg( "procedural_textures" )      = false,
           py::arg( "rotation" )                 = py::none(),
           py::arg( "pan" )                      = py::none(),
           py::arg( "pivot" )                    = py::none(),
           py::arg( "light_top_intensity" )      = py::none(),
           py::arg( "light_bottom_intensity" )   = py::none(),
           py::arg( "light_side_intensity" )     = py::none(),
           py::arg( "light_camera_intensity" )   = py::none(),
           py::arg( "light_side_elevation" )     = 60,
           py::arg( "variant" )                  = std::string(),
           R"DOC(Render the 3D view of a .kicad_pcb to a PNG or JPEG image.

Mirrors `kicad-cli pcb render`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_PCB_RENDER dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files produced (from JOB::GetOutputs(); falls back
                              to the configured `output` if the job did not
                              register any outputs)

Enum kwargs (string values, case-sensitive):
  format       'png' (default) | 'jpg' | 'jpeg'
  side         'top' (default) | 'bottom' | 'left' | 'right' | 'front' | 'back'
  background   'default' (default) | 'transparent' | 'opaque'
  quality      'basic' (default) | 'high' | 'user' | 'job_settings'

Other kwargs:
  preset                      Appearance preset name (e.g. follow-pcb-editor,
                              follow-plot-settings, or a user-defined preset).
                              Empty string leaves the job's default.
  use_board_stackup_colors    Override preset colors with board stackup colors
                              (default True, matches CLI).
  width, height               Image dimensions in pixels (default 1600x900,
                              matches the CLI).
  zoom                        Camera zoom (default 1.0).
  perspective                 Perspective projection instead of orthogonal.
  floor                       Force floor + shadows + post-processing on, even
                              if the quality preset disables them.
  anti_alias                  Enable anti-aliasing (default True).
  post_process                Enable post-processing.
  procedural_textures         Enable procedural textures.
  rotation, pan, pivot        3-element sequences (X, Y, Z); None = default.
  light_{top,bottom,side,camera}_intensity
                              3-element (R, G, B) intensity, range 0-1; None =
                              default.
  light_side_elevation        Side light elevation angle in degrees, 0-90
                              (default 60).
  variant                     Variant name to apply (optional).

Raises:
  RuntimeError if no live KIWAY is available (KiCad GUI not running).
  ValueError   if an enum string is unrecognized or a vector kwarg is not a
               3-element sequence (or None).
)DOC" );
}
