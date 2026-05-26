/*
 * KliCAD subsystem binding: 3D viewer programmatic control.
 *
 * Exposes the running EDA_3D_VIEWER_FRAME as klicad_native_3d_viewer.* —
 * snapshot capture, view presets, render mode toggle, layer visibility,
 * camera control.
 *
 * Pattern B (kiface-resident).  The 3D viewer has no kiface of its own:
 * it's spawned by pcbnew via KIWAY::Player(FRAME_PCB_DISPLAY3D, true).
 * Rather than stand up a 3d-viewer/api/ register infra for one binding,
 * we host the registration inside the pcbnew kiface (which already owns
 * the 3D viewer frame lifecycle and already links 3d-viewer/).
 *
 * Frame discovery: we cannot dynamic_cast<EDA_3D_VIEWER_FRAME*> across
 * the kiface boundary safely, so we identify via
 * EDA_BASE_FRAME::GetFrameType() == FRAME_PCB_DISPLAY3D and static_cast.
 * Same caveat as bindings_pcb_state.cpp.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <3d_enums.h>
#include <gal/3d/camera.h>

#include <3d_viewer/eda_3d_viewer_frame.h>
#include <3d_viewer/eda_3d_viewer_settings.h>
#include <3d_canvas/board_adapter.h>
#include <3d_canvas/eda_3d_canvas.h>
#include <pcb_base_frame.h>

#include <wx/image.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;


KIWAY* find_live_kiway_for_3d_viewer()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( holder->HasKiway() )
                return &holder->Kiway();
    }
    return nullptr;
}


EDA_3D_VIEWER_FRAME* find_3d_viewer_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( base && base->GetFrameType() == FRAME_PCB_DISPLAY3D )
            return static_cast<EDA_3D_VIEWER_FRAME*>( base );
    }
    return nullptr;
}


// Find any PCB_BASE_FRAME — PCB_EDIT_FRAME or FOOTPRINT_EDIT_FRAME both
// inherit it and both expose CreateAndShow3D_Frame().  Walk top-level
// windows the same way as the other Pattern B helpers (cross-kiface
// dynamic_cast isn't safe — match on GetFrameType then static_cast).
PCB_BASE_FRAME* find_pcb_base_frame_for_3d()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        FRAME_T t = base->GetFrameType();

        if( t == FRAME_PCB_EDITOR || t == FRAME_FOOTPRINT_EDITOR )
            return static_cast<PCB_BASE_FRAME*>( base );
    }
    return nullptr;
}


EDA_3D_VIEWER_FRAME* require_3d_viewer_frame()
{
    if( EDA_3D_VIEWER_FRAME* frame = find_3d_viewer_frame() )
        return frame;

    // Upstream KiCad doesn't spawn FRAME_PCB_DISPLAY3D through the kiway
    // frame factory — IFACE::CreateKiWindow has no case for it.  Instead
    // PCB_BASE_FRAME::CreateAndShow3D_Frame() constructs the viewer as a
    // child of the PCB editor.  Make sure the PCB editor exists, then go
    // through that path.
    KIWAY* kiway = find_live_kiway_for_3d_viewer();

    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running?" );

    // Spawn the PCB editor if it isn't up yet.
    if( !find_pcb_base_frame_for_3d() )
        kiway->Player( FRAME_PCB_EDITOR, true );

    PCB_BASE_FRAME* pcb = find_pcb_base_frame_for_3d();

    if( !pcb )
        throw std::runtime_error( "failed to obtain PCB_BASE_FRAME for 3D viewer parent" );

    EDA_3D_VIEWER_FRAME* frame = pcb->CreateAndShow3D_Frame();

    if( !frame )
        throw std::runtime_error( "PCB_BASE_FRAME::CreateAndShow3D_Frame returned nullptr" );

    return frame;
}


EDA_3D_VIEWER_SETTINGS* require_cfg( EDA_3D_VIEWER_FRAME* aFrame )
{
    EDA_3D_VIEWER_SETTINGS* cfg = aFrame->GetAdapter().m_Cfg;

    if( !cfg )
        throw std::runtime_error( "BOARD_ADAPTER has no EDA_3D_VIEWER_SETTINGS" );

    return cfg;
}


EDA_3D_CANVAS* require_canvas( EDA_3D_VIEWER_FRAME* aFrame )
{
    EDA_3D_CANVAS* c = aFrame->GetCanvas();

    if( !c )
        throw std::runtime_error( "3D viewer frame has no canvas" );

    return c;
}


VIEW3D_TYPE parse_view_preset( const std::string& aName )
{
    if( aName == "top" )    return VIEW3D_TYPE::VIEW3D_TOP;
    if( aName == "bottom" ) return VIEW3D_TYPE::VIEW3D_BOTTOM;
    if( aName == "front" )  return VIEW3D_TYPE::VIEW3D_FRONT;
    if( aName == "back" )   return VIEW3D_TYPE::VIEW3D_BACK;
    if( aName == "left" )   return VIEW3D_TYPE::VIEW3D_LEFT;
    if( aName == "right" )  return VIEW3D_TYPE::VIEW3D_RIGHT;
    if( aName == "fit" )    return VIEW3D_TYPE::VIEW3D_FIT_SCREEN;
    if( aName == "flip" )   return VIEW3D_TYPE::VIEW3D_FLIP;

    // 'iso' / 'top_iso' aren't first-class VIEW3D_TYPE values upstream.
    if( aName == "iso" || aName == "top_iso" )
        throw py::value_error( "preset '" + aName + "' not exposed by "
            "upstream 3D viewer C++ API; compose via rotate('x',..)+rotate('y',..)" );

    throw py::value_error( "unknown view preset '" + aName +
        "'; expected: top/bottom/front/back/left/right/fit/flip" );
}


std::string lower_str( std::string s )
{
    std::transform( s.begin(), s.end(), s.begin(),
                    []( unsigned char c ) { return std::tolower( c ); } );
    return s;
}


// ──────────────────────────────────────────────────────────────────────────
// is_open  — non-spawning probe
// ──────────────────────────────────────────────────────────────────────────
bool viewer_is_open()
{
    return find_3d_viewer_frame() != nullptr;
}


// ──────────────────────────────────────────────────────────────────────────
// take_snapshot
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_take_snapshot( const std::string& aPath,
                                 const std::string& aFormat,
                                 int aWidth, int aHeight )
{
    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );

    std::string fmt = lower_str( aFormat );
    wxBitmapType wxFmt;

    if( fmt == "png" )
        wxFmt = wxBITMAP_TYPE_PNG;
    else if( fmt == "jpg" || fmt == "jpeg" )
        wxFmt = wxBITMAP_TYPE_JPEG;
    else
        throw py::value_error( "format must be 'png' or 'jpeg' (got '" + aFormat + "')" );

    // Upstream's headless captureScreenshot(aSize) is private to the
    // frame, so we resize the live canvas instead.  User sees the resize.
    if( aWidth > 0 && aHeight > 0 )
    {
        frame->SetClientSize( wxSize( aWidth, aHeight ) );
        canvas->SetSize( wxSize( aWidth, aHeight ) );
    }

    EDA_3D_VIEWER_SETTINGS::RENDER_SETTINGS& cfg = require_cfg( frame )->m_Render;
    bool was_highlight = cfg.highlight_on_rollover;
    cfg.highlight_on_rollover = false;

    canvas->DoRePaint();
    canvas->DoRePaint();

    wxImage img;
    canvas->GetScreenshot( img );

    cfg.highlight_on_rollover = was_highlight;

    py::dict result;

    if( !img.IsOk() )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "canvas GetScreenshot returned invalid image" );
        return result;
    }

    wxString path = wxString::FromUTF8( aPath.c_str() );

    if( !img.SaveFile( path, wxFmt ) )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "wxImage::SaveFile failed for '" ) + aPath + "'";
        return result;
    }

    result[ "ok" ]     = true;
    result[ "path" ]   = aPath;
    result[ "format" ] = fmt;
    result[ "width" ]  = img.GetWidth();
    result[ "height" ] = img.GetHeight();
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// set_view_preset
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_set_view_preset( const std::string& aPreset )
{
    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );

    VIEW3D_TYPE view = parse_view_preset( aPreset );
    bool        ok   = canvas->SetView3D( view );
    canvas->Request_refresh( true );

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "preset" ] = aPreset;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// set_render_mode
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_set_render_mode( const std::string& aMode )
{
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    RENDER_ENGINE        engine;

    if( aMode == "opengl" )
        engine = RENDER_ENGINE::OPENGL;
    else if( aMode == "raytracing" || aMode == "raytrace" )
        engine = RENDER_ENGINE::RAYTRACING;
    else
        throw py::value_error( "mode must be 'opengl' or 'raytracing' (got '" + aMode + "')" );

    require_cfg( frame )->m_Render.engine = engine;
    frame->NewDisplay( true );

    py::dict result;
    result[ "ok" ]   = true;
    result[ "mode" ] = aMode;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// Layer visibility toggles
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_show_silkscreen( bool aVisible )
{
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    EDA_3D_VIEWER_SETTINGS* cfg = require_cfg( frame );
    cfg->m_Render.show_silkscreen_top    = aVisible;
    cfg->m_Render.show_silkscreen_bottom = aVisible;
    frame->NewDisplay( true );

    py::dict r;
    r[ "ok" ] = true;
    r[ "visible" ] = aVisible;
    return r;
}


py::object viewer_show_solder_mask( bool aVisible )
{
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    EDA_3D_VIEWER_SETTINGS* cfg = require_cfg( frame );
    cfg->m_Render.show_soldermask_top    = aVisible;
    cfg->m_Render.show_soldermask_bottom = aVisible;
    frame->NewDisplay( true );

    py::dict r;
    r[ "ok" ] = true;
    r[ "visible" ] = aVisible;
    return r;
}


py::object viewer_show_3d_models( bool aVisible )
{
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    EDA_3D_VIEWER_SETTINGS* cfg = require_cfg( frame );
    // Normal THT + SMD + virtual buckets; leave DNP / not-in-pos alone.
    cfg->m_Render.show_footprints_normal  = aVisible;
    cfg->m_Render.show_footprints_insert  = aVisible;
    cfg->m_Render.show_footprints_virtual = aVisible;
    frame->NewDisplay( true );

    py::dict r;
    r[ "ok" ] = true;
    r[ "visible" ] = aVisible;
    return r;
}


py::object viewer_show_axes( bool aVisible )
{
    // Upstream's C++ viewer has no axis-gizmo toggle; the navigator
    // widget is the closest overlay.  Document the mapping in the result.
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    require_cfg( frame )->m_Render.show_navigator = aVisible;

    if( EDA_3D_CANVAS* c = frame->GetCanvas() )
        c->Request_refresh( true );

    py::dict r;
    r[ "ok" ]      = true;
    r[ "visible" ] = aVisible;
    r[ "note" ]    = std::string( "mapped to navigator-widget visibility; "
                                  "no separate axis-gizmo upstream" );
    return r;
}


// ──────────────────────────────────────────────────────────────────────────
// set_background_color (top + bottom set to same solid color)
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_set_background_color( double r, double g, double b )
{
    auto clamp01 = []( double v ) -> float
    {
        if( v < 0.0 ) return 0.0f;
        if( v > 1.0 ) return 1.0f;
        return static_cast<float>( v );
    };

    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();
    BOARD_ADAPTER&       ad    = frame->GetAdapter();
    SFVEC4F              c( clamp01( r ), clamp01( g ), clamp01( b ), 1.0f );

    ad.m_BgColorTop = c;
    ad.m_BgColorBot = c;

    if( EDA_3D_CANVAS* canvas = frame->GetCanvas() )
        canvas->Request_refresh( true );

    py::dict result;
    result[ "ok" ] = true;
    result[ "r" ]  = c.r;
    result[ "g" ]  = c.g;
    result[ "b" ]  = c.b;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// Zoom
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_zoom_step( VIEW3D_TYPE aDir )
{
    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );
    canvas->SetView3D( aDir );
    canvas->Request_refresh( true );

    py::dict r;
    r[ "ok" ] = true;
    return r;
}


py::object viewer_zoom_in()  { return viewer_zoom_step( VIEW3D_TYPE::VIEW3D_ZOOM_IN );  }
py::object viewer_zoom_out() { return viewer_zoom_step( VIEW3D_TYPE::VIEW3D_ZOOM_OUT ); }
py::object viewer_zoom_fit() { return viewer_zoom_step( VIEW3D_TYPE::VIEW3D_FIT_SCREEN ); }


py::object viewer_zoom_to( double aScale )
{
    if( aScale <= 0.0 )
        throw py::value_error( "zoom scale must be > 0" );

    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );

    // CAMERA::Zoom divides m_zoom by aFactor — after ZoomReset (m_zoom=1),
    // passing 1/scale yields absolute zoom of `scale`.  Min/max clamps apply.
    CAMERA& cam = frame->GetCurrentCamera();
    cam.ZoomReset();
    cam.Zoom( static_cast<float>( 1.0 / aScale ) );

    canvas->Request_refresh( true );

    py::dict r;
    r[ "ok" ]    = true;
    r[ "scale" ] = aScale;
    return r;
}


// ──────────────────────────────────────────────────────────────────────────
// set_perspective
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_set_perspective( bool aPerspective )
{
    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );

    canvas->SetProjectionMode( aPerspective ? (int) PROJECTION_TYPE::PERSPECTIVE
                                            : (int) PROJECTION_TYPE::ORTHO );
    canvas->Request_refresh( true );

    py::dict r;
    r[ "ok" ]          = true;
    r[ "perspective" ] = aPerspective;
    return r;
}


// ──────────────────────────────────────────────────────────────────────────
// rotate
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_rotate( const std::string& aAxis, double aDegrees )
{
    EDA_3D_VIEWER_FRAME* frame  = require_3d_viewer_frame();
    EDA_3D_CANVAS*       canvas = require_canvas( frame );

    CAMERA&     cam  = frame->GetCurrentCamera();
    float       rad  = static_cast<float>( aDegrees ) * DEG_TO_RAD;
    std::string axis = lower_str( aAxis );

    if( axis == "x" )      cam.RotateX( rad );
    else if( axis == "y" ) cam.RotateY( rad );
    else if( axis == "z" ) cam.RotateZ( rad );
    else
        throw py::value_error( "axis must be 'x', 'y', or 'z' (got '" + aAxis + "')" );

    canvas->Request_refresh( true );

    py::dict r;
    r[ "ok" ]      = true;
    r[ "axis" ]    = axis;
    r[ "degrees" ] = aDegrees;
    return r;
}


// ──────────────────────────────────────────────────────────────────────────
// refresh
// ──────────────────────────────────────────────────────────────────────────
py::object viewer_refresh( bool aFullRebuild )
{
    EDA_3D_VIEWER_FRAME* frame = require_3d_viewer_frame();

    if( aFullRebuild )
        frame->NewDisplay( true );
    else if( EDA_3D_CANVAS* c = frame->GetCanvas() )
        c->Request_refresh( true );

    py::dict r;
    r[ "ok" ]           = true;
    r[ "full_rebuild" ] = aFullRebuild;
    return r;
}

} // anon


void klicad_register_3d_viewer_bindings( py::module_& m )
{
    m.doc() = "KliCAD 3D viewer binding — programmatic control of the "
              "running EDA_3D_VIEWER_FRAME.  Hosted in the pcbnew kiface "
              "because the 3D viewer has no kiface of its own.";

    m.def( "is_open", &viewer_is_open,
           "Return True if the 3D viewer frame is currently spawned. "
           "Non-spawning probe (every other call auto-spawns pcbnew + viewer)." );

    m.def( "take_snapshot", &viewer_take_snapshot,
           py::arg( "path" ),
           py::arg( "format" ) = std::string( "png" ),
           py::arg( "width" )  = 1920,
           py::arg( "height" ) = 1080,
           "Capture the 3D viewport to file (png/jpeg). "
           "If width/height > 0, resizes the live canvas first (user sees the resize). "
           "Returns {ok, path, format, width, height, error?}." );

    m.def( "set_view_preset", &viewer_set_view_preset, py::arg( "preset" ),
           "Switch to: top/bottom/front/back/left/right/fit/flip. "
           "'iso'/'top_iso' raise ValueError (no upstream enumerator)." );

    m.def( "set_render_mode", &viewer_set_render_mode, py::arg( "mode" ),
           "Switch render engine: 'opengl' or 'raytracing'. Triggers full rebuild." );

    m.def( "show_silkscreen", &viewer_show_silkscreen, py::arg( "visible" ),
           "Top+bottom silkscreen visibility. Triggers full rebuild." );

    m.def( "show_solder_mask", &viewer_show_solder_mask, py::arg( "visible" ),
           "Top+bottom soldermask visibility. Triggers full rebuild." );

    m.def( "show_3d_models", &viewer_show_3d_models, py::arg( "visible" ),
           "Normal THT+SMD+virtual model visibility. Does NOT touch DNP/not-in-pos. "
           "Triggers full rebuild." );

    m.def( "show_axes", &viewer_show_axes, py::arg( "visible" ),
           "Mapped to navigator-widget visibility. No upstream axis-gizmo toggle. "
           "Result dict has a 'note' field documenting this." );

    m.def( "set_background_color", &viewer_set_background_color,
           py::arg( "r" ), py::arg( "g" ), py::arg( "b" ),
           "Set both top and bottom gradient colors to the same RGB (each in [0,1]). "
           "Alpha fixed at 1.0." );

    m.def( "zoom_in",  &viewer_zoom_in,  "Step zoom in (VIEW3D_ZOOM_IN)." );
    m.def( "zoom_out", &viewer_zoom_out, "Step zoom out (VIEW3D_ZOOM_OUT)." );
    m.def( "zoom_fit", &viewer_zoom_fit, "Fit board to viewport (VIEW3D_FIT_SCREEN)." );
    m.def( "zoom_to",  &viewer_zoom_to,  py::arg( "scale" ),
           "Absolute zoom: scale>1 = in, <1 = out, 1.0 = reset. Min/max clamps apply." );

    m.def( "set_perspective", &viewer_set_perspective, py::arg( "perspective" ),
           "True -> PERSPECTIVE, False -> ORTHO." );

    m.def( "rotate", &viewer_rotate, py::arg( "axis" ), py::arg( "degrees" ),
           "Rotate camera around 'x'/'y'/'z' by degrees (positive = CCW per "
           "CAMERA::RotateX/Y/Z semantics; non-animated)." );

    m.def( "refresh", &viewer_refresh, py::arg( "full_rebuild" ) = false,
           "Force refresh. full_rebuild=True calls NewDisplay(true) (needed after "
           "layer/visibility/color changes). False = canvas Request_refresh(true) only." );
}
