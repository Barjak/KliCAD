/*
 * KliCAD subsystem binding: symbol-library file-format upgrade.
 *
 * Exposes the symbol-library upgrade-to-current-format operation as
 *   klicad_native_sym_upgrade.run(library_path, ...) -> dict
 *
 * Mirrors `kicad-cli sym upgrade` (see kicad/cli/command_sym_upgrade.cpp) and
 * the schematic-side symmetric binding in bindings_sch_upgrade.cpp.  Wraps
 * JOB_SYM_UPGRADE and dispatches via the live KIWAY (found by walking
 * wxTopLevelWindows) into the eeschema kiface (FACE_SCH), which owns the
 * EESCHEMA_JOBS_HANDLER::JobSymUpgrade entry point.
 *
 * WARNING: this UPGRADES THE .kicad_sym FILE IN PLACE when no
 * output_library_path is supplied.  EESCHEMA_JOBS_HANDLER::JobSymUpgrade
 * loads, resaves (if forced or out-of-date), and rewrites the same path.
 * The optional output_library_path kwarg writes to a different file
 * instead, mirroring the CLI's positional <output> argument; for legacy /
 * non-KiCad libraries the handler REQUIRES output_library_path to be set
 * (it refuses to convert in place).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job.h>
#include <jobs/job_sym_upgrade.h>
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

// Walk live wxTopLevelWindows for any frame that is a KIWAY_HOLDER with a
// live KIWAY, and return that KIWAY*.  Per-subsystem name to avoid ODR
// clashes with the matching helpers in the sibling bindings_*.cpp TUs:
// bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_sch_upgrade.cpp
// (find_live_kiway_for_sch_upgrade), bindings_pcb_upgrade.cpp
// (find_live_kiway_for_pcb_upgrade), bindings_sym_export_svg.cpp
// (find_live_kiway_for_sym_export_svg), bindings_fp_export_svg.cpp
// (find_live_kiway_for_fp_export_svg), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_sch_bom.cpp
// (find_live_kiway_for_export_sch_bom), bindings_export_sch_netlist.cpp
// (find_live_kiway_for_export_sch_netlist), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), and bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_sym_upgrade()
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


py::object run_sym_upgrade( const std::string& library_path,
                            const std::string& output_library_path,
                            bool               force )
{
    KIWAY* kiway = find_live_kiway_for_sym_upgrade();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(symbol-library upgrade needs the eeschema kiface "
                                  "to be loaded)" );

    // EESCHEMA_JOBS_HANDLER expects the schematic editor frame to be live
    // when running in GUI mode (the eeschema kiface owns the jobs handler).
    // Player(..., true) creates the frame if missing; no-op if already up.
    kiway->Player( FRAME_SCH, true );

    JOB_SYM_UPGRADE job;
    job.m_libraryPath       = wxString::FromUTF8( library_path );
    job.m_outputLibraryPath = wxString::FromUTF8( output_library_path );
    job.m_force             = force;

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

    // EESCHEMA_JOBS_HANDLER::JobSymUpgrade does not populate JOB::m_outputs.
    // On clean exit, surface the path the handler actually wrote to so the
    // caller can see what was touched: output_library_path if set, else the
    // input library_path (which was rewritten in place).
    if( exitCode == 0 && py::len( output_paths ) == 0 )
    {
        if( !output_library_path.empty() )
            output_paths.append( output_library_path );
        else
            output_paths.append( library_path );
    }

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = output_paths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_sym_upgrade, m )
{
    m.doc() = "KliCAD symbol-library file-format upgrade binding (calls "
              "JOB_SYM_UPGRADE under the hood via the eeschema kiface). "
              "Returns a structured dict; never streams. Requires KiCad's "
              "GUI to be running (needs a live KIWAY). WARNING: when "
              "output_library_path is empty (default), this rewrites the "
              ".kicad_sym file IN PLACE — make a backup before calling if "
              "you need to preserve the original.";

    m.def( "run", &run_sym_upgrade,
           py::arg( "library_path" ),
           py::arg( "output_library_path" ) = "",
           py::arg( "force" )               = false,
           R"DOC(Upgrade the given .kicad_sym symbol library's format to the current version.

Mirrors `kicad-cli sym upgrade`. Returns a dict with keys:
  - ok            bool        True iff job exited cleanly (exit_code == 0)
  - exit_code     int         raw exit code from JOB_SYM_UPGRADE dispatch
  - messages      str         reporter output (status, info, warnings; e.g.
                              'Saving symbol library in updated format' on a
                              real upgrade, or 'Symbol library was not
                              updated' when already current)
  - output_paths  list[str]   files written by the job (from JOB::GetOutputs()).
                              The eeschema handler does not populate this, so
                              on clean exit the list falls back to
                              [output_library_path] if supplied, else
                              [library_path] (which was rewritten in place).

Kwargs:
  output_library_path  Optional separate destination path. When empty (default)
                       the upgrade rewrites library_path IN PLACE. Must be set
                       for legacy / non-KiCad symbol libraries — the handler
                       refuses to convert those in place. If the destination
                       file already exists the job aborts with
                       ERR_INVALID_OUTPUT_CONFLICT.
  force                Force a resave even if the library is already at the
                       current format version (mirrors --force on the CLI).
                       Default False: a no-op upgrade returns ok without
                       touching the file and the reporter logs 'Symbol library
                       was not updated'.

WARNING: with output_library_path empty (default), this operation rewrites
library_path in place. Copy the file first if you need a backup of the
pre-upgrade content.

Raises RuntimeError if no live KIWAY is available (KiCad GUI not running).
)DOC" );
}
