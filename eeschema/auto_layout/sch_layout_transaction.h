/**
 * @file sch_layout_transaction.h
 *
 * Transactional writeback for the auto-layout adapters.  See GOAL.md
 * F-S2 (Transactional writeback) for the structural rationale.
 *
 * Invariant: code under eeschema/auto_layout/ never calls
 * SCH_SCREEN::Append / Remove / Update / SetContentModified directly,
 * and never calls SCH_EDIT_FRAME::OnModify / HardRedraw /
 * RecalculateConnections directly.  All mutation funnels through this
 * class, which wraps SCH_COMMIT; commit() (or destruction) fires undo
 * recording, connectivity rebuild, dirty mark, and canvas refresh as
 * the SCH_COMMIT::Push side effect.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <math/vector2d.h>
#include <sch_commit.h>
#include <sch_sheet_path.h>

#include <wx/string.h>

class SCH_EDIT_FRAME;
class SCH_SCREEN;
class SCH_ITEM;
class SCH_LINE;
class SCH_LABEL_BASE;
class SCH_JUNCTION;
class CONNECTION_GRAPH;
class SCHEMATIC;

namespace klicad::auto_layout {

/** Which SCH_LABEL_BASE subclass to instantiate for add_label(). */
enum class LabelKind
{
    LOCAL,         ///< SCH_LABEL — single-sheet local net label.
    GLOBAL,        ///< SCH_GLOBAL_LABEL — cross-sheet.
    HIERARCHICAL,  ///< SCH_HIER_LABEL — hierarchical port.
};


/**
 * Single-transaction view of a SCH_SCREEN mutation.  Constructed against
 * a live SCH_EDIT_FRAME; commits at explicit commit() call or in dtor.
 *
 * Calling commit() twice is a no-op on the second call (single-commit
 * lifecycle is the guarantee).  For the two-commit GOAL.md flow, the
 * caller constructs two SchLayoutTransaction objects in sequence.
 */
class SchLayoutTransaction
{
public:
    SchLayoutTransaction( SCH_EDIT_FRAME& aFrame );
    ~SchLayoutTransaction();

    SchLayoutTransaction( const SchLayoutTransaction& ) = delete;
    SchLayoutTransaction& operator=( const SchLayoutTransaction& ) = delete;

    /** The owning eeschema frame.  F-S3 compose uses this for
     *  RecalculateConnections and the lib-symbol adapter. */
    SCH_EDIT_FRAME& frame() const { return m_frame; }

    /** The schematic the transaction is editing. */
    SCHEMATIC&  schematic() const;
    /** The active root SCH_SCREEN.  All staged items default to this screen. */
    SCH_SCREEN& screen() const;
    /** The connectivity graph as of construction (pre-mutation). */
    const CONNECTION_GRAPH& connection_graph() const;
    /** The active sheet path (root). */
    const SCH_SHEET_PATH&   sheet_path() const { return m_sheetPath; }

    /** Stage a CHT_MODIFY snapshot and move the item.  Caller must NOT
     *  call SCH_SCREEN::Update; the commit will. */
    void move( SCH_ITEM& aItem, const VECTOR2I& aNewPos );

    /** Create a new SCH_LINE on LAYER_WIRE and stage CHT_ADD.  Returns
     *  the new line so the caller can refer to it later in the same
     *  transaction (e.g. for set_endpoint).  Ownership transfers to the
     *  screen at commit. */
    SCH_LINE& add_wire( const VECTOR2I& aStart, const VECTOR2I& aEnd );

    /** Create and stage a label item.  Returns the new label. */
    SCH_LABEL_BASE& add_label( const VECTOR2I& aPos,
                               const wxString& aText,
                               LabelKind       aKind );

    /** Create and stage an SCH_JUNCTION at aPos.  F-S1d invariant 2:
     *  emit at every coordinate where ≥3 wire endpoints (or 2 wire
     *  endpoints + pin + label) coincide, and at any coord where a
     *  wire endpoint sits mid-segment of another (junctions bind to
     *  mid-segments via connection_graph.cpp:1399–1411). */
    SCH_JUNCTION& add_junction( const VECTOR2I& aPos );

    /** Stage a CHT_REMOVE; the commit will remove from screen and view. */
    void remove( SCH_ITEM& aItem );

    /** Explicit commit.  Idempotent. */
    void commit( const wxString& aMessage = wxT( "Auto-layout" ) );

private:
    SCH_EDIT_FRAME& m_frame;
    SCH_SHEET_PATH  m_sheetPath;
    SCH_COMMIT      m_commit;
    bool            m_pushed = false;
};

}  // namespace klicad::auto_layout
