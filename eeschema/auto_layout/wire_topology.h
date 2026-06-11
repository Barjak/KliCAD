/**
 * @file wire_topology.h
 *
 * F-S1d connectivity-invariant pass for the ELK→KiCad writeback.
 *
 * Input: a flat list of orthogonal wire segments (KiCad IU) the
 * writeback emitted from ELK section polylines, with endpoints
 * already overridden to exact pin connection points (invariant 1).
 *
 * Output: the same wire list (unchanged) plus the set of
 * coordinates that need an SCH_JUNCTION (invariants 2 + 3):
 *
 *   (2) ≥3 distinct wire endpoints coincide at one coordinate —
 *       the multi-way meet (star-pattern anchors fall here).
 *   (3) A wire endpoint sits strictly inside another wire's
 *       interior — the mid-segment tap that survives KiCad's
 *       on-save colinear-wire merge (connection_graph.cpp:1399–1411
 *       binds junctions to mid-segments).
 *
 * The function is pure (no SCH_* / wx / pybind11 dependencies) so
 * it is doctest-unit-testable in elk-cpp's test harness or any
 * equivalent.  See sch_elk_adapter.cpp for the call site that
 * forwards each computed coordinate to
 * `SchLayoutTransaction::add_junction`.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <set>
#include <utility>
#include <vector>

#include <math/vector2d.h>

namespace klicad::auto_layout {

/** One orthogonal wire segment in KiCad IU. */
struct WireSeg
{
    VECTOR2I a;
    VECTOR2I b;
};


/** Compute the set of coordinates that require an SCH_JUNCTION
 *  given the writeback's flat wire list.  Returns coordinates as
 *  (x, y) integer pairs (KiCad IU) sorted by std::set ordering
 *  for stable iteration.
 *
 *  Algorithm (O(N²) on segment count; fine at M3-M4 scale):
 *    1. Count wire-endpoint occurrences per coord.  ≥3 → junction.
 *    2. For each endpoint coord that didn't already qualify under
 *       (1), check whether it lies strictly inside any other
 *       orthogonal segment.  If yes → junction.
 *
 *  Invariants:
 *    - Output is deduplicated (std::set).
 *    - Output excludes pure endpoint-on-endpoint pairs with count
 *      == 2 (no junction needed; both endpoints connect through
 *      the connection-graph coordinate map directly).
 *    - Diagonal segments (a.x != b.x && a.y != b.y) are ignored
 *      for the mid-segment containment check.  ELK orthogonal
 *      routing only emits orthogonal segments; diagonals would
 *      be a layout bug surfaced elsewhere.
 */
std::set<std::pair<int, int>>
compute_junctions( const std::vector<WireSeg>& aWires );


/** Count orthogonal wire crossings — pairs of (horizontal, vertical)
 *  segments that intersect at a point that is NOT an endpoint of
 *  either segment.  GOAL.md F-S7 layout-quality hint.
 *
 *  Same-axis overlapping segments are NOT counted (those are a
 *  separate concern that F-S1d's junction pass already handles).
 *  Diagonal segments are also not counted; ELK with EDGE_ROUTING =
 *  ORTHOGONAL doesn't emit them, and a diagonal is a layout bug
 *  surfaced elsewhere.
 *
 *  Complexity: O(N²) on segment count.  Fine for M3 / M4 scale
 *  (~30-100 wires). */
int count_crossings( const std::vector<WireSeg>& aWires );

}  // namespace klicad::auto_layout
