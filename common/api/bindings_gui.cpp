/*
 * KliCAD subsystem binding: GUI frame management.
 *
 * Wraps KIWAY::Player(FRAME_T, true) so scripted clients can pop up any
 * KiCad window programmatically — schematic editor, simulator, 3D viewer,
 * gerber viewer, page layout editor, footprint editor, calculator, etc.
 *
 *   from kipy import KiCad
 *   k = KiCad()
 *   k.run_python("import klicad_native_gui as g; g.show_frame('simulator')")
 *
 * Frame-name strings are stable identifiers we define here — they do NOT
 * match KiCad's internal FRAME_T enum names verbatim, but are documented in
 * the m.def docstring and listed below.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace
{

KIWAY* find_live_kiway_for_gui()
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


// String → FRAME_T mapping.  Names are KliCAD-stable; comment notes the
// upstream enum.  Multiple aliases are accepted for the common ones.
const std::unordered_map<std::string, FRAME_T>& frame_name_table()
{
    static const std::unordered_map<std::string, FRAME_T> map = {
        // Schematic side
        { "schematic",        FRAME_SCH },              // eeschema main editor
        { "schematic_editor", FRAME_SCH },
        { "eeschema",         FRAME_SCH },
        { "symbol_editor",    FRAME_SCH_SYMBOL_EDITOR },
        { "symbol_viewer",    FRAME_SCH_VIEWER },
        { "symbol_chooser",   FRAME_SYMBOL_CHOOSER },
        { "simulator",        FRAME_SIMULATOR },        // SPICE
        { "spice",            FRAME_SIMULATOR },
        { "sch_diff",         FRAME_SCH_DIFF },
        { "symbol_diff",      FRAME_SYM_DIFF },

        // PCB side
        { "pcb",              FRAME_PCB_EDITOR },       // pcbnew main editor
        { "pcb_editor",       FRAME_PCB_EDITOR },
        { "pcbnew",           FRAME_PCB_EDITOR },
        { "footprint_editor", FRAME_FOOTPRINT_EDITOR },
        { "footprint_chooser",FRAME_FOOTPRINT_CHOOSER },
        { "footprint_viewer", FRAME_FOOTPRINT_VIEWER },
        { "footprint_wizard", FRAME_FOOTPRINT_WIZARD },
        { "viewer_3d",        FRAME_PCB_DISPLAY3D },    // raytraced 3D viewer
        { "3d_viewer",        FRAME_PCB_DISPLAY3D },
        { "3d",               FRAME_PCB_DISPLAY3D },
        { "pcb_diff",         FRAME_PCB_DIFF },
        { "footprint_diff",   FRAME_FOOTPRINT_DIFF },

        // CvPcb (component-to-footprint linker)
        { "cvpcb",            FRAME_CVPCB },

        // Auxiliary tools
        { "gerbview",         FRAME_GERBER },           // gerber viewer
        { "gerber_viewer",    FRAME_GERBER },
        { "page_layout",      FRAME_PL_EDITOR },        // drawing-sheet editor
        { "drawing_sheet",    FRAME_PL_EDITOR },
        { "pl_editor",        FRAME_PL_EDITOR },
        { "bitmap2component", FRAME_BM2CMP },
        { "bitmap2cmp",       FRAME_BM2CMP },
        { "calculator",       FRAME_CALC },             // PCB calculator
        { "pcb_calculator",   FRAME_CALC },
    };
    return map;
}


FRAME_T frame_from_string( const std::string& name )
{
    const auto& map = frame_name_table();
    auto it = map.find( name );
    if( it == map.end() )
    {
        std::string accepted;
        bool first = true;
        for( const auto& [k, _] : map )
        {
            if( !first )
                accepted += ", ";
            accepted += "'" + k + "'";
            first = false;
        }
        throw std::invalid_argument(
            "unknown frame name '" + name + "'.  accepted: " + accepted );
    }
    return it->second;
}


py::object show_frame( const std::string& name, bool raise_to_front )
{
    KIWAY* kiway = find_live_kiway_for_gui();
    if( !kiway )
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running?" );

    FRAME_T frame_id = frame_from_string( name );

    // The 3D viewer is NOT spawned through the kiway frame factory —
    // upstream's IFACE::CreateKiWindow in pcbnew has no case for
    // FRAME_PCB_DISPLAY3D.  It's constructed as a child of the PCB editor
    // via PCB_BASE_FRAME::CreateAndShow3D_Frame(), which we can't call
    // from libkicommon (PCB_BASE_FRAME lives in pcbnew kiface).  Direct
    // users to the 3D viewer binding's auto-spawning path instead.
    if( frame_id == FRAME_PCB_DISPLAY3D )
    {
        py::dict r;
        r[ "ok" ]       = false;
        r[ "name" ]     = name;
        r[ "frame_id" ] = static_cast<int>( frame_id );
        r[ "raised" ]   = false;
        r[ "error" ]    = std::string(
            "FRAME_PCB_DISPLAY3D is not spawnable via show_frame() — upstream "
            "spawns it as a child of PCB_EDIT_FRAME, not through the kiway "
            "frame factory.  Use klicad_native_3d_viewer instead (any call "
            "auto-spawns it via PCB_BASE_FRAME::CreateAndShow3D_Frame), e.g.: "
            "klicad_native_3d_viewer.is_open()" );
        return r;
    }

    // Some frames need a specific parent frame to be alive before their
    // constructor can succeed (the upstream ctor dereferences the parent
    // without a null-check).  Spawn the prerequisite first.
    //
    // Confirmed by crash:
    //   - FRAME_SIMULATOR: SIMULATOR_FRAME_UI::SIMULATOR_FRAME_UI dereferences
    //     SCH_BASE_FRAME::eeconfig() on its (potentially-null) SCH parent.
    //
    // For self-sufficient frames we leave parent=null (their ctor handles it).
    KIWAY_PLAYER* parent_frame = nullptr;
    if( frame_id == FRAME_SIMULATOR )
        parent_frame = kiway->Player( FRAME_SCH, true );

    // Player(_, true, parent) creates the frame if missing, with the
    // explicit parent for ctors that need it.
    KIWAY_PLAYER* player = kiway->Player( frame_id, true, parent_frame );

    py::dict result;
    result[ "ok" ]   = ( player != nullptr );
    result[ "name" ] = name;
    result[ "frame_id" ] = static_cast<int>( frame_id );

    if( !player )
    {
        result[ "raised" ] = false;
        result[ "error" ]  = "Player() returned nullptr — kiface failed to load";
        return result;
    }

    if( raise_to_front )
    {
        player->Show( true );
        player->Raise();
    }

    result[ "raised" ] = raise_to_front;
    result[ "title" ]  = player->GetTitle().ToStdString();
    return result;
}


py::list list_frame_names()
{
    py::list out;
    for( const auto& [name, _] : frame_name_table() )
        out.append( name );
    return out;
}


py::dict dismiss_dialogs( const std::string& title_substr )
{
    // Collect first, mutate second — modifying wxTopLevelWindows during
    // iteration is asking for trouble.
    std::vector<wxDialog*> targets;
    for( wxWindow* w : wxTopLevelWindows )
    {
        wxDialog* dlg = dynamic_cast<wxDialog*>( w );
        if( !dlg )
            continue;

        if( !title_substr.empty() )
        {
            wxString title = dlg->GetTitle();
            if( title.Find( wxString::FromUTF8( title_substr ) ) == wxNOT_FOUND )
                continue;
        }
        targets.push_back( dlg );
    }

    py::list dismissed_titles;
    for( wxDialog* dlg : targets )
    {
        dismissed_titles.append( dlg->GetTitle().ToStdString() );
        if( dlg->IsModal() )
            dlg->EndModal( wxID_OK );
        else
            dlg->Close( true );
    }

    py::dict result;
    result[ "count" ] = (int) targets.size();
    result[ "titles" ] = dismissed_titles;
    return result;
}


// Walk every wxButton descendant of `parent` depth-first, applying `visit`.
void walk_buttons( wxWindow* parent, const std::function<void(wxButton*)>& visit )
{
    if( !parent )
        return;
    for( wxWindow* child : parent->GetChildren() )
    {
        if( wxButton* btn = dynamic_cast<wxButton*>( child ) )
            visit( btn );
        walk_buttons( child, visit );
    }
}


py::list list_dialog_buttons( const std::string& title_substr )
{
    py::list out;
    for( wxWindow* w : wxTopLevelWindows )
    {
        wxDialog* dlg = dynamic_cast<wxDialog*>( w );
        if( !dlg )
            continue;

        if( !title_substr.empty() )
        {
            if( dlg->GetTitle().Find( wxString::FromUTF8( title_substr ) ) == wxNOT_FOUND )
                continue;
        }

        const wxString title = dlg->GetTitle();
        const wxWindow* def  = dlg->GetDefaultItem();
        walk_buttons( dlg, [&]( wxButton* btn )
        {
            py::dict d;
            d[ "dialog_title" ] = title.ToStdString();
            d[ "label" ]        = btn->GetLabel().ToStdString();
            d[ "id" ]           = btn->GetId();
            d[ "enabled" ]      = btn->IsEnabled();
            d[ "is_default" ]   = ( btn == def );
            out.append( d );
        } );
    }
    return out;
}


py::dict click_dialog_button( const std::string& button_label,
                              const std::string& dialog_title_substr )
{
    wxDialog* target_dlg = nullptr;
    wxButton* target_btn = nullptr;

    for( wxWindow* w : wxTopLevelWindows )
    {
        wxDialog* dlg = dynamic_cast<wxDialog*>( w );
        if( !dlg )
            continue;

        if( !dialog_title_substr.empty() )
        {
            if( dlg->GetTitle().Find( wxString::FromUTF8( dialog_title_substr ) ) == wxNOT_FOUND )
                continue;
        }

        const wxString needle = wxString::FromUTF8( button_label );
        walk_buttons( dlg, [&]( wxButton* btn )
        {
            if( target_btn )
                return; // already found

            // Match against the raw label, or a stripped form without wx's
            // "&" mnemonic markers (so "Annotate" matches "&Annotate").
            wxString label = btn->GetLabel();
            wxString stripped = label;
            stripped.Replace( wxS( "&" ), wxS( "" ) );

            if( label.Find( needle ) != wxNOT_FOUND
                || stripped.Find( needle ) != wxNOT_FOUND )
            {
                target_dlg = dlg;
                target_btn = btn;
            }
        } );

        if( target_btn )
            break;
    }

    py::dict result;
    if( !target_btn )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "no matching button in any open dialog" );
        return result;
    }

    const int btn_id = target_btn->GetId();

    // Two delivery paths:
    //  (a) Modal dialog + standard ID (OK/Cancel/Yes/No/Apply/Close): EndModal(id)
    //      is the simplest path — same effect as if the user clicked, and KiCad
    //      relies on standard-ID semantics throughout.
    //  (b) Otherwise: post a wxEVT_BUTTON command event to the button's own
    //      handler.  That's what wxWidgets does internally on a click, so any
    //      custom handler the dialog registered fires.
    auto is_std_modal_id = []( int id )
    {
        return id == wxID_OK || id == wxID_CANCEL || id == wxID_YES || id == wxID_NO
               || id == wxID_APPLY || id == wxID_CLOSE;
    };

    bool used_endmodal = false;
    if( target_dlg->IsModal() && is_std_modal_id( btn_id ) )
    {
        target_dlg->EndModal( btn_id );
        used_endmodal = true;
    }
    else
    {
        wxCommandEvent ev( wxEVT_BUTTON, btn_id );
        ev.SetEventObject( target_btn );
        target_btn->GetEventHandler()->ProcessEvent( ev );
    }

    result[ "ok" ]            = true;
    result[ "dialog_title" ]  = target_dlg->GetTitle().ToStdString();
    result[ "button_label" ]  = target_btn->GetLabel().ToStdString();
    result[ "button_id" ]     = btn_id;
    result[ "via_end_modal" ] = used_endmodal;
    return result;
}


py::list list_open_frames()
{
    py::list out;
    for( wxWindow* w : wxTopLevelWindows )
    {
        py::dict d;
        d[ "class" ] = wxString( w->GetClassInfo()->GetClassName() ).ToStdString();
        wxString title;
        if( auto* tlw = dynamic_cast<wxTopLevelWindow*>( w ) )
            title = tlw->GetTitle();
        d[ "title" ] = title.ToStdString();
        // dynamic_cast<KIWAY_PLAYER*> would require its typeinfo from outside
        // libkicommon — KIWAY_HOLDER is sufficient (every Player is a Holder).
        d[ "is_kiway_holder" ] = ( dynamic_cast<KIWAY_HOLDER*>( w ) != nullptr );
        d[ "is_shown" ] = w->IsShown();
        out.append( d );
    }
    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// close_topmost_dialog — last-resort dismissal.
//
// Some dialogs use non-standard wx IDs for their Close button so the
// EndModal-on-standard-id path in click_dialog_button doesn't fire,
// and dispatching a wxEVT_BUTTON to a custom handler isn't reliable
// (some handlers defer via wxYield, leaving the dialog up until the
// IPC client's next blocking call wakes the loop).
//
// This binding bypasses the button entirely: find the topmost
// modal wxDialog and call EndModal(wxID_CANCEL) directly.  Equivalent
// to the user pressing Escape.
// ──────────────────────────────────────────────────────────────────────────
py::dict close_topmost_dialog( const std::string& dialog_title_substr )
{
    wxDialog* target = nullptr;

    for( wxWindow* w : wxTopLevelWindows )
    {
        wxDialog* dlg = dynamic_cast<wxDialog*>( w );
        if( !dlg )
            continue;
        if( !dlg->IsShown() )
            continue;

        if( !dialog_title_substr.empty() )
        {
            if( dlg->GetTitle().Find( wxString::FromUTF8( dialog_title_substr ) )
                == wxNOT_FOUND )
                continue;
        }

        target = dlg;
        break;
    }

    py::dict result;
    if( !target )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "no shown wxDialog found" );
        return result;
    }

    const std::string title = target->GetTitle().ToStdString();

    if( target->IsModal() )
    {
        target->EndModal( wxID_CANCEL );
    }
    else
    {
        // Non-modal: just close it.
        target->Close( /*force=*/false );
    }

    result[ "ok" ]    = true;
    result[ "title" ] = title;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_gui, m )
{
    m.doc() = "KliCAD GUI control: launch/raise KiCad frames programmatically. "
              "Wraps KIWAY::Player() for every known FRAME_T.";

    m.def( "show_frame", &show_frame,
           py::arg( "name" ),
           py::arg( "raise_to_front" ) = true,
           R"DOC(Spawn (if needed) and raise a KiCad frame by name.

Returns a dict with keys: ok (bool), name (str), frame_id (int), raised
(bool), title (str), and optionally error (str) on failure.

Frame names (string -> upstream FRAME_T):
  - 'schematic' / 'schematic_editor' / 'eeschema' -> FRAME_SCH
  - 'symbol_editor' -> FRAME_SCH_SYMBOL_EDITOR
  - 'symbol_viewer' -> FRAME_SCH_VIEWER
  - 'symbol_chooser' -> FRAME_SYMBOL_CHOOSER
  - 'simulator' / 'spice' -> FRAME_SIMULATOR
  - 'sch_diff' / 'symbol_diff' -> diff viewers
  - 'pcb' / 'pcb_editor' / 'pcbnew' -> FRAME_PCB_EDITOR
  - 'footprint_editor' -> FRAME_FOOTPRINT_EDITOR
  - 'footprint_chooser' / 'footprint_viewer' / 'footprint_wizard'
  - '3d' / '3d_viewer' / 'viewer_3d' -> FRAME_PCB_DISPLAY3D
  - 'pcb_diff' / 'footprint_diff' -> diff viewers
  - 'cvpcb' -> FRAME_CVPCB (component-to-footprint linker)
  - 'gerbview' / 'gerber_viewer' -> FRAME_GERBER
  - 'page_layout' / 'drawing_sheet' / 'pl_editor' -> FRAME_PL_EDITOR
  - 'bitmap2component' / 'bitmap2cmp' -> FRAME_BM2CMP
  - 'calculator' / 'pcb_calculator' -> FRAME_CALC
)DOC" );

    m.def( "list_frame_names", &list_frame_names,
           "Return the list of all accepted strings for show_frame()." );

    m.def( "dismiss_dialogs", &dismiss_dialogs,
           py::arg( "title_substr" ) = std::string(),
           R"DOC(Programmatically dismiss every wxDialog currently open.

Returns ``{count: int, titles: list[str]}``.  For modal dialogs, calls
EndModal(wxID_OK) (default-button equivalent of pressing Return).  For
modeless dialogs, calls Close(true).

If ``title_substr`` is non-empty, only dismisses dialogs whose title
contains that substring (case-sensitive).  Useful for targeting known
error popups without affecting unrelated dialogs the user has open.
)DOC" );

    m.def( "list_open_frames", &list_open_frames,
           "Return a list of dicts describing every currently-open top-level "
           "window in the KiCad process (class, title, is_kiway_*, is_shown)." );

    m.def( "list_dialog_buttons", &list_dialog_buttons,
           py::arg( "title_substr" ) = std::string(),
           R"DOC(Enumerate every wxButton in every currently-open wxDialog.

Returns a list of ``{dialog_title, label, id, enabled, is_default}`` dicts.
Use this to discover which actions are available before driving a dialog
programmatically.

If ``title_substr`` is non-empty, only inspects dialogs whose title contains
that substring (case-sensitive).
)DOC" );

    m.def( "click_dialog_button", &click_dialog_button,
           py::arg( "button_label" ),
           py::arg( "dialog_title_substr" ) = std::string(),
           R"DOC(Click a button in an open dialog (substring match on the label).

For modal dialogs with standard button IDs (OK/Cancel/Yes/No/Apply/Close),
this is implemented as EndModal(id) — semantically identical to the user
clicking the button.  For everything else, it posts a wxEVT_BUTTON command
event to the button so the dialog's own handler fires.

``button_label`` is matched both against the raw wx label and against a
version with the ``&`` mnemonic markers stripped (so 'Annotate' matches
'&Annotate').

If ``dialog_title_substr`` is non-empty, only matches buttons in dialogs
whose title contains that substring.

Returns ``{ok, dialog_title, button_label, button_id, via_end_modal}`` on
success, or ``{ok: False, error}`` if no matching button was found.

Note: this binding is preventive, not curative.  If a modal dialog is
already blocking the main thread when you call it, the wxEvent never gets
dispatched until something else unblocks the main loop.  Pair with a
pre-flight ``dismiss_dialogs`` or a known-good wmctrl close.
)DOC" );

    m.def( "close_topmost_dialog", &close_topmost_dialog,
           py::arg( "dialog_title_substr" ) = std::string(),
           R"DOC(Force-close the topmost wxDialog via EndModal(wxID_CANCEL).

For dialogs whose Close button uses a non-standard wx ID — where
click_dialog_button takes the wxEVT_BUTTON path but the custom handler
doesn't reliably dismiss (e.g., the Electrical Rules Checker).  This
bypasses the button and calls EndModal directly, equivalent to the
user pressing Escape on a standard modal.

If ``dialog_title_substr`` is non-empty, only matches dialogs whose
title contains that substring.

Returns ``{ok, title}`` on success, ``{ok: False, error}`` otherwise.
)DOC" );
}
