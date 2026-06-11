/**
 * @file elk_hierarchy_builder.h
 *
 * GOAL.md F-S5 Phase B — materialize the hierarchy-wide
 * ConnectionGraphProjection into an ELK graph with compound nodes
 * per sheet.
 *
 * Output: a root ElkNode owning one compound child per SCH_SHEET_PATH
 * referenced by the projection.  Each compound child carries:
 *   - one nested ElkNode per SCH_SYMBOL on that sheet (identifier =
 *     SCH_SYMBOL::m_Uuid as a string; one ElkPort per pin, identifier
 *     "<symbol-kiid>:<pin-number>", anchored at the pin's local
 *     position inside the symbol's pin-extent bbox);
 *   - one ElkPort per SCH_SHEET_PIN on the parent sheet's view of a
 *     child sheet (identifier = sheet-pin KIID), so cross-sheet edges
 *     have a typed boundary endpoint to terminate on;
 *   - one ElkPort per SCH_HIER_LABEL on the child sheet (identifier =
 *     hier-label KIID).
 *
 * Cross-sheet nets become ElkEdges connecting symbol-pin ports across
 * compound boundaries.  When a net has >2 endpoints the builder emits
 * the star-pattern fan-out the existing adapter uses (one ElkSimpleEdge
 * per leaf, anchored to the first endpoint); hyperedge promotion is a
 * future elk-cpp upgrade, not this phase's concern.
 *
 * `HIERARCHY_HANDLING = INCLUDE_CHILDREN` is set on the root so ELK
 * Layered routes the whole hierarchy in a single pass (invariant 15).
 *
 * The builder is stateless across instances; one call per compose.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>

#include "elk/graph/elk_graph.h"

class CONNECTION_GRAPH;
class SCH_EDIT_FRAME;
class SCH_SHEET_PATH;


namespace klicad::auto_layout {

class ConnectionGraphProjection;


/** GOAL.md F-S5 Phase B — turn the hierarchy-wide projection into an
 *  ELK graph with compound nodes per sheet.  The returned root
 *  carries HIERARCHY_HANDLING = INCLUDE_CHILDREN by construction;
 *  nothing else in the auto-layout pipeline is permitted to set it. */
class ElkHierarchyBuilder
{
public:
    explicit ElkHierarchyBuilder( const ConnectionGraphProjection& aProj );

    /** Build the ELK graph from the projection.  Returns the root
     *  ElkNode (owns the whole graph).  Throws std::runtime_error if
     *  the projection references no symbols (empty hierarchy is a
     *  caller bug, not a layout case). */
    std::unique_ptr<elk::graph::ElkNode> build( SCH_EDIT_FRAME& aFrame );

private:
    const ConnectionGraphProjection& m_proj;
};

}  // namespace klicad::auto_layout
