/**
 * @file sch_layout_transaction.cpp
 *
 * Implementation of the GOAL.md F-S2 transactional writeback wrapper.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sch_layout_transaction.h"

#include <connection_graph.h>
#include <layer_ids.h>
#include <sch_edit_frame.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <schematic.h>

namespace klicad::auto_layout {

SchLayoutTransaction::SchLayoutTransaction( SCH_EDIT_FRAME& aFrame ) :
        m_frame( aFrame ),
        m_sheetPath( aFrame.GetCurrentSheet() ),
        m_commit( static_cast<EDA_DRAW_FRAME*>( &aFrame ) )
{
}


SchLayoutTransaction::~SchLayoutTransaction()
{
    if( !m_pushed )
        commit();
}


SCHEMATIC& SchLayoutTransaction::schematic() const
{
    return m_frame.Schematic();
}


SCH_SCREEN& SchLayoutTransaction::screen() const
{
    SCH_SCREEN* s = m_sheetPath.LastScreen();
    wxASSERT( s );
    return *s;
}


const CONNECTION_GRAPH& SchLayoutTransaction::connection_graph() const
{
    CONNECTION_GRAPH* g = m_frame.Schematic().ConnectionGraph();
    wxASSERT( g );
    return *g;
}


void SchLayoutTransaction::move( SCH_ITEM& aItem, const VECTOR2I& aNewPos )
{
    // SCH_COMMIT::Modify snapshots the pre-mutation state for undo; we
    // then mutate, and Push() picks up the dirty flag and calls
    // SCH_SCREEN::Update() to refresh the r-tree.
    m_commit.Modify( &aItem, &screen() );
    aItem.SetPosition( aNewPos );
}


SCH_LINE& SchLayoutTransaction::add_wire( const VECTOR2I& aStart, const VECTOR2I& aEnd )
{
    SCH_LINE* line = new SCH_LINE( aStart, LAYER_WIRE );
    line->SetEndPoint( aEnd );
    // CHT_ADD (no CHT_DONE) — SCH_COMMIT::pushSchEdit will Append to
    // screen and Add to view at commit time.
    m_commit.Add( line, &screen() );
    return *line;
}


SCH_LABEL_BASE& SchLayoutTransaction::add_label( const VECTOR2I& aPos,
                                                 const wxString& aText,
                                                 LabelKind       aKind )
{
    SCH_LABEL_BASE* label = nullptr;

    switch( aKind )
    {
    case LabelKind::LOCAL:
        label = new SCH_LABEL( aPos, aText );
        break;
    case LabelKind::GLOBAL:
        label = new SCH_GLOBALLABEL( aPos, aText );
        break;
    case LabelKind::HIERARCHICAL:
        label = new SCH_HIERLABEL( aPos, aText );
        break;
    }

    wxASSERT( label );
    m_commit.Add( label, &screen() );
    return *label;
}


SCH_JUNCTION& SchLayoutTransaction::add_junction( const VECTOR2I& aPos )
{
    SCH_JUNCTION* j = new SCH_JUNCTION( aPos );
    m_commit.Add( j, &screen() );
    return *j;
}


void SchLayoutTransaction::remove( SCH_ITEM& aItem )
{
    m_commit.Remove( &aItem, &screen() );
}


void SchLayoutTransaction::commit( const wxString& aMessage )
{
    if( m_pushed )
        return;

    m_pushed = true;
    // SCH_COMMIT::Push runs the full pipeline: undo recording,
    // connectivity rebuild (RecalculateConnections), dirty mark
    // (OnModify), canvas refresh.  See sch_commit.cpp:475-495 and
    // pushSchEdit at sch_commit.cpp:460.
    m_commit.Push( aMessage );
}

}  // namespace klicad::auto_layout
