/*
 * KliCAD subsystem binding: settings (COMMON_SETTINGS, kicad_common.json).
 *
 * Exposes a typed JSON-pointer API onto KiCad's COMMON_SETTINGS so scripts
 * can programmatically read/write things the GUI Preferences dialog otherwise
 * monopolises (autosave interval, themes, env vars, file history size, ...).
 *
 * Module: klicad_native_settings
 *
 *   get(path)           -> value (int/float/str/bool/list/dict)
 *   set(path, value)    -> {ok, old_value, new_value, persisted}
 *   dump(prefix='')     -> nested dict, optionally rooted at prefix
 *   save()              -> {ok, files: [str, ...]}
 *
 * Coverage: COMMON_SETTINGS only.  Per-app settings (KICAD_SETTINGS,
 * EESCHEMA_SETTINGS, PCBNEW_SETTINGS, ...) are reached via
 * SETTINGS_MANAGER::GetAppSettings<T>() which requires the typed template
 * argument at compile time per app — outside the scope of this single TU.
 * `save()` does flush the whole SETTINGS_MANAGER pool though, so any other
 * subsystem that has mutated its in-memory JSON_SETTINGS pool will be
 * persisted alongside common settings.
 *
 * Note: not every setting takes effect immediately.  Some are sampled only
 * when the relevant frame is constructed (toolbar icon size, language, ...)
 * and need a KiCad restart.  Autosave interval, env vars, file history size,
 * and similar runtime-read fields generally apply on next use.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <pgm_base.h>
#include <settings/common_settings.h>
#include <settings/json_settings.h>
#include <settings/json_settings_internals.h>
#include <settings/settings_manager.h>

#include <json_common.h>

#include <wx/string.h>

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Helper unique to this TU (settings) — keep name distinct from other bindings_*.
COMMON_SETTINGS* get_common_settings()
{
    PGM_BASE* pgm = PgmOrNull();
    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised" );

    SETTINGS_MANAGER& mgr = pgm->GetSettingsManager();
    COMMON_SETTINGS*  cs  = mgr.GetCommonSettings();
    if( !cs )
        throw std::runtime_error( "GetCommonSettings() returned null — settings not loaded" );
    return cs;
}


SETTINGS_MANAGER& get_settings_manager_for_settings()
{
    PGM_BASE* pgm = PgmOrNull();
    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised" );
    return pgm->GetSettingsManager();
}


// Convert a nlohmann::json node to a Python object.  Recursive for objects/arrays.
py::object json_to_py( const nlohmann::json& aJson )
{
    using value_t = nlohmann::json::value_t;

    switch( aJson.type() )
    {
    case value_t::null:
        return py::none();

    case value_t::boolean:
        return py::bool_( aJson.get<bool>() );

    case value_t::number_integer:
        return py::int_( aJson.get<long long>() );

    case value_t::number_unsigned:
        return py::int_( aJson.get<unsigned long long>() );

    case value_t::number_float:
        return py::float_( aJson.get<double>() );

    case value_t::string:
        return py::str( aJson.get<std::string>() );

    case value_t::array:
    {
        py::list out;
        for( const auto& item : aJson )
            out.append( json_to_py( item ) );
        return out;
    }

    case value_t::object:
    {
        py::dict out;
        for( auto it = aJson.begin(); it != aJson.end(); ++it )
            out[py::str( it.key() )] = json_to_py( it.value() );
        return out;
    }

    default:
        // binary, discarded — not used in KiCad settings
        return py::none();
    }
}


// Convert a Python object to nlohmann::json.  Recursive for lists/dicts.
nlohmann::json py_to_json( const py::handle& aObj )
{
    if( aObj.is_none() )
        return nullptr;

    if( py::isinstance<py::bool_>( aObj ) )
        return aObj.cast<bool>();

    if( py::isinstance<py::int_>( aObj ) )
        return aObj.cast<long long>();

    if( py::isinstance<py::float_>( aObj ) )
        return aObj.cast<double>();

    if( py::isinstance<py::str>( aObj ) )
        return aObj.cast<std::string>();

    if( py::isinstance<py::list>( aObj ) || py::isinstance<py::tuple>( aObj ) )
    {
        nlohmann::json arr = nlohmann::json::array();
        for( auto item : aObj )
            arr.push_back( py_to_json( item ) );
        return arr;
    }

    if( py::isinstance<py::dict>( aObj ) )
    {
        nlohmann::json obj = nlohmann::json::object();
        for( auto kv : aObj.cast<py::dict>() )
            obj[py::str( kv.first ).cast<std::string>()] = py_to_json( kv.second );
        return obj;
    }

    throw std::runtime_error(
            "unsupported Python value type for settings.set() — "
            "use bool, int, float, str, list, dict, or None" );
}


// Resolve a dotted path to a nlohmann::json node inside the given settings.
// Returns std::nullopt if the path doesn't exist.  Empty path -> whole tree.
std::optional<nlohmann::json> resolve_json_at( COMMON_SETTINGS* aCs,
                                               const std::string& aPath )
{
    // Make sure the JSON tree reflects the current in-memory C++ params.
    aCs->Store();

    if( aPath.empty() )
        return std::optional<nlohmann::json>{ *aCs->Internals() };

    return aCs->GetJson( aPath );
}


py::object settings_get( const std::string& aPath )
{
    COMMON_SETTINGS* cs = get_common_settings();

    std::optional<nlohmann::json> node = resolve_json_at( cs, aPath );
    if( !node )
        throw py::key_error( "setting path '" + aPath + "' not found in COMMON_SETTINGS" );

    return json_to_py( *node );
}


py::dict settings_dump( const std::string& aPrefix )
{
    COMMON_SETTINGS* cs = get_common_settings();

    std::optional<nlohmann::json> node = resolve_json_at( cs, aPrefix );
    if( !node )
        throw py::key_error( "setting prefix '" + aPrefix + "' not found in COMMON_SETTINGS" );

    if( !node->is_object() )
    {
        // A leaf was requested via dump() — wrap it under its own key so the return
        // contract (dict) is upheld.  Caller can pick whatever they like back out.
        py::dict out;
        out[py::str( aPrefix.empty() ? std::string( "value" ) : aPrefix )] =
                json_to_py( *node );
        return out;
    }

    return py::cast<py::dict>( json_to_py( *node ) );
}


py::dict settings_set( const std::string& aPath, const py::object& aValue )
{
    if( aPath.empty() )
        throw std::invalid_argument( "set() requires a non-empty path" );

    COMMON_SETTINGS*  cs  = get_common_settings();
    SETTINGS_MANAGER& mgr = get_settings_manager_for_settings();

    // Sync C++ -> JSON so 'old_value' reflects the live runtime value, not just
    // whatever happens to be on disk (or stale in the JSON tree).
    cs->Store();

    py::object old_py = py::none();
    if( std::optional<nlohmann::json> prev = cs->GetJson( aPath ) )
        old_py = json_to_py( *prev );

    // Write the new value into the JSON document.
    nlohmann::json newJson = py_to_json( aValue );

    // Use the typed Set<>() overloads where possible (they go through
    // SetFromString -> json_pointer and update m_modified accounting).
    if( newJson.is_boolean() )
        cs->Set<bool>( aPath, newJson.get<bool>() );
    else if( newJson.is_number_integer() )
        cs->Set<int>( aPath, newJson.get<int>() );
    else if( newJson.is_number_unsigned() )
        cs->Set<unsigned int>( aPath, newJson.get<unsigned int>() );
    else if( newJson.is_number_float() )
        cs->Set<double>( aPath, newJson.get<double>() );
    else if( newJson.is_string() )
        cs->Set<std::string>( aPath, newJson.get<std::string>() );
    else
        // Arrays / objects / null: assign whole subtree via the generic Set<json>.
        cs->Set<nlohmann::json>( aPath, newJson );

    // Re-Load() runs every PARAM's load callback, which pulls the new JSON value
    // back into the corresponding C++ member field (m_System.foo, m_Backup.bar,
    // etc).  Without this step the JSON has the new value but the live struct
    // still has the old one, so KiCad's runtime keeps using the stale field.
    cs->Load();

    // Persist to disk.  We save only COMMON_SETTINGS here so we don't accidentally
    // overwrite other in-flight settings the user might have changed via the GUI.
    bool persisted = false;
    {
        py::gil_scoped_release nogil;
        mgr.Save( cs );
        persisted = true;
    }

    // Re-read to confirm the new value as actually stored.
    py::object new_py = py::none();
    cs->Store();
    if( std::optional<nlohmann::json> after = cs->GetJson( aPath ) )
        new_py = json_to_py( *after );

    py::dict result;
    result["ok"]        = true;
    result["path"]      = aPath;
    result["old_value"] = old_py;
    result["new_value"] = new_py;
    result["persisted"] = persisted;
    return result;
}


py::dict settings_save()
{
    SETTINGS_MANAGER& mgr = get_settings_manager_for_settings();

    // Make sure COMMON_SETTINGS' in-memory C++ struct is flushed to its JSON
    // before the manager writes the file.  (Other settings classes handle their
    // own Store() in SaveToFile.)
    COMMON_SETTINGS* cs = get_common_settings();
    cs->Store();

    py::list files;
    files.append( cs->GetFullFilename().ToStdString() );

    {
        py::gil_scoped_release nogil;
        mgr.Save();
    }

    py::dict result;
    result["ok"]    = true;
    result["files"] = files;  // Only the file we can name; manager writes others too.
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_settings, m )
{
    m.doc() = "KliCAD settings binding — read/write COMMON_SETTINGS (kicad_common.json) "
              "via JSON-pointer dotted paths.  Persists via SETTINGS_MANAGER. "
              "Per-app settings (KICAD/EESCHEMA/PCBNEW) are not exposed here.";

    m.def( "get", &settings_get,
           py::arg( "setting_path" ),
           R"DOC(Read a value from COMMON_SETTINGS at the given dotted JSON path.

Example: get("system.local_history_debounce") -> 5
         get("api.enable_server")              -> True
         get("appearance.canvas_scale")        -> 0.0
         get("environment.vars")               -> {"KICAD_USER_FP_DIR": "...", ...}

Returns the native Python value (int / float / str / bool / list / dict / None).
Raises KeyError if the path does not exist.
)DOC" );

    m.def( "set", &settings_set,
           py::arg( "setting_path" ),
           py::arg( "value" ),
           R"DOC(Write a value into COMMON_SETTINGS and persist to disk.

Example: set("system.local_history_debounce", 0)
         set("api.enable_server", True)
         set("auto_backup.enabled", False)

`value` can be bool, int, float, str, list, dict, or None — converted to JSON.

The C++ struct backing the setting is refreshed via Load() so KiCad's running
code sees the new value where it samples at runtime.  Some settings only take
effect on restart (e.g. toolbar icon size, language) — those will still be
written to disk and applied on the next KiCad launch.

Returns:
    {ok: True, path, old_value, new_value, persisted: True}

Raises:
    ValueError on empty path.
    RuntimeError on unsupported value type or if settings aren't loaded.
)DOC" );

    m.def( "dump", &settings_dump,
           py::arg( "prefix" ) = std::string(),
           R"DOC(Return COMMON_SETTINGS as a nested Python dict.

If `prefix` is given (e.g. "system" or "auto_backup"), only that subtree is
returned.  Empty prefix returns the whole settings document.

Note: this is the JSON view after Store() — it reflects the live in-memory
state, including any pending changes that haven't yet been flushed to disk.

Raises KeyError if the prefix does not resolve to a node.
)DOC" );

    m.def( "save", &settings_save,
           R"DOC(Flush all registered SETTINGS_MANAGER settings to disk.

Equivalent to calling Save() at KiCad shutdown.  Returns:
    {ok: True, files: [kicad_common.json full path]}

The `files` list only names kicad_common.json explicitly; the manager also
writes any other registered settings objects that are dirty (kicad_settings,
per-app settings, color themes via their own path, etc).
)DOC" );
}
