/**
 * @file sch_ogdf_adapter.cpp
 *
 * @brief Implementation of the SCH_SCREEN → OGDF-fork adapter.
 *        See sch_ogdf_adapter.h for the API and
 *        ~/projects/KliCAD_development/research/c5-adapter-design.md
 *        for the design rationale.
 *
 * Status: Session 1 implementation.  buildNodes + buildPorts
 * are functional; buildEdges, runLayout, and writeBackToScreen
 * are stubs awaiting follow-up sessions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sch_ogdf_adapter.h"

#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <lib_symbol.h>
#include <pin_type.h>
#include <core/typeinfo.h>

#include <ogdf/portconstraints/PortBarycenterHeuristic.h>
#include <ogdf/portconstraints/OrthoPortRouter.h>

namespace klicad::auto_layout {

namespace {

/**
 * @brief Translate KiCad's PIN_ORIENTATION to OGDF PortSide.
 *
 * KiCad encodes "which way the pin extends from its connection
 * point", which is the *opposite* of the body side the pin sits
 * on.  A PIN_RIGHT pin extends right from its tip — so the tip
 * is on the body's LEFT side (West).
 *
 * Mirrors the mapping documented in pin_type.h:104-136.
 */
ogdf::PortSide pinOrientationToSide( PIN_ORIENTATION aOri )
{
	switch( aOri )
	{
	case PIN_ORIENTATION::PIN_RIGHT: return ogdf::PortSide::West;
	case PIN_ORIENTATION::PIN_LEFT:  return ogdf::PortSide::East;
	case PIN_ORIENTATION::PIN_UP:    return ogdf::PortSide::South;
	case PIN_ORIENTATION::PIN_DOWN:  return ogdf::PortSide::North;
	case PIN_ORIENTATION::INHERIT:
	default:                         return ogdf::PortSide::Undefined;
	}
}

}  // anonymous namespace


SchOgdfAdapter::SchOgdfAdapter( SCH_SCREEN& aScreen )
	: m_screen( aScreen )
	, m_GA( m_graph, ogdf::GraphAttributes::nodeGraphics
	                  | ogdf::GraphAttributes::edgeGraphics )
	, m_PGA( m_GA )
{
}


SchOgdfAdapter::~SchOgdfAdapter() = default;


void SchOgdfAdapter::buildFromScreen()
{
	buildNodes();
	buildPorts();
	buildEdges();
}


void SchOgdfAdapter::buildNodes()
{
	// One OGDF node per SCH_SYMBOL on the screen.  Body dimensions
	// come from the LIB_SYMBOL bounding box; current position
	// seeds GraphAttributes (so partial-layout / incremental work
	// in M4 can preserve unmoved symbols' coordinates).
	for( SCH_ITEM* item : m_screen.Items().OfType( SCH_SYMBOL_T ) )
	{
		SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
		ogdf::node  n   = m_graph.newNode();
		m_symbolToNode[sym] = n;
		m_nodeToSymbol[n]   = sym;

		const std::unique_ptr<LIB_SYMBOL>& libSym = sym->GetLibSymbolRef();
		if( libSym )
		{
			const BOX2I bbox = libSym->GetBoundingBox();
			m_GA.width( n )  = static_cast<double>( bbox.GetWidth() );
			m_GA.height( n ) = static_cast<double>( bbox.GetHeight() );
		}
		else
		{
			m_GA.width( n )  = 0.0;
			m_GA.height( n ) = 0.0;
		}

		const VECTOR2I pos = sym->GetPosition();
		m_GA.x( n ) = static_cast<double>( pos.x );
		m_GA.y( n ) = static_cast<double>( pos.y );
	}
}


void SchOgdfAdapter::buildPorts()
{
	// For each placed symbol, add an OGDF Port per pin.  The pin's
	// anchor is its position relative to the symbol body's
	// top-left corner — KiCad pin positions are absolute, so we
	// subtract the symbol's (x, y) to get a node-local anchor.
	//
	// Side is derived from PIN_ORIENTATION via the local helper.
	// All ports are FIXED_POS (default) because KiCad symbols
	// have library-baked pin positions that the router must
	// honor exactly.
	for( auto& [sym, n] : m_symbolToNode )
	{
		const double nodeX = m_GA.x( n );
		const double nodeY = m_GA.y( n );

		for( SCH_PIN* pin : sym->GetPins() )
		{
			const ogdf::PortSide side =
				pinOrientationToSide( pin->GetOrientation() );

			const VECTOR2I pinPos = pin->GetPosition();
			const double   anchorX = static_cast<double>( pinPos.x ) - nodeX;
			const double   anchorY = static_cast<double>( pinPos.y ) - nodeY;

			const int portIdx = m_PGA.addPort( n, side, anchorX, anchorY );
			m_pinToPort[pin] = { n, portIdx };
		}
	}
}


void SchOgdfAdapter::buildEdges()
{
	// TODO (next session): walk the screen's CONNECTION_GRAPH (or
	// for the simplest M1 case, walk SCH_LINE items + match
	// endpoints to known pin positions in m_pinToPort) to derive
	// the edge set.
	//
	// Stubbed for the C.5 Session 1 scaffold so the build link
	// resolves; without edges, the layout pipeline runs but
	// produces no routing — useful only for placement-only tests.
}


void SchOgdfAdapter::runLayout()
{
	// TODO (next session): build a Hierarchy from the graph,
	// run port-aware Sugiyama (PortBarycenterHeuristic),
	// FastHierarchyLayout for x-placement, then
	// OrthoPortRouter::routeBetween for each adjacent layer
	// pair.  See sch_ogdf_adapter.h docstring for the phase
	// breakdown.
}


LayoutReport SchOgdfAdapter::writeBackToScreen()
{
	// TODO (next session): translate m_GA positions back to
	// SCH_SYMBOL m_Pos (recentering on body, not corner) and
	// emit SCH_LINE items from m_GA.bends().
	LayoutReport report;
	report.ok             = false;  // not yet implemented
	report.symbols_placed = static_cast<int>( m_symbolToNode.size() );
	report.wires_emitted  = 0;
	return report;
}


LayoutReport SchOgdfAdapter::run()
{
	buildFromScreen();
	runLayout();
	return writeBackToScreen();
}

}  // namespace klicad::auto_layout
