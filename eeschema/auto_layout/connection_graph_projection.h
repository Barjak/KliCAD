/**
 * @file connection_graph_projection.h
 *
 * Typed projection of the live CONNECTION_GRAPH for the auto-layout
 * adapter to consume.  GOAL.md F-S1a: replaces the NetSpec IPC
 * side-channel.  The adapter never reads connectivity from any other
 * source; ERC and the adapter see the same graph by construction.
 *
 * Invariant: no helper here probes net names with string comparisons
 * (no isLowSideNet, no n[0] == '-' polarity guessing).  Net
 * classification comes from CONNECTION_SUBGRAPH::GetDriverPriority()
 * and the driver SCH_ITEM type.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <kiid.h>

#include <vector>

#include <wx/string.h>

class CONNECTION_GRAPH;
class CONNECTION_SUBGRAPH;
class SCH_ITEM;
class SCH_SHEET_PATH;


namespace klicad::auto_layout {

/** Classification of a net, derived from
 *  CONNECTION_SUBGRAPH::GetDriverPriority() + driver SCH_ITEM type.
 *  No polarity (HIGH/LOW); that comes from per-pin electrical type
 *  at the adapter, not from string-name probing. */
enum class NetKind
{
    SIGNAL,    ///< Regular electrical net.
    POWER,     ///< Driven by a power symbol (LOCAL_POWER_PIN | GLOBAL_POWER_PIN priority).
    HIER_PORT, ///< Driven by a hierarchical label / sheet pin.
};


/** Identifies a single pin uniquely by the parent symbol's UUID plus
 *  the pin number string.  KIID has operator< and std::hash, so this
 *  is usable as a map key. */
struct PinKey
{
    KIID     symbol_uuid;
    wxString pin_number;

    bool operator==( const PinKey& other ) const
    {
        return symbol_uuid == other.symbol_uuid && pin_number == other.pin_number;
    }
};


/** A single net as the adapter sees it: name, classification, the set
 *  of pins it touches, and the resolved driver SCH_ITEM for
 *  introspection. */
struct NetView
{
    wxString            name;
    NetKind             kind;
    std::vector<PinKey> pins;
    /** SCH_SHEET_PIN / SCH_HIERLABEL items that share this net.  M4
     *  closure: lets the ElkHierarchyBuilder add boundary ports as
     *  edge endpoints so ELK actually routes from symbol pins to the
     *  sheet boundary instead of leaving the connection dangling. */
    std::vector<KIID>   boundary_kiids;
    const SCH_ITEM*     driver;
};


/** Walks the CONNECTION_GRAPH at construction and stores per-net
 *  views indexed by net name.  Intended lifetime: one layout call. */
class ConnectionGraphProjection
{
public:
    /** @param aGraph the live connection graph, must already be
     *      RecalculateConnections'd.
     *  @param aSheet the sheet path whose subgraphs to project. */
    ConnectionGraphProjection( const CONNECTION_GRAPH& aGraph,
                               const SCH_SHEET_PATH&   aSheet );

    /** GOAL.md F-S5 Phase A — hierarchy-wide projection.  Visits every
     *  subgraph across the full sheet tree and collapses nets that share
     *  a name into a single NetView (the typed cross-sheet fusion via
     *  m_matchedEndpoint is F-S5 Phase B / F-S4b dependent).  Until
     *  Phase B lands, this constructor's behavior matches the
     *  sheet-local one in scope but extends it: pins on child sheets
     *  contribute to the same NetView as parent pins on the same
     *  net name.  The hier-port boundaries are recorded for the
     *  ElkHierarchyBuilder when it arrives. */
    explicit ConnectionGraphProjection( const CONNECTION_GRAPH& aGraph );

    /** All nets on the projected sheet.  One entry per net name; pins
     *  deduped per (symbol_uuid, pin_number). */
    const std::vector<NetView>& nets() const { return m_nets; }

    /** Reverse index pin → containing net, or nullptr if unknown. */
    const NetView* netForPin( const PinKey& aKey ) const;

private:
    static NetKind classify( const CONNECTION_SUBGRAPH& aSubgraph );

    std::vector<NetView> m_nets;
};

}  // namespace klicad::auto_layout
