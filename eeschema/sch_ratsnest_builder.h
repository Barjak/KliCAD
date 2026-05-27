/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @brief Adapter that builds a schematic ratsnest from a CONNECTION_GRAPH.
 *
 * For each net in the schematic's connectivity graph, collects all
 * SCH_PIN positions, runs a Delaunay triangulation on those points,
 * extracts the minimum spanning tree via Kruskal, and appends one
 * SCH_RATSNEST_EDGE per MST edge to the target SCH_RATSNEST_ITEM.
 *
 * Parallels pcbnew/ratsnest/ratsnest_data.cpp (RN_NET::compute /
 * kruskalMST / TRIANGULATOR_STATE::Triangulate), but operates on flat
 * 2D pin coordinates and emits VECTOR2I-pair edges directly.  No
 * persistent triangulator state is kept — BuildFrom is a pure function
 * over its inputs.
 */

#ifndef SCH_RATSNEST_BUILDER_H
#define SCH_RATSNEST_BUILDER_H

#include <vector>
#include <wx/string.h>
#include <math/vector2d.h>

class CONNECTION_GRAPH;
class SCH_RATSNEST_ITEM;


namespace SCH_RATSNEST_BUILDER
{
    /**
     * Clear @p aOut and repopulate it with one ratsnest edge per MST edge,
     * for every net in @p aGraph that has two or more distinct SCH_PIN
     * positions.
     *
     * Nets with fewer than two pins contribute no edges.  Pins that share
     * the exact same position are coalesced into a single node (no zero-
     * length self-edges are emitted), but every unique-position pin in a
     * net of size N >= 2 ends up connected, yielding N-1 MST edges.
     *
     * @param aGraph schematic connectivity graph (CONNECTION_GRAPH::GetNetMap()).
     * @param aOut   target ratsnest item; ClearEdges() is called first.
     */
    void BuildFrom( const CONNECTION_GRAPH& aGraph, SCH_RATSNEST_ITEM& aOut );

    /**
     * Internal helper exposed for unit testing.
     *
     * Given a flat list of pin positions for a single net, compute the
     * Delaunay-derived MST edges and append them to @p aOut tagged with
     * @p aNetName.  No clearing is performed.  This is the function B.4's
     * refresh hook would use if it wanted to bypass CONNECTION_GRAPH
     * iteration (e.g. for synthetic test inputs).
     *
     * Handles three special cases that the raw Delaunator would mishandle:
     *   - <2 distinct points: emit nothing.
     *   - exactly 2 distinct points: emit a single edge.
     *   - >=3 colinear points: sort along the dominant axis and chain.
     */
    void BuildNet( const std::vector<VECTOR2I>& aPinPositions,
                   const wxString&              aNetName,
                   SCH_RATSNEST_ITEM&           aOut );
}

#endif // SCH_RATSNEST_BUILDER_H
