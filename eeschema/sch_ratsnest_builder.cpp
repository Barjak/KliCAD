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

#include <sch_ratsnest_builder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <delaunator.hpp>

#include <connection_graph.h>
#include <sch_item.h>
#include <sch_pin.h>
#include <sch_ratsnest_item.h>


namespace
{

/**
 * Disjoint-set / union-find for Kruskal.  Mirrors pcbnew/ratsnest_data.cpp:75.
 */
class disjoint_set
{
public:
    disjoint_set( std::size_t aSize )
    {
        m_data.resize( aSize );
        m_depth.assign( aSize, 0 );
        std::iota( m_data.begin(), m_data.end(), 0 );
    }

    int find( int aVal )
    {
        int root = aVal;

        while( m_data[root] != root )
            root = m_data[root];

        // Path compression
        while( m_data[aVal] != aVal )
        {
            int next = m_data[aVal];
            m_data[aVal] = root;
            aVal = next;
        }

        return root;
    }

    bool unite( int aA, int aB )
    {
        int ra = find( aA );
        int rb = find( aB );

        if( ra == rb )
            return false;

        if( m_depth[ra] < m_depth[rb] )
        {
            m_data[ra] = rb;
        }
        else if( m_depth[ra] > m_depth[rb] )
        {
            m_data[rb] = ra;
        }
        else
        {
            m_data[rb] = ra;
            m_depth[ra]++;
        }

        return true;
    }

private:
    std::vector<int> m_data;
    std::vector<int> m_depth;
};


/**
 * Edge between two indices into the unique-position node array, with weight =
 * squared Euclidean distance.  Sorted ascending by weight for Kruskal.
 */
struct CANDIDATE_EDGE
{
    int      a;
    int      b;
    int64_t  weight_sq;

    bool operator<( const CANDIDATE_EDGE& aOther ) const
    {
        return weight_sq < aOther.weight_sq;
    }
};


/**
 * @return true if all points lie on a single line (also true for <=2 points).
 *
 * Points must have unique coordinates.  Mirrors areNodesColinear in
 * pcbnew/ratsnest/ratsnest_data.cpp:146.
 */
bool areNodesColinear( const std::vector<VECTOR2I>& aPoints )
{
    if( aPoints.size() <= 2 )
        return true;

    const VECTOR2I p0 = aPoints[0];
    const VECTOR2I v0 = aPoints[1] - p0;

    for( std::size_t i = 2; i < aPoints.size(); ++i )
    {
        const VECTOR2I v1 = aPoints[i] - p0;

        if( v0.Cross( v1 ) != 0 )
            return false;
    }

    return true;
}


int64_t distSq( const VECTOR2I& aA, const VECTOR2I& aB )
{
    const int64_t dx = static_cast<int64_t>( aA.x ) - static_cast<int64_t>( aB.x );
    const int64_t dy = static_cast<int64_t>( aA.y ) - static_cast<int64_t>( aB.y );

    return dx * dx + dy * dy;
}

}  // namespace


