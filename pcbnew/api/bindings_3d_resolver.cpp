/*
 * KliCAD subsystem binding: 3D model path resolver (FILENAME_RESOLVER).
 *
 * Exposes the search-path management and alias resolution side of KiCad's 3D
 * model lookup pipeline to scripts.  Useful for headless / CLI flows that need
 * to discover where a `${KICAD10_3DMODEL_DIR}/...` reference actually lives,
 * inject extra search paths for a one-off run, or sanity-check that the
 * configured aliases point at real directories.
 *
 * Module: klicad_native_3d_resolver
 *
 *   list_search_paths()                    -> [ {alias, path, description, can_modify}, ... ]
 *   add_search_path(alias, path, desc='')  -> {ok, alias, path}
 *   remove_search_path(alias)              -> {ok, removed, alias}
 *   resolve(path_with_alias)               -> {resolved_path, exists, ok}
 *   expand_env_vars(path)                  -> str
 *
 * Coverage / limitations:
 *   - FILENAME_RESOLVER is reachable from libkicommon (it lives in
 *     common/filename_resolver.cpp), so search-path management and path
 *     resolution are fully wired here.
 *   - S3D_CACHE and the MODEL_SUBSTITUTION::STEP_CATALOG live in the
 *     3d-viewer/ library which is NOT linkable into libkicommon.  As a
 *     result, .wrl/.wrz -> STEP substitution and on-disk scenegraph cache
 *     loading are out of scope for this binding.  Callers that need those
 *     should reach into 3d-viewer via a Pattern B (kiface-resident) binding
 *     after the relevant editor has been spawned.
 *   - The resolver instance owned by this module is process-local and is
 *     re-bound to the active PROJECT/PGM_BASE on every call so changes to
 *     KiCad's loaded project are reflected immediately.  It does NOT share
 *     state with the live S3D_CACHE's resolver in the 3D viewer — paths
 *     added here will not appear in the GUI's 3D model browser unless the
 *     user also persists them through the Preferences dialog.
 *
 * No GUI interaction is required (no KIWAY walk) — FILENAME_RESOLVER works
 * off PGM_BASE + the active PROJECT, both reachable through PgmOrNull() and
 * SETTINGS_MANAGER::Prj().
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <pgm_base.h>
#include <project.h>
#include <settings/settings_manager.h>

#include <common.h>
#include <embedded_files.h>
#include <filename_resolver.h>

#include <wx/filename.h>
#include <wx/string.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Helpers unique to this TU (3d_resolver) — keep names distinct from other bindings_*.

static std::mutex                            g_3d_resolver_mutex;
static std::unique_ptr<FILENAME_RESOLVER>    g_3d_resolver;


PGM_BASE* require_pgm_for_3d()
{
    PGM_BASE* pgm = PgmOrNull();
    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised" );
    return pgm;
}


/**
 * Return (and lazily construct) the module-local FILENAME_RESOLVER.
 *
 * Re-binds PGM_BASE and the active PROJECT on every call so:
 *   - Newly-defined env vars (e.g. user just edited Preferences > Paths) are
 *     picked up.
 *   - The ${KIPRJMOD} alias tracks whichever project is currently loaded in
 *     KiCad's SETTINGS_MANAGER.
 */
FILENAME_RESOLVER* resolver_for_3d()
{
    std::lock_guard<std::mutex> lock( g_3d_resolver_mutex );

    PGM_BASE*         pgm = require_pgm_for_3d();
    SETTINGS_MANAGER& mgr = pgm->GetSettingsManager();

    if( !g_3d_resolver )
        g_3d_resolver = std::make_unique<FILENAME_RESOLVER>();

    g_3d_resolver->SetProgramBase( pgm );

    // SETTINGS_MANAGER::Prj() always returns a valid PROJECT&; if no project
    // file is loaded it returns the implicit empty project.  Either way it is
    // safe to feed to SetProject(); failure (e.g. project dir does not exist)
    // is benign here — we still want a usable resolver for env-var aliases.
    PROJECT& prj = mgr.Prj();
    g_3d_resolver->SetProject( &prj, /*flgChanged=*/nullptr );

    return g_3d_resolver.get();
}


/// Alias entrypoint with the name the task spec calls out.
FILENAME_RESOLVER* find_filename_resolver()
{
    return resolver_for_3d();
}


/**
 * True when the entry was injected via FILENAME_RESOLVER::createPathList()
 * (env-var alias) or SetProject() (${KIPRJMOD}) rather than a user
 * UpdatePathList()/addPath() call.  These can't be safely removed because
 * createPathList() will simply re-add them on the next resolve.
 */
