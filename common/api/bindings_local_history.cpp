/*
 * KliCAD subsystem binding: local history (git-backed project versioning).
 *
 * Exposes KiCad's LOCAL_HISTORY — the libgit2-backed autosave/snapshot system
 * that lives in `<project>/.history/` — so scripts can list, inspect, and roll
 * back to prior project states.
 *
 * Module: klicad_native_local_history
 *
 *   init(project_dir='')                       -> {ok, project_dir, created}
 *   list_commits(project_dir='', max=100)      -> [{hash, message, timestamp_iso,
 *                                                  author, summary, files_changed}]
 *   get_commit(commit_hash, project_dir='')    -> {hash, message, timestamp_iso,
 *                                                  author, summary,
 *                                                  modified_files: [str, ...]}
 *   restore_commit(commit_hash, project_dir='') -> {ok, restored_hash}
 *                                                  (destructive; calls
 *                                                  LOCAL_HISTORY::RestoreCommit)
 *   get_current_state(project_dir='')          -> {is_clean, modified_files,
 *                                                  head_commit_hash}
 *
 * Notes:
 *   - Empty project_dir resolves to Pgm().GetSettingsManager().Prj()
 *       .GetProjectDirectory().
 *   - LOCAL_HISTORY owns its own libgit2 repo at `<project>/.history/`. For
 *     read-only operations (list_commits / get_commit / get_current_state) we
 *     talk to libgit2 directly — LOCAL_HISTORY exposes only LoadSnapshots()
 *     privately, and we need richer fields (author, ISO timestamp, per-commit
 *     file list) than its public surface offers.
 *   - restore_commit is destructive. It calls LOCAL_HISTORY::RestoreCommit with
 *     aParent=nullptr so it never pops a wxMessageBox; on failure (locked
 *     files, missing commit, etc.) it raises RuntimeError with whatever signal
 *     we can recover.
 *   - The live LOCAL_HISTORY instance hangs off KIWAY (one per process via
 *     Kiway().LocalHistory()).  When a KIWAY is reachable we use that instance
 *     so any in-memory savers/pending state are honoured; in the headless
 *     api-server case we fall back to a local stack-scoped LOCAL_HISTORY whose
 *     Init/RestoreCommit/etc. operate purely on disk.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <project.h>
#include <local_history.h>
#include <kiway.h>
#include <kiway_holder.h>

#include <git2.h>

#include <wx/string.h>
#include <wx/filename.h>
#include <wx/datetime.h>
#include <wx/window.h>

#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Walk the live wxTopLevelWindows for a KIWAY_HOLDER with a live KIWAY.  The
// LOCAL_HISTORY instance is owned by KIWAY (one per process); if no GUI frame
// is up we fall back to a stack-scoped LOCAL_HISTORY in the call site.
KIWAY* find_kiway_for_local_history()
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


// Resolve the project directory: explicit arg wins, else fall back to the
// active project via SETTINGS_MANAGER.  Always returns a path WITHOUT trailing
// separator (wx-normalised) — LOCAL_HISTORY internally re-normalises anyway.
wxString resolve_project_dir( const std::string& aArg )
{
    if( !aArg.empty() )
    {
        wxString p = wxString::FromUTF8( aArg );
        if( p.EndsWith( wxFileName::GetPathSeparator() ) )
            p.RemoveLast();
        return p;
    }

    PGM_BASE* pgm = PgmOrNull();
    if( !pgm )
        throw std::runtime_error( "no PGM_BASE — KiCad isn't initialised; "
                                  "pass project_dir explicitly" );

    PROJECT& prj = pgm->GetSettingsManager().Prj();
    wxString dir = prj.GetProjectDirectory();

    if( dir.IsEmpty() )
        throw std::runtime_error( "no active project — pass project_dir explicitly" );

    if( dir.EndsWith( wxFileName::GetPathSeparator() ) )
        dir.RemoveLast();
    return dir;
}


// Return a LOCAL_HISTORY reference usable for this call.  Prefer the KIWAY-
// owned instance (preserves savers + pending state) but fall back to a
// caller-owned one (held in `aFallback`) when running headless.
LOCAL_HISTORY& find_local_history_for_project( std::unique_ptr<LOCAL_HISTORY>& aFallback )
{
    if( KIWAY* kw = find_kiway_for_local_history() )
        return kw->LocalHistory();

    if( !aFallback )
        aFallback = std::make_unique<LOCAL_HISTORY>();
    return *aFallback;
}


// Format a unix time_t (UTC seconds since epoch) as ISO-8601 in UTC.
std::string iso_from_git_time( git_time_t aT )
{
    wxDateTime d( static_cast<time_t>( aT ) );
    // FormatISOCombined uses 'T' separator. We append 'Z' since git stores UTC
    // (commit time + offset; here we report the absolute UTC instant).
    return ( d.ToUTC().FormatISOCombined( 'T' ) + wxS( "Z" ) ).ToStdString();
}


// RAII helper: open the .history git repo for a project. Throws if missing.
struct OpenedRepo
{
    git_repository* repo = nullptr;
    wxString        history_path;

    OpenedRepo( const wxString& aProjectDir )
    {
        wxFileName p( aProjectDir, wxEmptyString );
        p.AppendDir( wxS( ".history" ) );
        history_path = p.GetPath();

        if( !wxDirExists( history_path ) )
            throw std::runtime_error( "no local history for project '"
                                      + aProjectDir.ToStdString()
                                      + "' (no .history/ directory) — call init() first" );

        int rc = git_repository_open( &repo, history_path.mb_str().data() );
        if( rc != 0 )
        {
            const git_error* err = git_error_last();
            throw std::runtime_error(
                    std::string( "git_repository_open failed: " )
                    + ( err ? err->message : "unknown" ) );
        }
    }

    ~OpenedRepo()
    {
        if( repo )
            git_repository_free( repo );
    }

    OpenedRepo( const OpenedRepo& ) = delete;
    OpenedRepo& operator=( const OpenedRepo& ) = delete;
};


// Extract "summary" (first line up to ':' if formatted "<title>: <n> files
// changed", otherwise the full first line) — mirrors LoadSnapshots.
std::string extract_summary( const std::string& aMessage )
{
    auto nl = aMessage.find( '\n' );
    std::string firstLine = ( nl == std::string::npos ) ? aMessage : aMessage.substr( 0, nl );
    auto colon = firstLine.find( ':' );
    if( colon == std::string::npos )
        return firstLine;
    return firstLine.substr( 0, colon );
}


// Build per-commit file-list by diffing against first parent (or against empty
// tree for the root commit). Returns posix-style paths.
std::vector<std::string> commit_files_changed( git_repository* aRepo, git_commit* aCommit )
{
    std::vector<std::string> out;

    git_tree* commit_tree = nullptr;
    if( git_commit_tree( &commit_tree, aCommit ) != 0 )
        return out;

    git_tree* parent_tree = nullptr;
    git_commit* parent = nullptr;

    if( git_commit_parentcount( aCommit ) > 0 )
    {
        if( git_commit_parent( &parent, aCommit, 0 ) == 0 )
            git_commit_tree( &parent_tree, parent );
    }

    git_diff* diff = nullptr;
    if( git_diff_tree_to_tree( &diff, aRepo, parent_tree, commit_tree, nullptr ) == 0 )
    {
        size_t n = git_diff_num_deltas( diff );
        out.reserve( n );
        for( size_t i = 0; i < n; ++i )
        {
            const git_diff_delta* d = git_diff_get_delta( diff, i );
            if( d && d->new_file.path )
                out.emplace_back( d->new_file.path );
            else if( d && d->old_file.path )
                out.emplace_back( d->old_file.path );
        }
        git_diff_free( diff );
    }

    if( parent_tree ) git_tree_free( parent_tree );
    if( parent )      git_commit_free( parent );
    if( commit_tree ) git_tree_free( commit_tree );
    return out;
}


py::dict make_commit_dict( git_repository* aRepo, git_commit* aCommit, bool aIncludeFiles )
{
    const git_oid* oid = git_commit_id( aCommit );
    std::string hash = git_oid_tostr_s( oid );

    const char* msg_raw = git_commit_message( aCommit );
    std::string msg = msg_raw ? msg_raw : "";

    const git_signature* sig = git_commit_author( aCommit );
    std::string author;
    if( sig )
    {
        author = std::string( sig->name ? sig->name : "" );
        if( sig->email && sig->email[0] )
        {
            author += " <";
            author += sig->email;
            author += ">";
        }
    }

    py::dict d;
    d["hash"]          = hash;
    d["message"]       = msg;
    d["summary"]       = extract_summary( msg );
    d["timestamp_iso"] = iso_from_git_time( git_commit_time( aCommit ) );
    d["author"]        = author;

    if( aIncludeFiles )
        d["modified_files"] = commit_files_changed( aRepo, aCommit );

    return d;
}


// ----- exposed operations ------------------------------------------------

py::dict lh_init( const std::string& aProjectDir )
{
    wxString projDir = resolve_project_dir( aProjectDir );

    wxFileName histPath( projDir, wxEmptyString );
    histPath.AppendDir( wxS( ".history" ) );
    bool existedBefore = wxDirExists( histPath.GetPath() );

    std::unique_ptr<LOCAL_HISTORY> fallback;
    LOCAL_HISTORY& lh = find_local_history_for_project( fallback );

    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = lh.Init( projDir );
    }

    py::dict r;
    r["ok"]          = ok;
    r["project_dir"] = projDir.ToStdString();
    r["created"]     = ok && !existedBefore;
    return r;
}


py::list lh_list_commits( const std::string& aProjectDir, int aMax )
{
    if( aMax < 0 )
        throw std::invalid_argument( "max must be >= 0" );

    wxString projDir = resolve_project_dir( aProjectDir );

    py::list out;

    OpenedRepo repo( projDir );

    git_revwalk* walk = nullptr;
    if( git_revwalk_new( &walk, repo.repo ) != 0 )
        throw std::runtime_error( "git_revwalk_new failed" );

    std::unique_ptr<git_revwalk, decltype( &git_revwalk_free )> walkPtr( walk, &git_revwalk_free );

    git_revwalk_sorting( walk, GIT_SORT_TIME );

    if( git_revwalk_push_head( walk ) != 0 )
    {
        // No HEAD yet — empty history.
        return out;
    }

    git_oid oid;
    int count = 0;
    while( git_revwalk_next( &oid, walk ) == 0 )
    {
        if( aMax > 0 && count >= aMax )
            break;

        git_commit* c = nullptr;
        if( git_commit_lookup( &c, repo.repo, &oid ) != 0 )
            continue;

        // Don't slurp file list for the listing path — too expensive for big
        // histories; callers should use get_commit() for that.
        py::dict d = make_commit_dict( repo.repo, c, /*aIncludeFiles=*/false );

        // We still expose a cheap files_changed count: parse it from the
        // message tail like LoadSnapshots does, but only as best-effort.
        d["files_changed"] = py::none();

        out.append( d );
        git_commit_free( c );
        ++count;
    }

    return out;
}


