/*
 * KliCAD subsystem binding: GUI frame management.
 *
 * Wraps KIWAY::Player(FRAME_T, true) so scripted clients can pop up any
 * KiCad window programmatically — schematic editor, simulator, 3D viewer,
 * gerber viewer, page layout editor, footprint editor, calculator, etc.
 *
 *   from kipy import KiCad
 *   k = KiCad()
 *   k.run_python("import kicad_native_gui as g; g.show_frame('simulator')")
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

#include <wx/dialog.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

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

    // Some frames need a specific parent frame to be alive before their
    // constructor can succeed (the upstream ctor dereferences the parent
    // without a null-check).  Spawn the prerequisite first.
    //
    // Confirmed by crash:
    //   - FRAME_SIMULATOR: SIMULATOR_FRAME_UI::SIMULATOR_FRAME_UI dereferences
    //     SCH_BASE_FRAME::eeconfig() on its (potentially-null) SCH parent.
    //   - FRAME_PCB_DISPLAY3D: needs board context from PCB editor (kiface
    //     load fails to nullptr otherwise).
    //
    // For self-sufficient frames we leave parent=null (their ctor handles it).
    KIWAY_PLAYER* parent_frame = nullptr;
    if( frame_id == FRAME_SIMULATOR )
        parent_frame = kiway->Player( FRAME_SCH, true );
    else if( frame_id == FRAME_PCB_DISPLAY3D )
        parent_frame = kiway->Player( FRAME_PCB_EDITOR, true );

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

} // anon


PYBIND11_EMBEDDED_MODULE( kicad_native_gui, m )
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
}
