/*
 * GOAL.md F-S4d implementation skeleton.  See header for the spec.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "connection_graph_bus_propagator.h"

#include <connection_graph.h>
#include <sch_connection.h>

namespace klicad {

void BusPropagator::propagate( CONNECTION_GRAPH&     /*aGraph*/,
                               CONNECTION_SUBGRAPH&  /*aParent*/ )
{
    // F-S4d full body lands after F-S4b populates m_matchedEndpoint.
    // For now this is a no-op so the legacy in-line bus logic in
    // CONNECTION_GRAPH::propagateToNeighbors stays load-bearing.
    // When this body lands, the name-arithmetic block at
    // connection_graph.cpp:2993-3043 + the baseMatch fallback at
    // :3121-3124 are deleted with no replacement.
}


bool BusPropagator::involvesBus( const CONNECTION_SUBGRAPH& aSubgraph )
{
    // F-S4d will inspect the subgraph driver's connection type plus
    // any hier port / hier pin connection type.  Stubbed false for
    // now so propagateToNeighbors keeps its inline handling
    // unchanged until the typed reference walk is in place.
    (void) aSubgraph;
    return false;
}

}  // namespace klicad
