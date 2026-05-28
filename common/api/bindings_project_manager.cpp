/*
 * KliCAD subsystem binding: project manager.
 *
 * Exposes KiCad's PROJECT / SETTINGS_MANAGER project lifecycle (load, unload,
 * save, save-as, save-copy) plus the PROJECT_ARCHIVER (zip/unzip a project
 * directory).  All work routes through `Pgm().GetSettingsManager()` --- both
 * SETTINGS_MANAGER and PROJECT live inside libkicommon, so this is Pattern A
 * (`PYBIND11_EMBEDDED_MODULE`); no kiface symbols required.
 *
 * Module: klicad_native_project_manager
 *
 *   get_current_project()           -> dict | None
 *   list_open_projects()            -> list[dict]
 *   load_project(path, set_active)  -> dict
 *   unload_project(path='')         -> dict
 *   save_project(path='')           -> dict
 *   save_project_as(new_path, src='') -> dict
 *   save_project_copy(new_path)     -> dict
 *   archive(archive_path, src_dir='') -> dict
 *   unarchive(archive_path, dest_dir) -> dict
 *   is_project_open()               -> bool
 *   is_project_open_not_dummy()     -> bool
 *
 * All structured returns use {ok: bool, ...}.  Errors raise RuntimeError.
 *
 * Limitations:
 *   - SETTINGS_MANAGER::LoadProject() in GUI mode unloads the previously-active
 *     project before loading the new one (see settings_manager.cpp).  Multiple
 *     concurrent projects aren't fully supported by KiCad yet; list_open_projects()
 *     usually returns at most one entry.
 *   - SaveProjectAs / SaveProjectCopy return void in SETTINGS_MANAGER (no failure
 *     channel).  We optimistically report ok=True; the underlying writer logs
 *     wx errors but doesn't surface them here.
 *   - Archive() / Unarchive() messages are captured via WX_STRING_REPORTER and
 *     returned in `messages`; the per-file list is parsed best-effort from the
 *     reporter output (one path per line).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <kiway.h>
#include <kiway_holder.h>
#include <mail_type.h>
#include <pgm_base.h>
#include <project.h>
#include <project/project_archiver.h>
#include <reporter.h>
#include <settings/settings_manager.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Helpers unique to this TU --- avoid ODR clashes with other bindings_*.cpp.

PGM_BASE& get_pgm_for_project_manager()
{
    PGM_BASE* pgm = PgmOrNull();
    if( !pgm )
        throw std::runtime_error( "no PGM_BASE --- KiCad isn't initialised" );
    return *pgm;
}


SETTINGS_MANAGER& get_settings_manager_for_project_manager()
{
    return get_pgm_for_project_manager().GetSettingsManager();
}


py::dict project_to_dict( const PROJECT& aProject )
{
    py::dict d;
    d["full_name"]   = aProject.GetProjectFullName().ToStdString();
    d["path"]        = aProject.GetProjectPath().ToStdString();
    d["name"]        = aProject.GetProjectName().ToStdString();
    d["is_readonly"] = aProject.IsReadOnly();
    d["is_null"]     = aProject.IsNullProject();
    return d;
}


py::object pm_get_current_project()
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    if( !mgr.IsProjectOpen() )
        return py::none();

    return project_to_dict( mgr.Prj() );
}


py::list pm_list_open_projects()
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    py::list out;
    for( const wxString& fullPath : mgr.GetOpenProjects() )
    {
        if( PROJECT* p = mgr.GetProject( fullPath ) )
            out.append( project_to_dict( *p ) );
        else
        {
            // Defensive: GetOpenProjects() said it's open, GetProject() disagreed.
            // Still report the path so caller has something to act on.
            py::dict d;
            d["full_name"]   = fullPath.ToStdString();
            d["path"]        = std::string();
            d["name"]        = std::string();
            d["is_readonly"] = false;
            d["is_null"]     = false;
            out.append( d );
        }
    }
    return out;
}


// Locate a live KIWAY by walking wxTopLevelWindows.  Returns nullptr in headless
// (kicad-cli api-server) mode where no GUI frame is up --- that's fine, because
// without frames there's no SCHEMATIC/BOARD holding a PROJECT* to dangle.
// Mirrors the find_live_kiway() helpers in bindings_drc.cpp / bindings_erc.cpp.
KIWAY* find_live_kiway_for_project_manager()
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


// Tell any live SCH_EDIT_FRAME / PCB_EDIT_FRAME to disconnect their SCHEMATIC /
// BOARD from the currently-active PROJECT, before SETTINGS_MANAGER unloads it.
// Mirrors the wx file-open path (eeschema/files-io.cpp:199, pcbnew/files.cpp:602)
// which the API path used to skip --- that omission left SCHEMATIC::m_project
// pointing at a freed PROJECT and crashed the next SetProject() call.
void disconnect_frames_from_active_project()
{
    KIWAY* kiway = find_live_kiway_for_project_manager();
    if( !kiway )
        return;

    std::string payload;

    // doCreate=false on the receiver side --- the mail handlers no-op when the
    // frame doesn't exist, but we don't even want to instantiate one just to
    // tell it to disconnect.  ExpressMail itself routes by FRAME_T and silently
    // drops if the player isn't up.
    kiway->ExpressMail( FRAME_SCH, MAIL_PROJECT_TEARDOWN, payload );
    kiway->ExpressMail( FRAME_PCB_EDITOR, MAIL_PROJECT_TEARDOWN, payload );
}


py::dict pm_load_project( const std::string& aPath, bool aSetActive )
{
    if( aPath.empty() )
        throw std::invalid_argument( "load_project() requires a non-empty path" );

    SETTINGS_MANAGER& mgr  = get_settings_manager_for_project_manager();
    const wxString    full = wxString::FromUTF8( aPath );

    // SETTINGS_MANAGER::LoadProject() in GUI mode unloads the previously-active
    // project (freeing the PROJECT object) before loading the new one.  The wx
    // file-open paths in eeschema/pcbnew handle this correctly by first calling
    // Schematic().SetProject(nullptr) / BOARD::ClearProject() so the editor's
    // SCHEMATIC/BOARD stops pointing at the about-to-be-freed PROJECT.  The
    // API path used to skip that disconnect, leaving SCHEMATIC::m_project
    // dangling and crashing the next SetProject() call inside the editor.
    // Replay the wx-flow ordering here: send a teardown mail to the live
    // editor frames, then explicitly unload the current project, then load
    // the new one.
    bool ok = false;
    {
        py::gil_scoped_release nogil;

        if( aSetActive && mgr.IsProjectOpen() )
        {
            disconnect_frames_from_active_project();
            mgr.UnloadProject( &mgr.Prj(), /* aSave */ false );
        }

        ok = mgr.LoadProject( full, aSetActive );
    }

    py::dict result;
    result["ok"] = ok;
    if( ok )
    {
        if( PROJECT* p = mgr.GetProject( full ) )
            result["full_name"] = p->GetProjectFullName().ToStdString();
        else
            result["full_name"] = aPath;
    }
    else
    {
        result["full_name"] = aPath;
        result["error"]     = "SETTINGS_MANAGER::LoadProject returned false "
                              "(file missing, parse failure, or already open)";
    }
    return result;
}


