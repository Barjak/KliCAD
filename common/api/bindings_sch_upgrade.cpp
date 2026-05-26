/*
 * KliCAD subsystem binding: schematic file-format upgrade.
 *
 * Exposes the schematic upgrade-to-current-format operation as
 *   klicad_native_sch_upgrade.run(schematic_path, ...) -> dict
 *
 * Mirrors the behavior of `kicad-cli sch upgrade` (see
 * kicad/cli/command_sch_upgrade.cpp) and the PCB-side symmetric binding in
 * bindings_pcb_upgrade.cpp.  Wraps JOB_SCH_UPGRADE and dispatches via the
 * live KIWAY (found by walking wxTopLevelWindows).
 *
 * NOTE: this UPGRADES THE .kicad_sch FILE IN PLACE.  The underlying
 * JOB_SCH_UPGRADE has a single filename field and the schematic jobs
 * handler resaves to that same path; there is no separate output-path knob
 * on the JOB.  Callers who need a backup must copy the file before
 * invoking.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_sch_upgrade.h>
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
// (find_live_kiway_for_erc), bindings_pcb_upgrade.cpp
// (find_live_kiway_for_pcb_upgrade), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_sch_bom.cpp
// (find_live_kiway_for_export_sch_bom), bindings_export_sch_netlist.cpp
// (find_live_kiway_for_export_sch_netlist), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_sch_upgrade()
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


py::object run_sch_upgrade( const std::string& schematic_path,
                            bool               force )
{
    KIWAY* kiway = find_live_kiway_for_sch_upgrade();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(schematic upgrade needs the eeschema kiface to be loaded)" );

    // The schematic editor frame must exist for the schematic-loading paths
    // used by JOB_SCH_UPGRADE dispatch.  Player(..., true) creates it if
    // absent.
    kiway->Player( FRAME_SCH, true );

    JOB_SCH_UPGRADE job;
    job.m_filename = wxString::FromUTF8( schematic_path );
    job.m_force    = force;

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // ProcessJob blocks on the main thread; release the GIL so any nested
        // Python callbacks (none today, but future bindings might) can
        // re-acquire.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, &job, &reporter );
    }

    py::list output_paths;
    for( const JOB_OUTPUT& out : job.GetOutputs() )
        output_paths.append( std::string( out.m_outputPath.ToUTF8() ) );

    // The upgrade rewrites the input file in place.  If the JOB didn't
    // record an output, surface the schematic_path so callers can see what
    // was touched.
    if( exitCode == 0 && py::len( output_paths ) == 0 )
        output_paths.append( schematic_path );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_sch_upgrade, m )
{
    m.doc() = "KliCAD schematic file-format upgrade binding (calls "
              "JOB_SCH_UPGRADE under the hood). Returns a structured dict; "
              "never streams. Requires KiCad's GUI to be running (needs a "
              "live KIWAY). WARNING: upgrades the .kicad_sch file IN PLACE — "
              "make a backup before calling if you need to preserve the "
              "original.";

    m.def( "run", &run_sch_upgrade,
           py::arg( "schematic_path" ),
           py::arg( "force" ) = false,
           R"DOC(Upgrade the given .kicad_sch file's format to the current version, in place.

Mirrors `kicad-cli sch upgrade`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly
  - exit_code     int         raw exit code from JOB_SCH_UPGRADE dispatch
  - messages      str         reporter output (status, info, warnings)
  - output_paths  list[str]   files written by the job (from JOB::GetOutputs());
                              falls back to [schematic_path] on clean exit if
                              the job didn't record an output, since the
                              upgrade rewrites the input file in place.

Kwargs:
  force           Force a resave even if the schematic is already at the current
                  format version (mirrors --force on the CLI). Default False:
                  a no-op upgrade just returns ok without touching the file.

WARNING: this operation rewrites schematic_path in place. Copy the file first
if you need a backup of the pre-upgrade content.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