namespace SCH_RATSNEST_BUILDER
{

void BuildNet( const std::vector<VECTOR2I>& aPinPositions,
               const wxString&              aNetName,
               SCH_RATSNEST_ITEM&           aOut )
{
    if( aPinPositions.size() < 2 )
        return;

    // Coalesce duplicate positions: the Delaunator chokes on coincident points,
    // and a zero-length edge is useless visually.  Stable order preserved via
    // first-seen insertion.
    std::vector<VECTOR2I> unique;
    unique.reserve( aPinPositions.size() );

    {
        std::vector<VECTOR2I> sorted = aPinPositions;
        std::sort( sorted.begin(), sorted.end(),
                   []( const VECTOR2I& a, const VECTOR2I& b )
                   {
                       if( a.x != b.x )
                           return a.x < b.x;
                       return a.y < b.y;
                   } );

        for( const VECTOR2I& p : sorted )
        {
            if( unique.empty() || unique.back() != p )
                unique.push_back( p );
        }
    }

    if( unique.size() < 2 )
        return;

    if( unique.size() == 2 )
    {
        aOut.AddEdge( unique[0], unique[1], aNetName );
        return;
    }

    // Gather candidate edges from either the colinear chain or the Delaunay
    // triangulation (mirroring pcbnew/ratsnest_data.cpp:228-256).
    std::vector<CANDIDATE_EDGE> candidates;

    if( areNodesColinear( unique ) )
    {
        // The Delaunator can't triangulate colinear points; chain them along
        // the dominant axis (already sorted lex-by-x-then-y above).
        candidates.reserve( unique.size() - 1 );

        for( std::size_t i = 0; i + 1 < unique.size(); ++i )
        {
            candidates.push_back( { static_cast<int>( i ),
                                    static_cast<int>( i + 1 ),
                                    distSq( unique[i], unique[i + 1] ) } );
        }
    }
    else
    {
        std::vector<double> coords;
        coords.reserve( 2 * unique.size() );

        for( const VECTOR2I& p : unique )
        {
            coords.push_back( static_cast<double>( p.x ) );
            coords.push_back( static_cast<double>( p.y ) );
        }

        delaunator::Delaunator d( coords );
        const auto&            triangles = d.triangles;

        candidates.reserve( triangles.size() );  // 3 edges per triangle, dedup later

        auto pushEdge = [&]( std::size_t i, std::size_t j )
        {
            int ia = static_cast<int>( i );
            int ib = static_cast<int>( j );

            if( ia == ib )
                return;

            if( ia > ib )
                std::swap( ia, ib );

            candidates.push_back( { ia, ib, distSq( unique[ia], unique[ib] ) } );
        };

        for( std::size_t i = 0; i < triangles.size(); i += 3 )
        {
            pushEdge( triangles[i],     triangles[i + 1] );
            pushEdge( triangles[i + 1], triangles[i + 2] );
            pushEdge( triangles[i + 2], triangles[i]     );
        }

        // Dedupe (each interior edge appears in two triangles).
        std::sort( candidates.begin(), candidates.end(),
                   []( const CANDIDATE_EDGE& a, const CANDIDATE_EDGE& b )
                   {
                       if( a.a != b.a )
                           return a.a < b.a;
                       return a.b < b.b;
                   } );

        candidates.erase(
                std::unique( candidates.begin(), candidates.end(),
                             []( const CANDIDATE_EDGE& a, const CANDIDATE_EDGE& b )
                             {
                                 return a.a == b.a && a.b == b.b;
                             } ),
                candidates.end() );
    }

    // Kruskal: sort by weight, accept edges that connect new components.
    std::sort( candidates.begin(), candidates.end() );

    disjoint_set dset( unique.size() );

    for( const CANDIDATE_EDGE& e : candidates )
    {
        if( dset.unite( e.a, e.b ) )
            aOut.AddEdge( unique[e.a], unique[e.b], aNetName );
    }
}


void BuildFrom( const CONNECTION_GRAPH& aGraph, SCH_RATSNEST_ITEM& aOut )
{
    aOut.ClearEdges();

    const NET_MAP& netMap = aGraph.GetNetMap();

    for( const auto& [ key, subgraphs ] : netMap )
    {
        // Skip the "unconnected" / empty-name bucket; pcbnew likewise ignores
        // netcode 0 for ratsnest purposes.  The bucket is identified by an
        // empty net name (the Netcode 0 case) -- still walk its pins below if
        // present, since two named pins on a synthetic net should still draw.
        std::vector<VECTOR2I> pinPositions;

        for( const CONNECTION_SUBGRAPH* sg : subgraphs )
        {
            if( !sg )
                continue;

            for( SCH_ITEM* item : sg->GetItems() )
            {
                if( item && item->Type() == SCH_PIN_T )
                {
                    SCH_PIN* pin = static_cast<SCH_PIN*>( item );
                    pinPositions.push_back( pin->GetPosition() );
                }
            }
        }

        if( pinPositions.size() < 2 )
            continue;

        BuildNet( pinPositions, key.Name, aOut );
    }
}

}  // namespace SCH_RATSNEST_BUILDER