py::dict pm_unload_project( const std::string& aPath )
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    PROJECT* target = nullptr;
    if( aPath.empty() )
    {
        if( !mgr.IsProjectOpen() )
            throw std::runtime_error( "unload_project(): no project is currently open" );
        target = &mgr.Prj();
    }
    else
    {
        target = mgr.GetProject( wxString::FromUTF8( aPath ) );
        if( !target )
            throw std::runtime_error( "unload_project(): no loaded project matches '"
                                      + aPath + "'" );
    }

    const std::string unloadedName = target->GetProjectFullName().ToStdString();

    bool ok = false;
    {
        py::gil_scoped_release nogil;
        ok = mgr.UnloadProject( target, /* aSave */ true );
    }

    py::dict result;
    result["ok"]              = ok;
    result["unloaded_full_name"] = unloadedName;
    return result;
}


py::dict pm_save_project( const std::string& aPath )
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    if( aPath.empty() && !mgr.IsProjectOpen() )
        throw std::runtime_error( "save_project(): no project is currently open" );

    const wxString full = aPath.empty() ? wxString() : wxString::FromUTF8( aPath );

    bool ok = false;
    {
        py::gil_scoped_release nogil;
        ok = mgr.SaveProject( full, /* aProject */ nullptr );
    }

    py::dict result;
    result["ok"] = ok;
    return result;
}


