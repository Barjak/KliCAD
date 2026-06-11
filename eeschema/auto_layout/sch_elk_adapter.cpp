/**
 * @file sch_elk_adapter.cpp
 *
 * SCH_EDIT_FRAME → ELK Layered adapter.  GOAL.md F-S1a/b/c/F-S2
 * structural rewrite (2026-06-02): the impoverished projection, the
 * NetSpec side-channel, the star-pattern fan-out, the post-layout
 * label translate / coalesce / pin-snap / pin-bridge stages, and the
 * isLowSideNet string heuristic are all gone.  The adapter now reads
 * connectivity from the live CONNECTION_GRAPH via
 * ConnectionGraphProjection, builds one ELK hyperedge per net, runs
 * Layered, and writes back through SchLayoutTransaction (which fires
 * the SCH_COMMIT side-effects: undo, RecalculateConnections, OnModify,
 * canvas refresh).
 *
 * Pre-condition: caller has constructed the live frame with symbols
 * already on the SCH_SCREEN (e.g. via Circuit.to_schematic) and the
 * CONNECTION_GRAPH has been rebuilt at least once.  The adapter calls
 * RecalculateConnections itself at entry to guarantee the projection
 * is current.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sch_elk_adapter.h"

#include "layout_report.h"
#include "sch_layout_transaction.h"
#include "connection_graph_projection.h"
#include "elk_coords.h"
#include "elk_hierarchy_builder.h"
#include "wire_topology.h"

#include <connection_graph.h>
#include <layer_ids.h>
#include <lib_symbol.h>
#include <pin_type.h>
#include <sch_edit_frame.h>
#include <sch_field.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <template_fieldnames.h>

#include "elk/elk.h"
#include "elk/alg/layered/p4nodes/bk/bk_options.h"
#include "elk/core/options/core_options.h"
#include "elk/core/options/direction.h"
#include "elk/core/options/edge_routing.h"
#include "elk/core/options/port_constraints.h"
#include "elk/core/options/port_side.h"
#include "elk/core/util/elk_progress_monitor.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace klicad::auto_layout {

namespace {

namespace co = elk::core::options;

using elk::graph::ElkNode;
using elk::graph::ElkPort;
using elk::graph::ElkEdge;
using elk::graph::ElkBendPoint;
namespace ElkGraphUtil = elk::graph::ElkGraphUtil;


/** KiCad pin extends AWAY from its connection point; the tip (the
 *  thing that touches a wire) sits on the OPPOSITE body side.  So
 *  PIN_RIGHT pins have tips on the WEST side of the body. */
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


/** One ElkNode per SCH_SYMBOL, indexed by symbol UUID. */
struct NodeRec
{
    SCH_SYMBOL* sym  = nullptr;
    ElkNode*    node = nullptr;
    /// `bbox_top_left_world − sym->GetPosition()` at adapter-entry time.
    /// Used to convert ELK's laid-out node origin (which we aligned to
    /// bbox top-left) back to the KiCad symbol reference position.
    VECTOR2I    bboxOffset { 0, 0 };
    /// F-S5 Phase B: leaf-node coords from ELK are relative to the
    /// containing compound; the writeback wants absolute root-frame
    /// coords.  `parentOffset` is the sum of ancestor compound (x, y)
    /// values harvested top-down.  Add to `node->getX()/getY()` to
    /// recover the laid-out absolute position.
    double      parentX = 0.0;
    double      parentY = 0.0;
    std::map<wxString, ElkPort*> portByNum;
};

}  // anonymous namespace


