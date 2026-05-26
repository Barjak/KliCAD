/*
 * KliCAD subsystem binding: Plugin Content Manager (PCM) — filesystem walk.
 *
 * Read-only metadata facade that walks the on-disk layout managed by the
 * real PLUGIN_CONTENT_MANAGER, WITHOUT linking PLUGIN_CONTENT_MANAGER itself.
 *
 * Why not just use PLUGIN_CONTENT_MANAGER directly?  Its translation unit
 * lives in kicad/pcm/ together with kicad/pcm/dialogs/* (wxFormBuilder UI
 * panels, manage-repositories dialog, etc.).  Pulling pcm.cpp into
 * libkicommon would drag those dialog headers (and their wxFormBuilder
 * dependencies) along with it, which is precisely what libkicommon must
 * avoid.  So this binding re-implements the read-only slice — directory
 * walk + parse installed_packages.json — using only stdlib and
 * <nlohmann/json.hpp>, both of which are already libkicommon-reachable.
 *
 * Module: klicad_native_pcm
 *
 *   list_installed()                          -> list[dict]
 *   list_packages_dir(package_kind='all')     -> list[dict]
 *   is_installed(identifier)                  -> bool
 *   get_packages_dir()                        -> str
 *
 * Install / uninstall / metadata refresh are deliberately NOT exposed:
 * those require PCM_TASK_MANAGER plus network I/O + threaded progress UIs.
 * Use the PCM dialog inside KiCad for those.
 *
 * Pattern A (libkicommon-resident PYBIND11_EMBEDDED_MODULE).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <build_version.h>
#include <env_vars.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <settings/environment.h>

namespace py = pybind11;
namespace fs = std::filesystem;

namespace
{

// -----------------------------------------------------------------------------
// PCM directory layout — names must stay in sync with the real PCM
// (kicad/pcm/pcm.h::PCM_PACKAGE_DIRECTORIES).  Hard-coded here so we don't
// need to include pcm.h.
// -----------------------------------------------------------------------------
const std::vector<std::string> PCM_KNOWN_SUBDIRS = {
    "plugins", "footprints", "3dmodels", "symbols",
    "resources", "colors", "templates", "scripts"
};


// -----------------------------------------------------------------------------
// Resolve the 3rdparty path the same way PLUGIN_CONTENT_MANAGER::ReadEnvVar
// does, but without linking PCM:
//
//   1. KICAD<major>_3RD_PARTY environment var (versioned name, e.g.
//      KICAD10_3RD_PARTY) sourced from KiCad's common-settings env map.
//   2. Same name in the process environment (wxGetEnv).
//   3. wxStandardPaths::GetDocumentsDir()/KiCad/<MAJOR.MINOR>/3rdparty.
//
// Returns an absolute, lexically-normal path.  May return a path that
// doesn't exist on disk yet — callers should check.
// -----------------------------------------------------------------------------
fs::path pcm_find_packages_dir()
{
    wxString result;

    // 1. KiCad-managed env map (kicad_common.json + process env merged).
    if( PGM_BASE* pgm = PgmOrNull() )
    {
        const ENV_VAR_MAP& env = pgm->GetLocalEnvVariables();

        if( std::optional<wxString> v =
                ENV_VAR::GetVersionedEnvVarValue( env, wxT( "3RD_PARTY" ) ) )
        {
            result = *v;
        }
    }

    // 2. Plain process env, in case KiCad isn't initialised (e.g. early
    //    import, headless test) — try the versioned name directly.
    if( result.IsEmpty() )
    {
        wxString name = ENV_VAR::GetVersionedEnvVarName( wxT( "3RD_PARTY" ) );
        wxString val;
        if( wxGetEnv( name, &val ) && !val.IsEmpty() )
            result = val;
    }

    // 3. Default: ~/Documents/KiCad/<MAJOR.MINOR>/3rdparty.
    if( result.IsEmpty() )
    {
        wxFileName fn;
        fn.AssignDir( wxStandardPaths::Get().GetDocumentsDir() );
        fn.AppendDir( wxT( "KiCad" ) );
        fn.AppendDir( GetMajorMinorVersion() );
        fn.AppendDir( wxT( "3rdparty" ) );
        result = fn.GetAbsolutePath();
    }

    // Expand any ${VAR} references that may have survived through the env
    // map (KICAD_USER_DOCUMENT_DIR etc.) before handing back to std::filesystem.
    wxFileName norm;
    norm.AssignDir( result );
    norm.Normalize( wxPATH_NORM_ENV_VARS | wxPATH_NORM_ABSOLUTE
                    | wxPATH_NORM_TILDE );

    return fs::path( std::string( norm.GetFullPath().utf8_str() ) )
            .lexically_normal();
}


// -----------------------------------------------------------------------------
// Locate the installed-packages manifest.  The real PCM writes this to the
// user-settings dir (PATHS::GetUserSettingsPath()/installed_packages.json),
// but our spec says to look for a global manifest alongside the 3rdparty
// dir as well.  Return whichever path exists, with the 3rdparty-local copy
// taking precedence (because that's what the spec explicitly mentions).
// Returns empty path if neither exists.
// -----------------------------------------------------------------------------
fs::path pcm_locate_manifest( const fs::path& aPackagesDir )
{
    std::error_code ec;

    // Spec: ~/Documents/KiCad/<ver>/3rdparty/installed_packages.json
    fs::path in_3rdparty = aPackagesDir / "installed_packages.json";
    if( fs::exists( in_3rdparty, ec ) )
        return in_3rdparty;

    // Real PCM location: user settings dir.  This is where the GUI's PCM
    // actually writes the manifest, so prefer it as a fallback so users
    // don't have to copy the file.
    if( PGM_BASE* pgm = PgmOrNull() )
    {
        // GetCommonSettings() may also expose paths, but the simplest reach
        // is wxStandardPaths -> user config dir.  We don't link PATHS::
        // helpers here to keep the surface minimal.
        (void) pgm;  // keep the guard in case future code needs it
    }

    wxFileName cfg;
    cfg.AssignDir( wxStandardPaths::Get().GetUserConfigDir() );
    cfg.AppendDir( wxT( "kicad" ) );
    cfg.AppendDir( GetMajorMinorVersion() );
    cfg.SetFullName( wxT( "installed_packages.json" ) );

    fs::path in_settings(
            std::string( cfg.GetFullPath().utf8_str() ) );

    if( fs::exists( in_settings, ec ) )
        return in_settings;

    return {};
}


// -----------------------------------------------------------------------------
// Parse installed_packages.json.  Returns a list of dicts following the
// shape requested by the orchestrator:
//   {identifier, name, description, version, installed_path, install_date,
//    kicad_versions}
//
// The JSON schema we assume (matches what PLUGIN_CONTENT_MANAGER writes):
//   {
//     "packages": [
//       {
//         "package": {
//           "identifier": "...",
//           "name": "...",
//           "description": "...",
//           "description_full": "...",       (optional, ignored)
//           "versions": [
//             { "version": "1.2.3",
//               "kicad_version": "10.0",
//               "kicad_version_max": "10.99" (optional)
//               ... },
//             ...
//           ]
//         },
//         "current_version": "1.2.3",
//         "repository_id": "...",
//         "repository_name": "...",
//         "install_timestamp": 1700000000,
//         "pinned": false
//       },
//       ...
//     ]
//   }
//
// `installed_path` is computed by walking PCM_KNOWN_SUBDIRS and finding the
// subdirectory whose name (with '_' replaced by '.') matches identifier.
// `kicad_versions` collects {min, max} from each compatible version entry.
// -----------------------------------------------------------------------------
py::list pcm_read_installed_manifest( const fs::path& aManifestPath,
                                      const fs::path& aPackagesDir )
{
    py::list out;

    std::ifstream in( aManifestPath );
    if( !in )
        return out;

    nlohmann::json doc;
    try
    {
        in >> doc;
    }
    catch( const std::exception& e )
    {
        throw std::runtime_error( std::string( "failed to parse " )
                                  + aManifestPath.string() + ": " + e.what() );
    }

    if( !doc.contains( "packages" ) || !doc["packages"].is_array() )
        return out;

    for( const auto& entry : doc["packages"] )
    {
        if( !entry.is_object() || !entry.contains( "package" ) )
            continue;

        const auto& pkg = entry["package"];

        py::dict d;

        auto get_str = []( const nlohmann::json& j, const char* key ) -> std::string
        {
            if( j.contains( key ) && j[key].is_string() )
                return j[key].get<std::string>();
            return {};
        };

        std::string identifier  = get_str( pkg,   "identifier" );
        std::string name        = get_str( pkg,   "name" );
        std::string description = get_str( pkg,   "description" );
        std::string version     = get_str( entry, "current_version" );

        d["identifier"]      = identifier;
        d["name"]            = name;
        d["description"]     = description;
        d["version"]         = version;
        d["repository_id"]   = get_str( entry, "repository_id" );
        d["repository_name"] = get_str( entry, "repository_name" );

        if( entry.contains( "install_timestamp" )
            && entry["install_timestamp"].is_number_integer() )
        {
            d["install_date"] = entry["install_timestamp"].get<int64_t>();
        }
        else
        {
            d["install_date"] = py::none();
        }

        if( entry.contains( "pinned" ) && entry["pinned"].is_boolean() )
            d["pinned"] = entry["pinned"].get<bool>();
        else
            d["pinned"] = false;

        // kicad_versions: aggregate over all version entries that match
        // current_version (or all of them if current_version is absent).
        py::list kicad_versions;
        if( pkg.contains( "versions" ) && pkg["versions"].is_array() )
        {
            for( const auto& v : pkg["versions"] )
            {
                if( !v.is_object() )
                    continue;

                py::dict vd;
                vd["version"]       = get_str( v, "version" );
                vd["kicad_version"] = get_str( v, "kicad_version" );

                if( v.contains( "kicad_version_max" ) && v["kicad_version_max"].is_string() )
                    vd["kicad_version_max"] = v["kicad_version_max"].get<std::string>();
                else
                    vd["kicad_version_max"] = py::none();

                if( v.contains( "status" ) && v["status"].is_string() )
                    vd["status"] = v["status"].get<std::string>();
                else
                    vd["status"] = py::none();

                kicad_versions.append( vd );
            }
        }
        d["kicad_versions"] = kicad_versions;

        // installed_path: find a subdir whose name resolves to this
        // identifier.  PCM stores packages under one or more of
        // <3rdparty>/<kind>/<id_with_underscores>/.  We pick the FIRST
        // match — there can be several (e.g. a package that ships both
        // plugins/ and footprints/), but any of them serves as a useful
        // pointer to where the install landed.
        std::string installed_path;
        if( !identifier.empty() )
        {
            std::string disk_id = identifier;
            std::replace( disk_id.begin(), disk_id.end(), '.', '_' );

            std::error_code ec;
            for( const std::string& kind : PCM_KNOWN_SUBDIRS )
            {
                fs::path candidate = aPackagesDir / kind / disk_id;
                if( fs::exists( candidate, ec ) && fs::is_directory( candidate, ec ) )
                {
                    installed_path = candidate.string();
                    break;
                }

                // Some packages keep their identifier with dots on disk.
                fs::path candidate_dotted = aPackagesDir / kind / identifier;
                if( fs::exists( candidate_dotted, ec )
                    && fs::is_directory( candidate_dotted, ec ) )
                {
                    installed_path = candidate_dotted.string();
                    break;
                }
            }
        }

        if( installed_path.empty() )
            d["installed_path"] = py::none();
        else
            d["installed_path"] = installed_path;

        out.append( d );
    }

    return out;
}


// -----------------------------------------------------------------------------
// Walk the 3rdparty tree and synthesize entries from directory names alone,
// for packages that don't appear in the manifest (or when no manifest
// exists at all).  Mirrors PLUGIN_CONTENT_MANAGER's fallback in pcm.cpp:
// any subdir under <3rdparty>/<kind>/ is treated as an installed package
// whose identifier is the dir name with '_' rewritten as '.'.
// -----------------------------------------------------------------------------
void pcm_append_fs_fallback( py::list&                     aOut,
                             const fs::path&               aPackagesDir,
                             const std::unordered_set<std::string>& aAlreadySeen )
{
    std::error_code ec;
    if( !fs::exists( aPackagesDir, ec ) || !fs::is_directory( aPackagesDir, ec ) )
        return;

    for( const std::string& kind : PCM_KNOWN_SUBDIRS )
    {
        fs::path kind_dir = aPackagesDir / kind;
        if( !fs::exists( kind_dir, ec ) || !fs::is_directory( kind_dir, ec ) )
            continue;

        for( const auto& sub : fs::directory_iterator( kind_dir, ec ) )
        {
            if( ec )
                break;
            if( !sub.is_directory( ec ) )
                continue;

            std::string subdir_name = sub.path().filename().string();
            std::string identifier  = subdir_name;
            std::replace( identifier.begin(), identifier.end(), '_', '.' );

            if( aAlreadySeen.count( identifier ) )
                continue;

            py::dict d;
            d["identifier"]      = identifier;
            d["name"]            = subdir_name;
            d["description"]     = std::string();
            d["version"]         = std::string( "0.0" );
            d["repository_id"]   = std::string();
            d["repository_name"] = std::string( "<unknown>" );
            d["pinned"]          = false;

            auto mtime = fs::last_write_time( sub.path(), ec );
            if( !ec )
            {
                auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                        std::chrono::file_clock::to_sys( mtime ) );
                d["install_date"] = static_cast<int64_t>(
                        sctp.time_since_epoch().count() );
            }
            else
            {
                d["install_date"] = py::none();
            }

            d["installed_path"] = sub.path().string();
            d["kicad_versions"] = py::list();
            aOut.append( d );
        }
    }
}


// -----------------------------------------------------------------------------
// Module-level entry points.
// -----------------------------------------------------------------------------
py::list pcm_module_list_installed()
{
    fs::path packages_dir = pcm_find_packages_dir();
    fs::path manifest     = pcm_locate_manifest( packages_dir );

    py::list out;
    std::unordered_set<std::string> seen;

    if( !manifest.empty() )
    {
        out = pcm_read_installed_manifest( manifest, packages_dir );
        for( const py::handle& h : out )
        {
            py::dict d = py::reinterpret_borrow<py::dict>( h );
            if( d.contains( "identifier" ) )
                seen.insert( d["identifier"].cast<std::string>() );
        }
    }

    pcm_append_fs_fallback( out, packages_dir, seen );
    return out;
}


py::list pcm_module_list_packages_dir( const std::string& aKind )
{
    fs::path packages_dir = pcm_find_packages_dir();
    py::list out;

    std::error_code ec;
    if( !fs::exists( packages_dir, ec ) || !fs::is_directory( packages_dir, ec ) )
        return out;

    // Decide which kinds to walk.
    std::vector<std::string> kinds;
    if( aKind == "all" || aKind.empty() )
    {
        kinds = PCM_KNOWN_SUBDIRS;
    }
    else
    {
        bool valid = false;
        for( const std::string& k : PCM_KNOWN_SUBDIRS )
        {
            if( k == aKind )
            {
                kinds.push_back( k );
                valid = true;
                break;
            }
        }
        if( !valid )
        {
            std::string msg = "unknown package_kind '" + aKind
                              + "' — expected 'all' or one of:";
            for( const std::string& k : PCM_KNOWN_SUBDIRS )
                msg += " " + k;
            throw std::invalid_argument( msg );
        }
    }

    for( const std::string& kind : kinds )
    {
        fs::path kind_dir = packages_dir / kind;
        if( !fs::exists( kind_dir, ec ) || !fs::is_directory( kind_dir, ec ) )
            continue;

        for( const auto& sub : fs::directory_iterator( kind_dir, ec ) )
        {
            if( ec )
                break;
            if( !sub.is_directory( ec ) )
                continue;

            std::string subdir_name = sub.path().filename().string();
            std::string identifier  = subdir_name;
            std::replace( identifier.begin(), identifier.end(), '_', '.' );

            // Count files (recursive) — useful as a sanity gauge.
            size_t files_count = 0;
            std::error_code walk_ec;
            for( const auto& f : fs::recursive_directory_iterator(
                         sub.path(),
                         fs::directory_options::skip_permission_denied,
                         walk_ec ) )
            {
                if( walk_ec )
                    break;
                if( f.is_regular_file( walk_ec ) )
                    ++files_count;
            }

            py::dict d;
            d["kind"]        = kind;
            d["identifier"]  = identifier;
            d["path"]        = sub.path().string();
            d["files_count"] = files_count;
            out.append( d );
        }
    }

    return out;
}


bool pcm_module_is_installed( const std::string& aIdentifier )
{
    if( aIdentifier.empty() )
        return false;

    py::list installed = pcm_module_list_installed();
    for( const py::handle& h : installed )
    {
        py::dict d = py::reinterpret_borrow<py::dict>( h );
        if( d.contains( "identifier" )
            && d["identifier"].cast<std::string>() == aIdentifier )
        {
            return true;
        }
    }
    return false;
}


std::string pcm_module_get_packages_dir()
{
    return pcm_find_packages_dir().string();
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_pcm, m )
{
    m.doc() = "KliCAD Plugin Content Manager binding — read-only filesystem "
              "walk over the 3rdparty/ tree managed by KiCad's PCM.  Lists "
              "installed packages from installed_packages.json plus any "
              "subdirectory-only fallback, and enumerates plugins/footprints/"
              "symbols/3dmodels/etc. by package_kind.  Does NOT install, "
              "uninstall, or refresh repository metadata — use the PCM dialog "
              "in the KiCad GUI for those (they need network I/O + threaded "
              "progress UI and are out of scope for this libkicommon binding).";

    m.def( "list_installed", &pcm_module_list_installed,
           R"DOC(Return the list of installed PCM packages.

Reads installed_packages.json (preferred from <3rdparty>/installed_packages.json,
falling back to the user settings dir copy that KiCad's PCM actually writes),
then fills in any subdirectory-only packages by walking <3rdparty>/<kind>/.

Each entry is a dict:
    {
        identifier:      str,          # e.g. "com.github.user.plugin"
        name:            str,
        description:     str,
        version:         str,          # current_version from manifest, or "0.0"
        repository_id:   str,
        repository_name: str,          # "<unknown>" for fs-walk fallbacks
        pinned:          bool,
        install_date:    int | None,   # unix timestamp
        installed_path:  str | None,   # <3rdparty>/<kind>/<id> if found on disk
        kicad_versions:  [ {version, kicad_version, kicad_version_max, status}, ... ]
    }
)DOC" );

    m.def( "list_packages_dir", &pcm_module_list_packages_dir,
           py::arg( "package_kind" ) = std::string( "all" ),
           R"DOC(Walk the per-kind subdirs of the 3rdparty packages tree.

`package_kind` is 'all' (default) or one of: plugins, footprints, 3dmodels,
symbols, resources, colors, templates, scripts.

Each entry is a dict {kind, identifier, path, files_count} where
`files_count` is the recursive count of regular files under `path`.

Note that the SAME package may appear multiple times if it ships content
under more than one kind (e.g. plugins/ + footprints/).
)DOC" );

    m.def( "is_installed", &pcm_module_is_installed,
           py::arg( "identifier" ),
           "Return True iff a package with the given identifier appears in "
           "list_installed()." );

    m.def( "get_packages_dir", &pcm_module_get_packages_dir,
           R"DOC(Return the resolved 3rdparty packages directory.

Resolution order:
    1. KICAD<MAJOR>_3RD_PARTY from KiCad's env-var map (kicad_common.json).
    2. KICAD<MAJOR>_3RD_PARTY from the process environment.
    3. ~/Documents/KiCad/<MAJOR.MINOR>/3rdparty (default).

The returned path is absolute and lexically normalised.  It may not exist
on disk if no packages have been installed yet.
)DOC" );
}