py::dict pm_save_project_as( const std::string& aNewPath, const std::string& aSourcePath )
{
    if( aNewPath.empty() )
        throw std::invalid_argument( "save_project_as() requires a non-empty new_path" );

    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    PROJECT* src = nullptr;
    if( aSourcePath.empty() )
    {
        if( !mgr.IsProjectOpen() )
            throw std::runtime_error( "save_project_as(): no active project to save" );
        src = &mgr.Prj();
    }
    else
    {
        src = mgr.GetProject( wxString::FromUTF8( aSourcePath ) );
        if( !src )
            throw std::runtime_error( "save_project_as(): no loaded project matches '"
                                      + aSourcePath + "'" );
    }

    const wxString newFull = wxString::FromUTF8( aNewPath );

    {
        py::gil_scoped_release nogil;
        mgr.SaveProjectAs( newFull, src );
    }

    py::dict result;
    // SaveProjectAs is void; if the project was read-only it silently no-ops.
    // Report ok=true here, but include the readonly flag so the caller can spot it.
    result["ok"]            = !src->IsReadOnly();
    result["new_full_name"] = aNewPath;
    result["was_readonly"]  = src->IsReadOnly();
    return result;
}


py::dict pm_save_project_copy( const std::string& aNewPath )
{
    if( aNewPath.empty() )
        throw std::invalid_argument( "save_project_copy() requires a non-empty new_path" );

    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    if( !mgr.IsProjectOpen() )
        throw std::runtime_error( "save_project_copy(): no active project to copy" );

    PROJECT*       src     = &mgr.Prj();
    const wxString newFull = wxString::FromUTF8( aNewPath );

    {
        py::gil_scoped_release nogil;
        mgr.SaveProjectCopy( newFull, src );
    }

    py::dict result;
    result["ok"]            = true;
    result["new_full_name"] = aNewPath;
    return result;
}


// Split the WX_STRING_REPORTER buffer into one entry per non-empty line.  The
// archiver emits one path per Report() call so this gives us a reasonable
// "files touched" list.
std::vector<std::string> split_reporter_messages( const wxString& aBlob )
{
    std::vector<std::string> out;
    wxString                 line;
    for( wxChar c : aBlob )
    {
        if( c == '\n' || c == '\r' )
        {
            if( !line.IsEmpty() )
                out.push_back( line.ToStdString() );
            line.clear();
        }
        else
        {
            line += c;
        }
    }
    if( !line.IsEmpty() )
        out.push_back( line.ToStdString() );
    return out;
}


py::dict pm_archive( const std::string& aArchivePath, const std::string& aSourceDir )
{
    if( aArchivePath.empty() )
        throw std::invalid_argument( "archive() requires a non-empty archive_path" );

    SETTINGS_MANAGER& mgr = get_settings_manager_for_project_manager();

    wxString srcDir;
    if( aSourceDir.empty() )
    {
        if( !mgr.IsProjectOpenNotDummy() )
            throw std::runtime_error( "archive(): no real project open --- pass source_dir "
                                      "explicitly to archive an arbitrary directory" );
        srcDir = mgr.Prj().GetProjectPath();
    }
    else
    {
        srcDir = wxString::FromUTF8( aSourceDir );
    }

    const wxString destFile = wxString::FromUTF8( aArchivePath );

    WX_STRING_REPORTER reporter;
    bool               ok = false;
    {
        py::gil_scoped_release nogil;
        ok = PROJECT_ARCHIVER::Archive( srcDir, destFile, reporter,
                                        /* aVerbose */ true,
                                        /* aIncludeExtraFiles */ false );
    }

    py::dict result;
    result["ok"]       = ok;
    result["files"]    = split_reporter_messages( reporter.GetMessages() );
    result["messages"] = reporter.GetMessages().ToStdString();
    return result;
}


py::dict pm_unarchive( const std::string& aArchivePath, const std::string& aDestDir )
{
    if( aArchivePath.empty() )
        throw std::invalid_argument( "unarchive() requires a non-empty archive_path" );
    if( aDestDir.empty() )
        throw std::invalid_argument( "unarchive() requires a non-empty dest_dir" );

    const wxString srcFile = wxString::FromUTF8( aArchivePath );
    const wxString destDir = wxString::FromUTF8( aDestDir );

    WX_STRING_REPORTER reporter;
    bool               ok = false;
    {
        py::gil_scoped_release nogil;
        ok = PROJECT_ARCHIVER::Unarchive( srcFile, destDir, reporter );
    }

    py::dict result;
    result["ok"]       = ok;
    result["files"]    = split_reporter_messages( reporter.GetMessages() );
    result["messages"] = reporter.GetMessages().ToStdString();
    return result;
}


