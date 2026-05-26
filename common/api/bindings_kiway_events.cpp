/*
 * KliCAD subsystem binding: KIWAY mail events.
 *
 * KIWAY mail (KIWAY_EXPRESS / EDA_KIWAY_MAIL_RECEIVED) is the in-process,
 * cross-frame messaging channel KiCad uses to communicate between the SCH
 * and PCB editors (and CvPcb, etc.) about cross-probing, netlist updates,
 * footprint reassignment, refresh requests, and so on.
 *
 * This binding bridges that channel out to scripted Python via a simple
 * pull-based queue:
 *
 *   1. subscribe()           installs a passive observer on every live
 *                            KIWAY_PLAYER's wxEvtHandler.  The observer
 *                            pushes (mail_type, payload, source) records
 *                            to a thread-safe FIFO and calls Skip() so the
 *                            normal handler chain still runs.
 *   2. get_pending_events()  pops up to N records off the FIFO.
 *   3. unsubscribe()         unbinds the observer and clears the FIFO.
 *   4. send_mail()           wraps KIWAY::ExpressMail() — fires-and-forgets
 *                            a mail event at a destination frame.
 *
 * This is the "no-streaming, no-callbacks" path that fits the bundle
 * paradigm (scripted clients poll on demand).  A true push channel for
 * external clients is a separate, larger change.
 *
 * Module: klicad_native_kiway_events
 *
 *   list_mail_types() -> list[str]
 *   subscribe(types=None) -> dict
 *   unsubscribe() -> dict
 *   get_pending_events(max=100) -> list[dict]
 *   send_mail(dest_frame, mail_type, payload='') -> dict
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <eda_base_frame.h>
#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_mail.h>
#include <mail_type.h>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/window.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace py = pybind11;

namespace
{

// -----------------------------------------------------------------------------
// MAIL_T <-> string mapping.  Hand-built to avoid pulling in magic_enum.  Must
// stay in sync with include/mail_type.h.  Add new entries here when the enum
// gains members upstream.
// -----------------------------------------------------------------------------

struct MailTypeEntry
{
    MAIL_T      value;
    const char* name;
};

const std::vector<MailTypeEntry>& mail_type_entries()
{
    static const std::vector<MailTypeEntry> entries = {
        { MAIL_CROSS_PROBE,            "MAIL_CROSS_PROBE" },
        { MAIL_SELECTION,              "MAIL_SELECTION" },
        { MAIL_SELECTION_FORCE,        "MAIL_SELECTION_FORCE" },
        { MAIL_ASSIGN_FOOTPRINTS,      "MAIL_ASSIGN_FOOTPRINTS" },
        { MAIL_SCH_SAVE,               "MAIL_SCH_SAVE" },
        { MAIL_EESCHEMA_NETLIST,       "MAIL_EESCHEMA_NETLIST" },
        { MAIL_SYMBOL_NETLIST,         "MAIL_SYMBOL_NETLIST" },
        { MAIL_PCB_UPDATE,             "MAIL_PCB_UPDATE" },
        { MAIL_SCH_UPDATE,             "MAIL_SCH_UPDATE" },
        { MAIL_IMPORT_FILE,            "MAIL_IMPORT_FILE" },
        { MAIL_SCH_GET_NETLIST,        "MAIL_SCH_GET_NETLIST" },
        { MAIL_SCH_GET_ITEM,           "MAIL_SCH_GET_ITEM" },
        { MAIL_PCB_GET_NETLIST,        "MAIL_PCB_GET_NETLIST" },
        { MAIL_PCB_UPDATE_LINKS,       "MAIL_PCB_UPDATE_LINKS" },
        { MAIL_SCH_REFRESH,            "MAIL_SCH_REFRESH" },
        { MAIL_ADD_LOCAL_LIB,          "MAIL_ADD_LOCAL_LIB" },
        { MAIL_LIB_EDIT,               "MAIL_LIB_EDIT" },
        { MAIL_FP_EDIT,                "MAIL_FP_EDIT" },
        { MAIL_RELOAD_LIB,             "MAIL_RELOAD_LIB" },
        { MAIL_RELOAD_PLUGINS,         "MAIL_RELOAD_PLUGINS" },
        { MAIL_REFRESH_SYMBOL,         "MAIL_REFRESH_SYMBOL" },
        { MAIL_SCH_NAVIGATE_TO_SHEET,  "MAIL_SCH_NAVIGATE_TO_SHEET" },
    };
    return entries;
}


std::string mail_type_to_string( MAIL_T aType )
{
    for( const MailTypeEntry& e : mail_type_entries() )
        if( e.value == aType )
            return e.name;

    // Fallback for unmapped values (e.g., enum extended upstream without
    // this TU updated yet).  Surface the raw int so the caller still has
    // something usable.
    return "MAIL_UNKNOWN_" + std::to_string( static_cast<int>( aType ) );
}


bool string_to_mail_type( const std::string& aName, MAIL_T& aOut )
{
    for( const MailTypeEntry& e : mail_type_entries() )
    {
        if( aName == e.name )
        {
            aOut = e.value;
            return true;
        }
    }
    return false;
}


// -----------------------------------------------------------------------------
// FRAME_T <-> string mapping.  Mirrors bindings_gui.cpp::frame_name_table()
// but kept local so the two TUs don't have to share a header.  Same aliases
// accepted on send_mail()'s dest_frame argument.
// -----------------------------------------------------------------------------

const std::unordered_map<std::string, FRAME_T>& kiway_events_frame_name_table()
{
    static const std::unordered_map<std::string, FRAME_T> map = {
        { "schematic",        FRAME_SCH },
        { "schematic_editor", FRAME_SCH },
        { "sch",              FRAME_SCH },
        { "eeschema",         FRAME_SCH },
        { "symbol_editor",    FRAME_SCH_SYMBOL_EDITOR },
        { "symbol_viewer",    FRAME_SCH_VIEWER },
        { "symbol_chooser",   FRAME_SYMBOL_CHOOSER },
        { "simulator",        FRAME_SIMULATOR },
        { "spice",            FRAME_SIMULATOR },
        { "sch_diff",         FRAME_SCH_DIFF },
        { "symbol_diff",      FRAME_SYM_DIFF },

        { "pcb",              FRAME_PCB_EDITOR },
        { "pcb_editor",       FRAME_PCB_EDITOR },
        { "pcbnew",           FRAME_PCB_EDITOR },
        { "footprint_editor", FRAME_FOOTPRINT_EDITOR },
        { "footprint_chooser",FRAME_FOOTPRINT_CHOOSER },
        { "footprint_viewer", FRAME_FOOTPRINT_VIEWER },
        { "footprint_wizard", FRAME_FOOTPRINT_WIZARD },
        { "viewer_3d",        FRAME_PCB_DISPLAY3D },
        { "3d_viewer",        FRAME_PCB_DISPLAY3D },
        { "3d",               FRAME_PCB_DISPLAY3D },
        { "pcb_diff",         FRAME_PCB_DIFF },
        { "footprint_diff",   FRAME_FOOTPRINT_DIFF },

        { "cvpcb",            FRAME_CVPCB },

        { "gerbview",         FRAME_GERBER },
        { "gerber_viewer",    FRAME_GERBER },
        { "page_layout",      FRAME_PL_EDITOR },
        { "drawing_sheet",    FRAME_PL_EDITOR },
        { "pl_editor",        FRAME_PL_EDITOR },
        { "bitmap2component", FRAME_BM2CMP },
        { "bitmap2cmp",       FRAME_BM2CMP },
        { "calculator",       FRAME_CALC },
        { "pcb_calculator",   FRAME_CALC },
    };
    return map;
}


FRAME_T kiway_events_frame_from_string( const std::string& aName )
{
    const auto& map = kiway_events_frame_name_table();
    auto        it  = map.find( aName );
    if( it == map.end() )
        throw std::invalid_argument( "unknown dest_frame name '" + aName + "'" );
    return it->second;
}


std::string frame_type_to_string( FRAME_T aType )
{
    for( const auto& [k, v] : kiway_events_frame_name_table() )
    {
        // Return the first canonical-ish alias for each FRAME_T.  The map's
        // iteration order is unspecified but every frame has a stable
        // primary spelling listed in bindings_gui.cpp.  For source reporting
        // we just need something human-readable — uniqueness isn't required.
        if( v == aType )
            return k;
    }
    return "frame_" + std::to_string( static_cast<int>( aType ) );
}


// -----------------------------------------------------------------------------
// Event queue.  Static state, guarded by a mutex.  Singleton instance.
// -----------------------------------------------------------------------------

struct EventRecord
{
    MAIL_T      mail_type;
    std::string payload;
    std::string source;        // best-effort frame title / class / "(unknown)"
    long long   timestamp_ms;  // wall-clock since epoch
};


struct EventQueue
{
    std::mutex              mtx;
    std::deque<EventRecord> records;

    // Whitelist of mail types to capture.  Empty == all.
    std::unordered_set<int> filter;

    bool subscribed = false;

    // Track which wxWindow IDs we bound the observer onto, so we can
    // best-effort unbind cleanly.  Storing IDs (not raw wxEvtHandler*)
    // because windows may be destroyed between subscribe and unsubscribe.
    std::vector<wxWindowID> bound_window_ids;
};


EventQueue& kiway_events_get_queue()
{
    static EventQueue q;
    return q;
}


// -----------------------------------------------------------------------------
// Observer.  A free function bound via wxEvtHandler::Bind so we can capture
// KIWAY_MAIL_EVENTs as they flow into each KIWAY_PLAYER.  Calls Skip() so
// the normal KIWAY_PLAYER::kiway_express handler still runs after us.
// -----------------------------------------------------------------------------

void kiway_events_observer( KIWAY_MAIL_EVENT& aEvent )
{
    EventQueue& q = kiway_events_get_queue();

    MAIL_T cmd = aEvent.Command();

    {
        std::lock_guard<std::mutex> lock( q.mtx );

        if( !q.subscribed )
        {
            aEvent.Skip();
            return;
        }

        if( !q.filter.empty() && q.filter.find( static_cast<int>( cmd ) ) == q.filter.end() )
        {
            aEvent.Skip();
            return;
        }

        EventRecord rec;
        rec.mail_type = cmd;
        rec.payload   = aEvent.GetPayload();

        // Best-effort source description.  The event object is the wxWindow*
        // passed to ExpressMail() — usually the sending KIWAY_PLAYER frame.
        if( wxObject* obj = aEvent.GetEventObject() )
        {
            // EDA_BASE_FRAME typeinfo isn't exported from libkicommon —
            // can't dynamic_cast.  Fall back to wxWindow class name as
            // the source identifier.
            if( wxWindow* w = dynamic_cast<wxWindow*>( obj ) )
                rec.source = wxString( w->GetClassInfo()->GetClassName() ).ToStdString();
            else
                rec.source = "(non-window)";
        }
        else
        {
            rec.source = "(unknown)";
        }

        rec.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch() )
                                   .count();

        // Bounded queue — drop oldest if we ever overflow.  16k is generous
        // for interactive cross-probing rates.
        constexpr size_t MAX_QUEUE = 16384;
        if( q.records.size() >= MAX_QUEUE )
            q.records.pop_front();

        q.records.push_back( std::move( rec ) );
    }

    // Crucially: let the normal handler chain continue so KiCad's own
    // KIWAY_PLAYER::kiway_express still gets to process this event.
    aEvent.Skip();
}


// -----------------------------------------------------------------------------
// Hook install / uninstall.
// -----------------------------------------------------------------------------

KIWAY* find_live_kiway_for_kiway_events()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* h = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( h->HasKiway() )
                return &h->Kiway();
    }
    return nullptr;
}


// Bind kiway_events_observer onto every top-level KIWAY_HOLDER frame's
// event handler.  Records each window's ID so we can unbind later.
int kiway_events_install_hook()
{
    EventQueue& q = kiway_events_get_queue();
    int         count = 0;

    for( wxWindow* w : wxTopLevelWindows )
    {
        // Every KIWAY_PLAYER is a KIWAY_HOLDER AND a wxWindow (and a
        // wxEvtHandler).  We can't dynamic_cast to EDA_BASE_FRAME or
        // KIWAY_PLAYER from libkicommon (their typeinfo lives in libcommon
        // or the kifaces), but wxWindow is base and reachable.  The
        // KIWAY_HOLDER check filters out non-Kiway frames; once it passes
        // we bind on the wxWindow directly (wxWindow is a wxEvtHandler).
        KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w );

        if( !holder || !holder->HasKiway() )
            continue;

        w->Bind( EDA_KIWAY_MAIL_RECEIVED, &kiway_events_observer );
        q.bound_window_ids.push_back( w->GetId() );
        ++count;
    }

    return count;
}


// Unbind the observer from every window we recorded in bound_window_ids.
// Windows that have been destroyed in the meantime are silently skipped.
int kiway_events_uninstall_hook()
{
    EventQueue& q = kiway_events_get_queue();
    int         count = 0;

    for( wxWindowID id : q.bound_window_ids )
    {
        wxWindow* w = wxWindow::FindWindowById( id );
        if( !w )
            continue;  // window gone — its event table dies with it

        w->Unbind( EDA_KIWAY_MAIL_RECEIVED, &kiway_events_observer );
        ++count;
    }

    q.bound_window_ids.clear();
    return count;
}


// -----------------------------------------------------------------------------
// Python-visible functions.
// -----------------------------------------------------------------------------

py::list kiway_events_list_mail_types()
{
    py::list out;
    for( const MailTypeEntry& e : mail_type_entries() )
        out.append( py::str( e.name ) );
    return out;
}


py::dict kiway_events_subscribe( const std::vector<std::string>& aTypes )
{
    EventQueue& q = kiway_events_get_queue();

    py::dict result;
    py::list subscribed_types;

    {
        std::lock_guard<std::mutex> lock( q.mtx );

        // Build filter set.  Empty input = no filter (capture everything).
        q.filter.clear();
        for( const std::string& name : aTypes )
        {
            MAIL_T mt;
            if( !string_to_mail_type( name, mt ) )
                throw std::invalid_argument(
                        "unknown mail type '" + name + "' — see list_mail_types()" );
            q.filter.insert( static_cast<int>( mt ) );
            subscribed_types.append( py::str( name ) );
        }

        // If no filter, list all known types in the response for clarity.
        if( aTypes.empty() )
        {
            for( const MailTypeEntry& e : mail_type_entries() )
                subscribed_types.append( py::str( e.name ) );
        }

        // If we were already subscribed, tear down the old binding first.
        // (Easier than diff-ing; subscribe is idempotent this way.)
        if( q.subscribed )
        {
            // Mutex must be released before calling install/uninstall, which
            // walk wxTopLevelWindows and call into wx — keep this lock as
            // narrow as possible.  Defer to after the lock-guard scope.
        }

        q.subscribed = true;
    }

    // Re-install: uninstall stale binds (if any), then bind on every live
    // frame.  Outside the lock — install_hook may touch wx event tables.
    if( !q.bound_window_ids.empty() )
        kiway_events_uninstall_hook();

    int n_bound = kiway_events_install_hook();

    result["ok"]               = true;
    result["subscribed_types"] = subscribed_types;
    result["frames_hooked"]    = n_bound;

    return result;
}


py::dict kiway_events_unsubscribe()
{
    EventQueue& q = kiway_events_get_queue();

    int n_unbound = kiway_events_uninstall_hook();

    {
        std::lock_guard<std::mutex> lock( q.mtx );
        q.subscribed = false;
        q.filter.clear();
        q.records.clear();
    }

    py::dict result;
    result["ok"]              = true;
    result["frames_unhooked"] = n_unbound;
    return result;
}


py::list kiway_events_get_pending( int aMax )
{
    if( aMax < 0 )
        throw std::invalid_argument( "max must be >= 0" );

    EventQueue& q = kiway_events_get_queue();

    std::vector<EventRecord> popped;
    {
        std::lock_guard<std::mutex> lock( q.mtx );

        size_t n = std::min<size_t>( static_cast<size_t>( aMax ), q.records.size() );
        popped.reserve( n );

        for( size_t i = 0; i < n; ++i )
        {
            popped.push_back( std::move( q.records.front() ) );
            q.records.pop_front();
        }
    }

    py::list out;
    for( const EventRecord& rec : popped )
    {
        py::dict d;
        d["mail_type"]    = mail_type_to_string( rec.mail_type );
        d["payload"]      = rec.payload;
        d["source"]       = rec.source;
        d["timestamp_ms"] = rec.timestamp_ms;
        out.append( d );
    }
    return out;
}


py::dict kiway_events_send_mail( const std::string& aDestFrame,
                                 const std::string& aMailType,
                                 const std::string& aPayload )
{
    py::dict result;
    result["ok"] = false;

    KIWAY* kiway = find_live_kiway_for_kiway_events();
    if( !kiway )
    {
        result["error"] = std::string( "no live KIWAY — is KiCad's GUI running?" );
        return result;
    }

    FRAME_T dest;
    try
    {
        dest = kiway_events_frame_from_string( aDestFrame );
    }
    catch( const std::exception& e )
    {
        result["error"] = std::string( e.what() );
        return result;
    }

    MAIL_T cmd;
    if( !string_to_mail_type( aMailType, cmd ) )
    {
        result["error"] = std::string( "unknown mail type '" + aMailType
                                       + "' — see list_mail_types()" );
        return result;
    }

    // ExpressMail takes payload by non-const reference (the KIWAY_MAIL_EVENT
    // holds a reference to it).  Copy into a local so we don't lose ownership
    // of the caller's string.  Synchronous path — ProcessEvent returns before
    // ExpressMail does.
    std::string payload_copy = aPayload;
    {
        py::gil_scoped_release nogil;
        kiway->ExpressMail( dest, cmd, payload_copy, /*aSource=*/ nullptr,
                            /*aFromOtherThread=*/ false );
    }

    result["ok"]         = true;
    result["dest_frame"] = aDestFrame;
    result["mail_type"]  = aMailType;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_kiway_events, m )
{
    m.doc() = "KliCAD KIWAY mail events binding.  Subscribe to KIWAY_EXPRESS "
              "events (cross-probe, netlist updates, refresh requests, ...) "
              "via a pull-based FIFO, and send mails via KIWAY::ExpressMail.";

    m.def( "list_mail_types", &kiway_events_list_mail_types,
           R"DOC(Return the list of all MAIL_T enum value names.

These are the strings accepted by subscribe()'s 'types' arg and by send_mail()'s
'mail_type' arg.  Example values: 'MAIL_CROSS_PROBE', 'MAIL_SCH_GET_NETLIST',
'MAIL_PCB_UPDATE', 'MAIL_SCH_REFRESH', ...
)DOC" );

    m.def( "subscribe", &kiway_events_subscribe,
           py::arg( "types" ) = std::vector<std::string>{},
           R"DOC(Install an in-process hook that captures KIWAY mail events
into a FIFO queue.  Drain it with get_pending_events().

Args:
    types: list of MAIL_T name strings to filter on.  Empty/None = capture all.

Returns:
    {ok: True, subscribed_types: [str, ...], frames_hooked: int}

Idempotent: re-calling re-installs hooks on every currently-live KIWAY_PLAYER
frame.  Newly-spawned frames after subscribe() will NOT receive the hook
unless you call subscribe() again.  (This is a known limitation of the
pull-based design; a frame-creation hook would be a separate change.)

Raises ValueError on unknown mail type name.
)DOC" );

    m.def( "unsubscribe", &kiway_events_unsubscribe,
           R"DOC(Remove the event hook from every frame it was bound to and
clear the pending event queue.

Returns:
    {ok: True, frames_unhooked: int}

Safe to call without a prior subscribe() — becomes a no-op.
)DOC" );

    m.def( "get_pending_events", &kiway_events_get_pending,
           py::arg( "max" ) = 100,
           R"DOC(Pop up to `max` events from the FIFO and return them.

Each event is a dict:
    {
        mail_type: str,        # e.g. 'MAIL_CROSS_PROBE'
        payload:   str,        # raw payload text (often s-expression)
        source:    str,        # best-effort sending frame name
        timestamp_ms: int,     # wall-clock millis since epoch
    }

Non-blocking — returns immediately, possibly with an empty list.

Internal cap: queue holds at most 16384 events; oldest are dropped on overflow.
)DOC" );

    m.def( "send_mail", &kiway_events_send_mail,
           py::arg( "dest_frame" ),
           py::arg( "mail_type" ),
           py::arg( "payload" ) = std::string(),
           R"DOC(Send a KIWAY mail to the named destination frame.

Args:
    dest_frame: frame name string ('sch', 'pcb', 'cvpcb', ...) — see
                klicad_native_gui frame name table for the full set.
    mail_type:  MAIL_T name string — see list_mail_types().
    payload:    raw payload string; usually s-expression text.  Defaults to ''.

Returns:
    {ok: True, dest_frame, mail_type}  on success
    {ok: False, error: str}            on failure

Dispatched synchronously (ProcessEvent, not QueueEvent).  If the destination
frame isn't currently alive, the mail is silently dropped by KIWAY — that's
not flagged as an error here.
)DOC" );
}