LayoutReport runElkLayout( SchLayoutTransaction& aTxn )
{
    LayoutReport report;
    SCH_EDIT_FRAME& aFrame = aTxn.frame();

    // Ensure CONNECTION_GRAPH reflects the live SCH_SCREEN state before
    // the projection reads it.  Without this, any caller that mutated
    // the screen between project load and the layout call would see
    // stale net membership.
    aFrame.RecalculateConnections( nullptr, GLOBAL_CLEANUP );

    SCH_SHEET_PATH sheetPath = aFrame.GetCurrentSheet();
    SCH_SCREEN*    screen    = sheetPath.LastScreen();
    if( !screen )
    {
        std::fprintf( stderr, "[elk_adapter] no SCH_SCREEN on current sheet\n" );
        return report;
    }

    CONNECTION_GRAPH* cg = aFrame.Schematic().ConnectionGraph();
    if( !cg )
    {
        std::fprintf( stderr, "[elk_adapter] no CONNECTION_GRAPH\n" );
        return report;
    }

    // F-S5 Phase A: hierarchy-wide projection.  The sheet-local
    // overload is gone; one projection covers the entire sheet tree.
    ConnectionGraphProjection proj( *cg );

    // ------------------------------------------------------------------
    // Build the ELK graph via the F-S5 Phase B hierarchy builder.
    // The builder mints compound nodes per SCH_SHEET_PATH, one symbol
    // node per SCH_SYMBOL, ports for pins + sheet-pin / hier-label
    // boundaries, and edges from the projection's NetViews.
    // HIERARCHY_HANDLING = INCLUDE_CHILDREN is set on the root inside
    // the builder (invariant 15); nothing here sets it.
    // ------------------------------------------------------------------
    ElkHierarchyBuilder builder( proj );
    std::unique_ptr<ElkNode> root = builder.build( aFrame );

    // GOAL.md #1 — DIRECTION=DOWN, trust DIRECTION_PREPROCESSOR.
    root->setProperty( co::CoreOptions::DIRECTION(), co::Direction::DOWN );
    root->setProperty( co::CoreOptions::EDGE_ROUTING(), co::EdgeRouting::ORTHOGONAL );
    // SPACING_NODE_NODE controls WITHIN-LAYER spacing only.  The big
    // knob for inter-layer routing channel width is
    // SPACING_NODE_NODE_BETWEEN_LAYERS — without it, ELK Layered
    // defaults to 20-unit gaps and routed sections collapse to
    // sub-grid stubs on grid-snap.  Verified via
    // ~/projects/elk-cpp/cpp/parity/kicad_vdiv_smoke.cpp.
    root->setProperty( co::CoreOptions::SPACING_NODE_NODE(),
                       4.0 * static_cast<double>( SCH_GRID_IU ) );
    root->setProperty( co::CoreOptions::SPACING_EDGE_NODE(),
                       2.0 * static_cast<double>( SCH_GRID_IU ) );
    root->setProperty( co::CoreOptions::SPACING_EDGE_EDGE(),
                       2.0 * static_cast<double>( SCH_GRID_IU ) );
    root->setProperty(
            elk::alg::layered::p4nodes::bk::LayeredOptions::SPACING_NODE_NODE_BETWEEN_LAYERS(),
            4.0 * static_cast<double>( SCH_GRID_IU ) );
    root->setProperty(
            elk::alg::layered::p4nodes::bk::LayeredOptions::SPACING_EDGE_NODE_BETWEEN_LAYERS(),
            2.0 * static_cast<double>( SCH_GRID_IU ) );
    root->setProperty(
            elk::alg::layered::p4nodes::bk::LayeredOptions::SPACING_EDGE_EDGE_BETWEEN_LAYERS(),
            2.0 * static_cast<double>( SCH_GRID_IU ) );

    // Rebuild the writeback lookup tables (KIID → NodeRec + per-pin
    // port map) by walking the ELK graph the builder produced.  Each
    // symbol-node carries its SCH_SYMBOL::m_Uuid as the ELK identifier;
    // we resolve back to the live SCH_SYMBOL hierarchy-wide via
    // SCHEMATIC::Hierarchy().ResolveItem() — the projection scope is
    // the entire sheet tree (F-S5 Phase A), so a sheet-local scan
    // would miss leaf symbols on child sheets.  Compound (sheet)
    // nodes have identifiers starting with "sheet:"; the harvest
    // lambda recurses into them and propagates the cumulative parent
    // origin so leaf-coord writeback gets an absolute root-frame
    // position (invariant 14).
    std::map<KIID, NodeRec> byUuid;

    SCH_SHEET_LIST hierarchy = aFrame.Schematic().Hierarchy();

    std::function<void( ElkNode*, double, double )> harvest =
            [&]( ElkNode* node, double parentX, double parentY )
    {
        for( const auto& child : node->getChildren() )
        {
            ElkNode* cn = child.get();
            const std::string& id = cn->getIdentifier();
            if( id.rfind( "sheet:", 0 ) == 0 )
            {
                harvest( cn, parentX + cn->getX(), parentY + cn->getY() );
                continue;
            }
            if( id.empty() )
                continue;

            KIID kiid( wxString::FromUTF8( id.c_str() ) );
            SCH_SHEET_PATH foundPath;
            SCH_ITEM* item = hierarchy.ResolveItem( kiid, &foundPath,
                                                    /*aAllowNullptrReturn*/ true );
            SCH_SYMBOL* sym = dynamic_cast<SCH_SYMBOL*>( item );
            if( !sym )
                continue;

            NodeRec rec;
            rec.sym     = sym;
            rec.node    = cn;
            rec.parentX = parentX;
            rec.parentY = parentY;

            // Recompute pinMin (the bbox offset) on the fly from the
            // live symbol — the builder uses the same construction
            // (pin-extent), so the offset matches.
            const std::vector<SCH_PIN*>& pins = rec.sym->GetPins();
            VECTOR2I pinMin( 0, 0 );
            bool     havePin = false;
            for( const SCH_PIN* p : pins )
            {
                const VECTOR2I local = p->GetPosition() - rec.sym->GetPosition();
                if( !havePin )
                {
                    pinMin  = local;
                    havePin = true;
                }
                else
                {
                    pinMin.x = std::min( pinMin.x, local.x );
                    pinMin.y = std::min( pinMin.y, local.y );
                }
            }
            rec.bboxOffset = pinMin;

            // Map ports back by pin number via the builder's
            // identifier format "<kiid>:<pin-num>".
            for( const auto& portUp : cn->getPorts() )
            {
                ElkPort* port = portUp.get();
                const std::string& pid = port->getIdentifier();
                const size_t       colon = pid.find( ':' );
                if( colon == std::string::npos )
                    continue;
                const wxString num =
                        wxString::FromUTF8( pid.c_str() + colon + 1 );
                rec.portByNum[num] = port;
            }

            byUuid.emplace( rec.sym->m_Uuid, std::move( rec ) );
        }
    };
    harvest( root.get(), 0.0, 0.0 );

    if( byUuid.empty() )
    {
        std::fprintf( stderr, "[elk_adapter] no symbols on screen\n" );
        report.ok = true;
        return report;
    }

    // ------------------------------------------------------------------
    // Run ELK Layered.
    // ------------------------------------------------------------------
    try
    {
        elk::core::util::BasicProgressMonitor pm;
        elk::Layered().layout( *root, pm );
    }
    catch( const std::exception& ex )
    {
        std::fprintf( stderr, "[elk_adapter] layout failed: %s\n", ex.what() );
        return report;
    }

    // Translate so the layout's min-corner lands at TARGET_ORIGIN_IU
    // (the 40 mm page margin convention).  Leaf-symbol coords inside
    // compound nodes are relative to the containing compound; the
    // harvest lambda already captured the cumulative parent origin in
    // rec.parentX/parentY so we operate in absolute root-frame coords
    // here (invariant 14).
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    for( const auto& [uuid, rec] : byUuid )
    {
        minX = std::min( minX, rec.parentX + rec.node->getX() );
        minY = std::min( minY, rec.parentY + rec.node->getY() );
    }
    const double offX = std::isfinite( minX ) ? ( TARGET_ORIGIN_IU - minX ) : 0.0;
    const double offY = std::isfinite( minY ) ? ( TARGET_ORIGIN_IU - minY ) : 0.0;

    // ------------------------------------------------------------------
    // Writeback through the caller-owned transaction (Fork 5 (a)):
    // we do NOT create one here.  The transaction was opened by the
    // entry point (`compose()` in F-S3, or the legacy
    // `runElkLayout(SCH_EDIT_FRAME&)` wrapper).  Caller commits.
    // ------------------------------------------------------------------
    SchLayoutTransaction& txn = aTxn;

    // Move each symbol to its laid-out position.  ELK node origin is
    // the BBOX top-left; subtract bboxOffset to recover the symbol's
    // reference position (which is what SCH_SYMBOL::SetPosition wants).
    for( auto& [uuid, rec] : byUuid )
    {
        const VECTOR2I nodePos = from_elk(
                ElkPoint{ rec.parentX + rec.node->getX() + offX,
                          rec.parentY + rec.node->getY() + offY } );
        const VECTOR2I newSymPos = nodePos - rec.bboxOffset;
        txn.move( *rec.sym, newSymPos );
        ++report.symbols_placed;
    }

    // F-S1d invariant 1: build a port → post-move pin world coord
    // table.  After txn.move() above, pin->GetPosition() returns the
    // post-layout pin coord — that's the ground truth for connectivity.
    // We use this to override ELK section endpoints below: ELK's
    // section start/end usually match the port world coord, but small
    // double→IU rounding mismatches at the seam can leave wire
    // endpoints one IU off the pin, producing pin_not_connected ERC
    // errors with a visually-correct render.  See [[consumer-invariants]].
    // Keyed by ElkConnectableShape* because edge->getSources() returns
    // that base type even though the elements are always ports here.
    std::map<elk::graph::ElkConnectableShape*, VECTOR2I> portToPinWorld;
    for( auto& [uuid, rec] : byUuid )
    {
        for( SCH_PIN* pin : rec.sym->GetPins() )
        {
            const wxString num = pin->GetNumber();
            auto it = rec.portByNum.find( num );
            if( it != rec.portByNum.end() )
                portToPinWorld[ it->second ] = pin->GetPosition();
        }
    }

    // Remove pre-existing wires AND pre-layout SCH_LABELs.  The
    // labels were emitted by to_schematic's _label_pins at the
    // ORIGINAL pin positions; after the layout pass moves symbols,
    // those labels are stranded ("label_dangling" ERC errors). Until
    // F-S3 deletes _label_pins entirely, the adapter cleans them up
    // post-layout and re-emits the structurally-correct one-per-named-
    // net set after wire emission below.  Hierarchical labels and
    // global labels are LEFT IN PLACE (they participate in cross-
    // sheet connectivity that ELK doesn't own).
    {
        std::vector<SCH_ITEM*> oldWires;
        std::vector<SCH_ITEM*> oldLabels;
        for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
        {
            SCH_LINE* line = static_cast<SCH_LINE*>( item );
            if( line->GetLayer() == LAYER_WIRE )
                oldWires.push_back( line );
        }
        for( SCH_ITEM* item : screen->Items().OfType( SCH_LABEL_T ) )
            oldLabels.push_back( item );
        std::fprintf( stderr,
                      "[elk_adapter] writeback: removing %zu wires, %zu labels\n",
                      oldWires.size(), oldLabels.size() );
        for( SCH_ITEM* w : oldWires )
            txn.remove( *w );
        for( SCH_ITEM* l : oldLabels )
            txn.remove( *l );
    }

    // Emit new SCH_LINEs from the ELK edge sections.  No pin-snap,
    // no coalescing, no pin-bridge: hyperedge routing + the seam's
    // grid snap (from_elk snaps for us) produce orthogonal segments
    // with endpoints already at port positions.  If they don't,
    // that's a real layout bug, not something to paper over.
    //
    // Per-net label tracking: remember the first emitted wire
    // endpoint per net name so we can drop one SCH_LABEL per named
    // signal net after wire emission (the F-S3 per-named-net policy
    // applied surgically; full F-S3 deletes the to_schematic per-pin
    // emit and moves the structural responsibility entirely here).
    std::map<wxString, VECTOR2I> firstEndpointByNet;

    // F-S1d invariants 2 + 3: track every emitted wire's endpoints
    // so after emission we can identify coords needing SCH_JUNCTION.
    // Two cases trigger a junction:
    //   (2) ≥3 distinct wire endpoints coincide at one coord —
    //       multi-way meets (star-pattern anchors fall here).
    //   (3) A wire endpoint sits strictly inside another wire's
    //       segment — the "mid-segment tap" that survives KiCad's
    //       on-save colinear-wire merge.  The junction binds the
    //       tap to the through-wire via the connection_graph
    //       midsegment special case (upstream
    //       connection_graph.cpp:1399–1411).
    std::vector<WireSeg> allWires;

    // F-S5 Phase B fix: ElkHierarchyBuilder mints one compound node per
    // SCH_SHEET_PATH and attaches intra-sheet edges to that compound, not
    // to the root.  Even a single-sheet circuit has one compound (root
    // sheet "/"), so root->getContainedEdges() comes back empty.  Flatten
    // edges from every node in the tree before emit.
    std::vector<ElkEdge*> allEdges;
    {
        std::function<void( ElkNode* )> collectEdges =
                [&]( ElkNode* n )
        {
            for( const auto& edgeUp : n->getContainedEdges() )
                allEdges.push_back( edgeUp.get() );
            for( const auto& childUp : n->getChildren() )
                collectEdges( childUp.get() );
        };
        collectEdges( root.get() );
    }
    std::fprintf( stderr,
                  "[elk_adapter] writeback: %zu edges flattened from hierarchy\n",
                  allEdges.size() );
    for( ElkEdge* edge : allEdges )
    {
        const wxString netName = wxString::FromUTF8( edge->getIdentifier().c_str() );
        std::fprintf( stderr,
                      "[elk_adapter]   edge id=%s sections=%zu\n",
                      edge->getIdentifier().c_str(),
                      edge->getSections().size() );

        // F-S1d invariant 1: look up the source/target pin coords for
        // this edge.  Star-pattern emission means each edge has exactly
        // one source port (the anchor) and one target port (the leaf).
        VECTOR2I srcPinCoord{ 0, 0 };
        VECTOR2I tgtPinCoord{ 0, 0 };
        bool     haveSrcPin = false;
        bool     haveTgtPin = false;
        if( !edge->getSources().empty() )
        {
            auto it = portToPinWorld.find( edge->getSources().front() );
            if( it != portToPinWorld.end() ) { srcPinCoord = it->second; haveSrcPin = true; }
        }
        if( !edge->getTargets().empty() )
        {
            auto it = portToPinWorld.find( edge->getTargets().front() );
            if( it != portToPinWorld.end() ) { tgtPinCoord = it->second; haveTgtPin = true; }
        }

        for( const auto& sec : edge->getSections() )
        {
            std::fprintf( stderr,
                "[elk_adapter]     section start=(%.0f,%.0f) end=(%.0f,%.0f) bends=%zu\n",
                sec->getStartX(), sec->getStartY(),
                sec->getEndX(), sec->getEndY(),
                sec->getBendPoints().size() );

            // Build the polyline in KiCad IU.  Snap every interior
            // point via from_elk; force the section endpoints to the
            // ground-truth pin coords (invariant 1).
            std::vector<VECTOR2I> kp;
            kp.reserve( 2 + sec->getBendPoints().size() );
            kp.push_back( from_elk( { sec->getStartX() + offX,
                                      sec->getStartY() + offY } ) );
            for( const ElkBendPoint& bp : sec->getBendPoints() )
                kp.push_back( from_elk( { bp.getX() + offX, bp.getY() + offY } ) );
            kp.push_back( from_elk( { sec->getEndX() + offX,
                                      sec->getEndY() + offY } ) );

            if( haveSrcPin )
                kp.front() = srcPinCoord;
            if( haveTgtPin )
                kp.back() = tgtPinCoord;

            // After pin-snap, the first/last segment may have gained a
            // diagonal component if ELK's section endpoint was off by
            // <grid from the pin.  Fix by injecting an L-bend that
            // preserves orthogonality: enter the pin along the axis the
            // adjacent ELK segment was already on.
            auto ensureOrthoEntry =
                    [&]( std::vector<VECTOR2I>& pts, size_t pinIdx, size_t neighborIdx )
            {
                if( pinIdx >= pts.size() || neighborIdx >= pts.size() )
                    return;
                const VECTOR2I& pin = pts[pinIdx];
                const VECTOR2I& nb  = pts[neighborIdx];
                if( pin.x == nb.x || pin.y == nb.y )
                    return;  // already orthogonal
                // Original ELK segment direction: whichever axis the
                // pre-override endpoint and the neighbor differed on.
                // Insert a bend that keeps the neighbor-side axis, then
                // turns toward the pin.  Heuristic: enter the pin on
                // the axis of larger displacement so the visible
                // tweak is shorter.
                const VECTOR2I bend{ pin.x, nb.y };  // horizontal first from neighbor
                pts.insert( pts.begin() + ( pinIdx > neighborIdx
                                              ? pinIdx
                                              : neighborIdx ),
                            bend );
            };
            if( haveSrcPin && kp.size() >= 2 )
                ensureOrthoEntry( kp, 0, 1 );
            if( haveTgtPin && kp.size() >= 2 )
                ensureOrthoEntry( kp, kp.size() - 1, kp.size() - 2 );

            for( size_t i = 1; i < kp.size(); ++i )
            {
                const VECTOR2I& a = kp[ i - 1 ];
                const VECTOR2I& b = kp[ i ];
                std::fprintf( stderr,
                    "[elk_adapter]       seg (%d,%d)->(%d,%d)\n",
                    a.x, a.y, b.x, b.y );
                if( a == b )
                    continue;

                txn.add_wire( a, b );
                allWires.push_back( WireSeg{ a, b } );
                ++report.wires_emitted;
                ++report.bends;
                if( !netName.IsEmpty()
                    && firstEndpointByNet.find( netName ) == firstEndpointByNet.end() )
                {
                    firstEndpointByNet[netName] = a;
                }
                const double dx = static_cast<double>( b.x ) - a.x;
                const double dy = static_cast<double>( b.y ) - a.y;
                report.total_wirelength += std::sqrt( dx * dx + dy * dy );
            }
        }
    }

    // F-S1d invariants 2 + 3: emit SCH_JUNCTIONs at every coord
    // where the wire topology requires one.  Pure logic factored
    // into wire_topology.cpp; unit tests cover the canonical
    // shapes (test_wire_topology.cpp).
    {
        const auto junctions = compute_junctions( allWires );
        std::fprintf( stderr,
                      "[elk_adapter] writeback: emitting %zu junctions\n",
                      junctions.size() );
        for( const auto& [x, y] : junctions )
            txn.add_junction( VECTOR2I( x, y ) );
    }

    // F-S7 layout-quality hint: count orthogonal wire crossings on
    // the emitted wire set.  ELK's CROSSING_MINIMIZATION_STRATEGY
    // defaults to LAYER_SWEEP and runs by default, but its result
    // is only observable through what we draw.  The count surfaces
    // to ComposeReport so M3/M4 acceptance can guardrail against
    // regressions and so callers have a numeric quality signal that
    // complements the visual-is-not-evidence standing rule.
    report.crossings = count_crossings( allWires );
    std::fprintf( stderr,
                  "[elk_adapter] writeback: %d wire crossings\n",
                  report.crossings );

    // Emit one SCH_LABEL per named SIGNAL net at the first wire
    // endpoint we recorded for that net.  Power and ground nets are
    // already named by their power-symbol drivers (per
    // ConnectionGraphProjection::NetView::kind) — adding labels for
    // those would create a `same_local_global_label` ERC warning.
    // Cross-sheet nets are carried by hier-label / sheet-pin ports
    // on the compound boundaries the F-S5 Phase B builder mints; the
    // F-S6 writeback owns the typed hier-label / sheet-pin emit.
    for( const NetView& nv : proj.nets() )
    {
        if( nv.kind != NetKind::SIGNAL )
            continue;
        const wxString name = nv.name;
        // Strip leading "/" — CONNECTION_GRAPH returns names like
        // "/MID" but the SCH_LABEL text wants "MID".
        wxString labelText = name.StartsWith( wxT("/") )
                                 ? name.Mid( 1 )
                                 : name;
        if( labelText.IsEmpty() )
            continue;
        auto endpointIt = firstEndpointByNet.find( name );
        if( endpointIt == firstEndpointByNet.end() )
            continue;
        txn.add_label( endpointIt->second, labelText, LabelKind::LOCAL );
    }

    // No commit here — caller owns the transaction.
    report.ok = true;
    return report;
}

}  // namespace klicad::auto_layout