bool pm_is_project_open()
{
    return get_settings_manager_for_project_manager().IsProjectOpen();
}


bool pm_is_project_open_not_dummy()
{
    return get_settings_manager_for_project_manager().IsProjectOpenNotDummy();
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_project_manager, m )
{
    m.doc() = "KliCAD project manager binding --- drive SETTINGS_MANAGER's "
              "project lifecycle (load/unload/save/save-as/save-copy) and "
              "PROJECT_ARCHIVER (zip/unzip a project directory).  All functions "
              "return structured dicts; failures raise RuntimeError.";

    m.def( "get_current_project", &pm_get_current_project,
           R"DOC(Return the currently-active project as a dict, or None.

Dict keys: full_name, path, name, is_readonly, is_null.
Returns None when no project is open (SETTINGS_MANAGER::IsProjectOpen() is false).
)DOC" );

    m.def( "list_open_projects", &pm_list_open_projects,
           R"DOC(Return all projects SETTINGS_MANAGER currently tracks.

In GUI mode this is normally a list of one (KiCad is not yet MDI-capable).
Each entry has the same shape as get_current_project().
)DOC" );

    m.def( "load_project", &pm_load_project,
           py::arg( "path" ),
           py::arg( "set_active" ) = true,
           R"DOC(Load a .kicad_pro file via SETTINGS_MANAGER::LoadProject.

Returns: {ok, full_name, error?}.  When set_active=True the loaded project
becomes Prj(); in GUI mode this also unloads the previously-active project.
)DOC" );

    m.def( "unload_project", &pm_unload_project,
           py::arg( "path" ) = std::string(),
           R"DOC(Save and unload a project.

Empty path = unload Prj() (the active project).  Otherwise the path must match
a currently-loaded project's full filename.  Returns {ok, unloaded_full_name}.
)DOC" );

    m.def( "save_project", &pm_save_project,
           py::arg( "path" ) = std::string(),
           R"DOC(Save a loaded project via SETTINGS_MANAGER::SaveProject.

Empty path = save the active project.  Returns {ok}.
)DOC" );

    m.def( "save_project_as", &pm_save_project_as,
           py::arg( "new_path" ),
           py::arg( "source_path" ) = std::string(),
           R"DOC(Rename + save the active (or named) project to new_path.

This changes the project's identity --- subsequent Prj().GetProjectFullName()
returns new_path.  Wraps SETTINGS_MANAGER::SaveProjectAs (which is a no-op on
read-only projects --- the returned dict surfaces that in `was_readonly`).
)DOC" );

    m.def( "save_project_copy", &pm_save_project_copy,
           py::arg( "new_path" ),
           R"DOC(Save a copy of the active project to new_path without changing identity.

Wraps SETTINGS_MANAGER::SaveProjectCopy.  Will save even if the source project
is marked read-only (per KiCad's behaviour).  Returns {ok, new_full_name}.
)DOC" );

    m.def( "archive", &pm_archive,
           py::arg( "archive_path" ),
           py::arg( "source_dir" ) = std::string(),
           R"DOC(Zip a project directory via PROJECT_ARCHIVER::Archive.

Empty source_dir = archive Prj().GetProjectPath() (requires a non-dummy active
project).  Returns {ok, files: [...], messages: str} --- `files` is parsed from
the reporter (one path per line); `messages` is the raw reporter buffer.
)DOC" );

    m.def( "unarchive", &pm_unarchive,
           py::arg( "archive_path" ),
           py::arg( "dest_dir" ),
           R"DOC(Extract a project zip via PROJECT_ARCHIVER::Unarchive.

WARNING: this overwrites files in dest_dir.  Returns {ok, files, messages}.
Caller is responsible for reloading state (e.g. calling load_project on the
new .kicad_pro) after unarchiving over a live project.
)DOC" );

    m.def( "is_project_open", &pm_is_project_open,
           R"DOC(True if SETTINGS_MANAGER::IsProjectOpen() --- i.e. Prj() is safe to call.)DOC" );

    m.def( "is_project_open_not_dummy", &pm_is_project_open_not_dummy,
           R"DOC(True if a real (non-dummy / non-null) project is currently active.)DOC" );
}
