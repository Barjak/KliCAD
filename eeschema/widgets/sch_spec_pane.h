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

#pragma once

#include <wx/panel.h>
#include <wx/string.h>

class wxStyledTextCtrl;
class wxStyledTextEvent;
class SCH_EDIT_FRAME;


/**
 * Right-hand pane that displays a klicad-python spec file (the
 * `build_schematic.py`-style file that authored the current schematic)
 * and re-runs it whenever the user commits an edit — either by pressing
 * Enter, or by letting the editor lose focus.
 *
 * The "authored by" link is established via the `Klicad.SpecSrc` field
 * stamped on every emitted symbol / sheet / hier-label by
 * klicad-python's `to_schematic(..., mode='replace')` pipeline.  When
 * the pane has a spec path, every commit-style event triggers
 * `CommitAndRegen()` which writes the current buffer to disk and shells
 * out to the project's Python interpreter to re-run the spec — KliCAD
 * subsequently sees the freshly regenerated schematic the next time it
 * reloads from disk.  The synchronous subprocess path is the v1 choice:
 * the bench showed `to_schematic` on the ~50-part driver-board
 * reference at ~10s/regen under ASan, which is unsuitable for
 * keystroke-rate sync but acceptable for "commit each line as you
 * write it" cadence pending a release-build re-bench.
 *
 * Spec path: set via `SetSpecPath()`.  v1 has no project-file
 * association — callers (a klicad_native binding, or a future File >
 * Open Spec menu item) feed the path explicitly.  An empty path
 * disables the regen behavior; the pane stays usable as a read-only
 * code viewer.
 *
 * Cursor sync (canvas selection → cursor jump; cursor → highlight) is
 * NOT wired in v1.  It's the obvious iteration-2 addition; the
 * `Klicad.SpecSrc` fields needed to drive it are already populated by
 * the klicad-python emit side.
 */
class SCH_SPEC_PANE : public wxPanel
{
public:
    SCH_SPEC_PANE( SCH_EDIT_FRAME* aFrame );
    ~SCH_SPEC_PANE() override;

    /**
     * Point the pane at a Python spec file.  Loads the file's current
     * contents into the editor; the file path becomes the regen target
     * on the next commit event.  Empty string clears the pane and
     * disables regen.
     */
    void SetSpecPath( const wxString& aPath );

    /**
     * @return the absolute path the pane is currently bound to, or an
     * empty string when no spec has been loaded.
     */
    wxString GetSpecPath() const { return m_specPath; }

    /**
     * Force a commit + regen now.  Same path as the Enter / focus-loss
     * handlers; exposed so a hot-key or menu-item can trigger it.
     */
    void CommitAndRegen();

private:
    /// Bind on the editor.  `wxEVT_KEY_DOWN` with `WXK_RETURN` does
    /// commit-on-Enter (we use KEY_DOWN rather than the Scintilla
    /// CHARADDED event so a plain Enter — not just a printable char —
    /// fires the commit).
    void onKeyDown( wxKeyEvent& aEvent );

    /// Bind on the editor.  `wxEVT_KILL_FOCUS` fires when the editor
    /// loses keyboard focus (tab away, click canvas, etc.).  Treat
    /// that as an implicit commit so users don't lose work on
    /// inadvertent focus changes.
    void onKillFocus( wxFocusEvent& aEvent );

    SCH_EDIT_FRAME*   m_frame;
    wxStyledTextCtrl* m_editor;
    wxString          m_specPath;
};
