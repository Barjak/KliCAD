/*
 * KliCAD subsystem binding: port-aware auto-layout (Pattern B, eeschema
 * kiface).
 *
 * Exposes the OGDF-fork-based schematic auto-layout as
 * `klicad_native_auto_layout.run(sch_path)`.  Loads the schematic, runs
 * port-constrained Sugiyama + orthogonal routing on its top SCH_SCREEN,
 * writes results back, saves the file.  Returns a LayoutReport dict for
 * klicad-python to log a sidecar `LayoutReport.json`.
 *
 * Pattern B: see bindings_annotation.cpp for the rationale; this file
 * just exposes klicad_register_auto_layout_bindings(py::module_&) which
 * klicad_kiface_register.cpp wires into a runtime-created module.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <eeschema_helpers.h>
#include <schematic.h>
#include <sch_screen.h>

#include "../auto_layout/sch_auto_layout.h"

namespace py = pybind11;

namespace {

py::dict auto_layout_run( const std::string& aSchPath )
{
	// Open the schematic — matching the convention used by
	// JobSchErc and other CLI-style operations.  Eager open
	// (not deferred) so layout-result errors surface here, not
	// at file-write time.
	SCHEMATIC* sch = EESCHEMA_HELPERS::LoadSchematic( aSchPath, true, false );
	if( sch == nullptr )
	{
		py::dict err;
		err[ "ok" ]    = false;
		err[ "error" ] = std::string( "failed to load schematic at " ) + aSchPath;
		return err;
	}

	SCH_SCREEN* screen = sch->RootScreen();
	if( screen == nullptr )
	{
		py::dict err;
		err[ "ok" ]    = false;
		err[ "error" ] = std::string( "schematic has no root screen" );
		return err;
	}

	klicad::auto_layout::LayoutReport report =
		klicad::auto_layout::runAutoLayout( *screen );

	// TODO (next session): save the schematic back to disk so the
	// layout changes persist.  For Session 1, layout runs but is
	// thrown away on return.

	py::dict result;
	result[ "ok" ]               = report.ok;
	result[ "symbols_placed" ]   = report.symbols_placed;
	result[ "wires_emitted" ]    = report.wires_emitted;
	result[ "crossings" ]        = report.crossings;
	result[ "bends" ]            = report.bends;
	result[ "total_wirelength" ] = report.total_wirelength;
	return result;
}

}  // anonymous namespace


void klicad_register_auto_layout_bindings( py::module_& m )
{
	m.def( "run", &auto_layout_run, py::arg( "sch_path" ),
	       "Run port-aware auto-layout on the schematic at sch_path.  "
	       "Returns a dict { ok, symbols_placed, wires_emitted, crossings, "
	       "bends, total_wirelength }." );
}