py::dict lh_get_commit( const std::string& aHash, const std::string& aProjectDir )
{
    if( aHash.empty() )
        throw std::invalid_argument( "commit_hash must be non-empty" );

    wxString projDir = resolve_project_dir( aProjectDir );
    OpenedRepo repo( projDir );

    git_oid oid;
    if( git_oid_fromstr( &oid, aHash.c_str() ) != 0 )
        throw std::runtime_error( "invalid commit hash: '" + aHash + "'" );

    git_commit* c = nullptr;
    if( git_commit_lookup( &c, repo.repo, &oid ) != 0 )
        throw std::runtime_error( "commit not found: '" + aHash + "'" );

    std::unique_ptr<git_commit, decltype( &git_commit_free )> cPtr( c, &git_commit_free );

    return make_commit_dict( repo.repo, c, /*aIncludeFiles=*/true );
}


py::dict lh_restore_commit( const std::string& aHash, const std::string& aProjectDir )
{
    if( aHash.empty() )
        throw std::invalid_argument( "commit_hash must be non-empty" );

    wxString projDir = resolve_project_dir( aProjectDir );

    // Validate the commit exists up front so we can throw a useful Python
    // error.  RestoreCommit returns plain bool on any failure (locked files,
    // missing commit, lock contention) — we want better signal than that.
    {
        OpenedRepo repo( projDir );

        git_oid oid;
        if( git_oid_fromstr( &oid, aHash.c_str() ) != 0 )
            throw std::runtime_error( "invalid commit hash: '" + aHash + "'" );

        git_commit* c = nullptr;
        if( git_commit_lookup( &c, repo.repo, &oid ) != 0 )
            throw std::runtime_error( "commit not found: '" + aHash + "'" );
        git_commit_free( c );
    }

    std::unique_ptr<LOCAL_HISTORY> fallback;
    LOCAL_HISTORY& lh = find_local_history_for_project( fallback );

    bool ok;
    {
        // RestoreCommit walks the project, acquires a file lock, extracts the
        // tree to a temp dir, swaps atomically.  Plenty of I/O — drop the GIL.
        // aParent=nullptr suppresses the wxMessageBox path.
        py::gil_scoped_release nogil;
        ok = lh.RestoreCommit( projDir, wxString::FromUTF8( aHash ), nullptr );
    }

    if( !ok )
        throw std::runtime_error(
                "RestoreCommit failed — common causes: open files in editor "
                "(release locks first), history lock contention, or insufficient "
                "permissions on the project directory" );

    py::dict r;
    r["ok"]            = true;
    r["restored_hash"] = aHash;
    r["project_dir"]   = projDir.ToStdString();
    return r;
}


