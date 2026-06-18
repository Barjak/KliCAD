/**
 * @file elk_hierarchy_builder.cpp
 *
 * GOAL.md F-S5 Phase B implementation.  See elk_hierarchy_builder.h
 * for the contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "elk_hierarchy_builder.h"

#include "connection_graph_projection.h"
#include "elk_coords.h"

#include <connection_graph.h>
#include <sch_edit_frame.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <template_fieldnames.h>

#include "elk/elk.h"
#include "elk/alg/layered/options/layered_options_ext.h"
#include "elk/core/options/core_options.h"
#include "elk/core/options/hierarchy_handling.h"
#include "elk/core/options/port_constraints.h"
#include "elk/core/options/port_side.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>


namespace klicad::auto_layout {

namespace {

namespace co = elk::core::options;
namespace lo = elk::alg::layered::options::LayeredOptions;

using elk::graph::ElkNode;
using elk::graph::ElkPort;
using elk::graph::ElkEdge;
namespace ElkGraphUtil = elk::graph::ElkGraphUtil;


/** KiCad pin extends AWAY from its connection point; the tip (the
 *  thing that touches a wire) sits on the OPPOSITE body side.  So
 *  PIN_RIGHT pins have tips on the WEST side of the body.  Duplicated
 *  from sch_elk_adapter.cpp deliberately — the seam between adapter
 *  and builder is intentional and this small mapping is the only thing
 *  that would otherwise need a shared header. */
co::PortSide pinOrientationToSide( PIN_ORIENTATION aOri )
{
    switch( aOri )
    {
    case PIN_ORIENTATION::PIN_RIGHT: return co::PortSide::WEST;
    case PIN_ORIENTATION::PIN_LEFT:  return co::PortSide::EAST;
    case PIN_ORIENTATION::PIN_UP:    return co::PortSide::SOUTH;
    case PIN_ORIENTATION::PIN_DOWN:  return co::PortSide::NORTH;
    case PIN_ORIENTATION::INHERIT:
    default:                         return co::PortSide::UNDEFINED;
    }
}


/** A single resolved symbol: which SCH_SYMBOL it is, which sheet path
 *  contains it, and the ElkNode/Ports we minted for it.  Keyed by
 *  KIID externally; the symbol's UUID is the canonical id at every
 *  seam (the writeback reads node identifier → KIID → SCH_SYMBOL). */
struct SymbolBuild
{
    SCH_SYMBOL*                  sym = nullptr;
    SCH_SHEET_PATH               path;
    ElkNode*                     node = nullptr;
    std::map<wxString, ElkPort*> portByNum;
};


/** Build the symbol node + per-pin ports inside the given compound.
 *  Mirrors the pin-extent dimensioning the flat adapter used: node
 *  dim = (pinMax − pinMin); port anchors = pin local − pinMin.  Single-
 *  pin symbols get a 1×1 dim so ELK accepts the node. */
