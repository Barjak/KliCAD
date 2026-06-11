/*
 * F-S1d connectivity-invariant unit tests for
 * klicad::auto_layout::compute_junctions.
 *
 * Covers the five canonical shapes called out in GOAL.md:
 *  - two_pin: 2-pin net with one wire → no junctions
 *  - three_pin_star: 3 wires meet at one endpoint → 1 junction
 *  - tee_split: one wire endpoint lands mid-segment of another → 1 junction
 *  - colinear_merge: shorter wire subsumed by longer wire's interior
 *    (the M3 BJT amp anchor-merge case) → 1 junction at the tap
 *  - no_false_positives: two endpoints coinciding (2 not ≥3) → no junction
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include "../../../eeschema/auto_layout/wire_topology.h"

using klicad::auto_layout::WireSeg;
using klicad::auto_layout::compute_junctions;
using klicad::auto_layout::count_crossings;

namespace
{

inline VECTOR2I P( int x, int y ) { return { x, y }; }

}  // namespace


BOOST_AUTO_TEST_SUITE( WireTopology )


BOOST_AUTO_TEST_CASE( TwoPinNoJunction )
{
    // 2-pin net: one wire from pin A to pin B; no junction.
    std::vector<WireSeg> wires = {
        { P( 0, 0 ), P( 1000, 0 ) },
    };
    auto js = compute_junctions( wires );
    BOOST_CHECK_EQUAL( js.size(), 0u );
}


BOOST_AUTO_TEST_CASE( ThreePinStarOneJunction )
{
    // Star pattern: anchor at (0,0) with three radiating wires.
    // The anchor sees 3 wire endpoints coinciding → junction at (0,0).
    std::vector<WireSeg> wires = {
        { P( 0, 0 ), P( 1000, 0 ) },
        { P( 0, 0 ), P( 0, 1000 ) },
        { P( 0, 0 ), P( -1000, 0 ) },
    };
    auto js = compute_junctions( wires );
    BOOST_REQUIRE_EQUAL( js.size(), 1u );
    BOOST_CHECK( js.count( { 0, 0 } ) == 1 );
}


BOOST_AUTO_TEST_CASE( TeeSplitMidSegment )
{
    // One vertical wire whose endpoint (0,500) lies mid-segment of a
    // horizontal wire that runs from (-1000, 500) through (0, 500) to
    // (1000, 500).  The tee branch's endpoint hits the through-wire's
    // interior → junction at (0, 500).
    std::vector<WireSeg> wires = {
        { P( -1000, 500 ), P( 1000, 500 ) },  // horizontal through-wire
        { P( 0, 500 ),     P( 0, 1500 ) },    // vertical tee branch
    };
    auto js = compute_junctions( wires );
    BOOST_REQUIRE_EQUAL( js.size(), 1u );
    BOOST_CHECK( js.count( { 0, 500 } ) == 1 );
}


BOOST_AUTO_TEST_CASE( ColinearMergeAnchorTap )
{
    // The M3 BJT amp case.  Two colinear vertical segments share
    // anchor (0,0): one short to (0, 500), one long to (0, 1000).
    // (These are the two star-edge first-segments KiCad merges into
    // (0,0)→(0,1000) on save.)  A third segment branches at the
    // intermediate point (0, 500) → (1000, 500) — its left endpoint
    // sits at coord (0, 500), which is mid-interior of the long
    // segment.  Without a junction at (0, 500) the connection is
    // lost after merge.
    std::vector<WireSeg> wires = {
        { P( 0, 0 ),    P( 0, 500 ) },     // short anchor segment
        { P( 0, 0 ),    P( 0, 1000 ) },    // long anchor segment (subsumes the short)
        { P( 0, 500 ),  P( 1000, 500 ) },  // branch out at intermediate
    };
    auto js = compute_junctions( wires );
    // (0, 0) has 2 endpoints (the two anchor segments) — not yet ≥3.
    // (0, 500) has 2 endpoints (short's end + branch's start) — also
    // not ≥3, but the branch's endpoint sits mid-segment of the long
    // anchor → junction required by invariant 3.
    BOOST_REQUIRE( js.count( { 0, 500 } ) == 1 );
}


BOOST_AUTO_TEST_CASE( TwoEndpointsNoJunction )
{
    // Two wires meeting at a single coord (endpoint↔endpoint) — only
    // 2 endpoints at the meet, the connection-graph map binds them
    // without needing a junction dot.  Junction-set must NOT
    // include the meet.
    std::vector<WireSeg> wires = {
        { P( 0, 0 ),    P( 0, 1000 ) },
        { P( 0, 1000 ), P( 1000, 1000 ) },
    };
    auto js = compute_junctions( wires );
    BOOST_CHECK_EQUAL( js.size(), 0u );
}


BOOST_AUTO_TEST_CASE( CountCrossingsBasic )
{
    // F-S7: classic + sign — one horizontal and one vertical
    // segment crossing at the midpoint.  No shared endpoints.
    // Expect exactly 1 crossing.
    std::vector<WireSeg> wires = {
        { P( -1000, 0 ), P( 1000, 0 ) },     // horizontal through origin
        { P( 0, -1000 ), P( 0, 1000 ) },     // vertical through origin
    };
    BOOST_CHECK_EQUAL( count_crossings( wires ), 1 );
}


BOOST_AUTO_TEST_CASE( CountCrossingsEndpointTouchIsNotCrossing )
{
    // L-bend share — vertical's endpoint touches horizontal's
    // endpoint at the corner.  That's a connection, not a
    // crossing.  Expect 0.
    std::vector<WireSeg> wires = {
        { P( 0, 0 ),    P( 1000, 0 ) },
        { P( 1000, 0 ), P( 1000, 1000 ) },
    };
    BOOST_CHECK_EQUAL( count_crossings( wires ), 0 );
}


BOOST_AUTO_TEST_CASE( CountCrossingsTeeIsNotCrossing )
{
    // T-junction — vertical endpoint sits ON the horizontal's
    // mid-segment.  F-S1d treats this as a junction case; for
    // crossing-count purposes, we count it as NOT a crossing
    // (it's a connection that wants a junction dot, not a
    // visual crossover).  The function's strict-inequality on
    // the y-axis check rules this out.
    std::vector<WireSeg> wires = {
        { P( -1000, 0 ), P( 1000, 0 ) },    // horizontal through origin
        { P( 0, 0 ),     P( 0, 1000 ) },    // vertical endpoint at (0,0) — tee
    };
    BOOST_CHECK_EQUAL( count_crossings( wires ), 0 );
}


BOOST_AUTO_TEST_CASE( CountCrossingsMultiplePairs )
{
    // Grid: 2 horizontals × 2 verticals → 4 crossings.
    std::vector<WireSeg> wires = {
        { P( -1000, 100 ),  P( 1000, 100 ) },   // h1
        { P( -1000, 200 ),  P( 1000, 200 ) },   // h2
        { P( -500, -100 ),  P( -500, 500 ) },   // v1
        { P( 500, -100 ),   P( 500, 500 ) },    // v2
    };
    BOOST_CHECK_EQUAL( count_crossings( wires ), 4 );
}


BOOST_AUTO_TEST_SUITE_END()