py::dict lh_get_current_state( const std::string& aProjectDir )
{
    wxString projDir = resolve_project_dir( aProjectDir );

    py::dict r;
    r["head_commit_hash"] = py::none();
    r["is_clean"]         = true;
    r["modified_files"]   = py::list();

    wxFileName histPath( projDir, wxEmptyString );
    histPath.AppendDir( wxS( ".history" ) );

    if( !wxDirExists( histPath.GetPath() ) )
    {
        // No history initialised — treat as a clean / empty state with no head.
        r["project_dir"] = projDir.ToStdString();
        r["history_exists"] = false;
        return r;
    }

    OpenedRepo repo( projDir );
    r["history_exists"] = true;

    // HEAD hash.
    git_oid head_oid;
    if( git_reference_name_to_id( &head_oid, repo.repo, "HEAD" ) == 0 )
        r["head_commit_hash"] = std::string( git_oid_tostr_s( &head_oid ) );

    // Re-point workdir at the project directory so git_status walks the real
    // files (the repo lives in .history/; LOCAL_HISTORY re-sets workdir before
    // each commit and we have to do the same to mirror that view).
    if( git_repository_set_workdir( repo.repo, projDir.mb_str().data(), 0 ) != 0 )
    {
        // Can't probe modified files — fall through with what we have.
        r["project_dir"] = projDir.ToStdString();
        return r;
    }

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show  = GIT_STATUS_SHOW_WORKDIR_ONLY;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
               | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
               | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

    git_status_list* status_list = nullptr;
    if( git_status_list_new( &status_list, repo.repo, &opts ) == 0 )
    {
        py::list modified;
        size_t n = git_status_list_entrycount( status_list );
        for( size_t i = 0; i < n; ++i )
        {
            const git_status_entry* e = git_status_byindex( status_list, i );
            if( !e || e->status == GIT_STATUS_CURRENT )
                continue;

            const char* path = nullptr;
            if( e->index_to_workdir && e->index_to_workdir->new_file.path )
                path = e->index_to_workdir->new_file.path;
            else if( e->head_to_index && e->head_to_index->new_file.path )
                path = e->head_to_index->new_file.path;

            if( path )
                modified.append( std::string( path ) );
        }
        git_status_list_free( status_list );

        r["modified_files"] = modified;
        r["is_clean"]       = ( py::len( modified ) == 0 );
    }

    r["project_dir"] = projDir.ToStdString();
    return r;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_local_history, m )
{
    m.doc() = "KliCAD local-history binding — git-backed project versioning at "
              "<project>/.history/.  Lists snapshots, inspects commits, and "
              "rolls the project back via LOCAL_HISTORY.  Empty project_dir "
              "resolves to the currently active project.";

    m.def( "init", &lh_init,
           py::arg( "project_dir" ) = std::string(),
           R"DOC(Initialise / open the local history repository for a project.

If `project_dir` is empty, uses the active project from SETTINGS_MANAGER.
Creates `.history/` and the libgit2 repo if missing; on a brand-new repo
LOCAL_HISTORY also takes an initial snapshot of all project files and tags
it as the first manual save.

Returns: {ok, project_dir, created} — `created` is True if .history/ did not
exist before this call.

Raises RuntimeError if no project_dir can be resolved.
)DOC" );

    m.def( "list_commits", &lh_list_commits,
           py::arg( "project_dir" ) = std::string(),
           py::arg( "max" )         = 100,
           R"DOC(List snapshots in the local history, newest first.

Returns a list of dicts:
    {hash, message, summary, timestamp_iso, author, files_changed}

`files_changed` is currently None in the listing (computing per-commit diffs
for every commit would be expensive); call `get_commit(hash)` for the full
file list.

`max=0` returns all commits.
)DOC" );

    m.def( "get_commit", &lh_get_commit,
           py::arg( "commit_hash" ),
           py::arg( "project_dir" ) = std::string(),
           R"DOC(Return full info for one commit, including modified file paths.

Returns:
    {hash, message, summary, timestamp_iso, author, modified_files: [str, ...]}

`modified_files` is the diff against the first parent (or against the empty
tree for the root commit), paths relative to the project root in POSIX form.

Raises RuntimeError on invalid/missing hash.
)DOC" );

    m.def( "restore_commit", &lh_restore_commit,
           py::arg( "commit_hash" ),
           py::arg( "project_dir" ) = std::string(),
           R"DOC(Destructively roll the project back to a specific commit.

Wraps LOCAL_HISTORY::RestoreCommit:
  1. Refuses if any project file is locked (open in an editor on this or
     another machine).
  2. Acquires the .history file/index lock.
  3. Auto-creates a "Pre-restore backup" snapshot of the current state.
  4. Extracts the target tree to <project>_restore_temp.
  5. Atomically swaps temp/current; deletes files not in the target tree.
  6. Records the restore in history.

Returns: {ok: True, restored_hash, project_dir}

Raises RuntimeError if the commit is missing, files are locked, or the
underlying restore failed for any reason (KiCad does not surface fine-grained
error codes from RestoreCommit — diagnose via the trace log).
)DOC" );

    m.def( "get_current_state", &lh_get_current_state,
           py::arg( "project_dir" ) = std::string(),
           R"DOC(Snapshot of where the working project stands vs. the history HEAD.

Returns:
    {head_commit_hash, is_clean, modified_files: [str, ...],
     project_dir, history_exists}

`modified_files` is computed by `git_status` against HEAD with the workdir
set to the project directory (LOCAL_HISTORY mirrors the project into its
git repo for snapshots, so untracked files in the project also show up
here as "modified" — that's by design).

If `.history/` doesn't exist yet, returns `is_clean=True`, empty
modified_files, `head_commit_hash=None`, and `history_exists=False`.
)DOC" );
}