SymbolBuild makeSymbolNode( SCH_SYMBOL* aSym, ElkNode* aCompound )
{
    SymbolBuild rec;
    rec.sym  = aSym;
    rec.node = ElkGraphUtil::createNode( aCompound );

    // Identifier = SCH_SYMBOL's KIID stringified.  The writeback
    // recovers the SCH_SYMBOL* by KIID lookup against the live
    // SCH_SCREEN; using KIID (not the REFERENCE field) keeps the
    // identifier stable across annotation passes.
    rec.node->setIdentifier( aSym->m_Uuid.AsString().ToStdString() );

    const std::vector<SCH_PIN*>& pins = aSym->GetPins();
    VECTOR2I pinMin( 0, 0 );
    VECTOR2I pinMax( 0, 0 );
    bool     havePin = false;
    for( const SCH_PIN* p : pins )
    {
        const VECTOR2I local = p->GetPosition() - aSym->GetPosition();
        if( !havePin )
        {
            pinMin  = local;
            pinMax  = local;
            havePin = true;
        }
        else
        {
            pinMin.x = std::min( pinMin.x, local.x );
            pinMin.y = std::min( pinMin.y, local.y );
            pinMax.x = std::max( pinMax.x, local.x );
            pinMax.y = std::max( pinMax.y, local.y );
        }
    }

    const double w = std::max( 1.0, static_cast<double>( pinMax.x - pinMin.x ) );
    const double h = std::max( 1.0, static_cast<double>( pinMax.y - pinMin.y ) );
    rec.node->setDimensions( w, h );
    rec.node->setProperty( co::CoreOptions::PORT_CONSTRAINTS(),
                           co::PortConstraints::FIXED_POS );

    for( SCH_PIN* pin : pins )
    {
        ElkPort* port = ElkGraphUtil::createPort( rec.node );
        port->setProperty( co::CoreOptions::PORT_SIDE(),
                           pinOrientationToSide( pin->GetOrientation() ) );
        const VECTOR2I local  = pin->GetPosition() - aSym->GetPosition();
        const VECTOR2I anchor = local - pinMin;
        port->setLocation( static_cast<double>( anchor.x ),
                           static_cast<double>( anchor.y ) );
        port->setDimensions( 0.0, 0.0 );

        const wxString num = pin->GetNumber();
        if( !num.IsEmpty() )
        {
            port->setIdentifier(
                    rec.node->getIdentifier() + ":" + num.ToStdString() );
            rec.portByNum[num] = port;
        }
    }

    return rec;
}


/** Comparator that orders sheet paths by their hash; lets us key a
 *  std::map<SCH_SHEET_PATH, ...> without needing operator< on the
 *  path itself (SCH_SHEET_PATH::Cmp is signed-3-way, not strict <). */
struct SheetPathLess
{
    bool operator()( const SCH_SHEET_PATH& a, const SCH_SHEET_PATH& b ) const
    {
        return a.GetCurrentHash() < b.GetCurrentHash();
    }
};

}  // anonymous namespace


ElkHierarchyBuilder::ElkHierarchyBuilder( const ConnectionGraphProjection& aProj ) :
    m_proj( aProj )
{
}


