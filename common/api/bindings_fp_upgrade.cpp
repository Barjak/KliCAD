/*
 * KliCAD subsystem binding: footprint-library file-format upgrade.
 *
 * Exposes the footprint-library upgrade-to-current-format operation as
 *   klicad_native_fp_upgrade.run(library_path, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli fp upgrade` (see
 * kicad/cli/command_fp_upgrade.cpp).  Wraps JOB_FP_UPGRADE and dispatches
 * via the live KIWAY (found by walking wxTopLevelWindows).  Footprint-
 * library work is handled by the pcbnew kiface (FACE_PCB), so we make
 * sure the PCB editor frame is up before dispatch — same dance as
 * bindings_fp_export_svg / bindings_pcb_upgrade.
 *
 * NOTE: by default this UPGRADES THE FOOTPRINT LIBRARY IN PLACE.  The
 * underlying JOB_FP_UPGRADE has an optional `m_outputLibraryPath`; when
 * empty (the default, matching the CLI when --output is omitted) the
 * pcbnew jobs handler resaves into the source library.  Callers who need
 * a backup must either pass `output_library_path` or copy the library
 * before invoking.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_fp_upgrade.h>
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
// (find_live_kiway_for_pcb_upgrade), bindings_sch_upgrade.cpp
// (find_live_kiway_for_sch_upgrade), bindings_render.cpp
// (find_live_kiway_for_render), bindings_fp_export_svg.cpp
// (find_live_kiway_for_fp_export_svg), and bindings_sym_export_svg.cpp
// (find_live_kiway_for_sym_export_svg).
KIWAY* find_live_kiway_for_fp_upgrade()
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


py::object run_fp_upgrade( const std::string& library_path,
                           const std::string& output_library_path,
                           bool               force )
{
    KIWAY* kiway = find_live_kiway_for_fp_upgrade();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(footprint-library upgrade needs the pcbnew kiface "
                                  "to be loaded)" );

    // Footprint-library upgrade is dispatched through the pcbnew kiface
    // (KIWAY::FACE_PCB), matching the CLI in command_fp_upgrade.cpp.  Ensure
    // the PCB editor frame is up so the kiface is loaded.  Player(..., true)
    // creates the frame if missing.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_FP_UPGRADE job;
    job.m_libraryPath = wxString::FromUTF8( library_path );
    if( !output_library_path.empty() )
        job.m_outputLibraryPath = wxString::FromUTF8( output_library_path );
    job.m_force = force;

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

    // The upgrade rewrites the library in place when no output path was given.
    // If the JOB didn't record an output, surface whichever path was effectively
    // touched (output_library_path if set, else library_path) so callers can see
    // what was modified.  Mirrors bindings_pcb_upgrade.cpp's fallback shape.
    if( exitCode == 0 && py::len( output_paths ) == 0 )
    {
        output_paths.append( output_library_path.empty() ? library_path
                                                         : output_library_path );
    }

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_fp_upgrade, m )
{
    m.doc() = "KliCAD footprint-library file-format upgrade binding (calls "
              "JOB_FP_UPGRADE under the hood). Returns a structured dict; never "
              "streams. Requires KiCad's GUI to be running (needs a live KIWAY). "
              "WARNING: by default upgrades the footprint library IN PLACE — pass "
              "`output_library_path` to write the upgraded library elsewhere, or "
              "make a backup before calling if you need to preserve the original.";

    m.def( "run", &run_fp_upgrade,
           py::arg( "library_path" ),
           py::arg( "output_library_path" ) = std::string(),
           py::arg( "force" )               = false,
           R"DOC(Upgrade the given footprint library's format to the current version.

Mirrors `kicad-cli fp upgrade`. The library path should be a footprint library
(typically a .pretty directory, or whatever the footprint plugin resolves as a
library). Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_FP_UPGRADE dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files/directories written by the job (from
                              JOB::GetOutputs()); falls back to
                              [output_library_path or library_path] on clean
                              exit if the job didn't record an output, since
                              the upgrade rewrites in place when no output is
                              given.

Kwargs:
  output_library_path  Optional destination for the upgraded library (CLI
                       positional output arg). Empty (default) means upgrade
                       library_path IN PLACE.
  force                Force a resave even if the library is already at the
                       current format version (mirrors --force on the CLI).
                       Default False: a no-op upgrade just returns ok without
                       rewriting the library.

WARNING: this operation rewrites library_path in place when output_library_path
is empty. Copy the library first if you need a backup of the pre-upgrade
content.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