bool is_builtin_alias( const wxString& aAlias )
{
    if( aAlias == wxS( "${KIPRJMOD}" ) || aAlias == wxS( "$(KIPRJMOD)" ) )
        return true;

    // env-var-form aliases are auto-generated for every KiCad path env var;
    // remove() against them would only fight createPathList().
    if( aAlias.StartsWith( wxS( "${" ) ) || aAlias.StartsWith( wxS( "$(" ) ) )
        return true;

    // Bare env-var names (createPathList() pushes both bare and ${...} forms).
    PGM_BASE* pgm = PgmOrNull();
    if( pgm )
    {
        std::list<wxString> kicadPaths;
        if( g_3d_resolver && g_3d_resolver->GetKicadPaths( kicadPaths ) )
        {
            for( const wxString& p : kicadPaths )
                if( p == aAlias )
                    return true;
        }
    }

    return false;
}


py::list resolver_list_search_paths()
{
    FILENAME_RESOLVER* res = resolver_for_3d();

    const std::list<SEARCH_PATH>* paths = res->GetPaths();

    py::list out;
    if( !paths )
        return out;

    for( const SEARCH_PATH& sp : *paths )
    {
        py::dict entry;
        entry["alias"]       = sp.m_Alias.ToStdString();
        entry["path"]        = sp.m_Pathexp.ToStdString();
        entry["path_var"]    = sp.m_Pathvar.ToStdString();
        entry["description"] = sp.m_Description.ToStdString();
        entry["can_modify"]  = !is_builtin_alias( sp.m_Alias );
        out.append( entry );
    }

    return out;
}


py::dict resolver_add_search_path( const std::string& aAlias, const std::string& aPath,
                                   const std::string& aDescription )
{
    if( aAlias.empty() )
        throw std::invalid_argument( "add_search_path() requires a non-empty alias" );
    if( aPath.empty() )
        throw std::invalid_argument( "add_search_path() requires a non-empty path" );

    FILENAME_RESOLVER* res = resolver_for_3d();

    // Copy the existing user-defined (non-env-var, non-KIPRJMOD) entries plus
    // the new one, then ask the resolver to swap them in.  UpdatePathList()
    // preserves the built-in env-var entries (it only pops trailing user
    // entries before re-adding from the supplied vector).
    std::vector<SEARCH_PATH> newList;

    if( const std::list<SEARCH_PATH>* existing = res->GetPaths() )
    {
        for( const SEARCH_PATH& sp : *existing )
        {
            if( is_builtin_alias( sp.m_Alias ) )
                continue;
            if( sp.m_Alias == wxString::FromUTF8( aAlias ) )
                throw std::runtime_error( "alias '" + aAlias + "' already exists" );

            newList.push_back( sp );
        }
    }

    SEARCH_PATH np;
    np.m_Alias       = wxString::FromUTF8( aAlias );
    np.m_Pathvar     = wxString::FromUTF8( aPath );
    np.m_Description = wxString::FromUTF8( aDescription );
    newList.push_back( np );

    bool ok = res->UpdatePathList( newList );

    py::dict result;
    result["ok"]    = ok;
    result["alias"] = aAlias;
    result["path"]  = aPath;
    return result;
}


py::dict resolver_remove_search_path( const std::string& aAlias )
{
    if( aAlias.empty() )
        throw std::invalid_argument( "remove_search_path() requires a non-empty alias" );

    wxString wxAlias = wxString::FromUTF8( aAlias );

    if( is_builtin_alias( wxAlias ) )
        throw std::runtime_error(
                "alias '" + aAlias + "' is built-in (env-var or ${KIPRJMOD}) and cannot be "
                "removed; manage it via the corresponding environment variable instead" );

    FILENAME_RESOLVER* res = resolver_for_3d();

    bool                     removed = false;
    std::vector<SEARCH_PATH> newList;

    if( const std::list<SEARCH_PATH>* existing = res->GetPaths() )
    {
        for( const SEARCH_PATH& sp : *existing )
        {
            if( is_builtin_alias( sp.m_Alias ) )
                continue;

            if( sp.m_Alias == wxAlias )
            {
                removed = true;
                continue;
            }
            newList.push_back( sp );
        }
    }

    bool ok = false;
    if( removed )
        ok = res->UpdatePathList( newList );

    py::dict result;
    result["ok"]      = ok;
    result["removed"] = removed;
    result["alias"]   = aAlias;
    return result;
}


py::dict resolver_resolve( const std::string& aPathWithAlias )
{
    FILENAME_RESOLVER* res = resolver_for_3d();

    wxString resolved = res->ResolvePath( wxString::FromUTF8( aPathWithAlias ),
                                          wxEmptyString,
                                          std::vector<const EMBEDDED_FILES*>{} );

    bool exists = false;
    if( !resolved.empty() )
        exists = wxFileName::FileExists( resolved ) || wxFileName::DirExists( resolved );

    py::dict result;
    result["resolved_path"] = resolved.ToStdString();
    result["exists"]        = exists;
    result["ok"]            = !resolved.empty();
    return result;
}


