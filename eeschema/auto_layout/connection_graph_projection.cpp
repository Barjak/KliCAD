/**
 * @file connection_graph_projection.cpp
 *
 * Implementation of the GOAL.md F-S1a projection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "connection_graph_projection.h"

#include <connection_graph.h>
#include <sch_item.h>
#include <sch_pin.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>

#include <set>
#include <unordered_map>

namespace klicad::auto_layout {

NetKind ConnectionGraphProjection::classify( const CONNECTION_SUBGRAPH& aSubgraph )
{
    // Strip const for the non-const PRIORITY accessor; we don't mutate.
    CONNECTION_SUBGRAPH& sg = const_cast<CONNECTION_SUBGRAPH&>( aSubgraph );

    using PRI = CONNECTION_SUBGRAPH::PRIORITY;
    switch( sg.GetDriverPriority() )
    {
    case PRI::LOCAL_POWER_PIN:
    case PRI::GLOBAL_POWER_PIN:
        return NetKind::POWER;

    case PRI::HIER_LABEL:
    case PRI::SHEET_PIN:
        return NetKind::HIER_PORT;

    case PRI::INVALID:
    case PRI::NONE:
    case PRI::PIN:
    case PRI::LOCAL_LABEL:
    case PRI::GLOBAL:
    default:
        return NetKind::SIGNAL;
    }
}


ConnectionGraphProjection::ConnectionGraphProjection( const CONNECTION_GRAPH& aGraph,
                                                       const SCH_SHEET_PATH&   aSheet )
{
    // Build by name so multiple subgraphs that share a net (e.g. one
    // per sheet) collapse into a single NetView for the queried sheet.
    std::unordered_map<wxString, NetView, wxStringHash, wxStringEqual> byName;

    for( const auto& [key, subgraphs] : aGraph.GetNetMap() )
    {
        for( CONNECTION_SUBGRAPH* sg : subgraphs )
        {
            if( !sg || sg->GetSheet() != aSheet )
                continue;

            wxString netName = sg->GetNetName();
            if( netName.IsEmpty() )
                continue;

            auto [it, inserted] = byName.emplace( netName, NetView{} );
            NetView& nv = it->second;

            if( inserted )
            {
                nv.name = netName;
                nv.kind = classify( *sg );
                nv.driver = sg->GetDriver();
            }

            // Walk the subgraph's items, collect SCH_PIN entries +
            // sheet-boundary KIIDs (sheet pins, hier labels).
            std::set<std::pair<KIID, wxString>> seen;
            std::set<KIID> seenBoundary;
            for( SCH_ITEM* item : sg->GetItems() )
            {
                if( !item )
                    continue;

                if( item->Type() == SCH_PIN_T )
                {
                    SCH_PIN* pin = static_cast<SCH_PIN*>( item );
                    const SCH_SYMBOL* sym = dynamic_cast<const SCH_SYMBOL*>(
                            pin->GetParentSymbol() );

                    if( !sym )
                        continue;

                    std::pair<KIID, wxString> k{ sym->m_Uuid, pin->GetNumber() };
                    if( !seen.insert( k ).second )
                        continue;

                    nv.pins.push_back( PinKey{ k.first, k.second } );
                }
                else if( item->Type() == SCH_SHEET_PIN_T
                         || item->Type() == SCH_HIER_LABEL_T )
                {
                    if( seenBoundary.insert( item->m_Uuid ).second )
                        nv.boundary_kiids.push_back( item->m_Uuid );
                }
            }
        }
    }

    m_nets.reserve( byName.size() );
    for( auto& [name, nv] : byName )
        m_nets.push_back( std::move( nv ) );
}


ConnectionGraphProjection::ConnectionGraphProjection( const CONNECTION_GRAPH& aGraph )
{
    // F-S5 Phase A — hierarchy-wide.  Same shape as the sheet-local
    // constructor but doesn't filter by SCH_SHEET_PATH; every
    // subgraph contributes.  Cross-sheet pins on a single name fold
    // into one NetView.  When F-S4b lands the typed reference walk,
    // this loop will additionally consume m_hier_parent /
    // m_hier_children links via m_matchedEndpoint and record each
    // crossing as a SheetPinBoundary; for now name-fusion is the
    // proxy.
    std::unordered_map<wxString, NetView, wxStringHash, wxStringEqual> byName;

    for( const auto& [key, subgraphs] : aGraph.GetNetMap() )
    {
        for( CONNECTION_SUBGRAPH* sg : subgraphs )
        {
            if( !sg )
                continue;

            wxString netName = sg->GetNetName();
            if( netName.IsEmpty() )
                continue;

            auto [it, inserted] = byName.emplace( netName, NetView{} );
            NetView& nv = it->second;

            if( inserted )
            {
                nv.name = netName;
                nv.kind = classify( *sg );
                nv.driver = sg->GetDriver();
            }

            std::set<std::pair<KIID, wxString>> seen;
            std::set<KIID> seenBoundary;
            for( const PinKey& existing : nv.pins )
                seen.insert( { existing.symbol_uuid, existing.pin_number } );
            for( const KIID& kid : nv.boundary_kiids )
                seenBoundary.insert( kid );

            for( SCH_ITEM* item : sg->GetItems() )
            {
                if( !item )
                    continue;

                if( item->Type() == SCH_PIN_T )
                {
                    SCH_PIN* pin = static_cast<SCH_PIN*>( item );
                    const SCH_SYMBOL* sym = dynamic_cast<const SCH_SYMBOL*>(
                            pin->GetParentSymbol() );
                    if( !sym )
                        continue;

                    std::pair<KIID, wxString> k{ sym->m_Uuid, pin->GetNumber() };
                    if( !seen.insert( k ).second )
                        continue;

                    nv.pins.push_back( PinKey{ k.first, k.second } );
                }
                else if( item->Type() == SCH_SHEET_PIN_T
                         || item->Type() == SCH_HIER_LABEL_T )
                {
                    // M4 closure: lift the SCH_SHEET_PIN / SCH_HIERLABEL
                    // KIIDs into the NetView so the ElkHierarchyBuilder
                    // has both an endpoint to route to (single-sheet
                    // bypass enrols these via portByBoundaryKiid).
                    if( seenBoundary.insert( item->m_Uuid ).second )
                        nv.boundary_kiids.push_back( item->m_Uuid );
                }
            }
        }
    }

    m_nets.reserve( byName.size() );
    for( auto& [name, nv] : byName )
        m_nets.push_back( std::move( nv ) );
}


const NetView* ConnectionGraphProjection::netForPin( const PinKey& aKey ) const
{
    for( const NetView& nv : m_nets )
    {
        for( const PinKey& p : nv.pins )
        {
            if( p == aKey )
                return &nv;
        }
    }
    return nullptr;
}

}  // namespace klicad::auto_layout
