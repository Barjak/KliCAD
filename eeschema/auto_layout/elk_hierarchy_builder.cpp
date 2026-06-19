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

        // The root sheet IS the top-level ELK graph, not a nested compound.
        // Putting root symbols inside a root-path compound makes every
        // parent<->child net an edge from one compound's interior to a
        // sibling compound's port -- a multi-level crossing ELK Layered
        // rejects, which leaves the child compound's far boundary port
        // without an external-port dummy (the "more hierarchical ports than
        // dummies" failure).  Only child sheets (deeper paths) get compounds;
        // the root path maps to the top-level graph so root symbols sit at
        // the same level as the child compounds they connect to.
        if( aPath.size() <= 1 )
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

    // NOTE: compound (multi-sheet) boundaries get NO manually-minted ELK
    // ports here.  A cross-sheet net is expressed in Pass 5 as a single
    // DIRECT cross-hierarchy edge (an inside child-symbol pin → an outside
    // root-symbol pin).  ELK's compound preprocessor (ElkGraphImporter +
    // the hierarchical layer-sweep) splits that edge at the compound border
    // and creates exactly one EXTERNAL_PORT *dummy* with its matching
    // hierarchical LPort — the known-good model exercised by
    // elk-cpp/.../test_hierarchical_import.cpp.
    //
    // The previous F-S5 model minted an ElkPort per crossing on the compound
    // AND a "degree-2 bridge" (inside edge insidePin→port, outside edge
    // port→root-sibling).  ELK imports each manual external ElkPort as an
    // EXTERNAL_PORT dummy *inside* the compound, so the bridge's "outside"
    // edge then ran dummy→root-sibling = a fresh cross-hierarchy edge that
    // the preprocessor re-split into a SECOND external port on the same
    // compound.  Result: 2 manual ports → 4 hierarchical ports, none of
    // which carries PORT_DUMMY, tripping
    // LayerSweepCrossingMinimizer::sortPortDummiesByPortPositions
    // ("more hierarchical ports than dummies").  Letting the preprocessor
    // own the entire boundary representation yields #ports == #dummies by
    // construction.
    //
    // The writeback (sch_elk_adapter.cpp) does NOT read boundary-port
    // identifiers back: it emits wires from edge sections (forcing the
    // symbol-pin endpoints to their post-move world coords) and one
    // SCH_LABEL per named signal net, which is what carries cross-sheet
    // connectivity for ERC.  So dropping the manual sheet_pin:/hier_label:
    // ports loses no writeback identity.

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
        // Collect every symbol-pin port that participates in this net,
        // grouped by the ELK level (compound) it lives in.  The grouping
        // key is the containing compound ElkNode*: symbols on the same
        // sheet path share a compound (getCompound is memoised per path),
        // and the single-sheet/root path maps to root.  Pins in the SAME
        // group are starred together (intra-level edges); ONE direct edge
        // between two group anchors expresses a cross-hierarchy crossing,
        // which ELK's compound preprocessor splits into an EXTERNAL_PORT
        // dummy + its hierarchical LPort.
        std::map<ElkNode*, std::vector<ElkPort*>> portsByLevel;
        std::vector<ElkNode*> levelOrder;   // stable: first-seen order
        for( const PinKey& pk : nv.pins )
        {
            auto symIt = symbols.find( pk.symbol_uuid );
            if( symIt == symbols.end() )
                continue;
            auto portIt = symIt->second.portByNum.find( pk.pin_number );
            if( portIt == symIt->second.portByNum.end() )
                continue;
            ElkNode* level = getCompound( symIt->second.path );
            auto [lit, inserted] = portsByLevel.try_emplace( level );
            if( inserted )
                levelOrder.push_back( level );
            lit->second.push_back( portIt->second );
        }

        // Single-sheet sheet-node boundary ports (Pass 4 singleSheet
        // branch): these sit on a sibling sheet-node at root level, not on
        // a compound.  Enrol them into the root level's group so the net
        // routes from symbol pins to the sheet pins.  No compound crossing
        // is involved in single-sheet mode.
        std::set<ElkPort*> seenBoundaryPort;
        for( const KIID& kid : nv.boundary_kiids )
        {
            auto bit = portByBoundaryKiid.find( kid );
            if( bit == portByBoundaryKiid.end() )
                continue;
            if( !seenBoundaryPort.insert( bit->second ).second )
                continue;
            auto [lit, inserted] = portsByLevel.try_emplace( root.get() );
            if( inserted )
                levelOrder.push_back( root.get() );
            lit->second.push_back( bit->second );
        }

        std::size_t totalPorts = 0;
        for( const auto& [lvl, ports] : portsByLevel )
            totalPorts += ports.size();
        if( totalPorts < 2 )
            continue;

        auto addEdge = [&]( ElkPort* a, ElkPort* b )
        {
            ElkEdge* edge = ElkGraphUtil::createSimpleEdge( a, b );
            edge->setIdentifier( nv.name.ToStdString() );
        };

        // Star within each level (anchor = ports[0]); collect each level's
        // anchor in stable order.
        std::vector<ElkPort*> levelAnchors;
        for( ElkNode* lvl : levelOrder )
        {
            const std::vector<ElkPort*>& ports = portsByLevel[lvl];
            for( std::size_t k = 1; k < ports.size(); ++k )
                addEdge( ports[0], ports[k] );
            levelAnchors.push_back( ports[0] );
        }

        // Chain the level anchors with ONE direct edge each.  When two
        // anchors live in different compounds, that edge is a cross-
        // hierarchy crossing; ELK creates exactly one external port +
        // dummy per such edge (#hierarchical-ports == #dummies by
        // construction).  An intra-level net (one group) has no chaining
        // edge and falls through unchanged (M2/M3 behaviour preserved).
        for( std::size_t k = 1; k < levelAnchors.size(); ++k )
            addEdge( levelAnchors[k - 1], levelAnchors[k] );

        ++edgesBuilt;
    }

    std::fprintf( stderr,
                  "[elk_hierarchy_builder] %zu compounds, %zu symbols, "
                  "%zu boundary kiids, %d nets\n",
                  compoundByPath.size(), symbols.size(),
                  portByBoundaryKiid.size(), edgesBuilt );

    return root;
}

}  // namespace klicad::auto_layout
