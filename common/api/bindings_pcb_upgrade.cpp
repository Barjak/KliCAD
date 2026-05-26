/*
 * KliCAD subsystem binding: PCB file-format upgrade.
 *
 * Exposes the PCB upgrade-to-current-format operation as
 *   klicad_native_pcb_upgrade.run(board_path, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli pcb upgrade` (see
 * kicad/cli/command_pcb_upgrade.cpp).  Wraps JOB_PCB_UPGRADE and dispatches
 * via the live KIWAY (found by walking wxTopLevelWindows).
 *
 * NOTE: this UPGRADES THE .kicad_pcb FILE IN PLACE.  The underlying
 * JOB_PCB_UPGRADE has a single filename field and the PCB jobs handler
 * resaves to that same path; there is no separate output-path knob on the
 * JOB.  Callers who need a backup must copy the file before invoking.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_pcb_upgrade.h>
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
// (find_live_kiway_for_export_sch_plot), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_pcb_upgrade()
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


py::object run_pcb_upgrade( const std::string& board_path,
                            bool               force )
{
    KIWAY* kiway = find_live_kiway_for_pcb_upgrade();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(PCB upgrade needs the pcbnew kiface to be loaded)" );

    // The PCB editor frame must exist for board-loading paths used by
    // JOB_PCB_UPGRADE dispatch.  Player(..., true) creates it if absent.
    kiway->Player( FRAME_PCB_EDITOR, true );

    JOB_PCB_UPGRADE job;
    job.m_filename = wxString::FromUTF8( board_path );
    job.m_force    = force;

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

    // The upgrade rewrites the input file in place.  If the JOB didn't
    // record an output, surface the board_path so callers can see what
    // was touched.
    if( exitCode == 0 && py::len( output_paths ) == 0 )
        output_paths.append( board_path );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_pcb_upgrade, m )
{
    m.doc() = "KliCAD PCB file-format upgrade binding (calls JOB_PCB_UPGRADE "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY). "
              "WARNING: upgrades the .kicad_pcb file IN PLACE — make a backup "
              "before calling if you need to preserve the original.";

    m.def( "run", &run_pcb_upgrade,
           py::arg( "board_path" ),
           py::arg( "force" ) = false,
           R"DOC(Upgrade the given .kicad_pcb file's format to the current version, in place.

Mirrors `kicad-cli pcb upgrade`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_PCB_UPGRADE dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files written by the job (from JOB::GetOutputs());
                              falls back to [board_path] on clean exit if the
                              job didn't record an output, since the upgrade
                              rewrites the input file in place.

Kwargs:
  force           Force a resave even if the board is already at the current
                  format version (mirrors --force on the CLI). Default False:
                  a no-op upgrade just returns ok without touching the file.

WARNING: this operation rewrites board_path in place. Copy the file first
if you need a backup of the pre-upgrade content.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
