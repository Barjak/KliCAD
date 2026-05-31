/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <widgets/sch_spec_pane.h>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/process.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>
#include <wx/utils.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <widgets/ui_common.h>


SCH_SPEC_PANE::SCH_SPEC_PANE( SCH_EDIT_FRAME* aFrame ) :
        wxPanel( aFrame ),
        m_frame( aFrame ),
        m_editor( nullptr )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_editor = new wxStyledTextCtrl( this, wxID_ANY );

    // Python lexer + a small line-number margin.  Style 0 (default)
    // text takes the default font; no per-token coloring is wired in
    // v1 — Scintilla draws the Python tokens in the default style
    // until SetKeyWords + StyleSetForeground per token are added.
    // That's a quick follow-up; the editor is fully usable without it.
    m_editor->SetLexer( wxSTC_LEX_PYTHON );
    m_editor->SetMarginType( 0, wxSTC_MARGIN_NUMBER );
    m_editor->SetMarginWidth( 0, 40 );
    m_editor->SetTabWidth( 4 );
    m_editor->SetUseTabs( false );
    m_editor->SetIndent( 4 );
    m_editor->SetTabIndents( true );
    m_editor->SetBackSpaceUnIndents( true );

    sizer->Add( m_editor, 1, wxEXPAND );
    SetSizer( sizer );

    // Dark code-editor palette + monospace + Python token coloring.
    setupStyles();
    Bind( wxEVT_SYS_COLOUR_CHANGED, &SCH_SPEC_PANE::onThemeChanged, this );

    // Auto-discover the project's spec file on first show.  No
    // IPC call or project-file association needed — the pane finds
    // the spec by sibling naming convention.  See DiscoverSpecForCurrentSchematic().
    Bind( wxEVT_SHOW, &SCH_SPEC_PANE::onShow, this );

    // Commit-on-Enter — Bind to KEY_DOWN so we react to the bare
    // Enter keypress, not just printable characters added to the
    // buffer.  Skip() on the event so Scintilla still inserts the
    // newline normally; the regen runs as a side effect.
    m_editor->Bind( wxEVT_KEY_DOWN, &SCH_SPEC_PANE::onKeyDown, this );

    // Commit-on-focus-loss — clicking the canvas, switching tabs, or
    // tabbing away all dispatch wxEVT_KILL_FOCUS to the STC.  Any of
    // those should be treated as the user signaling "I'm done with
    // this edit", same as pressing Enter.
    m_editor->Bind( wxEVT_KILL_FOCUS, &SCH_SPEC_PANE::onKillFocus, this );
}


