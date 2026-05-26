/*
 * KliCAD subsystem binding: design-block libraries.
 *
 * Exposes the LIBRARY_TABLE_TYPE::DESIGN_BLOCK rows in the global and project
 * library managers, plus per-library enumeration and per-block metadata
 * lookup.  Mirrors bindings_library_tables.cpp's symbol/footprint surface but
 * routed through the DESIGN_BLOCK_LIBRARY_ADAPTER so we can list blocks inside
 * each library and resolve LIB_IDs to DESIGN_BLOCK metadata.
 *
 * Module: klicad_native_design_blocks
 *
 *   list_design_block_libs(scope='all')
 *       -> [ {name, uri, type, description, options, enabled, visible, scope}, ... ]
 *
 *   add_design_block_lib(name, uri, type='KiCad', description='',
 *                        enabled=True, scope='global')   -> {ok, name, scope}
 *
 *   remove_design_block_lib(name, scope='global')        -> {ok, name, scope, removed}
 *
 *   list_design_blocks_in_lib(library)                   -> [name, ...]
 *
 *   get_design_block_info(lib_id)                        -> {library, name, lib_id,
 *                                                            found, description,
 *                                                            keywords, fields_count}
 *
 * Notes:
 *   - Pattern A (libkicommon-resident PYBIND11_EMBEDDED_MODULE) — all the
 *     required symbols (LIBRARY_MANAGER, LIBRARY_TABLE, DESIGN_BLOCK,
 *     DESIGN_BLOCK_LIBRARY_ADAPTER) live in libkicommon.
 *   - Project-scope ops require an open project; global ops are always
 *     available once KiCad has booted.
 *   - Changes from add/remove are in-memory until klicad_native_library_tables.
 *     save() is called (the existing save() handler iterates symbol/footprint
 *     tables only -- the DB table is persisted by the same LIBRARY_TABLE::Save
 *     plumbing, but exposing a save here would duplicate that handler;
 *     callers should rely on KiCad's own persistence on project close, or
 *     extend save() if needed).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <design_block.h>
#include <design_block_library_adapter.h>
#include <lib_id.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/settings_manager.h>

#include <wx/string.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

// -- Helpers (unique names so they don't clash with other bindings_*.cpp) ---

PGM_BASE& get_pgm_for_design_blocks()
{
    PGM_BASE* pgm = PgmOrNull();

    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised" );

    return *pgm;
}


LIBRARY_MANAGER& find_design_block_lib_manager()
{
    return get_pgm_for_design_blocks().GetLibraryManager();
}


SETTINGS_MANAGER& get_settings_manager_for_design_blocks()
{
    return get_pgm_for_design_blocks().GetSettingsManager();
}


// Returns the live PROJECT* (or nullptr) — never throws.
PROJECT* find_live_project_for_design_blocks()
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_design_blocks();

    if( !mgr.IsProjectOpen() )
        return nullptr;

    return &mgr.Prj();
}


PROJECT& require_project_for_design_blocks()
{
    PROJECT* project = find_live_project_for_design_blocks();

    if( !project || project->IsNullProject() )
        throw std::runtime_error( "no active project — open a project before "
                                  "using scope='project' design-block-table ops" );

    return *project;
}


LIBRARY_TABLE_SCOPE parse_scope_for_design_blocks( const std::string& aScope,
                                                  bool               aAllowAll )
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


const char* scope_to_string_for_design_blocks( LIBRARY_TABLE_SCOPE aScope )
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


py::dict row_to_dict_for_design_blocks( const LIBRARY_TABLE_ROW& aRow,
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

    LIBRARY_TABLE_SCOPE scope =
            aRow.Scope() == LIBRARY_TABLE_SCOPE::UNINITIALIZED ? aTableScope
                                                               : aRow.Scope();
    d["scope"] = scope_to_string_for_design_blocks( scope );
    return d;
}


LIBRARY_TABLE* get_design_block_table( LIBRARY_TABLE_SCOPE aScope )
{
    if( aScope == LIBRARY_TABLE_SCOPE::PROJECT )
        require_project_for_design_blocks();  // throws if no project

    LIBRARY_MANAGER& mgr = find_design_block_lib_manager();

    std::optional<LIBRARY_TABLE*> tbl =
            mgr.Table( LIBRARY_TABLE_TYPE::DESIGN_BLOCK, aScope );

    if( !tbl || !*tbl )
        throw std::runtime_error( std::string( "design-block library table not "
                                               "available for " )
                                  + scope_to_string_for_design_blocks( aScope ) );

    return *tbl;
}


DESIGN_BLOCK_LIBRARY_ADAPTER& get_design_block_adapter()
{
    LIBRARY_MANAGER& mgr = find_design_block_lib_manager();

    std::optional<LIBRARY_MANAGER_ADAPTER*> adapter =
            mgr.Adapter( LIBRARY_TABLE_TYPE::DESIGN_BLOCK );

    if( !adapter || !*adapter )
        throw std::runtime_error( "design-block library adapter is not "
                                  "registered — has the design-block subsystem "
                                  "been initialised?" );

    DESIGN_BLOCK_LIBRARY_ADAPTER* dba =
            dynamic_cast<DESIGN_BLOCK_LIBRARY_ADAPTER*>( *adapter );

    if( !dba )
        throw std::runtime_error( "registered DESIGN_BLOCK adapter is not a "
                                  "DESIGN_BLOCK_LIBRARY_ADAPTER (internal error)" );

    return *dba;
}


// -- Bound functions -------------------------------------------------------

py::list list_design_block_libs( const std::string& aScope )
{
    LIBRARY_TABLE_SCOPE scope = parse_scope_for_design_blocks( aScope, true );

    LIBRARY_MANAGER& mgr = find_design_block_lib_manager();
    py::list         out;

    auto append_rows = [&]( LIBRARY_TABLE_SCOPE aWalkScope )
    {
        if( aWalkScope == LIBRARY_TABLE_SCOPE::PROJECT
            && !find_live_project_for_design_blocks() )
        {
            return;
        }

        std::optional<LIBRARY_TABLE*> tbl =
                mgr.Table( LIBRARY_TABLE_TYPE::DESIGN_BLOCK, aWalkScope );

        if( !tbl || !*tbl )
            return;

        for( const LIBRARY_TABLE_ROW& row : ( *tbl )->Rows() )
            out.append( row_to_dict_for_design_blocks( row, aWalkScope ) );
    };

    if( scope == LIBRARY_TABLE_SCOPE::BOTH )
    {
        append_rows( LIBRARY_TABLE_SCOPE::GLOBAL );
        append_rows( LIBRARY_TABLE_SCOPE::PROJECT );
    }
    else
    {
        if( scope == LIBRARY_TABLE_SCOPE::PROJECT )
            require_project_for_design_blocks();

        append_rows( scope );
    }

    return out;
}


py::dict add_design_block_lib( const std::string& aName,
                               const std::string& aUri,
                               const std::string& aType,
                               const std::string& aDescription,
                               bool               aEnabled,
                               const std::string& aScope )
{
    if( aName.empty() )
        throw std::invalid_argument( "library name (nickname) must be non-empty" );

    if( aUri.empty() )
        throw std::invalid_argument( "library URI must be non-empty" );

    if( aType.empty() )
        throw std::invalid_argument( "library type must be non-empty "
                                     "(e.g. 'KiCad')" );

    LIBRARY_TABLE_SCOPE scope = parse_scope_for_design_blocks( aScope, false );
    LIBRARY_TABLE*      table = get_design_block_table( scope );

    if( table->HasRow( wxString::FromUTF8( aName ) ) )
        throw std::runtime_error( "row '" + aName + "' already exists in the "
                                  + std::string( scope_to_string_for_design_blocks( scope ) )
                                  + " design-block library table" );

    LIBRARY_TABLE_ROW& row = table->InsertRow();
    row.SetNickname( wxString::FromUTF8( aName ) );
    row.SetURI( wxString::FromUTF8( aUri ) );
    row.SetType( wxString::FromUTF8( aType ) );
    row.SetDescription( wxString::FromUTF8( aDescription ) );
    row.SetDisabled( !aEnabled );
    row.SetScope( scope );

    py::dict result;
    result["ok"]    = true;
    result["name"]  = aName;
    result["scope"] = scope_to_string_for_design_blocks( scope );
    return result;
}


py::dict remove_design_block_lib( const std::string& aName,
                                  const std::string& aScope )
{
    if( aName.empty() )
        throw std::invalid_argument( "library name (nickname) must be non-empty" );

    LIBRARY_TABLE_SCOPE scope = parse_scope_for_design_blocks( aScope, false );
    LIBRARY_TABLE*      table = get_design_block_table( scope );

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
                                  + std::string( scope_to_string_for_design_blocks( scope ) )
                                  + " design-block library table" );

    py::dict result;
    result["ok"]      = true;
    result["name"]    = aName;
    result["scope"]   = scope_to_string_for_design_blocks( scope );
    result["removed"] = removed;
    return result;
}


py::list list_design_blocks_in_lib( const std::string& aLibrary )
{
    if( aLibrary.empty() )
        throw std::invalid_argument( "library nickname must be non-empty" );

    DESIGN_BLOCK_LIBRARY_ADAPTER& adapter = get_design_block_adapter();

    wxString nick = wxString::FromUTF8( aLibrary );

    if( !adapter.HasLibrary( nick ) )
        throw std::runtime_error( "no design-block library named '" + aLibrary
                                  + "' in the registered tables" );

    std::vector<wxString> names;
    {
        py::gil_scoped_release nogil;
        // Ensure the library is loaded before enumerating; LoadOne is a no-op
        // if already loaded.
        adapter.LoadOne( nick );
        names = adapter.GetDesignBlockNames( nick );
    }

    py::list out;
    for( const wxString& n : names )
        out.append( n.ToStdString() );

    return out;
}


py::dict get_design_block_info( const std::string& aLibId )
{
    if( aLibId.empty() )
        throw std::invalid_argument( "lib_id must be non-empty (format "
                                     "'library:block_name')" );

    LIB_ID id;
    int    parseErr = id.Parse( UTF8( aLibId ) );

    if( parseErr != -1 )
        throw std::invalid_argument( "could not parse lib_id '" + aLibId
                                     + "' (error at offset "
                                     + std::to_string( parseErr ) + ")" );

    wxString libNick   = id.GetLibNickname().wx_str();
    wxString blockName = id.GetLibItemName().wx_str();

    if( libNick.IsEmpty() || blockName.IsEmpty() )
        throw std::invalid_argument( "lib_id '" + aLibId + "' must include both "
                                     "library nickname and block name "
                                     "(format 'library:block_name')" );

    DESIGN_BLOCK_LIBRARY_ADAPTER& adapter = get_design_block_adapter();

    py::dict d;
    d["library"]      = libNick.ToStdString();
    d["name"]         = blockName.ToStdString();
    d["lib_id"]       = id.Format().wx_str().ToStdString();
    d["found"]        = false;
    d["description"]  = std::string();
    d["keywords"]     = std::string();
    d["fields_count"] = 0;

    if( !adapter.HasLibrary( libNick ) )
        return d;

    std::unique_ptr<DESIGN_BLOCK> block;
    {
        py::gil_scoped_release nogil;
        adapter.LoadOne( libNick );
        block.reset( adapter.LoadDesignBlock( libNick, blockName, false ) );
    }

    if( !block )
        return d;

    d["found"]        = true;
    d["description"]  = block->GetLibDescription().ToStdString();
    d["keywords"]     = block->GetKeywords().ToStdString();
    d["fields_count"] = static_cast<int>( block->GetFields().size() );
    return d;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_design_blocks, m )
{
    m.doc() = "KliCAD design-block binding — read/write the DESIGN_BLOCK "
              "LIBRARY_TABLE (global + project scope), enumerate blocks in a "
              "library, and fetch per-block metadata.  Changes from add/remove "
              "are in-memory until KiCad persists tables (project close or "
              "via the library-tables save handler).";

    m.def( "list_design_block_libs", &list_design_block_libs,
           py::arg( "scope" ) = std::string( "all" ),
           R"DOC(List design-block library rows.

scope: 'global' | 'project' | 'all' (default 'all').  Project scope requires
an open project; otherwise the project portion is silently skipped under 'all'
and raises RuntimeError under 'project'.

Returns: list of {name, uri, type, description, options, enabled, visible,
                  scope}.
)DOC" );

    m.def( "add_design_block_lib", &add_design_block_lib,
           py::arg( "name" ),
           py::arg( "uri" ),
           py::arg( "type" )        = std::string( "KiCad" ),
           py::arg( "description" ) = std::string(),
           py::arg( "enabled" )     = true,
           py::arg( "scope" )       = std::string( "global" ),
           R"DOC(Append a design-block library row to the given scope's table.

type: e.g. 'KiCad' (matches DESIGN_BLOCK_IO_MGR's accepted strings).
scope: 'global' (default) or 'project'.  Project scope requires an open project.

Raises RuntimeError if a row with the same name already exists, or on
invalid scope/type.  Changes are in-memory until the table is persisted.

Returns: {ok: True, name, scope}
)DOC" );

    m.def( "remove_design_block_lib", &remove_design_block_lib,
           py::arg( "name" ),
           py::arg( "scope" ) = std::string( "global" ),
           R"DOC(Remove a design-block library row from the given scope's table.

scope: 'global' (default) or 'project'.

Raises RuntimeError if no matching row exists.  Changes are in-memory until
the table is persisted.

Returns: {ok: True, name, scope, removed: True}
)DOC" );

    m.def( "list_design_blocks_in_lib", &list_design_blocks_in_lib,
           py::arg( "library" ),
           R"DOC(List the design-block names inside the given library.

library: the nickname of a registered design-block library (any scope).

Triggers a synchronous load of the library if it isn't already loaded.
Raises RuntimeError if the nickname is not registered in any table.

Returns: list of block-name strings.
)DOC" );

    m.def( "get_design_block_info", &get_design_block_info,
           py::arg( "lib_id" ),
           R"DOC(Fetch metadata for a single design block by LIB_ID.

lib_id: 'library_nickname:block_name'.

Triggers a synchronous load of the underlying library if needed.  When the
block can't be located (unknown library, or unknown name within the library),
the dict is returned with `found=False` and empty/zero metadata fields rather
than raising.

Returns: {library, name, lib_id, found, description, keywords, fields_count}
)DOC" );
}
