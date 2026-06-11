/**
 * @file elk_coords.h
 *
 * Single coordinate seam between KiCad's eeschema internal units (IU,
 * 1 IU = 0.1 µm; Y-axis points down, same as ELK's internal convention)
 * and the ELK Layered library's double-precision coordinate frame.
 *
 * GOAL.md F-S1c invariant: nothing in eeschema/auto_layout/ may do
 * ad-hoc `+offX`, `+offY`, `rotate90Clockwise`, or `snapToGrid` on raw
 * doubles or int IU values.  Every coordinate translation goes through
 * the free functions declared here.
 *
 * Rotation note: ELK's DIRECTION_PREPROCESSOR (the resolved decision in
 * GOAL.md #1) rotates the LGraph internally for DIRECTION=DOWN; the seam
 * itself is a straight IU↔double mapping with the page-margin offset
 * subtracted/added.  Direction handling is ELK's concern, not the
 * seam's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <math/box2.h>
#include <math/vector2d.h>


namespace klicad::auto_layout {

/** ELK coordinates are doubles in the same "world unit" the input
 *  graph defines — for KliCAD's seam, world units == IU.  Keeping
 *  this as a typed wrapper is what makes "no raw doubles" enforceable. */
struct ElkPoint
{
    double x;
    double y;
};


struct ElkSize
{
    double w;
    double h;
};


/** Single source of truth for the schematic grid.  50 mils = 1.27 mm
 *  at SCH_IU_PER_MM = 1e4.  The duplicated SCH_GRID_IU in
 *  sch_elk_adapter.cpp and sch_ogdf_adapter.cpp is being excised. */
constexpr int SCH_GRID_IU = 12700;


/** Canonical page-margin origin.  Layouts are translated so the
 *  bounding-box min-corner lands here.  40 mm == 400000 IU. */
constexpr double TARGET_ORIGIN_IU = 400000.0;


/** Snap an integer IU coordinate to the nearest multiple of
 *  SCH_GRID_IU.  Standard round-half-away-from-zero. */
inline int snap_to_grid( int aIu )
{
    if( aIu >= 0 )
        return ( ( aIu + SCH_GRID_IU / 2 ) / SCH_GRID_IU ) * SCH_GRID_IU;
    else
        return -snap_to_grid( -aIu );
}


inline VECTOR2I snap_to_grid( const VECTOR2I& p )
{
    return { snap_to_grid( p.x ), snap_to_grid( p.y ) };
}


/** KiCad → ELK.  Identity mapping (both Y-down, same scale); separate
 *  function so future seam changes are localized.  Caller is responsible
 *  for any pre-translation (e.g. subtracting the page origin). */
inline ElkPoint to_elk( const VECTOR2I& p )
{
    return ElkPoint{ static_cast<double>( p.x ), static_cast<double>( p.y ) };
}


inline ElkSize to_elk( const BOX2I& bbox )
{
    return ElkSize{ static_cast<double>( bbox.GetWidth() ),
                    static_cast<double>( bbox.GetHeight() ) };
}


/** ELK → KiCad.  Rounds and snaps to grid in one step — every
 *  position written back to SCH_SCREEN lands on the schematic grid by
 *  construction. */
inline VECTOR2I from_elk( const ElkPoint& p )
{
    auto round_to_int = []( double v ) -> int
    {
        return v >= 0.0 ? static_cast<int>( v + 0.5 )
                        : -static_cast<int>( -v + 0.5 );
    };

    return snap_to_grid( VECTOR2I{ round_to_int( p.x ), round_to_int( p.y ) } );
}


/** Page-margin offset.  Subtract before to_elk if the seam should
 *  hand ELK a layout with min-corner at (0, 0); add the offset back
 *  from_elk so the result lands at TARGET_ORIGIN_IU. */
inline VECTOR2I origin_offset()
{
    return { static_cast<int>( TARGET_ORIGIN_IU ), static_cast<int>( TARGET_ORIGIN_IU ) };
}

}  // namespace klicad::auto_layout
