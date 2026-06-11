/**
 * @file wire_topology.cpp
 *
 * Implementation of F-S1d's compute_junctions.  See wire_topology.h
 * for invariants and algorithm sketch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wire_topology.h"

#include <algorithm>
#include <map>

namespace klicad::auto_layout {

std::set<std::pair<int, int>>
compute_junctions( const std::vector<WireSeg>& aWires )
{
    std::map<std::pair<int, int>, int> endpointCount;
    for( const WireSeg& w : aWires )
    {
        ++endpointCount[ { w.a.x, w.a.y } ];
        ++endpointCount[ { w.b.x, w.b.y } ];
    }

    std::set<std::pair<int, int>> junctions;

    // Case (2): three or more endpoints at the same coordinate.
    for( const auto& [coord, n] : endpointCount )
        if( n >= 3 )
            junctions.insert( coord );

    // Case (3): endpoint sits strictly inside another segment.
    for( const auto& [coord, _] : endpointCount )
    {
        if( junctions.count( coord ) )
            continue;

        const VECTOR2I c( coord.first, coord.second );

        for( const WireSeg& w : aWires )
        {
            if( c == w.a || c == w.b )
                continue;  // endpoint, not interior

            if( w.a.x == w.b.x && c.x == w.a.x )
            {
                const int ylo = std::min( w.a.y, w.b.y );
                const int yhi = std::max( w.a.y, w.b.y );
                if( ylo < c.y && c.y < yhi )
                {
                    junctions.insert( coord );
                    break;
                }
            }
            else if( w.a.y == w.b.y && c.y == w.a.y )
            {
                const int xlo = std::min( w.a.x, w.b.x );
                const int xhi = std::max( w.a.x, w.b.x );
                if( xlo < c.x && c.x < xhi )
                {
                    junctions.insert( coord );
                    break;
                }
            }
        }
    }

    return junctions;
}


int count_crossings( const std::vector<WireSeg>& aWires )
{
    // Partition into horizontal and vertical (in KiCad IU; we trust
    // ELK ORTHOGONAL routing not to emit diagonals).
    struct H { int y, xlo, xhi; };
    struct V { int x, ylo, yhi; };

    std::vector<H> hSegs;
    std::vector<V> vSegs;
    hSegs.reserve( aWires.size() );
    vSegs.reserve( aWires.size() );

    for( const WireSeg& w : aWires )
    {
        if( w.a.y == w.b.y && w.a.x != w.b.x )
        {
            hSegs.push_back( { w.a.y,
                               std::min( w.a.x, w.b.x ),
                               std::max( w.a.x, w.b.x ) } );
        }
        else if( w.a.x == w.b.x && w.a.y != w.b.y )
        {
            vSegs.push_back( { w.a.x,
                               std::min( w.a.y, w.b.y ),
                               std::max( w.a.y, w.b.y ) } );
        }
        // Skip zero-length and diagonal segments.
    }

    int crossings = 0;
    for( const H& h : hSegs )
    {
        for( const V& v : vSegs )
        {
            // Geometric intersection: vertical's x lies strictly
            // inside horizontal's x range, and horizontal's y lies
            // strictly inside vertical's y range.  Strict inequality
            // excludes endpoint coincidence — an L-bend share is a
            // connection, not a crossing.
            if( h.xlo < v.x && v.x < h.xhi
                && v.ylo < h.y && h.y < v.yhi )
            {
                ++crossings;
            }
        }
    }

    return crossings;
}

}  // namespace klicad::auto_layout
