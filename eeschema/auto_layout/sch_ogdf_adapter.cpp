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
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_label.h>
#include <sch_field.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <lib_symbol.h>
#include <pin_type.h>
#include <layer_ids.h>
#include <template_fieldnames.h>
#include <core/typeinfo.h>

#include <ogdf/basic/geometry.h>
#include <ogdf/layered/FastHierarchyLayout.h>
#include <ogdf/layered/SugiyamaLayout.h>
#include <ogdf/portconstraints/PortBarycenterHeuristic.h>
#include <ogdf/portconstraints/OrthoPortRouter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
		// driven edge builder.  Read the REFERENCE field directly
		// rather than calling SCH_SYMBOL::GetRef(nullptr) — that
		// path goes through SCH_SHEET_PATH::Path() which derefs
		// an empty SCH_SHEET_INSTANCE vector and SEGVs in the
		// API-thread context where no current sheet path is set.
		// M2 hierarchy work will revisit this with proper sheet
		// path threading.
		std::string refStr;
		if( SCH_FIELD* refField = sym->GetField( FIELD_T::REFERENCE ) )
			refStr = refField->GetText().ToStdString();
		if( !refStr.empty() )
			m_refToNode[ refStr ] = n;

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

	// M2: hierarchical sheets are also nodes.  Each SCH_SHEET item on
	// the screen becomes one OGDF node with width/height = sheet
	// bounding box.  Its sheet pins become ports in buildPorts().
	// klicad-python's Circuit.instance(ref=...) emits one sheet per
	// subcircuit instance; the ref string lives in the sheet's
	// FIELD_T::SHEETNAME (or fallback to FIELD_T::REFERENCE).
	for( SCH_ITEM* item : m_screen.Items().OfType( SCH_SHEET_T ) )
	{
		SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );
		ogdf::node n     = m_graph.newNode();
		m_sheetToNode[ sheet ] = n;
		m_nodeToSheet[ n ]     = sheet;

		std::string refStr;
		if( SCH_FIELD* f = sheet->GetField( FIELD_T::SHEET_NAME ) )
			refStr = f->GetText().ToStdString();
		if( !refStr.empty() )
			m_refToNode[ refStr ] = n;

		const BOX2I bbox = sheet->GetBoundingBox();
		m_GA.width( n )  = static_cast<double>( bbox.GetWidth() );
		m_GA.height( n ) = static_cast<double>( bbox.GetHeight() );

		const VECTOR2I pos = sheet->GetPosition();
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

			// NOTE: addPort returns a GLOBAL port id (m_nextPortId++);
			// setEdgePortsByIndex consumes a PER-NODE vector index
			// (m_ports[v][idx]).  Capture the per-node index here so
			// buildEdgesFromSpec's lookups feed the right value to
			// setEdgePortsByIndex.
			m_PGA.addPort( n, side, anchorX, anchorY );
			const int portLocalIdx =
				static_cast<int>( m_PGA.ports( n ).size() ) - 1;

			const std::string pinNum = pin->GetNumber().ToStdString();
			m_nodePinToPort[ { n, pinNum } ] = portLocalIdx;
		}
	}

	// M2: sheet pins → ports.  Sheet pins use SHEET_SIDE (LEFT/RIGHT/
	// TOP/BOTTOM) where SCH_SYMBOL pins used PIN_ORIENTATION.  Keyed
	// by pin NAME (the wxString text) rather than number — klicad-
	// python's spec emits port-name keys for SubcircuitInstance
	// connections, matching the SCH_SHEET_PIN's GetText().
	auto sheetSideToPortSide = []( SHEET_SIDE s ) {
		switch( s )
		{
		case SHEET_SIDE::LEFT:   return ogdf::PortSide::West;
		case SHEET_SIDE::RIGHT:  return ogdf::PortSide::East;
		case SHEET_SIDE::TOP:    return ogdf::PortSide::North;
		case SHEET_SIDE::BOTTOM: return ogdf::PortSide::South;
		default:                 return ogdf::PortSide::Undefined;
		}
	};

	for( auto& [sheet, n] : m_sheetToNode )
	{
		const double nodeX = m_GA.x( n );
		const double nodeY = m_GA.y( n );

		for( SCH_SHEET_PIN* pin : sheet->GetPins() )
		{
			const ogdf::PortSide side = sheetSideToPortSide( pin->GetSide() );
			const VECTOR2I pinPos = pin->GetPosition();
			const double anchorX = static_cast<double>( pinPos.x ) - nodeX;
			const double anchorY = static_cast<double>( pinPos.y ) - nodeY;

			m_PGA.addPort( n, side, anchorX, anchorY );
			const int portLocalIdx =
				static_cast<int>( m_PGA.ports( n ).size() ) - 1;

			const std::string pinName = pin->GetText().ToStdString();
			m_nodePinToPort[ { n, pinName } ] = portLocalIdx;
		}
	}
}


