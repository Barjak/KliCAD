/*
 * KliCAD subsystem binding: library tables (symbol + footprint).
 *
 * Exposes the global and project-scoped LIBRARY_TABLEs that KiCad uses to
 * locate symbol and footprint libraries.  Lets scripts list, add, and remove
 * entries; resolve env-var-substituted URIs; and persist changes via the
 * LIBRARY_MANAGER / underlying LIBRARY_TABLE::Save() pipeline.
 *
 * Module: klicad_native_library_tables
 *
 *   list_symbol_libs(scope='all')      -> [ {name, uri, description, type,
 *                                            options, enabled, visible, scope}, ... ]
 *   list_footprint_libs(scope='all')   -> same shape
 *
 *   add_symbol_lib(name, uri, type, description='', options='',
 *                  enabled=True, visible=True, scope='global')   -> {ok, name}
 *   add_footprint_lib(...)             -> same shape
 *
 *   remove_symbol_lib(name, scope='global')      -> {ok, name, removed}
 *   remove_footprint_lib(name, scope='global')   -> {ok, name, removed}
 *
 *   resolve_path(path_with_vars)       -> str   (env vars + text vars expanded)
 *   list_env_vars()                    -> {name: value, ...}
 *
 *   save()                             -> {ok, files: [path, ...]}
 *
 * Notes:
 *   - Global rows are always accessible (KiCad creates global tables at
 *     startup).  Project-scoped operations require an active project; if none
 *     is loaded the call raises RuntimeError.
 *   - Pattern A (libkicommon-resident PYBIND11_EMBEDDED_MODULE) — all the
 *     types we need live in libkicommon (LIBRARY_TABLE, LIBRARY_MANAGER,
 *     PROJECT, SETTINGS_MANAGER).  No kiface symbols are touched, so the
 *     module is available from process start.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <common.h>             // ExpandEnvVarSubstitutions, ExpandTextVars
#include <env_vars.h>           // ENV_VAR::GetPredefinedEnvVars
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/common_settings.h>
#include <settings/environment.h>
#include <settings/settings_manager.h>

#include <wx/string.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// -- Helpers (unique names so they don't clash with other bindings_*.cpp) ---

PGM_BASE& get_pgm_for_library_tables()
{
    PGM_BASE* pgm = PgmOrNull();

    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised" );

    return *pgm;
}


LIBRARY_MANAGER& get_library_manager_for_library_tables()
{
    return get_pgm_for_library_tables().GetLibraryManager();
}


SETTINGS_MANAGER& get_settings_manager_for_library_tables()
{
    return get_pgm_for_library_tables().GetSettingsManager();
}


// Returns the live PROJECT* (or nullptr) — never throws.  Use when the caller
// might legitimately want to operate without a project (global-scope ops).
PROJECT* find_live_kiway_for_library_tables()
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_library_tables();

    if( !mgr.IsProjectOpen() )
        return nullptr;

    return &mgr.Prj();
}


// Returns the live PROJECT*; throws RuntimeError if no project is loaded.
// Use for project-scope read/write operations that simply have nothing to
// touch without a project.
PROJECT& require_project_for_library_tables()
{
    PROJECT* project = find_live_kiway_for_library_tables();

    if( !project || project->IsNullProject() )
        throw std::runtime_error( "no active project — open a project before "
                                  "using scope='project' library-table ops" );

    return *project;
}


LIBRARY_TABLE_SCOPE parse_scope_for_library_tables( const std::string& aScope,
                                                   bool aAllowAll )
{
    if( aScope == "global" )
        return LIBRARY_TABLE_SCOPE::GLOBAL;

    if( aScope == "project" )
        return LIBRARY_TABLE_SCOPE::PROJECT;

    if( aAllowAll && ( aScope == "all" || aScope == "both" ) )
        return LIBRARY_TABLE_SCOPE::BOTH;

    if( aAllowAll )
        throw std::invalid_argument( "scope must be one of 'all', 'global', "
                                     "'project' (got '" + aScope + "')" );

    throw std::invalid_argument( "scope must be one of 'global', 'project' "
                                 "(got '" + aScope + "')" );
}


const char* scope_to_string_for_library_tables( LIBRARY_TABLE_SCOPE aScope )
{
    switch( aScope )
    {
    case LIBRARY_TABLE_SCOPE::GLOBAL:        return "global";
    case LIBRARY_TABLE_SCOPE::PROJECT:       return "project";
    case LIBRARY_TABLE_SCOPE::BOTH:          return "both";
    case LIBRARY_TABLE_SCOPE::UNINITIALIZED: return "uninitialized";
    }
    return "unknown";
}


py::dict row_to_dict_for_library_tables( const LIBRARY_TABLE_ROW& aRow,
                                         LIBRARY_TABLE_SCOPE      aTableScope )
{
    py::dict d;
    d["name"]        = aRow.Nickname().ToStdString();
    d["uri"]         = aRow.URI().ToStdString();
    d["type"]        = aRow.Type().ToStdString();
    d["description"] = aRow.Description().ToStdString();
    d["options"]     = aRow.Options().ToStdString();
    d["enabled"]     = !aRow.Disabled();
    d["visible"]     = !aRow.Hidden();

    // Prefer the row's own scope if set; fall back to the containing table's
    // scope so callers always get a meaningful 'global'/'project' string.
    LIBRARY_TABLE_SCOPE scope =
            aRow.Scope() == LIBRARY_TABLE_SCOPE::UNINITIALIZED ? aTableScope
                                                               : aRow.Scope();
    d["scope"] = scope_to_string_for_library_tables( scope );
    return d;
}


// Returns the table for (aType, aScope) or throws if it can't be obtained.
// For project scope, requires a live project.
LIBRARY_TABLE* get_table_for_library_tables( LIBRARY_TABLE_TYPE  aType,
                                             LIBRARY_TABLE_SCOPE aScope )
{
    if( aScope == LIBRARY_TABLE_SCOPE::PROJECT )
        require_project_for_library_tables();  // throws if no project

    LIBRARY_MANAGER& mgr = get_library_manager_for_library_tables();

    std::optional<LIBRARY_TABLE*> tbl = mgr.Table( aType, aScope );

    if( !tbl || !*tbl )
        throw std::runtime_error( std::string( "library table not available for " )
                                  + scope_to_string_for_library_tables( aScope ) );

    return *tbl;
}


py::list list_libs_impl( LIBRARY_TABLE_TYPE aType, const std::string& aScope )
{
    LIBRARY_TABLE_SCOPE scope = parse_scope_for_library_tables( aScope, true );

    LIBRARY_MANAGER& mgr = get_library_manager_for_library_tables();

    // For BOTH, we walk via the manager so we cover both tables uniformly.
    // For a single scope, walk just that table.
    py::list out;

    auto append_rows = [&]( LIBRARY_TABLE_SCOPE aWalkScope )
    {
        // Project-scope walk is a no-op when no project is loaded.
        if( aWalkScope == LIBRARY_TABLE_SCOPE::PROJECT
            && !find_live_kiway_for_library_tables() )
        {
            return;
        }

        std::optional<LIBRARY_TABLE*> tbl = mgr.Table( aType, aWalkScope );

        if( !tbl || !*tbl )
            return;

        for( const LIBRARY_TABLE_ROW& row : ( *tbl )->Rows() )
            out.append( row_to_dict_for_library_tables( row, aWalkScope ) );
    };

    if( scope == LIBRARY_TABLE_SCOPE::BOTH )
    {
        append_rows( LIBRARY_TABLE_SCOPE::GLOBAL );
        append_rows( LIBRARY_TABLE_SCOPE::PROJECT );
    }
    else
    {
        // Single scope: enforce project requirement up front (throws if needed).
        if( scope == LIBRARY_TABLE_SCOPE::PROJECT )
            require_project_for_library_tables();

        append_rows( scope );
    }

    return out;
}


py::list list_symbol_libs( const std::string& aScope )
{
    return list_libs_impl( LIBRARY_TABLE_TYPE::SYMBOL, aScope );
}


py::list list_footprint_libs( const std::string& aScope )
{
    return list_libs_impl( LIBRARY_TABLE_TYPE::FOOTPRINT, aScope );
}


py::dict add_lib_impl( LIBRARY_TABLE_TYPE aType,
                       const std::string& aName,
                       const std::string& aUri,
                       const std::string& aType_,
                       const std::string& aDescription,
                       const std::string& aOptions,
                       bool               aEnabled,
                       bool               aVisible,
                       const std::string& aScope )
{
    if( aName.empty() )
        throw std::invalid_argument( "library name (nickname) must be non-empty" );

    if( aUri.empty() )
        throw std::invalid_argument( "library URI must be non-empty" );

    if( aType_.empty() )
        throw std::invalid_argument( "library type must be non-empty "
                                     "(e.g. 'KiCad', 'Legacy')" );

    LIBRARY_TABLE_SCOPE scope = parse_scope_for_library_tables( aScope, false );
    LIBRARY_TABLE*      table = get_table_for_library_tables( aType, scope );

    // Reject duplicate nicknames in the same table — KiCad's lookup is keyed
    // on (table_type, scope, nickname), so a duplicate would mask the original
    // unpredictably.
    if( table->HasRow( wxString::FromUTF8( aName ) ) )
        throw std::runtime_error( "row '" + aName + "' already exists in the "
                                  + std::string( scope_to_string_for_library_tables( scope ) )
                                  + " " + ( aType == LIBRARY_TABLE_TYPE::SYMBOL
                                            ? "symbol" : "footprint" )
                                  + " library table" );

    LIBRARY_TABLE_ROW& row = table->InsertRow();
    row.SetNickname( wxString::FromUTF8( aName ) );
    row.SetURI( wxString::FromUTF8( aUri ) );
    row.SetType( wxString::FromUTF8( aType_ ) );
    row.SetDescription( wxString::FromUTF8( aDescription ) );
    row.SetOptions( wxString::FromUTF8( aOptions ) );
    row.SetDisabled( !aEnabled );
    row.SetHidden( !aVisible );
    row.SetScope( scope );

    py::dict result;
    result["ok"]    = true;
    result["name"]  = aName;
    result["scope"] = scope_to_string_for_library_tables( scope );
    return result;
}


py::dict add_symbol_lib( const std::string& aName,
                         const std::string& aUri,
                         const std::string& aType_,
                         const std::string& aDescription,
                         const std::string& aOptions,
                         bool               aEnabled,
                         bool               aVisible,
                         const std::string& aScope )
{
    return add_lib_impl( LIBRARY_TABLE_TYPE::SYMBOL, aName, aUri, aType_,
                         aDescription, aOptions, aEnabled, aVisible, aScope );
}


py::dict add_footprint_lib( const std::string& aName,
                            const std::string& aUri,
                            const std::string& aType_,
                            const std::string& aDescription,
                            const std::string& aOptions,
                            bool               aEnabled,
                            bool               aVisible,
                            const std::string& aScope )
{
    return add_lib_impl( LIBRARY_TABLE_TYPE::FOOTPRINT, aName, aUri, aType_,
                         aDescription, aOptions, aEnabled, aVisible, aScope );
}


py::dict remove_lib_impl( LIBRARY_TABLE_TYPE aType,
                          const std::string& aName,
                          const std::string& aScope )
{
    if( aName.empty() )
        throw std::invalid_argument( "library name (nickname) must be non-empty" );

    LIBRARY_TABLE_SCOPE scope = parse_scope_for_library_tables( aScope, false );
    LIBRARY_TABLE*      table = get_table_for_library_tables( aType, scope );

    wxString nick = wxString::FromUTF8( aName );

    auto& rows = table->Rows();
    bool  removed = false;
    for( auto it = rows.begin(); it != rows.end(); ++it )
    {
        if( it->Nickname() == nick )
        {
            rows.erase( it );
            removed = true;
            break;
        }
    }

    if( !removed )
        throw std::runtime_error( "no row named '" + aName + "' in the "
                                  + std::string( scope_to_string_for_library_tables( scope ) )
                                  + " " + ( aType == LIBRARY_TABLE_TYPE::SYMBOL
                                            ? "symbol" : "footprint" )
                                  + " library table" );

    py::dict result;
    result["ok"]      = true;
    result["name"]    = aName;
    result["scope"]   = scope_to_string_for_library_tables( scope );
    result["removed"] = removed;
    return result;
}


py::dict remove_symbol_lib( const std::string& aName, const std::string& aScope )
{
    return remove_lib_impl( LIBRARY_TABLE_TYPE::SYMBOL, aName, aScope );
}


py::dict remove_footprint_lib( const std::string& aName, const std::string& aScope )
{
    return remove_lib_impl( LIBRARY_TABLE_TYPE::FOOTPRINT, aName, aScope );
}


std::string resolve_path( const std::string& aPathWithVars )
{
    // Run text-var expansion (project-aware if a project is open) then
    // env-var substitution.  This mirrors what LIBRARY_MANAGER::ExpandURI
    // does internally, but is reusable for any user-supplied path.
    PROJECT* project = find_live_kiway_for_library_tables();

    wxString s = wxString::FromUTF8( aPathWithVars );

    if( project )
        s = ExpandTextVars( s, project );

    s = ExpandEnvVarSubstitutions( s, project );

    return s.ToStdString();
}


py::dict list_env_vars()
{
    PGM_BASE& pgm = get_pgm_for_library_tables();
    py::dict  out;

    // Live env-var map (built-ins + user-defined, with current values).
    const ENV_VAR_MAP& vars = pgm.GetLocalEnvVariables();

    for( const auto& [name, item] : vars )
        out[py::str( name.ToStdString() )] = item.GetValue().ToStdString();

    // Make sure KiCad-predefined names are present even if their values are
    // empty / unset, so callers can probe the full set.
    for( const wxString& name : ENV_VAR::GetPredefinedEnvVars() )
    {
        std::string key = name.ToStdString();
        if( !out.contains( py::str( key ) ) )
            out[py::str( key )] = std::string();
    }

    return out;
}


py::dict save()
{
    LIBRARY_MANAGER& mgr = get_library_manager_for_library_tables();
    py::list         files;
    py::list         errors;

    auto save_table = [&]( LIBRARY_TABLE_TYPE aType, LIBRARY_TABLE_SCOPE aScope )
    {
        // For project scope, skip if no project is loaded.
        if( aScope == LIBRARY_TABLE_SCOPE::PROJECT
            && !find_live_kiway_for_library_tables() )
        {
            return;
        }

        std::optional<LIBRARY_TABLE*> tbl = mgr.Table( aType, aScope );

        if( !tbl || !*tbl )
            return;

        LIBRARY_RESULT<void> result = ( *tbl )->Save();

        if( result.has_value() )
        {
            files.append( ( *tbl )->Path().ToStdString() );
        }
        else
        {
            py::dict err;
            err["path"]    = ( *tbl )->Path().ToStdString();
            err["scope"]   = scope_to_string_for_library_tables( aScope );
            err["message"] = result.error().message.ToStdString();
            errors.append( err );
        }
    };

    {
        py::gil_scoped_release nogil;
        save_table( LIBRARY_TABLE_TYPE::SYMBOL,    LIBRARY_TABLE_SCOPE::GLOBAL );
        save_table( LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_SCOPE::GLOBAL );
        save_table( LIBRARY_TABLE_TYPE::SYMBOL,    LIBRARY_TABLE_SCOPE::PROJECT );
        save_table( LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_SCOPE::PROJECT );
    }

    py::dict result;
    result["ok"]     = ( py::len( errors ) == 0 );
    result["files"]  = files;
    result["errors"] = errors;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_library_tables, m )
{
    m.doc() = "KliCAD library-table binding — read/write the symbol and "
              "footprint LIBRARY_TABLEs (global + project scope).  Useful "
              "for fixing broken paths and bulk-importing libraries from a "
              "script.  Persist via save(); changes are otherwise in-memory.";

    m.def( "list_symbol_libs", &list_symbol_libs,
           py::arg( "scope" ) = std::string( "all" ),
           R"DOC(List symbol-library rows.

scope: 'global' | 'project' | 'all' (default 'all').  Project scope requires
an open project; otherwise the project portion is silently skipped under
'all' and raises RuntimeError under 'project'.

Returns: list of {name, uri, type, description, options, enabled, visible,
                  scope}.
)DOC" );

    m.def( "list_footprint_libs", &list_footprint_libs,
           py::arg( "scope" ) = std::string( "all" ),
           "List footprint-library rows.  Same shape and rules as "
           "list_symbol_libs()." );

    m.def( "add_symbol_lib", &add_symbol_lib,
           py::arg( "name" ),
           py::arg( "uri" ),
           py::arg( "type" ),
           py::arg( "description" ) = std::string(),
           py::arg( "options" )     = std::string(),
           py::arg( "enabled" )     = true,
           py::arg( "visible" )     = true,
           py::arg( "scope" )       = std::string( "global" ),
           R"DOC(Append a symbol-library row to the given scope's table.

type: e.g. 'KiCad', 'Legacy' (matches what SCH_IO::TypeFromName accepts).
scope: 'global' (default) or 'project'.  Project scope requires an open project.

Raises RuntimeError if a row with the same name already exists, or on
invalid scope/type.  Changes are in-memory until save() is called.

Returns: {ok: True, name, scope}
)DOC" );

    m.def( "add_footprint_lib", &add_footprint_lib,
           py::arg( "name" ),
           py::arg( "uri" ),
           py::arg( "type" ),
           py::arg( "description" ) = std::string(),
           py::arg( "options" )     = std::string(),
           py::arg( "enabled" )     = true,
           py::arg( "visible" )     = true,
           py::arg( "scope" )       = std::string( "global" ),
           "Append a footprint-library row.  Same args and rules as "
           "add_symbol_lib()." );

    m.def( "remove_symbol_lib", &remove_symbol_lib,
           py::arg( "name" ),
           py::arg( "scope" ) = std::string( "global" ),
           R"DOC(Remove a symbol-library row from the given scope's table.

scope: 'global' (default) or 'project'.

Raises RuntimeError if no matching row exists.  Changes are in-memory until
save() is called.

Returns: {ok: True, name, scope, removed: True}
)DOC" );

    m.def( "remove_footprint_lib", &remove_footprint_lib,
           py::arg( "name" ),
           py::arg( "scope" ) = std::string( "global" ),
           "Remove a footprint-library row.  Same args and rules as "
           "remove_symbol_lib()." );

    m.def( "resolve_path", &resolve_path,
           py::arg( "path_with_vars" ),
           R"DOC(Expand a path containing ${VAR} substitutions.

Applies project text-var expansion (e.g. ${KIPRJMOD}) when a project is open,
followed by KiCad/system env-var substitution (e.g. ${KICAD_USER_FP_DIR}).
Returns the fully expanded string.  Useful for checking whether a library
URI actually resolves to a path on disk.
)DOC" );

    m.def( "list_env_vars", &list_env_vars,
           R"DOC(Return KiCad-known environment variables and their values.

Includes both user-defined entries from kicad_common.json and the built-in
predefined set (KIPRJMOD, KICAD_USER_FP_DIR, KICAD<MAJOR>_3RD_PARTY, ...).
Variables that are predefined but unset appear with an empty string value.
)DOC" );

    m.def( "save", &save,
           R"DOC(Persist all live library tables (symbol + footprint, both
scopes) to disk.

Returns:
    {ok: bool, files: [path, ...], errors: [{path, scope, message}, ...]}

`ok` is True iff every table that was attempted saved cleanly.  Tables that
have no in-memory representation (e.g. project tables with no project loaded)
are skipped, not reported as errors.
)DOC" );
}
