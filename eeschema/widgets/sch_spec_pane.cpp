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
#include <wx/process.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>
#include <wx/utils.h>

#include <sch_edit_frame.h>


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

    // Shell out to Python.  v1 uses the system `python3` discovered
    // via $PATH plus the per-project `KLICAD_PYTHON_PATH` env-var
    // hook (see EMBEDDED_PYTHON::Init() — same hook the embedded
    // interpreter will use once it's exposed to eeschema).  Background
    // execution: the schematic re-emit is visible after KliCAD's IPC
    // server processes the to_schematic calls the spec issues, so
    // there's nothing to await synchronously here.
    wxString cmd = wxString::Format( wxS( "python3 \"%s\"" ), m_specPath );
    wxExecute( cmd, wxEXEC_ASYNC );
}