int SchOgdfAdapter::buildEdgesFromSpec( const std::vector<NetSpec>& aNets )
{
	int edgeCount = 0;

	// Augment the Python-emitted spec with power symbols actually
	// present on the screen.  Spec_pane_harness emits power symbols
	// (#PWR_VCC, #PWR_GND, etc.) at to_schematic time, but klicad-
	// python's _net_spec_for only iterates Circuit.parts and never
	// sees them — so VCC and GND nets show up with a single pin each
	// (the consumer pin) and get dropped by the >=2-pin filter below.
	// Without the augmentation power symbols become floating OGDF
	// nodes with no edges and the layout clusters them at arbitrary
	// positions.
	//
	// Power symbol convention: ref starts with '#PWR', Value field
	// equals the net name, single pin numbered "1" of PT_POWER_IN type.
	std::vector<NetSpec> nets = aNets;
	std::map<std::string, std::vector<std::pair<std::string,std::string>>*>
	    netByName;
	for( NetSpec& ns : nets )
		netByName[ ns.net_name ] = &ns.pins;

	int augmented = 0;
	for( SCH_ITEM* item : m_screen.Items().OfType( SCH_SYMBOL_T ) )
	{
		SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
		std::string ref;
		if( SCH_FIELD* f = sym->GetField( FIELD_T::REFERENCE ) )
			ref = f->GetText().ToStdString();
		if( ref.rfind( "#PWR", 0 ) != 0 )
			continue;

		std::string netName;
		if( SCH_FIELD* f = sym->GetField( FIELD_T::VALUE ) )
			netName = f->GetText().ToStdString();
		if( netName.empty() )
			continue;

		auto it = netByName.find( netName );
		if( it == netByName.end() )
		{
			nets.push_back( { netName, {} } );
			netByName[ netName ] = &nets.back().pins;
			it = netByName.find( netName );
		}

		// High-side power (VCC, +3V3, +5V, ...) goes to the FRONT of
		// the pin list so it becomes the star-pattern anchor and
		// Sugiyama places it ABOVE the consumer.  Low-side (GND, -5V,
		// VSS, ...) goes to the BACK so the consumer is above and the
		// power symbol sits below.  The heuristic checks the net name
		// directly — robust for the conventional KiCad power libraries
		// and easy to extend per-project.
		auto isLowSide = []( const std::string& n ) {
			return n == "GND" || n == "VSS" || n == "GNDA" || n == "GNDD"
			    || ( !n.empty() && n[0] == '-' );
		};
		if( isLowSide( netName ) )
			it->second->emplace_back( ref, std::string( "1" ) );
		else
			it->second->insert( it->second->begin(),
			                    std::make_pair( ref, std::string( "1" ) ) );
		m_powerDrivenNets.insert( netName );
		++augmented;
	}

	std::fprintf( stderr,
		"[ogdf_adapter] buildEdgesFromSpec: augmented %d power-symbol pins\n",
		augmented );

	for( const NetSpec& net : nets )
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
			auto symIt = m_refToNode.find( ref );
			if( symIt == m_refToNode.end() )
			{
				std::fprintf( stderr,
					"[ogdf_adapter] net %s: ref %s not in m_refToNode\n",
					net.net_name.c_str(), ref.c_str() );
				continue;
			}

			ogdf::node n = symIt->second;
			auto portIt = m_nodePinToPort.find( { n, pinNum } );
			if( portIt == m_nodePinToPort.end() )
			{
				std::fprintf( stderr,
					"[ogdf_adapter] net %s: ref %s pin %s not in m_nodePinToPort\n",
					net.net_name.c_str(), ref.c_str(), pinNum.c_str() );
				continue;
			}

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

	std::fprintf( stderr,
		"[ogdf_adapter] buildEdgesFromSpec: %d edges from %zu nets\n",
		edgeCount, aNets.size() );
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

	// Spacing in KiCad eeschema internal units.  SCH_IU_PER_MM = 1e4
	// (see include/base_units.h:74 — "Schematic internal units 1=100nm")
	// so 1 KiCad grid (2.54 mm) = 25,400 IU.  Without setting these
	// the FastHierarchyLayout defaults are tiny (~50 IU = 5 µm) and
	// the output piles up at the top-left page corner.
	const double GRID_IU = 25400.0;
	auto* hLayout = new ogdf::FastHierarchyLayout();
	hLayout->nodeDistance( 4.0 * GRID_IU );
	hLayout->layerDistance( 8.0 * GRID_IU );
	hLayout->fixedLayerDistance( true );  // diagnostic: skip the dyn-edge cap

	std::fprintf( stderr,
		"[ogdf_adapter] FHL settings: nodeDist=%.0f layerDist=%.0f fixed=%d\n",
		hLayout->nodeDistance(), hLayout->layerDistance(),
		hLayout->fixedLayerDistance() ? 1 : 0 );

	ogdf::SugiyamaLayout SL;
	SL.setCrossMin( heur );
	SL.setLayout( hLayout );

	try
	{
		SL.call( m_GA );
	}
	catch( const std::exception& )
	{
		// Layout failed; positions remain at their seed values.
		return;
	}

	// Stage B — derive layers from y-coordinates post-Sugiyama.
	// SugiyamaLayout's default orientation is top-down: ranks go
	// in Y, not X.  So nodes within a layer share approximately
	// the same y (one layer per rank), and adjacent layers are
	// distinct y-bands.  Group by a tolerance that's a fraction
	// of the expected inter-layer y-gap.  (The earlier x-based
	// version put cross-rank-connected nodes like R1→R2 in the
	// SAME layer because they were both at the same x — and the
	// router never saw the edge as cross-layer.  See
	// research/c5-adapter-design.md for the diagnosis.)
	std::vector<std::pair<double, ogdf::node>> byY;
	byY.reserve( m_graph.numberOfNodes() );
	for( ogdf::node n : m_graph.nodes )
		byY.emplace_back( m_GA.y( n ), n );

	std::sort( byY.begin(), byY.end(),
	           []( const auto& a, const auto& b ) { return a.first < b.first; } );

	double tolerance = 0.0;
	if( byY.size() >= 2 )
	{
		std::vector<double> gaps;
		for( size_t i = 1; i < byY.size(); ++i )
		{
			const double g = byY[i].first - byY[i - 1].first;
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
	double lastY = -1e30;
	for( const auto& [y, n] : byY )
	{
		if( layers.empty() || y - lastY > tolerance )
			layers.emplace_back();
		layers.back().push_back( n );
		lastY = y;
	}

	std::fprintf( stderr,
		"[ogdf_adapter] runLayout: derived %zu layers from y-coords\n",
		layers.size() );

	// Diagnostic: dump raw OGDF coords + node width/height so we can
	// tell whether tight clusters come from OGDF ignoring nodeDistance
	// or from a missing per-node bbox.
	for( ogdf::node nn : m_graph.nodes )
	{
		const char* ref = "?";
		auto it = m_nodeToSymbol.find( nn );
		if( it != m_nodeToSymbol.end() )
		{
			if( SCH_FIELD* f = it->second->GetField( FIELD_T::REFERENCE ) )
				ref = f->GetText().c_str();
		}
		std::fprintf( stderr,
			"[ogdf_adapter]   node %s  x=%.0f  y=%.0f  w=%.0f  h=%.0f\n",
			ref, m_GA.x( nn ), m_GA.y( nn ), m_GA.width( nn ), m_GA.height( nn ) );
	}

	// Stage C — port-aware orthogonal routing between adjacent layers.
	// Channel spacing: one KiCad grid unit (GRID_IU was defined above).
	ogdf::OrthoPortRouter router( m_PGA, /*edgeSpacing=*/GRID_IU );

	for( size_t i = 0; i + 1 < layers.size(); ++i )
	{
		router.routeBetween( layers[i], layers[i + 1] );
	}

	// Stage D — translate the entire layout so its bounding box upper-
	// left lands at (40 mm, 40 mm) on the KiCad page.  SugiyamaLayout
	// outputs in its own coordinate frame anchored at (0, 0); without
	// this translation, layouts with negative y end up above the page
	// top.  Also offset edge bend points so wires (if any) stay
	// aligned with their port endpoints.
	double minX = std::numeric_limits<double>::infinity();
	double minY = std::numeric_limits<double>::infinity();
	for( ogdf::node nn : m_graph.nodes )
	{
		minX = std::min( minX, m_GA.x( nn ) );
		minY = std::min( minY, m_GA.y( nn ) );
	}
	const double TARGET_IU = 40.0 * 10000.0;  // 40 mm in eeschema IU
	const double offX = TARGET_IU - minX;
	const double offY = TARGET_IU - minY;
	for( ogdf::node nn : m_graph.nodes )
	{
		m_GA.x( nn ) += offX;
		m_GA.y( nn ) += offY;
	}
	for( ogdf::edge e : m_graph.edges )
	{
		ogdf::DPolyline& bends = m_GA.bends( e );
		for( auto& p : bends )
		{
			p.m_x += offX;
			p.m_y += offY;
		}
	}
}


LayoutReport SchOgdfAdapter::writeBackToScreen()
{
	LayoutReport report;

	// Stage A — symbol positions.  Snapshot old (symbol, pin) positions
	// BEFORE moving anything so we can translate per-pin net labels by
	// the same delta in Stage A.1.
	//
	// The existing to_schematic pipeline emits a net label adjacent to
	// every pin (the "default pins-to-netlabels path").  When we move a
	// symbol, those labels stay at their original pin positions and
	// orphan visually unless we drag them along.
	struct OldPlacement
	{
		SCH_SYMBOL*           sym;
		VECTOR2I              oldSymPos;
		VECTOR2I              newSymPos;
		std::vector<VECTOR2I> oldPinPositions;
	};
	std::vector<OldPlacement> oldPlacements;
	oldPlacements.reserve( m_symbolToNode.size() );

	for( auto& [sym, n] : m_symbolToNode )
	{
		OldPlacement op;
		op.sym       = sym;
		op.oldSymPos = sym->GetPosition();
		op.newSymPos = VECTOR2I(
			static_cast<int>( std::lround( m_GA.x( n ) ) ),
			static_cast<int>( std::lround( m_GA.y( n ) ) ) );
		for( SCH_PIN* pin : sym->GetPins() )
			op.oldPinPositions.push_back( pin->GetPosition() );
		oldPlacements.push_back( std::move( op ) );
	}

	for( OldPlacement& op : oldPlacements )
	{
		op.sym->SetPosition( op.newSymPos );
		++report.symbols_placed;
	}

	// M2: sheet positions.  SCH_SHEET::Move() shifts the sheet's
	// origin AND its sheet-pins together.  Snapshot old sheet-pin
	// positions BEFORE moving so Stage A.1's label-translate logic
	// can find any label that was emitted at an old sheet-pin coord.
	for( auto& [sheet, n] : m_sheetToNode )
	{
		OldPlacement op;
		op.sym       = nullptr;  // sentinel: sheet, not symbol
		op.oldSymPos = sheet->GetPosition();
		op.newSymPos = VECTOR2I(
			static_cast<int>( std::lround( m_GA.x( n ) ) ),
			static_cast<int>( std::lround( m_GA.y( n ) ) ) );
		for( SCH_SHEET_PIN* p : sheet->GetPins() )
			op.oldPinPositions.push_back( p->GetPosition() );
		oldPlacements.push_back( std::move( op ) );

		const VECTOR2I delta = oldPlacements.back().newSymPos
		                       - oldPlacements.back().oldSymPos;
		sheet->Move( delta );
		++report.symbols_placed;
	}

	// Stage A.1 — translate every label that sits on (or very near) one
	// of the snapshotted OLD pin positions by that symbol's move delta.
	// Tolerance: 1 grid unit (2.54 mm).  Covers SCH_LABEL, SCH_GLOBAL_LABEL,
	// SCH_HIER_LABEL, and SCH_DIRECTIVE_LABEL all in one loop.
	const int LABEL_PIN_TOL = 25400;  // 2.54 mm in eeschema IU

	auto translate_labels_of_type = [&]( KICAD_T aType )
	{
		std::vector<SCH_ITEM*> toMove;
		for( SCH_ITEM* item : m_screen.Items().OfType( aType ) )
			toMove.push_back( item );

		for( SCH_ITEM* item : toMove )
		{
			const VECTOR2I labelPos = item->GetPosition();
			for( const OldPlacement& op : oldPlacements )
			{
				bool matched = false;
				for( const VECTOR2I& oldPin : op.oldPinPositions )
				{
					if( std::abs( labelPos.x - oldPin.x ) <= LABEL_PIN_TOL
					 && std::abs( labelPos.y - oldPin.y ) <= LABEL_PIN_TOL )
					{
						const VECTOR2I delta = op.newSymPos - op.oldSymPos;
						item->SetPosition( labelPos + delta );
						matched = true;
						break;
					}
				}
				if( matched )
					break;
			}
		}
	};
	translate_labels_of_type( SCH_LABEL_T );
	translate_labels_of_type( SCH_GLOBAL_LABEL_T );
	translate_labels_of_type( SCH_HIER_LABEL_T );
	translate_labels_of_type( SCH_DIRECTIVE_LABEL_T );

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

	// Stage E — remove redundant pin-labels.  For every net in
	// m_powerDrivenNets (nets that picked up a power symbol during
	// buildEdgesFromSpec augmentation), the to_schematic emit also
	// dropped a black SCH_LABEL with the same net name at every
	// consumer pin.  Once the OGDF router has wired the consumer to
	// the power symbol, both the wire AND the power symbol's text
	// declare the same net — the duplicate label crowds the pin
	// number and the resistor body without adding any information.
	// Delete it.
	if( !m_powerDrivenNets.empty() )
	{
		std::vector<SCH_ITEM*> labelsToRemove;
		for( SCH_ITEM* item : m_screen.Items().OfType( SCH_LABEL_T ) )
		{
			SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
			const std::string text = label->GetText().ToStdString();
			if( m_powerDrivenNets.count( text ) == 0 )
				continue;

			// Only delete labels that sit on a symbol pin or sheet pin
			// — those are the redundant pin-labels the harness
			// emitted.  Stand-alone labels (e.g. for nets without a
			// power symbol) stay.
			const VECTOR2I labelPos = label->GetPosition();
			bool onPin = false;
			for( auto& [sym, n] : m_symbolToNode )
			{
				for( SCH_PIN* pin : sym->GetPins() )
				{
					if( pin->GetPosition() == labelPos )
					{
						onPin = true;
						break;
					}
				}
				if( onPin )
					break;
			}
			if( !onPin )
			{
				for( auto& [sheet, n] : m_sheetToNode )
				{
					for( SCH_SHEET_PIN* p : sheet->GetPins() )
					{
						if( p->GetPosition() == labelPos )
						{
							onPin = true;
							break;
						}
					}
					if( onPin )
						break;
				}
			}
			if( onPin )
				labelsToRemove.push_back( item );
		}

		std::fprintf( stderr,
			"[ogdf_adapter] writeBack: removing %zu redundant pin-labels"
			" for power-driven nets\n", labelsToRemove.size() );

		for( SCH_ITEM* l : labelsToRemove )
			m_screen.Remove( l );
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