std::unique_ptr<ElkNode> ElkHierarchyBuilder::build( SCH_EDIT_FRAME& aFrame )
{
    SCH_SHEET_LIST hierarchy = aFrame.Schematic().Hierarchy();

    // ------------------------------------------------------------------
    // Pass 1: enumerate every SCH_SYMBOL referenced by the projection,
    // resolve its containing SCH_SHEET_PATH via Hierarchy().ResolveItem.
    // ------------------------------------------------------------------
    std::map<KIID, SymbolBuild> symbols;
    for( const NetView& nv : m_proj.nets() )
    {
        for( const PinKey& pk : nv.pins )
        {
            if( symbols.find( pk.symbol_uuid ) != symbols.end() )
                continue;

            SCH_SHEET_PATH path;
            SCH_ITEM* item = hierarchy.ResolveItem( pk.symbol_uuid, &path,
                                                    /*aAllowNullptrReturn*/ true );
            SCH_SYMBOL* sym = dynamic_cast<SCH_SYMBOL*>( item );
            if( !sym )
                continue;

            SymbolBuild rec;
            rec.sym  = sym;
            rec.path = path;
            symbols.emplace( pk.symbol_uuid, std::move( rec ) );
        }
    }

    if( symbols.empty() )
        throw std::runtime_error(
                "ElkHierarchyBuilder: projection references no symbols" );

    // ------------------------------------------------------------------
    // Build the root and set HIERARCHY_HANDLING (invariant 15).  Other
    // layout-tuning options (DIRECTION, SPACING_*, EDGE_ROUTING) are
    // set by the adapter; the builder owns hierarchy semantics only.
    // ------------------------------------------------------------------
    std::unique_ptr<ElkNode> root = ElkGraphUtil::createGraph();
    root->setProperty( lo::HIERARCHY_HANDLING(),
                       co::HierarchyHandling::INCLUDE_CHILDREN );

    // ------------------------------------------------------------------
    // Pass 2: one compound child per unique SCH_SHEET_PATH.  Compound
    // identifier = the path hash stringified so the writeback (and tests)
    // can correlate compound → sheet.
    //
    // Single-sheet bypass (2026-06-08): when the projection has only
    // one sheet path, wrapping it in a compound demonstrably hurts ELK
    // Layered's intra-sheet routing quality (M3 BJT amp regressed from
    // 0 to 7 routing errors with the wrapper; baseline restored when
    // bypassed).  In the single-sheet case, symbols live directly on
    // root and getCompound() returns root.  Multi-sheet layouts still
    // mint per-sheet compounds — that's the whole point of F-S5
    // Phase B and there's no regression for them.
    // ------------------------------------------------------------------
    std::set<SCH_SHEET_PATH, SheetPathLess> uniquePaths;
    for( const auto& [uuid, rec] : symbols )
        uniquePaths.insert( rec.path );
    const bool singleSheet = uniquePaths.size() <= 1;

    std::map<SCH_SHEET_PATH, ElkNode*, SheetPathLess> compoundByPath;
    auto getCompound = [&]( const SCH_SHEET_PATH& aPath ) -> ElkNode*
    {
        if( singleSheet )
            return root.get();

        auto it = compoundByPath.find( aPath );
        if( it != compoundByPath.end() )
            return it->second;

        ElkNode* compound = ElkGraphUtil::createNode( root.get() );
        compound->setIdentifier(
                wxString::Format( wxT("sheet:%zu"), aPath.GetCurrentHash() )
                        .ToStdString() );
        // Set INCLUDE_CHILDREN on every compound so ELK Layered
        // recurses into it for intra-sheet routing; INHERIT defaults
        // empirically don't route the intra-compound edges.
        compound->setProperty( lo::HIERARCHY_HANDLING(),
                               co::HierarchyHandling::INCLUDE_CHILDREN );
        compoundByPath.emplace( aPath, compound );
        return compound;
    };

    // Ensure every sheet path that hosts a resolved symbol has a
    // compound (no-op in single-sheet mode).
    for( const auto& [uuid, rec] : symbols )
        (void) getCompound( rec.path );

    // ------------------------------------------------------------------
    // Pass 3: mint the symbol nodes inside the right compound.
    // ------------------------------------------------------------------
    for( auto& [uuid, rec] : symbols )
    {
        ElkNode*    compound = getCompound( rec.path );
        SymbolBuild built    = makeSymbolNode( rec.sym, compound );
        rec.node             = built.node;
        rec.portByNum        = std::move( built.portByNum );
    }

    // ------------------------------------------------------------------
    // Pass 4: boundary ports.  For every sheet path that has a parent
    // in `hierarchy`, walk the parent screen for the SCH_SHEET item
    // pointing at this path's leaf SCH_SHEET; expose its sheet pins
    // as ports on the child's compound (parent-facing boundary).  For
    // the child sheet's own screen, walk SCH_HIER_LABEL items and
    // mint matching ports.
    //
    // Identifiers: sheet pin → "sheet_pin:<kiid>", hier label →
    // "hier_label:<kiid>".  The writeback (F-S6) will cross-reference
    // these to place SCH_SHEET_PIN / SCH_HIERLABEL items at the
    // ELK-chosen port positions; for this phase the ports exist so
    // ELK Layered can plan around them.
    // ------------------------------------------------------------------
    std::map<KIID, ElkPort*> portByBoundaryKiid;

    // M4 single-sheet bypass extension: when single-sheet mode is on
    // (compoundByPath is empty), boundary ports for any SCH_SHEET
    // items in the root screen still need to land on root.  Without
    // them, ELK has no port to route wires to the sheet pins, and
    // pin_not_connected ERC fires on the symbol pins that connect
    // through the sheet.  Walk the root sheet path's screen for
    // SCH_SHEET items + add ports on root for their sheet pins.
    if( singleSheet )
    {
        SCH_SHEET_PATH rootPath = aFrame.GetCurrentSheet();
        SCH_SCREEN* rootScreen = rootPath.LastScreen();
        if( rootScreen )
        {
            for( SCH_ITEM* item : rootScreen->Items().OfType( SCH_SHEET_T ) )
            {
                SCH_SHEET* sheet = dynamic_cast<SCH_SHEET*>( item );
                if( !sheet )
                    continue;

                // Mint a dedicated ELK node for the sheet (sibling of
                // symbol nodes), with sheet pins as its ports.  ELK
                // Layered rejects edges that cross hierarchy levels
                // (e.g. leaf-symbol-port → root-port), so the sheet
                // boundary needs to live at the same level as the
                // symbol nodes.  This sheet-node carries the SCH_SHEET's
                // KIID as its identifier so the writeback can recover
                // the SCH_SHEET* and translate ELK's position to
                // SCH_SHEET::SetPosition (a future writeback step;
                // M4 first needs wires emitted, then sheet positioning).
                ElkNode* sheetNode = ElkGraphUtil::createNode( root.get() );
                sheetNode->setIdentifier(
                        ( wxT("sheet:") + sheet->m_Uuid.AsString() )
                                .ToStdString() );
                sheetNode->setDimensions( 25.4 * 1e4, 25.4 * 1e4 );
                sheetNode->setProperty( co::CoreOptions::PORT_CONSTRAINTS(),
                                        co::PortConstraints::FREE );

                int spIdx = 0;
                for( SCH_SHEET_PIN* sp : sheet->GetPins() )
                {
                    if( !sp )
                        continue;
                    ElkPort* port = ElkGraphUtil::createPort( sheetNode );
                    port->setIdentifier(
                            ( wxT("sheet_pin:") + sp->m_Uuid.AsString() )
                                    .ToStdString() );
                    port->setProperty( co::CoreOptions::PORT_SIDE(),
                                       co::PortSide::WEST );
                    port->setDimensions( 0.0, 0.0 );
                    portByBoundaryKiid[sp->m_Uuid] = port;
                    ++spIdx;
                }
                (void) spIdx;
            }
        }
    }

    for( auto& [path, compound] : compoundByPath )
    {
        // Parent-facing: the SCH_SHEET item that hosts this path lives
        // on the path's penultimate sheet's screen.  Walk its pins.
        if( path.size() >= 1 )
        {
            SCH_SHEET* leafSheet = path.Last();
            if( leafSheet )
            {
                for( SCH_SHEET_PIN* sp : leafSheet->GetPins() )
                {
                    if( !sp )
                        continue;
                    ElkPort* port = ElkGraphUtil::createPort( compound );
                    port->setIdentifier(
                            ( wxT("sheet_pin:") + sp->m_Uuid.AsString() )
                                    .ToStdString() );
                    port->setDimensions( 0.0, 0.0 );
                    portByBoundaryKiid[sp->m_Uuid] = port;

                    // F-S5 port unification: a cross-sheet crossing is ONE
                    // external port in ELK's compound model, not a sheet-pin
                    // port plus a hier-label port.  Two ports per crossing
                    // leave two INSIDE_CONNECTIONS ports on the boundary but
                    // only one external-port dummy, which trips ELK's
                    // layer-sweep (sortPortDummiesByPortPositions: more
                    // hierarchical ports than dummies).  Map the matched
                    // child-side hier label (F-S4b bidirectional cross-link)
                    // onto this same port so the hier-label loop below reuses
                    // it instead of minting a second boundary port.
                    const KIID& matched = sp->GetMatchedEndpoint();
                    if( matched != niluuid )
                        portByBoundaryKiid[matched] = port;
                }
            }

            // Child-side: SCH_HIER_LABEL items on the leaf sheet's
            // own screen become ports on the same compound.  ELK
            // sees these as additional terminals on the boundary;
            // the writeback resolves them post-layout.
            SCH_SCREEN* leafScreen = path.LastScreen();
            if( leafScreen )
            {
                for( SCH_ITEM* item : leafScreen->Items().OfType( SCH_HIER_LABEL_T ) )
                {
                    if( !item )
                        continue;
                    // Already unified onto its matched sheet pin's port above?
                    // Reuse it — don't mint a second boundary port for the
                    // same crossing (see the F-S5 port-unification note).
                    if( portByBoundaryKiid.count( item->m_Uuid ) )
                        continue;
                    ElkPort* port = ElkGraphUtil::createPort( compound );
                    port->setIdentifier(
                            ( wxT("hier_label:") + item->m_Uuid.AsString() )
                                    .ToStdString() );
                    port->setDimensions( 0.0, 0.0 );
                    portByBoundaryKiid[item->m_Uuid] = port;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Pass 5: edges.  One star-pattern fan-out per NetView with ≥2
    // reachable ports.  Cross-sheet pins resolve to ports inside
    // different compounds; ELK Layered's compound layout handles
    // routing across the boundary.
    //
    // Hyperedge promotion (createEdge + multi-source/multi-target) is
    // a future elk-cpp port-completeness upgrade; star-pattern is the
    // robust idiom today (see sch_elk_adapter.cpp's prior note).
    // ------------------------------------------------------------------
    int edgesBuilt = 0;
    for( const NetView& nv : m_proj.nets() )
    {
        // Collect every port that participates in this net: symbol
        // pins (via portByNum) + boundary ports (sheet pins, hier
        // labels) via portByBoundaryKiid.  Without the boundary side,
        // M4 nets that cross from a symbol pin into a sheet pin have
        // only one reachable port and never get routed → wire-emit
        // count stays at 0.
        std::vector<ElkPort*> pinPorts;
        pinPorts.reserve( nv.pins.size() );

        for( const PinKey& pk : nv.pins )
        {
            auto symIt = symbols.find( pk.symbol_uuid );
            if( symIt == symbols.end() )
                continue;
            auto portIt = symIt->second.portByNum.find( pk.pin_number );
            if( portIt == symIt->second.portByNum.end() )
                continue;
            pinPorts.push_back( portIt->second );
        }

        // Deduplicate boundary ports: after F-S5 unification a crossing's
        // sheet-pin KIID and hier-label KIID resolve to the SAME ElkPort,
        // so a NetView that lists both must not enrol that port twice.
        std::vector<ElkPort*> boundaryPorts;
        std::set<ElkPort*> seenBoundaryPort;
        for( const KIID& kid : nv.boundary_kiids )
        {
            auto bit = portByBoundaryKiid.find( kid );
            if( bit == portByBoundaryKiid.end() )
                continue;
            if( seenBoundaryPort.insert( bit->second ).second )
                boundaryPorts.push_back( bit->second );
        }

        if( pinPorts.size() + boundaryPorts.size() < 2 )
            continue;

        // Anchor the star at a boundary port when the net crosses a sheet:
        // child pins then connect TO the external port (inside connections)
        // and the parent-side pin connects to the SAME port (outside
        // connection) — the compound-port shape ELK expects, yielding one
        // external-port dummy per crossing.  Pure intra-sheet nets keep a
        // pin anchor (unchanged M2/M3 behaviour).
        ElkPort* anchor = !boundaryPorts.empty() ? boundaryPorts.front()
                                                 : pinPorts.front();

        auto connect = [&]( ElkPort* aPort )
        {
            if( aPort == anchor )
                return;
            ElkEdge* edge = ElkGraphUtil::createSimpleEdge( anchor, aPort );
            edge->setIdentifier( nv.name.ToStdString() );
        };

        for( ElkPort* p : pinPorts )
            connect( p );
        for( ElkPort* p : boundaryPorts )
            connect( p );

        ++edgesBuilt;
    }

    std::fprintf( stderr,
                  "[elk_hierarchy_builder] %zu compounds, %zu symbols, "
                  "%zu boundary ports, %d nets\n",
                  compoundByPath.size(), symbols.size(),
                  portByBoundaryKiid.size(), edgesBuilt );

    return root;
}

}  // namespace klicad::auto_layout
