/**
 * @file sch_ogdf_adapter.cpp
 *
 * @brief Implementation of the SCH_SCREEN → OGDF-fork adapter.
 *        See sch_ogdf_adapter.h for the API and
 *        ~/projects/KliCAD_development/research/c5-adapter-design.md
 *        for the design rationale.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sch_ogdf_adapter.h"

#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <lib_symbol.h>
#include <pin_type.h>
#include <layer_ids.h>
#include <core/typeinfo.h>

#include <ogdf/basic/geometry.h>
#include <ogdf/layered/SugiyamaLayout.h>
#include <ogdf/portconstraints/PortBarycenterHeuristic.h>
#include <ogdf/portconstraints/OrthoPortRouter.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace klicad::auto_layout {

namespace {

/**
 * @brief Translate KiCad's PIN_ORIENTATION to OGDF PortSide.
 *
 * KiCad encodes "which way the pin extends from its connection
 * point", which is the *opposite* of the body side the pin sits
 * on.  A PIN_RIGHT pin extends right from its tip — so the tip
 * is on the body's LEFT side (West).  Mirrors the mapping
 * documented in pin_type.h:104-136.
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
}


void SchOgdfAdapter::buildNodes()
{
	for( SCH_ITEM* item : m_screen.Items().OfType( SCH_SYMBOL_T ) )
	{
		SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
		ogdf::node  n   = m_graph.newNode();
		m_symbolToNode[sym] = n;
		m_nodeToSymbol[n]   = sym;

		// Reference designator → SCH_SYMBOL* index for the spec-
		// driven edge builder.  Sheet path 0 == top-level; M2
		// hierarchy work will need to thread the right path here.
		const wxString ref = sym->GetRef( nullptr, false );
		m_refToSymbol[ ref.ToStdString() ] = sym;

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

			const std::string pinNum = pin->GetNumber().ToStdString();
			m_nodePinToPort[ { n, pinNum } ] = portIdx;
		}
	}
}


int SchOgdfAdapter::buildEdgesFromSpec( const std::vector<NetSpec>& aNets )
{
	int edgeCount = 0;

	for( const NetSpec& net : aNets )
	{
		if( net.pins.size() < 2 )
			continue;

		// Resolve every (ref, pin) to a (node, port-index) pair, skipping
		// references / pins not present in this screen (the caller may
		// pass a superset; we route what we can find).
		struct Endpoint { ogdf::node node; int portIdx; };
		std::vector<Endpoint> endpoints;
		endpoints.reserve( net.pins.size() );

		for( const auto& [ref, pinNum] : net.pins )
		{
			auto symIt = m_refToSymbol.find( ref );
			if( symIt == m_refToSymbol.end() )
				continue;

			ogdf::node n = m_symbolToNode[ symIt->second ];
			auto portIt = m_nodePinToPort.find( { n, pinNum } );
			if( portIt == m_nodePinToPort.end() )
				continue;

			endpoints.push_back( { n, portIt->second } );
		}

		if( endpoints.size() < 2 )
			continue;

		// Star pattern: connect every endpoint after the anchor to the
		// anchor.  Matches the existing Python ELK adapter's edge model
		// and is what OGDF's Sugiyama expects for hyper-nets.
		const Endpoint& anchor = endpoints.front();
		for( size_t i = 1; i < endpoints.size(); ++i )
		{
			const Endpoint& other = endpoints[i];
			ogdf::edge e = m_graph.newEdge( anchor.node, other.node );
			m_PGA.setEdgePortsByIndex( e, anchor.portIdx, other.portIdx );
			++edgeCount;
		}
	}

	return edgeCount;
}


void SchOgdfAdapter::runLayout()
{
	if( m_graph.numberOfNodes() == 0 )
		return;

	// Stage A — port-aware Sugiyama placement.
	//
	// SugiyamaLayout owns the LayerByLayerSweep pointer; we hand off
	// our PortBarycenterHeuristic with the PGA pre-injected.  The
	// heuristic's call() walks the internal GraphCopy and maps each
	// adj-entry back to the original edge via GC.original(e), then
	// reads the per-edge port from m_PGA — same path the OGDF-fork
	// test_port_barycenter.cpp validated.
	auto* heur = new ogdf::PortBarycenterHeuristic();
	heur->setPortGraphAttributes( &m_PGA );

	ogdf::SugiyamaLayout SL;
	SL.setCrossMin( heur );

	try
	{
		SL.call( m_GA );
	}
	catch( const std::exception& )
	{
		// Layout failed; positions remain at their seed values.
		return;
	}

	// Stage B — derive layers from x-coordinates post-Sugiyama so we
	// can route between adjacent layer pairs.  SugiyamaLayout
	// produces left-to-right placement; nodes within a layer share
	// (approximately) the same x.  Group by a tolerance that's a
	// fraction of the expected inter-layer spacing.
	std::vector<std::pair<double, ogdf::node>> byX;
	byX.reserve( m_graph.numberOfNodes() );
	for( ogdf::node n : m_graph.nodes )
		byX.emplace_back( m_GA.x( n ), n );

	std::sort( byX.begin(), byX.end(),
	           []( const auto& a, const auto& b ) { return a.first < b.first; } );

	// Tolerance: 1/4 of the median inter-x gap if the graph has
	// multiple layers, otherwise infinity (single-layer case).
	double tolerance = 0.0;
	if( byX.size() >= 2 )
	{
		std::vector<double> gaps;
		for( size_t i = 1; i < byX.size(); ++i )
		{
			const double g = byX[i].first - byX[i - 1].first;
			if( g > 0.0 )
				gaps.push_back( g );
		}
		if( !gaps.empty() )
		{
			std::sort( gaps.begin(), gaps.end() );
			tolerance = 0.25 * gaps[ gaps.size() / 2 ];
		}
	}

	std::vector<std::vector<ogdf::node>> layers;
	double lastX = -1e30;
	for( const auto& [x, n] : byX )
	{
		if( layers.empty() || x - lastX > tolerance )
			layers.emplace_back();
		layers.back().push_back( n );
		lastX = x;
	}

	// Stage C — port-aware orthogonal routing between adjacent layers.
	// Channel spacing: one KiCad grid unit (2.54 mm = 2540000 nm).
	const double GRID_NM = 2540000.0;
	ogdf::OrthoPortRouter router( m_PGA, /*edgeSpacing=*/GRID_NM );

	for( size_t i = 0; i + 1 < layers.size(); ++i )
	{
		router.routeBetween( layers[i], layers[i + 1] );
	}
}