void SCH_SPEC_PANE::setupStyles()
{
    if( !m_editor )
        return;

    // Force-dark palette regardless of system theme — the pane is a
    // code editor, and code reads best on a dark background even on
    // a light desktop.  If a "follow system theme" option is wanted
    // later, swap these for `dummy.GetForegroundColour()` /
    // `dummy.GetBackgroundColour()` (see scintilla_tricks.cpp:103).
    const wxColour bg(             0x1e, 0x1e, 0x1e );  // VSCode-ish base
    const wxColour fg(             0xd4, 0xd4, 0xd4 );  // soft white
    const wxColour comment(        0x6a, 0x99, 0x55 );  // green
    const wxColour string_(        0xce, 0x91, 0x78 );  // orange-tan
    const wxColour number(         0xb5, 0xce, 0xa8 );  // pale green
    const wxColour keyword(        0x56, 0x9c, 0xd6 );  // bright blue
    const wxColour builtin(        0x4e, 0xc9, 0xb0 );  // teal
    const wxColour defname(        0xdc, 0xdc, 0xaa );  // pale yellow
    const wxColour decorator(      0xdc, 0xdc, 0xaa );  // pale yellow
    const wxColour operator_(      0xd4, 0xd4, 0xd4 );  // same as fg
    const wxColour line_number_fg( 0x85, 0x85, 0x85 );  // dim grey
    const wxColour line_number_bg( 0x25, 0x25, 0x25 );  // slightly darker than bg

    // Apply base default style, then clear all to propagate it as the
    // starting point for every other style.
    m_editor->StyleSetForeground( wxSTC_STYLE_DEFAULT, fg );
    m_editor->StyleSetBackground( wxSTC_STYLE_DEFAULT, bg );
    m_editor->StyleClearAll();

    // Monospace font across every style.
    wxFont fixedFont = KIUI::GetMonospacedUIFont();

    for( size_t i = 0; i < wxSTC_STYLE_MAX; ++i )
        m_editor->StyleSetFont( i, fixedFont );

    // Selection: a soft blue wash on top of the dark base.
    m_editor->SetSelForeground( true, fg );
    m_editor->SetSelBackground( true, wxColour( 0x26, 0x4f, 0x78 ) );
    m_editor->SetCaretForeground( fg );
    m_editor->SetCaretLineVisible( true );
    m_editor->SetCaretLineBackground( wxColour( 0x2a, 0x2a, 0x2a ) );

    // Line-number margin: subdued, slightly darker background.
    m_editor->StyleSetForeground( wxSTC_STYLE_LINENUMBER, line_number_fg );
    m_editor->StyleSetBackground( wxSTC_STYLE_LINENUMBER, line_number_bg );

    // Python lexer keyword sets — 0 = primary keywords, 1 = builtins /
    // common names.  Coloring is wired via the wxSTC_P_WORD /
    // wxSTC_P_WORD2 styles below.
    m_editor->SetKeyWords( 0, wxS(
            "and as assert async await break class continue def del elif else "
            "except finally for from global if import in is lambda match "
            "nonlocal not or pass raise return try while with yield True "
            "False None" ) );
    m_editor->SetKeyWords( 1, wxS(
            "self cls print len range enumerate zip map filter list dict "
            "tuple set str int float bool bytes type isinstance hasattr "
            "getattr setattr open close staticmethod classmethod property "
            "super abs all any sorted reversed sum min max round __init__" ) );

    // Per-token Python styles (wxSTC_P_*).  Anything not in this list
    // inherits the default we set above.
    m_editor->StyleSetForeground( wxSTC_P_COMMENTLINE,  comment );
    m_editor->StyleSetForeground( wxSTC_P_COMMENTBLOCK, comment );
    m_editor->StyleSetForeground( wxSTC_P_NUMBER,       number );
    m_editor->StyleSetForeground( wxSTC_P_STRING,       string_ );
    m_editor->StyleSetForeground( wxSTC_P_CHARACTER,    string_ );
    m_editor->StyleSetForeground( wxSTC_P_TRIPLE,       string_ );
    m_editor->StyleSetForeground( wxSTC_P_TRIPLEDOUBLE, string_ );
    m_editor->StyleSetForeground( wxSTC_P_STRINGEOL,    string_ );
    m_editor->StyleSetForeground( wxSTC_P_WORD,         keyword );
    m_editor->StyleSetForeground( wxSTC_P_WORD2,        builtin );
    m_editor->StyleSetForeground( wxSTC_P_DEFNAME,      defname );
    m_editor->StyleSetForeground( wxSTC_P_CLASSNAME,    builtin );
    m_editor->StyleSetForeground( wxSTC_P_OPERATOR,     operator_ );
    m_editor->StyleSetForeground( wxSTC_P_IDENTIFIER,   fg );
    m_editor->StyleSetForeground( wxSTC_P_DECORATOR,    decorator );
}


void SCH_SPEC_PANE::onThemeChanged( wxSysColourChangedEvent& aEvent )
{
    setupStyles();
    aEvent.Skip();
}


void SCH_SPEC_PANE::onShow( wxShowEvent& aEvent )
{
    aEvent.Skip();

    // Auto-discover only on becoming visible, and only if no path is
    // already set — never clobber an explicit user choice.
    if( aEvent.IsShown() && m_specPath.IsEmpty() )
        DiscoverSpecForCurrentSchematic();
}


void SCH_SPEC_PANE::DiscoverSpecForCurrentSchematic()
{
    if( !m_specPath.IsEmpty() )
        return;

    if( !m_frame )
        return;

    SCHEMATIC& sch = m_frame->Schematic();

    if( !sch.IsValid() || !sch.RootScreen() )
        return;

    wxFileName schFn( sch.RootScreen()->GetFileName() );

    if( !schFn.IsOk() || !schFn.HasName() )
        return;

    // Search order — first hit wins.  The basename-prefixed form is
    // preferred so a project that ships multiple specs (e.g. dut +
    // testbench) can disambiguate by schematic.  The bare name is the
    // de-facto convention from the design-loop standing prompt and
    // exists as a fallback so existing projects "just work".
    //
    // Search both the schematic's own directory AND its parent.  KiCad
    // projects commonly put the .kicad_sch in a `kicad/` subdirectory
    // with non-CAD files (REQUIREMENTS.md, checkpoints/, the build
    // script itself) in the project root one level up.  Without the
    // parent search, auto-discovery silently misses the canonical case
    // — exactly the bug that motivated this method.
    const wxString base = schFn.GetName();

    const wxString schDir = schFn.GetPath();

    wxFileName parentDirFn( schDir, wxEmptyString );
    parentDirFn.RemoveLastDir();
    const wxString parentDir = parentDirFn.GetPath();

    const wxString candidates[] = {
        base + wxS( ".spec.py" ),
        wxS( "build_schematic.py" ),
    };
    const wxString dirs[] = { schDir, parentDir };

    for( const wxString& d : dirs )
    {
        if( d.IsEmpty() )
            continue;

        for( const wxString& name : candidates )
        {
            wxFileName cand( d, name );
            if( cand.FileExists() )
            {
                SetSpecPath( cand.GetFullPath() );
                return;
            }
        }
    }

    // No spec found.  Pane stays empty; user can still type into it
    // and SaveFile() will create whatever path SetSpecPath() points at.
}


