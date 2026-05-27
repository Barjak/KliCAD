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

#include <sch_ratsnest_item.h>

#include <layer_ids.h>


SCH_RATSNEST_ITEM::SCH_RATSNEST_ITEM() :
        EDA_ITEM( SCH_RATSNEST_ITEM_T )
{
}


void SCH_RATSNEST_ITEM::ClearEdges()
{
    m_edges.clear();
}


void SCH_RATSNEST_ITEM::AddEdge( const VECTOR2I& aA, const VECTOR2I& aB,
                                 const wxString& aNetName )
{
    m_edges.push_back( SCH_RATSNEST_EDGE{ aA, aB, aNetName } );
}


EDA_ITEM* SCH_RATSNEST_ITEM::Clone() const
{
    return new SCH_RATSNEST_ITEM( *this );
}


const BOX2I SCH_RATSNEST_ITEM::ViewBBox() const
{
    // Always-visible: the renderer (B.5) will iterate edges and clip per-edge.
    BOX2I bbox;
    bbox.SetMaximum();
    return bbox;
}


std::vector<int> SCH_RATSNEST_ITEM::ViewGetLayers() const
{
    return { LAYER_SCH_RATSNEST };
}
