/*
 * GOAL.md F-S4d (Fork 3 (c)) — BusPropagator walker class.
 *
 * Encapsulates bus-specific cross-sheet propagation in one place
 * with one entry point so the no-string-compare invariant
 * (closure invariant 9) becomes a checkable property of this
 * single file rather than a vibes-based reading of
 * propagateToNeighbors.
 *
 * Bus-specific responsibilities folded into this class:
 *  - per-slot bit fan-out for multi-channel repeat sheets
 *  - bus-bit-to-hier-label resolution via typed
 *    `SCH_HIERLABEL::m_matchedEndpoint` (F-S4a)
 *  - cross-sheet propagation through typed references (F-S4b)
 *
 * Once F-S4d ships:
 *   - `CONNECTION_GRAPH::propagateToNeighbors` becomes scalar-only;
 *     when it encounters a bus pin / hier label, it delegates here.
 *   - `repeatBusPinBitInfo` and the `baseMatch` fallback (the
 *     name-arithmetic body at `connection_graph.cpp:2993-3043` +
 *     `:3121-3124`) are deleted with no replacement.
 *
 * Status: SKELETON.  This file lays down the shape so F-S4d's
 * implementation has a target.  The actual logic still lives in
 * propagateToNeighbors today; flipping that over is gated on F-S4b
 * (the typed `m_matchedEndpoint` walk that this class consumes).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

class CONNECTION_GRAPH;
class CONNECTION_SUBGRAPH;
class SCH_SHEET_PATH;

namespace klicad {

/**
 * BusPropagator — single owner of bus-specific net propagation.
 *
 * Stateless per call: caller passes the parent subgraph and the
 * graph; the propagator walks bus-related children and absorbs
 * matching subgraphs.  No string compares of net names; everything
 * resolves via typed KIID references on `SCH_HIERLABEL` /
 * `SCH_SHEET_PIN`.
 */
class BusPropagator
{
public:
    BusPropagator() = default;
    ~BusPropagator() = default;

    BusPropagator( const BusPropagator& ) = delete;
    BusPropagator& operator=( const BusPropagator& ) = delete;

    /**
     * Propagate a bus parent into its matched bus children.
     *
     * @param aGraph the connection graph (used to look up subgraphs
     *               by item KIID via m_item_to_subgraph_map).
     * @param aParent the bus subgraph whose children to traverse.
     *
     * Walks `aParent`'s `m_hier_ports` (SCH_HIERLABEL items, parent
     * side) and `m_hier_pins` (SCH_SHEET_PIN items, child side),
     * resolving each via `m_matchedEndpoint` to the target subgraph,
     * then Absorb()-ing the match into the parent.
     *
     * For multi-channel repeat sheets, fans the bus out into per-slot
     * scalar bits and propagates each bit to the correctly-indexed
     * hier label on the child side.
     *
     * Status: NOT YET IMPLEMENTED — body is a no-op until F-S4b
     * (the typed reference walk) populates `m_matchedEndpoint`
     * during compose materialization of sheets / hier labels.
     */
    void propagate( CONNECTION_GRAPH& aGraph,
                    CONNECTION_SUBGRAPH& aParent );

    /**
     * Returns true when `aSubgraph` involves bus-type connections
     * (its driver or any hier port is a bus).  The caller in
     * `propagateToNeighbors` uses this to decide whether to delegate
     * to `BusPropagator::propagate` vs handle the subgraph inline as
     * a scalar.
     */
    static bool involvesBus( const CONNECTION_SUBGRAPH& aSubgraph );
};

}  // namespace klicad
