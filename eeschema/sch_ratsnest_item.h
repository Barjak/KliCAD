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
 * @brief Schematic ratsnest data item.
 *
 * Holds a flat list of logical pin-to-pin edges that should be drawn as
 * ratsnest lines on the schematic.  The class itself is a passive container
 * of edges; the renderer (SCH_PAINTER) and the data-source adapter that
 * populates the edge list are implemented separately.
 *
 * Parallels pcbnew/ratsnest/ratsnest_view_item.{h,cpp}.
 */

#ifndef SCH_RATSNEST_ITEM_H
#define SCH_RATSNEST_ITEM_H

#include <vector>
#include <eda_item.h>
#include <math/vector2d.h>
#include <wx/string.h>


/**
 * A single ratsnest edge: two endpoints in schematic world coordinates
 * plus an optional net name (for color-by-net / debug overlays).
 */
struct SCH_RATSNEST_EDGE
{
    VECTOR2I a;
    VECTOR2I b;
    wxString net_name;
};


class SCH_RATSNEST_ITEM : public EDA_ITEM
{
public:
    SCH_RATSNEST_ITEM();

    ~SCH_RATSNEST_ITEM() override = default;

    static inline bool ClassOf( const EDA_ITEM* aItem )
    {
        return aItem && aItem->Type() == SCH_RATSNEST_ITEM_T;
    }

    wxString GetClass() const override
    {
        return wxT( "SCH_RATSNEST_ITEM" );
    }

    /**
     * Remove all edges.  Called by the refresh hook before rebuilding.
     */
    void ClearEdges();

    /**
     * Append a single ratsnest edge.
     *
     * @param aA       first endpoint, schematic world coordinates
     * @param aB       second endpoint, schematic world coordinates
     * @param aNetName optional net name (used for color-by-net and debug)
     */
    void AddEdge( const VECTOR2I& aA, const VECTOR2I& aB,
                  const wxString& aNetName = wxEmptyString );

    /**
     * @return the current set of edges (read-only).
     */
    const std::vector<SCH_RATSNEST_EDGE>& GetEdges() const { return m_edges; }

    // EDA_ITEM overrides ----------------------------------------------------

    EDA_ITEM* Clone() const override;

    /// Ratsnest item is always visible; the renderer culls per-edge if needed.
    const BOX2I ViewBBox() const override;

    /// Renders on the dedicated schematic ratsnest layer added in M1.1.
    std::vector<int> ViewGetLayers() const override;

    /// Not selectable.
    bool HitTest( const VECTOR2I& aPosition, int aAccuracy = 0 ) const override
    {
        return false;
    }

#if defined( DEBUG )
    void Show( int nestLevel, std::ostream& os ) const override {}
#endif

protected:
    std::vector<SCH_RATSNEST_EDGE> m_edges;
};


#endif // SCH_RATSNEST_ITEM_H
