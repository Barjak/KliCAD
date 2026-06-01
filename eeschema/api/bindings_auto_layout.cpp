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

#include <wx/window.h>
#include <wx/toplevel.h>

#include <eda_base_frame.h>
#include <frame_type.h>
#include <sch_edit_frame.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <schematic.h>

#include "../auto_layout/sch_auto_layout.h"

namespace py = pybind11;

namespace {

// Walk wxTopLevelWindows for the SCH_EDIT_FRAME and return the live
// SCHEMATIC + RootScreen — the same pattern bindings_schematic_state.cpp
// uses.  Avoids re-LoadSchematic'ing an already-open project, which
// produces a second SCHEMATIC instance and is what the API-thread
// HandleUnsavedChanges path was hanging on.
SCH_EDIT_FRAME* find_sch_edit_frame_for_auto_layout()
{
	for( wxWindow* w : wxTopLevelWindows )
	{
		EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );
		if( !base )
			continue;
		if( base->GetFrameType() == FRAME_SCH )
			return static_cast<SCH_EDIT_FRAME*>( base );
	}
	return nullptr;
}


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

	// Operate on the currently-open SCHEMATIC, NOT re-LoadSchematic'd —
	// the caller is expected to have run pm.load_project on aSchPath
	// already, so the live SCH_EDIT_FRAME holds the canonical instance.
	// Re-LoadSchematic'ing produces a second SCHEMATIC instance whose
	// API-thread HandleUnsavedChanges path hangs.
	SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_auto_layout();
	if( frame == nullptr )
	{
		py::dict err;
		err[ "ok" ]    = false;
		err[ "error" ] = std::string( "no SCH_EDIT_FRAME found (call pm.load_project first)" );
		return err;
	}

	SCHEMATIC& sch = frame->Schematic();
	SCH_SCREEN* screen = sch.RootScreen();
	if( screen == nullptr )
	{
		py::dict err;
		err[ "ok" ]    = false;
		err[ "error" ] = std::string( "schematic has no root screen" );
		return err;
	}

	klicad::auto_layout::LayoutReport report =
		klicad::auto_layout::runAutoLayout( *screen, nets );

	// Save via SCH_EDIT_FRAME::SaveProject — mirrors how
	// bindings_schematic_state's save_schematic binding works.
	// Direct-IO SaveSchematicFile bypasses the live frame's state
	// and writes a stale snapshot (the schematic loaded from disk
	// before our modifications), producing a blank-page result.
	if( report.ok )
	{
		// CRITICAL: writeBackToScreen mutates the screen via raw
		// SetPosition / Move / Append / Remove — none of which set
		// the modified flag.  Without an explicit dirty mark
		// SaveProject's IsContentModified() check at files-io.cpp:1264
		// short-circuits to a silent no-op, and kicad-cli later
		// renders the pre-OGDF schematic the to_schematic emit
		// produced.  The PNG looks "wrong" because the file on disk
		// IS the pre-OGDF state.  See audit dated 2026-05-31.
		//
		// Also CRITICAL: SCH_SYMBOL::SetPosition / SCH_SHEET::Move /
		// raw SCH_SCREEN::Append+Remove update the model but never
		// notify KIGFX::VIEW.  The canvas keeps drawing the stale
		// cached geometry — the GUI shows the pre-OGDF layout even
		// though the model and (after the dirty fix) the file both
		// hold the post-OGDF positions.  Screenshots of the canvas
		// therefore *do* capture what's drawn — they capture the
		// stale paint.  HardRedraw() rebuilds the canvas view items
		// from the live screen, syncing canvas to model.
		screen->SetContentModified();
		frame->OnModify();
		frame->HardRedraw();
		try
		{
			frame->SaveProject( /*aSaveAs*/ false );
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