SCH_SPEC_PANE::~SCH_SPEC_PANE()
{
    // m_editor is a child of `this`; wxWidgets' parent-owned destruction
    // handles it.  No explicit cleanup needed.
}


void SCH_SPEC_PANE::SetSpecPath( const wxString& aPath )
{
    m_specPath = aPath;

    if( !m_editor )
        return;

    if( aPath.IsEmpty() )
    {
        m_editor->ClearAll();
        return;
    }

    // Load the spec file's current contents into the editor.  If the
    // file doesn't exist or can't be read, leave the editor empty
    // (the user can still type into it and the first commit will
    // create the file).
    if( wxFile::Exists( aPath ) )
    {
        m_editor->LoadFile( aPath );
    }
    else
    {
        m_editor->ClearAll();
    }
}


void SCH_SPEC_PANE::onKeyDown( wxKeyEvent& aEvent )
{
    // Ctrl+= / Ctrl++ / Ctrl+NumPad+   → zoom in
    // Ctrl+-   / Ctrl+NumPad-          → zoom out
    // Ctrl+0                           → reset zoom
    //
    // The editor's KEY_DOWN handler runs before the parent frame's
    // accelerator table, so we can intercept these before KiCad's
    // canvas-zoom bindings claim them.  Do NOT Skip() on a hit, or
    // the canvas zooms too.
    if( aEvent.ControlDown() && !aEvent.AltDown() && !aEvent.MetaDown() )
    {
        const int code = aEvent.GetKeyCode();

        if( code == '=' || code == '+' || code == WXK_NUMPAD_ADD )
        {
            m_editor->ZoomIn();
            return;
        }

        if( code == '-' || code == WXK_NUMPAD_SUBTRACT )
        {
            m_editor->ZoomOut();
            return;
        }

        if( code == '0' || code == WXK_NUMPAD0 )
        {
            m_editor->SetZoom( 0 );
            return;
        }
    }

    // Allow the normal newline insertion to proceed first; we just
    // piggyback the regen on top of it.  Skip() before the work so
    // any throw from CommitAndRegen() doesn't swallow the keystroke.
    aEvent.Skip();

    if( aEvent.GetKeyCode() == WXK_RETURN
            || aEvent.GetKeyCode() == WXK_NUMPAD_ENTER )
    {
        CommitAndRegen();
    }
}


void SCH_SPEC_PANE::onKillFocus( wxFocusEvent& aEvent )
{
    aEvent.Skip();
    CommitAndRegen();
}


void SCH_SPEC_PANE::CommitAndRegen()
{
    if( m_specPath.IsEmpty() || !m_editor )
        return;

    // Write the editor's current buffer to disk so the subprocess
    // sees the user's latest edits.  SaveFile() returns false on
    // I/O failure; for v1 we swallow the error rather than popping
    // a dialog every focus-out — a follow-up should surface this in
    // an error sub-pane.
    if( !m_editor->SaveFile( m_specPath ) )
        return;

    // Tell the spec which schematic it's editing + how to reach this
    // KliCAD process's IPC server.  Env vars are scoped to the spawned
    // subprocess via wxExecuteEnv — NOT wxSetEnv, which would leak
    // KLICAD_DISCARD_UNSAVED into the parent eeschema process and
    // silently discard interactive user edits on every save prompt.
    SCHEMATIC& sch = m_frame->Schematic();
    wxString   schPath;
    if( sch.IsValid() && sch.RootScreen() )
        schPath = sch.RootScreen()->GetFileName();

    wxExecuteEnv env;
    wxGetEnvMap( &env.env );  // inherit current env as the base
    env.env[ wxS( "KLICAD_SCH_PATH" ) ]        = schPath;
    env.env[ wxS( "KLICAD_DISCARD_UNSAVED" ) ] = wxS( "1" );
    // KLICAD_API_SOCKET passes through from inherited env when set.

    // Invoke the harness that runs the user's spec module and emits
    // the resulting Circuit to the schematic at KLICAD_SCH_PATH.  The
    // harness picks up either a top-level `circuit`/`top` variable or
    // an @v01.root-decorated function — the spec author doesn't have
    // to call to_schematic() themselves.
    wxString cmd = wxString::Format(
            wxS( "python3 -m klipy.spec_pane_harness \"%s\"" ), m_specPath );
    wxExecute( cmd, wxEXEC_ASYNC, nullptr, &env );
}
