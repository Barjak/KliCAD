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
#include <io/io_mgr.h>
#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <schematic.h>

#include "../auto_layout/sch_auto_layout.h"

namespace py = pybind11;

namespace {

py::dict auto_layout_run( const std::string& aSchPath, py::list aNets )
{
	// Marshal the Python net spec into the C++ NetSpec list.
	// Expected shape from klicad-python:
	//   [(net_name: str, [(sym_ref: str, pin_num: str), ...]), ...]
	std::vector<klicad::auto_layout::NetSpec> nets;
	nets.reserve( aNets.size() );

	for( const py::handle& netHandle : aNets )
	{
		py::tuple netT = netHandle.cast<py::tuple>();
		klicad::auto_layout::NetSpec spec;
		spec.net_name = netT[0].cast<std::string>();

		py::list pinList = netT[1].cast<py::list>();
		for( const py::handle& pinHandle : pinList )
		{
			py::tuple pinT = pinHandle.cast<py::tuple>();
			spec.pins.emplace_back(
				pinT[0].cast<std::string>(),
				pinT[1].cast<std::string>() );
		}
		nets.push_back( std::move( spec ) );
	}

	// Open the schematic — matching the convention used by JobSchErc
	// and other CLI-style operations.
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
		klicad::auto_layout::runAutoLayout( *screen, nets );

	// Save the modified schematic back to disk via the SCH_KICAD IO
	// plugin — the standard JobSchPlot save pattern.
	if( report.ok )
	{
		try
		{
			IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
			pi->SaveSchematicFile( aSchPath, &sch->Root(), sch );
		}
		catch( const std::exception& )
		{
			report.ok = false;
		}
	}

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
	m.def( "run", &auto_layout_run, py::arg( "sch_path" ), py::arg( "nets" ),
	       "Run port-aware auto-layout on the schematic at sch_path.  "
	       "nets is a list of (net_name, [(sym_ref, pin_number), ...]) "
	       "tuples.  Returns { ok, symbols_placed, wires_emitted, crossings, "
	       "bends, total_wirelength }." );
}