py::str resolver_expand_env_vars( const std::string& aPath )
{
    // ExpandEnvVarSubstitutions takes an active PROJECT for ${KIPRJMOD} —
    // grab whatever the SETTINGS_MANAGER thinks is current.
    PGM_BASE*         pgm = require_pgm_for_3d();
    SETTINGS_MANAGER& mgr = pgm->GetSettingsManager();
    PROJECT&          prj = mgr.Prj();

    wxString expanded = ExpandEnvVarSubstitutions( wxString::FromUTF8( aPath ), &prj );
    return py::str( expanded.ToStdString() );
}

} // anon


// Pattern B: kiface-resident binding.  FILENAME_RESOLVER lives in static
// libcommon (NOT the shared libkicommon), so we must register at kiface
// load time after libcommon is linked into the kiface module.  See
// pcbnew/api/klicad_kiface_register.cpp.
void klicad_register_3d_resolver_bindings( py::module_& m )
{
    m.doc() = "KliCAD 3D model path resolver binding — wraps FILENAME_RESOLVER from "
              "libcommon.  Provides search-path management (list/add/remove), "
              "${ENV_VAR}-aware alias resolution, and raw env-var expansion.\n\n"
              "Coverage limitation: S3D_CACHE and the .wrl -> STEP substitution catalog "
              "live in the 3d-viewer library which is not linkable into libkicommon, so "
              "scenegraph cache loading and model substitution are NOT exposed here — "
              "only search-path management and alias resolution are.  The resolver "
              "instance is process-local and does not share state with the GUI's live "
              "S3D_CACHE; changes here will not appear in the 3D model browser unless "
              "persisted through Preferences.";

    m.def( "list_search_paths", &resolver_list_search_paths,
           R"DOC(Return the resolver's current search-path table.

Each entry is a dict:
    {
        alias       : str   # e.g. "${KICAD10_3DMODEL_DIR}" or a user alias
        path        : str   # expanded absolute path (empty if env var unset)
        path_var    : str   # unexpanded form (with ${VAR} intact)
        description : str   # human description (only set on user entries)
        can_modify  : bool  # False for built-in env-var and ${KIPRJMOD} rows
    }

The built-in rows (${KIPRJMOD}, KICAD*_3DMODEL_DIR, KISYS3DMOD, etc) are
auto-generated and cannot be removed via this API — manage them through the
corresponding environment variables instead.
)DOC" );

    m.def( "add_search_path", &resolver_add_search_path,
           py::arg( "alias" ),
           py::arg( "path" ),
           py::arg( "description" ) = std::string(),
           R"DOC(Add a user-defined search-path alias to the resolver.

Example:
    add_search_path("MY_PARTS", "/Users/me/electronics/3dparts",
                    "Personal STEP library")

Subsequent calls to resolve("${MY_PARTS}/some_file.step") will then find the
file under that directory.

Raises:
    ValueError if alias or path is empty.
    RuntimeError if the alias already exists.

Returns:
    {ok: bool, alias: str, path: str}

Note: the alias is process-local for this resolver instance.  The GUI's
S3D_CACHE has its own resolver and will not see it.
)DOC" );

    m.def( "remove_search_path", &resolver_remove_search_path,
           py::arg( "alias" ),
           R"DOC(Remove a user-defined search-path alias.

Raises:
    ValueError on empty alias.
    RuntimeError if the alias is a built-in (env-var alias or ${KIPRJMOD}).

Returns:
    {ok: bool, removed: bool, alias: str}
)DOC" );

    m.def( "resolve", &resolver_resolve,
           py::arg( "path_with_alias" ),
           R"DOC(Resolve a 3D-model-style path through the search-path table.

Accepts aliased paths like:
    "${KICAD10_3DMODEL_DIR}/Capacitor_SMD.3dshapes/C_0603.step"
    "${KIPRJMOD}/local_models/U1.wrl"
    "MY_PARTS:Resistor_SMD/R_0402.step"   (legacy alias:relpath form)
    "/abs/path/to/model.step"             (already absolute — passed through)

Returns:
    {
        resolved_path : str   # absolute filesystem path, or "" on failure
        exists        : bool  # true iff resolved_path names a real file/dir
        ok            : bool  # true iff resolved_path is non-empty
    }

Note: .wrl / .wrz to STEP fallback substitution is NOT performed (that
logic lives in S3D_CACHE / MODEL_SUBSTITUTION inside 3d-viewer/, which is
not reachable from libkicommon).
)DOC" );

    m.def( "expand_env_vars", &resolver_expand_env_vars,
           py::arg( "path" ),
           R"DOC(Expand ${VAR} / $(VAR) tokens against the active KiCad environment.

Uses the same expansion routine as the rest of KiCad (ExpandEnvVarSubstitutions)
so KiCad-specific aliases such as ${KIPRJMOD} (project dir) and
${KICAD*_3DMODEL_DIR} are honoured.

Unresolved variables are left in the string as-is (this matches KiCad's
behaviour throughout the codebase).
)DOC" );
}