LayoutReport SchOgdfAdapter::writeBackToScreen()
{
	LayoutReport report;

	// Stage A — symbol positions.  Round to the nearest integer (KiCad
	// stores positions in internal nanometer-grid integer units).
	for( auto& [sym, n] : m_symbolToNode )
	{
		const VECTOR2I newPos( static_cast<int>( std::lround( m_GA.x( n ) ) ),
		                       static_cast<int>( std::lround( m_GA.y( n ) ) ) );
		sym->SetPosition( newPos );
		++report.symbols_placed;
	}

	// Stage B — clear any pre-existing wires (we replaced them with
	// our routing) then emit fresh SCH_LINE items from the bend lists.
	std::vector<SCH_ITEM*> wiresToRemove;
	for( SCH_ITEM* item : m_screen.Items().OfType( SCH_LINE_T ) )
	{
		SCH_LINE* line = static_cast<SCH_LINE*>( item );
		if( line->GetLayer() == LAYER_WIRE )
			wiresToRemove.push_back( line );
	}
	for( SCH_ITEM* w : wiresToRemove )
		m_screen.Remove( w );

	for( ogdf::edge e : m_graph.edges )
	{
		const ogdf::DPolyline& bends = m_GA.bends( e );
		if( bends.size() < 2 )
			continue;

		auto it = bends.begin();
		ogdf::DPoint prev = *it;
		++it;
		while( it != bends.end() )
		{
			const ogdf::DPoint curr = *it;
			++it;

			const VECTOR2I a( static_cast<int>( std::lround( prev.m_x ) ),
			                  static_cast<int>( std::lround( prev.m_y ) ) );
			const VECTOR2I b( static_cast<int>( std::lround( curr.m_x ) ),
			                  static_cast<int>( std::lround( curr.m_y ) ) );

			// Skip degenerate zero-length segments — happen at edge
			// endpoints where the bend coincides with the port anchor.
			if( a == b )
			{
				prev = curr;
				continue;
			}

			SCH_LINE* line = new SCH_LINE( a, LAYER_WIRE );
			line->SetEndPoint( b );
			m_screen.Append( line );
			++report.wires_emitted;
			++report.bends;

			const double dx = curr.m_x - prev.m_x;
			const double dy = curr.m_y - prev.m_y;
			report.total_wirelength += std::sqrt( dx * dx + dy * dy );

			prev = curr;
		}
	}

	report.ok = true;
	return report;
}


LayoutReport SchOgdfAdapter::run( const std::vector<NetSpec>& aNets )
{
	buildFromScreen();
	buildEdgesFromSpec( aNets );
	runLayout();
	return writeBackToScreen();
}

}  // namespace klicad::auto_layout
