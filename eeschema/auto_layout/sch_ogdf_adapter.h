/**
 * @file sch_ogdf_adapter.h
 *
 * @brief Adapter from KliCAD's SCH_SCREEN to the forked-OGDF
 *        port-constrained layout pipeline.  C.5 entry point.
 *
 * The adapter walks an existing SCH_SCREEN, builds an OGDF
 * `Graph` + `PortGraphAttributes` from its `SCH_SYMBOL` items
 * and their library pin geometries, runs port-aware Sugiyama
 * + orthogonal routing (using our `ogdf/portconstraints/`
 * extensions), and writes results back as updated symbol
 * positions + `SCH_LINE` wire segments.
 *
 * Lives in its own subdirectory so the OGDF dependency is
 * localized to this translation unit family — stock eeschema
 * code never sees OGDF symbols.
 *
 * Reference: ~/projects/KliCAD_development/research/c5-adapter-design.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <map>
#include <vector>

// OGDF / port-constraint fork.
#include <ogdf/basic/Graph.h>
#include <ogdf/basic/GraphAttributes.h>
#include <ogdf/portconstraints/Port.h>
#include <ogdf/portconstraints/PortGraphAttributes.h>

// Forward declarations to keep header weight light; sources include
// the full KliCAD headers.
class SCH_SCREEN;
class SCH_SYMBOL;
class SCH_PIN;
class LIB_SYMBOL;

namespace klicad::auto_layout {

/**
 * @brief Summary returned by SchOgdfAdapter::run.
 *
 * Surfaces to the IPC binding so klicad-python can log a
 * LayoutReport sidecar.  Fields chosen to match the M1
 * "Verify" stage criteria in HANDOFF.md.
 */
struct LayoutReport
{
	bool   ok               = false;
	int    symbols_placed   = 0;
	int    wires_emitted    = 0;
	int    crossings        = 0;
	int    bends            = 0;
	double total_wirelength = 0.0;
};

/**
 * @brief Adapter between SCH_SCREEN and the OGDF-fork layout
 *        pipeline.
 *
 * Construct → buildFromScreen → runLayout → writeBackToScreen.
 *
 * The three phases are exposed separately (rather than merged
 * into a single run()) so that callers can inspect / instrument
 * the intermediate OGDF state — useful during the M0/M1
 * validation phase where every layout iteration is reviewed
 * via PNG screenshot.
 */
class SchOgdfAdapter
{
public:
	explicit SchOgdfAdapter( SCH_SCREEN& aScreen );
	~SchOgdfAdapter();

	/** Walk the screen's items, populate the OGDF graph. */
	void buildFromScreen();

	/** Run port-aware Sugiyama + orthogonal routing.
	 *  Pre: buildFromScreen has been called. */
	void runLayout();

	/** Translate OGDF placements + bend lists back to symbol
	 *  positions + SCH_LINE wires on the screen.  Returns the
	 *  LayoutReport summarizing what changed. */
	LayoutReport writeBackToScreen();

	/** One-shot convenience. */
	LayoutReport run();

	/** Read-only access to the underlying OGDF graph for tests. */
	const ogdf::Graph&                  graph() const { return m_graph; }
	const ogdf::GraphAttributes&        graphAttributes() const { return m_GA; }
	const ogdf::PortGraphAttributes&    portGraphAttributes() const { return m_PGA; }

private:
	SCH_SCREEN&                m_screen;
	ogdf::Graph                m_graph;
	ogdf::GraphAttributes      m_GA;
	ogdf::PortGraphAttributes  m_PGA;

	// Cross-reference: SCH_SYMBOL ↔ OGDF node, SCH_PIN ↔ Port index.
	std::map<SCH_SYMBOL*, ogdf::node>                   m_symbolToNode;
	std::map<ogdf::node, SCH_SYMBOL*>                   m_nodeToSymbol;
	std::map<SCH_PIN*, std::pair<ogdf::node, int>>      m_pinToPort;

	// --- buildFromScreen substeps ---

	/** Materialize one OGDF node per SCH_SYMBOL.  Reads body
	 *  bbox from the symbol's LIB_SYMBOL for width/height; uses
	 *  the symbol's current m_Pos as the initial OGDF position
	 *  (so partial layouts that pin some symbols can seed a
	 *  layout that only moves the unfixed ones — future M4
	 *  incremental work). */
	void buildNodes();

	/** Per-symbol: for each pin in the LIB_SYMBOL, add a Port to
	 *  PortGraphAttributes with side derived from the pin's
	 *  orientation angle and anchor (x, y) relative to the
	 *  symbol body's top-left corner. */
	void buildPorts();

	/** Use the screen's CONNECTION_GRAPH (or, simpler for the
	 *  flat M1 case, walk SCH_LINE items + their endpoints) to
	 *  derive the edge set.  Power and labeled-hierarchical
	 *  nets are filtered out — they don't get OGDF-routed,
	 *  they get the annotate-stage power-symbol treatment per
	 *  HANDOFF.md Stage 3. */
	void buildEdges();
};

}  // namespace klicad::auto_layout
