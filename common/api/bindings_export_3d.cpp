/*
 * KliCAD subsystem binding: PCB 3D model export.
 *
 * Exposes 3D-model export (STEP, STEPZ, BREP, XAO, GLB, VRML, PLY, STL, U3D,
 * PDF) as
 *   klicad_native_export_3d.run(board_path, output, format='step', ...) -> dict
 *
 * Mirrors `kicad-cli pcb export {step,glb,brep,xao,stl,ply,vrml,...}` (see
 * kicad/cli/command_pcb_export_3d.cpp) and the typed-RPC handler
 * API_HANDLER_PCB::handleRunBoardJobExport3D in pcbnew/api/api_handler_pcb.cpp.
 * Wraps JOB_EXPORT_PCB_3D and dispatches via the live KIWAY (found by walking
 * wxTopLevelWindows).
 *
 * Note: the JOB carries one nested struct (`m_3dparams`, of type
 * EXPORTER_STEP_PARAMS — despite the name it is used for every 3D format)
 * with the bulk of the boolean toggles.  These are flattened out as
 * individual kwargs here, defaulting to match the CLI's behavior whenever
 * the CLI has an opinion; otherwise we use the EXPORTER_STEP_PARAMS ctor
 * defaults.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_export_pcb_3d.h>
#include <jobs/job.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <math/vector2d.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a live
// KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR clashes with
// the matching helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), and bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff).
KIWAY* find_live_kiway_for_export_3d()
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


// String -> JOB_EXPORT_PCB_3D::FORMAT.  Accept the same set of names the CLI
// uses (see command_pcb_export_3d.cpp ~line 270) plus 'vrml' which is reached
// via a separate CLI subcommand but is still a valid JOB format.
JOB_EXPORT_PCB_3D::FORMAT pcb_3d_format_from_string( const std::string& s )
{
    if( s == "step" ) return JOB_EXPORT_PCB_3D::FORMAT::STEP;
    if( s == "stpz" ) return JOB_EXPORT_PCB_3D::FORMAT::STEPZ;
    if( s == "stepz" ) return JOB_EXPORT_PCB_3D::FORMAT::STEPZ;
    if( s == "brep" ) return JOB_EXPORT_PCB_3D::FORMAT::BREP;
    if( s == "xao" )  return JOB_EXPORT_PCB_3D::FORMAT::XAO;
    if( s == "glb" )  return JOB_EXPORT_PCB_3D::FORMAT::GLB;
    if( s == "vrml" ) return JOB_EXPORT_PCB_3D::FORMAT::VRML;
    if( s == "ply" )  return JOB_EXPORT_PCB_3D::FORMAT::PLY;
    if( s == "stl" )  return JOB_EXPORT_PCB_3D::FORMAT::STL;
    if( s == "u3d" )  return JOB_EXPORT_PCB_3D::FORMAT::U3D;
    if( s == "pdf" )  return JOB_EXPORT_PCB_3D::FORMAT::PDF;
    throw std::invalid_argument(
            "format must be one of: 'step', 'stpz' (alias 'stepz'), 'brep', 'xao', "
            "'glb', 'vrml', 'ply', 'stl', 'u3d', 'pdf' (got '" + s + "')" );
}


// String -> JOB_EXPORT_PCB_3D::VRML_UNITS.  Matches the CLI's accepted set
// for `--units` on `kicad-cli pcb export vrml`.
JOB_EXPORT_PCB_3D::VRML_UNITS pcb_3d_vrml_units_from_string( const std::string& s )
{
    if( s == "in" )     return JOB_EXPORT_PCB_3D::VRML_UNITS::INCH;
    if( s == "inch" )   return JOB_EXPORT_PCB_3D::VRML_UNITS::INCH;
    if( s == "mm" )     return JOB_EXPORT_PCB_3D::VRML_UNITS::MM;
    if( s == "m" )      return JOB_EXPORT_PCB_3D::VRML_UNITS::METERS;
    if( s == "meters" ) return JOB_EXPORT_PCB_3D::VRML_UNITS::METERS;
    if( s == "tenths" ) return JOB_EXPORT_PCB_3D::VRML_UNITS::TENTHS;
    throw std::invalid_argument(
            "vrml_units must be one of: 'in' (alias 'inch'), 'mm', 'm' (alias "
            "'meters'), 'tenths' (got '" + s + "')" );
}


// Map JOB_EXPORT_PCB_3D::FORMAT to the matching EXPORTER_STEP_PARAMS::FORMAT
// (the inner struct keeps its own format enum, which we keep in sync with the
// outer enum — mirrors JOB_EXPORT_PCB_3D::SetStepFormat in the JOB ctor file).
// VRML has no EXPORTER_STEP_PARAMS::FORMAT counterpart (the VRML exporter
// path is selected by the outer m_format only).  Returns std::nullopt for VRML.
std::optional<EXPORTER_STEP_PARAMS::FORMAT>
        pcb_3d_inner_format_for( JOB_EXPORT_PCB_3D::FORMAT f )
{
    switch( f )
    {
    case JOB_EXPORT_PCB_3D::FORMAT::STEP:  return EXPORTER_STEP_PARAMS::FORMAT::STEP;
    case JOB_EXPORT_PCB_3D::FORMAT::STEPZ: return EXPORTER_STEP_PARAMS::FORMAT::STEPZ;
    case JOB_EXPORT_PCB_3D::FORMAT::BREP:  return EXPORTER_STEP_PARAMS::FORMAT::BREP;
    case JOB_EXPORT_PCB_3D::FORMAT::XAO:   return EXPORTER_STEP_PARAMS::FORMAT::XAO;
    case JOB_EXPORT_PCB_3D::FORMAT::GLB:   return EXPORTER_STEP_PARAMS::FORMAT::GLB;
    case JOB_EXPORT_PCB_3D::FORMAT::PLY:   return EXPORTER_STEP_PARAMS::FORMAT::PLY;
    case JOB_EXPORT_PCB_3D::FORMAT::STL:   return EXPORTER_STEP_PARAMS::FORMAT::STL;
    case JOB_EXPORT_PCB_3D::FORMAT::U3D:   return EXPORTER_STEP_PARAMS::FORMAT::U3D;
    case JOB_EXPORT_PCB_3D::FORMAT::PDF:   return EXPORTER_STEP_PARAMS::FORMAT::PDF;
    case JOB_EXPORT_PCB_3D::FORMAT::VRML:
    case JOB_EXPORT_PCB_3D::FORMAT::UNKNOWN:
    default:
        return std::nullopt;
    }
}


py::object run_export_3d( const std::string&                board_path,
                          const std::string&                output,
                          const std::string&                format,
                          // ---- top-level JOB_EXPORT_PCB_3D fields ----
                          const std::string&                variant,
                          // ---- m_3dparams (EXPORTER_STEP_PARAMS) fields ----
                          bool                              overwrite,
                          bool                              use_grid_origin,
                          bool                              use_drill_origin,
                          bool                              use_defined_origin,
                          bool                              use_pcb_center_origin,
                          const std::optional<double>&      user_origin_x_nm,
                          const std::optional<double>&      user_origin_y_nm,
                          bool                              include_unspecified,
                          bool                              include_dnp,
                          bool                              subst_models,
                          double                            board_outlines_chaining_epsilon,
                          bool                              board_only,
                          bool                              cut_vias_in_body,
                          bool                              export_board_body,
                          bool                              export_components,
                          bool                              export_tracks_and_vias,
                          bool                              export_pads,
                          bool                              export_zones,
                          bool                              export_inner_copper,
                          bool                              export_silkscreen,
                          bool                              export_soldermask,
                          bool                              fuse_shapes,
                          bool                              fill_all_vias,
                          bool                              optimize_step,
                          bool                              extra_pad_thickness,
                          const std::string&                net_filter,
                          const std::string&                component_filter,
                          // ---- VRML-only fields ----
                          const std::string&                vrml_units,
                          const std::string&                vrml_model_dir,
                          bool                              vrml_relative_paths )
{
    JOB_EXPORT_PCB_3D::FORMAT jobFormat = pcb_3d_format_from_string( format );

    KIWAY* kiway = find_live_kiway_for_export_3d();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(3D model export needs the pcbnew kiface and project "
                                  "state to be loaded)" );

    // PCB editor frame must exist for board-loading paths used by
    // JOB_EXPORT_PCB_3D dispatch.  Player(..., true) creates it if absent.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_EXPORT_PCB_3D job;
    job.m_filename = wxString::FromUTF8( board_path );
    job.SetConfiguredOutputPath( wxString::FromUTF8( output ) );
    job.m_format = jobFormat;

    // Keep the inner EXPORTER_STEP_PARAMS format in sync with the outer
    // JOB format, the same way JOB_EXPORT_PCB_3D::SetStepFormat does.
    // VRML stays at the EXPORTER_STEP_PARAMS default since there is no
    // matching inner enum value.
    if( auto inner = pcb_3d_inner_format_for( jobFormat ) )
        job.m_3dparams.m_Format = *inner;

    if( !variant.empty() )
        job.m_variant = wxString::FromUTF8( variant );

    // ---- m_3dparams ----
    EXPORTER_STEP_PARAMS& p = job.m_3dparams;

    p.m_Overwrite           = overwrite;
    p.m_UseGridOrigin       = use_grid_origin;
    p.m_UseDrillOrigin      = use_drill_origin;
    p.m_UseDefinedOrigin    = use_defined_origin;
    p.m_UsePcbCenterOrigin  = use_pcb_center_origin;
    p.m_IncludeUnspecified  = include_unspecified;
    p.m_IncludeDNP          = include_dnp;
    p.m_SubstModels         = subst_models;
    p.m_BoardOutlinesChainingEpsilon = board_outlines_chaining_epsilon;
    p.m_BoardOnly           = board_only;
    p.m_CutViasInBody       = cut_vias_in_body;
    p.m_ExportBoardBody     = export_board_body;
    p.m_ExportComponents    = export_components;
    p.m_ExportTracksVias    = export_tracks_and_vias;
    p.m_ExportPads          = export_pads;
    p.m_ExportZones         = export_zones;
    p.m_ExportInnerCopper   = export_inner_copper;
    p.m_ExportSilkscreen    = export_silkscreen;
    p.m_ExportSoldermask    = export_soldermask;
    p.m_FuseShapes          = fuse_shapes;
    p.m_FillAllVias         = fill_all_vias;
    p.m_OptimizeStep        = optimize_step;
    p.m_ExtraPadThickness   = extra_pad_thickness;

    if( !net_filter.empty() )
        p.m_NetFilter = wxString::FromUTF8( net_filter );
    if( !component_filter.empty() )
        p.m_ComponentFilter = wxString::FromUTF8( component_filter );

    // User origin: treat as supplied iff either coordinate was explicitly
    // passed.  Coordinates are in KiCad internal units (nm), matching the
    // typed-RPC handler's convention (see api_handler_pcb.cpp:2868).  Set
    // m_hasUserOrigin so JOB_EXPORT_PCB_3D dispatch picks the user origin
    // path rather than the board's own origin settings.
    if( user_origin_x_nm.has_value() || user_origin_y_nm.has_value() )
    {
        p.m_Origin = VECTOR2D( user_origin_x_nm.value_or( 0.0 ),
                               user_origin_y_nm.value_or( 0.0 ) );
        job.m_hasUserOrigin = true;
        // The typed-RPC handler also forces UseDefinedOrigin when an explicit
        // user origin is supplied; mirror that so the chosen origin actually
        // takes effect regardless of the boolean kwarg.
        p.m_UseDefinedOrigin = true;
    }

    // ---- VRML-only fields ----
    if( jobFormat == JOB_EXPORT_PCB_3D::FORMAT::VRML )
    {
        job.m_vrmlUnits         = pcb_3d_vrml_units_from_string( vrml_units );
        job.m_vrmlModelDir      = wxString::FromUTF8( vrml_model_dir );
        job.m_vrmlRelativePaths = vrml_relative_paths;
    }

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


PYBIND11_EMBEDDED_MODULE( klicad_native_export_3d, m )
{
    m.doc() = "KliCAD PCB 3D model export binding (calls JOB_EXPORT_PCB_3D "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_3d,
           py::arg( "board_path" ),
           py::arg( "output" ),
           py::arg( "format" )                          = std::string( "step" ),
           // top-level
           py::arg( "variant" )                         = std::string(),
           // m_3dparams (mirrors EXPORTER_STEP_PARAMS ctor defaults / CLI defaults)
           py::arg( "overwrite" )                       = false,
           py::arg( "use_grid_origin" )                 = false,
           py::arg( "use_drill_origin" )                = false,
           py::arg( "use_defined_origin" )              = false,
           py::arg( "use_pcb_center_origin" )           = false,
           py::arg( "user_origin_x_nm" )                = std::nullopt,
           py::arg( "user_origin_y_nm" )                = std::nullopt,
           py::arg( "include_unspecified" )             = true,
           py::arg( "include_dnp" )                     = true,
           py::arg( "subst_models" )                    = true,
           py::arg( "board_outlines_chaining_epsilon" ) = BOARD_DEFAULT_CHAINING_EPSILON,
           py::arg( "board_only" )                      = false,
           py::arg( "cut_vias_in_body" )                = false,
           py::arg( "export_board_body" )               = true,
           py::arg( "export_components" )               = true,
           py::arg( "export_tracks_and_vias" )          = false,
           py::arg( "export_pads" )                     = false,
           py::arg( "export_zones" )                    = false,
           py::arg( "export_inner_copper" )             = false,
           py::arg( "export_silkscreen" )               = false,
           py::arg( "export_soldermask" )               = false,
           py::arg( "fuse_shapes" )                     = false,
           py::arg( "fill_all_vias" )                   = false,
           py::arg( "optimize_step" )                   = true,
           py::arg( "extra_pad_thickness" )             = true,
           py::arg( "net_filter" )                      = std::string(),
           py::arg( "component_filter" )                = std::string(),
           // VRML-only
           py::arg( "vrml_units" )                      = std::string( "in" ),
           py::arg( "vrml_model_dir" )                  = std::string(),
           py::arg( "vrml_relative_paths" )             = false,
           R"DOC(Export a 3D model of the given .kicad_pcb to `output`.

Mirrors `kicad-cli pcb export {step,stpz,brep,xao,glb,vrml,ply,stl,u3d,pdf}`.
Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_EXPORT_PCB_3D dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files produced by the job (from JOB::GetOutputs())

format       Output format: 'step' (default) | 'stpz' (alias 'stepz') | 'brep'
             | 'xao' | 'glb' | 'vrml' | 'ply' | 'stl' | 'u3d' | 'pdf'.
             Raises ValueError on any other value.

Common kwargs (defaults match EXPORTER_STEP_PARAMS / CLI behavior):
  variant                          Variant name to apply (optional).
  overwrite                        Overwrite output if it already exists.
  use_grid_origin / use_drill_origin / use_defined_origin /
  use_pcb_center_origin            Origin-selection toggles.
  user_origin_x_nm / user_origin_y_nm
                                   Explicit origin in board internal units (nm).
                                   Passing either also sets m_hasUserOrigin
                                   and forces use_defined_origin (matches the
                                   typed-RPC handler's behavior).
  include_unspecified              Include 'Unspecified' footprint-type 3D models.
  include_dnp                      Include DNP 3D models.
  subst_models                     Substitute STEP/IGS in place of VRML models.
  board_outlines_chaining_epsilon  Min distance to treat outline points as the
                                   same (board units; mm here).
  board_only                       Only emit the board body, no components.
  cut_vias_in_body                 Cut via holes even if conductor layers aren't
                                   exported.
  export_board_body                Include the board body. (default True)
  export_components                Include component 3D models. (default True)
  export_tracks_and_vias / export_pads / export_zones / export_inner_copper /
  export_silkscreen / export_soldermask
                                   Optional copper / mechanical inclusions.
  fuse_shapes                      Fuse overlapping geometry.
  fill_all_vias                    Don't cut via holes in conductor layers.
  optimize_step                    Optimize STEP file (default True; STEP/STPZ only).
  extra_pad_thickness              Apply extra pad thickness (default True).
  net_filter                       Wildcard for copper-net filtering.
  component_filter                 Comma-separated reference-designator filter.

VRML-only kwargs (ignored unless format='vrml'):
  vrml_units                       'in' (alias 'inch', default) | 'mm' | 'm'
                                   (alias 'meters') | 'tenths'.
  vrml_model_dir                   Folder to write 3D models into; empty embeds
                                   them in the main VRML file.
  vrml_relative_paths              Emit relative paths into the VRML when
                                   vrml_model_dir is set.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running),
or ValueError if format / vrml_units is not in the accepted set.
)DOC" );
}
