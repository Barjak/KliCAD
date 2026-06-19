/*
 * KliCAD subsystem binding: schematic compose (GOAL.md F-S3).
 *
 * Single IPC entry replacing the legacy multi-call sequence
 * (open_schematic → N×add_symbol → N×add_label → auto_layout.run).
 *
 *   klicad_native_schematic_compose.compose(sch_path, program_dict)
 *
 * Fork 1 resolved as (b): the program travels as a pybind11
 * dict.  No proto toolchain; schema is the encoder/decoder
 * pair on each side, with drift caught by
 * test_compose_program_schema_round_trip.
 *
 * Fork 5 resolved as (a): `runElkLayout` is a free function
 * that consumes an `SchLayoutTransaction&`.  This binding is
 * the sole creator of the transaction; the adapter never opens
 * its own.  Layout and writeback are inside the same transaction,
 * which commits exactly once.
 *
 * The dict shape (mirrors klicad::auto_layout::SchematicProgram):
 *   {
 *     "sch_path": str,
 *     "mode":     "replace" | "diff" | "strict",   # only "replace" today
 *     "parts": [
 *       { "ref": "R1",
 *         "lib_id": "Device:R",
 *         "value": "10k",
 *         "footprint": "",
 *         "extra_fields": { ... },
 *       },
 *       ...
 *     ],
 *     "sheets": [...],   # F-S5 / M4
 *     "nets":   [
 *       { "name": "VCC", "kind": "POWER",
 *         "expect_external": false },
 *       ...
 *     ],
 *   }
 *
 * Returns a ComposeReport-shaped dict:
 *   { "ok": bool, "error": str,
 *     "parts_placed": int, "wires_emitted": int,
 *     "labels_placed": int, "crossings": int,
 *     "total_wirelength": float, ... }
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <wx/filename.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <eda_draw_frame.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <base_units.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_commit.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_pin.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <template_fieldnames.h>

#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>

#include "../auto_layout/layout_report.h"
#include "../auto_layout/sch_elk_adapter.h"
#include "../auto_layout/sch_layout_transaction.h"

namespace py = pybind11;

namespace
{

SCH_EDIT_FRAME* find_sch_frame_for_compose()
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


KIWAY* find_kiway_for_compose()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* h = dynamic_cast<KIWAY_HOLDER*>( w ) )
        {
            if( h->HasKiway() )
                return &h->Kiway();
        }
    }
    return nullptr;
}


SCH_EDIT_FRAME* require_sch_frame_for_compose()
{
    if( SCH_EDIT_FRAME* f = find_sch_frame_for_compose() )
        return f;

    KIWAY* kiway = find_kiway_for_compose();
    if( !kiway )
        throw std::runtime_error( "compose: no live KIWAY (is KiCad GUI running?)" );

    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* f = find_sch_frame_for_compose();
    if( !f )
        throw std::runtime_error( "compose: failed to obtain SCH_EDIT_FRAME" );
    return f;
}


/** Open the schematic file in the live eeschema frame.  No-op if
 *  already open. */
void compose_open_schematic( SCH_EDIT_FRAME* aFrame, const std::string& aPath )
{
    if( aPath.empty() )
        throw std::invalid_argument( "compose: sch_path is empty" );

    SCHEMATIC& sch    = aFrame->Schematic();
    wxString   wxPath = wxString::FromUTF8( aPath.c_str() );

    if( sch.IsValid() && sch.GetFileName() == wxPath )
        return;

    std::vector<wxString> files = { wxPath };
    if( !aFrame->OpenProjectFiles( files, /*aCtl*/ 0 ) )
        throw std::runtime_error( "compose: OpenProjectFiles failed for " + aPath );
}


/** Resolve a lib_id and instantiate an SCH_SYMBOL with the given
 *  ref/value/fields applied.  Caller is responsible for
 *  txn.add() / frame->AddToScreen / commit semantics.
 *  Returns nullptr if the library symbol can't be loaded. */
SCH_SYMBOL* compose_make_symbol( SCH_EDIT_FRAME* aFrame, const py::dict& aPart )
{
    const std::string libIdStr = aPart[ "lib_id" ].cast<std::string>();
    const std::string ref      = aPart[ "ref" ].cast<std::string>();

    LIB_ID libId;
    if( libId.Parse( libIdStr ) >= 0 )
        throw std::invalid_argument( "compose: lib_id parse failed for '"
                                     + libIdStr + "'" );

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &aFrame->Prj() );
    if( !adapter )
        throw std::runtime_error( "compose: SymbolLibAdapter unavailable" );

    LIB_SYMBOL* libSymbol = nullptr;
    try
    {
        libSymbol = adapter->LoadSymbol( libId );
    }
    catch( const std::exception& ex )
    {
        std::fprintf( stderr,
                      "[compose] LoadSymbol failed for '%s': %s\n",
                      libIdStr.c_str(), ex.what() );
        return nullptr;
    }

    if( !libSymbol )
    {
        std::fprintf( stderr,
                      "[compose] lib_id '%s' not found in project lib table\n",
                      libIdStr.c_str() );
        return nullptr;
    }

    // Origin (0, 0) — ELK places.  The writeback's
    // SchLayoutTransaction.move() relocates each symbol once the
    // post-layout coords are known.  F-S6 forbids any pre-layout
    // coordinate from Python, so the seed is uniformly (0, 0)
    // here regardless of caller hints.
    SCH_SYMBOL* sym = new SCH_SYMBOL( *libSymbol, libId,
                                      &aFrame->GetCurrentSheet(),
                                      /*unit*/ 1, /*bodyStyle*/ 0,
                                      VECTOR2I( 0, 0 ),
                                      &aFrame->Schematic() );

    if( !ref.empty() )
        sym->SetRef( &aFrame->GetCurrentSheet(),
                     wxString::FromUTF8( ref.c_str() ) );

    if( aPart.contains( "value" ) )
    {
        const std::string val = aPart[ "value" ].cast<std::string>();
        if( SCH_FIELD* f = sym->GetField( FIELD_T::VALUE ) )
            f->SetText( wxString::FromUTF8( val.c_str() ) );
    }

    if( aPart.contains( "footprint" ) )
    {
        const std::string fp = aPart[ "footprint" ].cast<std::string>();
        if( !fp.empty() )
            if( SCH_FIELD* f = sym->GetField( FIELD_T::FOOTPRINT ) )
                f->SetText( wxString::FromUTF8( fp.c_str() ) );
    }

    if( aPart.contains( "extra_fields" ) )
    {
        py::dict extra = aPart[ "extra_fields" ].cast<py::dict>();
        for( auto item : extra )
        {
            const std::string name = py::str( item.first ).cast<std::string>();
            const std::string val  = py::str( item.second ).cast<std::string>();
            const wxString wxName = wxString::FromUTF8( name.c_str() );
            SCH_FIELD* f = sym->GetField( wxName );
            if( !f )
            {
                SCH_FIELD newField( sym, FIELD_T::USER, wxName );
                newField.SetText( wxString::FromUTF8( val.c_str() ) );
                sym->AddField( newField );
            }
            else
            {
                f->SetText( wxString::FromUTF8( val.c_str() ) );
            }
        }
    }

    return sym;
}


py::dict schematic_compose( const std::string& aSchPath,
                            const py::dict&    aProgram )
{
    py::dict report;
    report[ "ok" ]               = false;
    report[ "error" ]            = std::string( "" );
    report[ "parts_placed" ]     = 0;
    report[ "sheets_placed" ]    = 0;
    report[ "wires_emitted" ]    = 0;
    report[ "labels_placed" ]    = 0;
    report[ "total_wirelength" ] = 0.0;
    report[ "crossings" ]        = 0;
    report[ "bends" ]            = 0;
    // GOAL.md F-S3 deferred-feature stub (2026-06-07).  Empty
    // string surfaces the field with the documented shape so callers
    // who expect ratsnest_spec to be present don't KeyError; populating
    // it is deferred.  Gist: compute the netlist string during
    // materialization and surface it for the caller to pipe into
    // klicad_native_ratsnest.set_spec.  Alternative: keep ratsnest
    // entirely outside compose; expose a pure-Python helper
    // Circuit.netlist_for_ratsnest() the caller pipes after compose
    // returns.
    report[ "ratsnest_spec" ]    = std::string( "" );

    try
    {
        SCH_EDIT_FRAME* frame = require_sch_frame_for_compose();

        compose_open_schematic( frame, aSchPath );

        SCH_SCREEN* screen = frame->GetScreen();
        if( !screen )
        {
            report[ "error" ] = std::string( "no active SCH_SCREEN after open" );
            return report;
        }

        // P3 fix: "replace" mode must yield a schematic that is EXACTLY the
        // Circuit -- not the Circuit stacked on top of whatever the (possibly
        // already-open, reused) screen still held.  compose_open_schematic is
        // a no-op when the file is already open, so without this clear every
        // compose APPENDS a fresh copy of every symbol/sheet.  That is the
        // symbol-accumulation bug (5 -> 10 -> ... per run) and it is amplified
        // by the IPC REQ-socket auto-retry: a slow compose gets resent and
        // doubles the contents.  Clear the root screen before materializing;
        // FreeDrawList also deletes accumulated SCH_SHEETs, releasing their
        // child screens.  The post-compose HardRedraw rebuilds the view.
        std::string mode = "replace";
        if( aProgram.contains( "mode" ) )
            mode = aProgram[ "mode" ].cast<std::string>();
        if( mode == "replace" )
            screen->Clear( /*aFree*/ true );

        // F-S3 invariant: one transaction wraps materialize + layout.
        // The adapter is forbidden to open its own (Fork 5 (a)).
        klicad::auto_layout::SchLayoutTransaction txn( *frame );

        // Materialize parts at coarse seed positions, then drop
        // transient per-pin labels carrying the net name from the
        // program's `connections` map.  The labels are the seed
        // CONNECTION_GRAPH needs to assign net membership before
        // ELK runs — without them, all pins-at-(0,0) collapse into
        // one giant subgraph and net info is lost.
        //
        // The labels are REMOVED by the writeback (sch_elk_adapter
        // already deletes pre-layout SCH_LABEL_T items before
        // emitting new ones), so they never persist in the saved
        // schematic.  "Labels-everywhere drift" is still gone in
        // the F-S3 invariant sense: labels exist only as a
        // transient seed inside one transaction, never as a
        // representation that leaves the C++ side.
        constexpr int SEED_PITCH_IU = 254000;   // 25.4 mm = 1 inch = 20 grid
        int partsPlaced = 0;
        if( aProgram.contains( "parts" ) )
        {
            py::list parts = aProgram[ "parts" ].cast<py::list>();
            int seedIdx = 0;
            for( auto item : parts )
            {
                py::dict part = item.cast<py::dict>();
                SCH_SYMBOL* sym = compose_make_symbol( frame, part );
                if( !sym )
                    continue;

                // Coarse seed position on a 20-wide grid.  ELK
                // throws this layout away and replaces with its own.
                const int row = seedIdx / 20;
                const int col = seedIdx % 20;
                const VECTOR2I seedPos( SEED_PITCH_IU * ( col + 2 ),
                                        SEED_PITCH_IU * ( row + 2 ) );
                sym->SetPosition( seedPos );

                frame->AddToScreen( sym, screen );

                // Transient per-pin seed labels.  Iterate the part's
                // connections (port_name → net_name) and the
                // kicad_pin_map (port_name → kicad_pin_number) to
                // identify each pin's net.  Drop a SCH_LABEL at the
                // pin's now-world coordinate.  The writeback removes
                // these post-layout.
                if( part.contains( "connections" ) && part.contains( "kicad_pin_map" ) )
                {
                    py::dict connections = part[ "connections" ].cast<py::dict>();
                    py::dict pinMap      = part[ "kicad_pin_map" ].cast<py::dict>();

                    for( auto connItem : connections )
                    {
                        const std::string portName = py::str( connItem.first ).cast<std::string>();
                        const std::string netName  = py::str( connItem.second ).cast<std::string>();
                        if( netName.empty() )
                            continue;

                        if( !pinMap.contains( portName.c_str() ) )
                            continue;
                        const std::string pinNum =
                                pinMap[ portName.c_str() ].cast<std::string>();

                        // Look up the SCH_PIN by number on this symbol.
                        const wxString wxPinNum =
                                wxString::FromUTF8( pinNum.c_str() );
                        SCH_PIN* foundPin = nullptr;
                        for( SCH_PIN* p : sym->GetPins() )
                        {
                            if( p->GetNumber() == wxPinNum )
                            {
                                foundPin = p;
                                break;
                            }
                        }
                        if( !foundPin )
                            continue;

                        const VECTOR2I pinWorld = foundPin->GetPosition();
                        const wxString labelText =
                                wxString::FromUTF8( netName.c_str() );

                        SCH_LABEL* label = new SCH_LABEL( pinWorld, labelText );
                        frame->AddToScreen( label, screen );
                    }
                }

                ++partsPlaced;
                ++seedIdx;
            }
        }

        report[ "parts_placed" ] = partsPlaced;

        // GOAL.md F-S5 Phase A: materialize ProgramSheet entries as
        // SCH_SHEET items with seed coordinates that ELK will overwrite
        // during F-S5 Phase B (ElkHierarchyBuilder consumes these as
        // compound nodes).  Closure invariants enforced here:
        //   #9  — no string compare on net names; port_map values are
        //         written into SCH_SHEET_PIN text only as human/UI
        //         labels.  Cross-sheet propagation matches by KIID via
        //         SCH_HIERLABEL::m_matchedEndpoint (F-S4a accessor),
        //         which this loop populates on the parent-pin side
        //         with a fresh per-pair KIID.  The matching child-side
        //         SCH_HIERLABEL needs to carry the same KIID — that
        //         link lands when child-sheet materialization ships
        //         (follow-up change set).  Until then, the typed walk
        //         in propagateToNeighbors will not find a child carrying
        //         the KIID and will fire unmatched_hier_reference ERC.
        //   #11 — sheet pins reference children by KIID, not by name.
        //         Parent side populated; child side pending.
        //   #14 — compose owns layout coords as TRANSIENT seeds.  Every
        //         (x, y) written below is a placeholder ELK replaces;
        //         F-S6 forbids these surviving as input to anything.
        //
        // Out of scope: child .kicad_sch emission, ELK reposition.
        // Those each land in their own change set.
        int sheetsPlaced = 0;
        if( aProgram.contains( "sheets" ) )
        {
            py::list sheets = aProgram[ "sheets" ].cast<py::list>();
            int sheetIdx = 0;
            // Sheets seed on a column to the right of the parts grid
            // (parts use cols 2..21).  Pure transient placement.
            constexpr int SHEET_SEED_COL = 25;
            // Placeholder sheet size: 25.4 mm x 25.4 mm = 254000 IU.
            // ELK's compound-node logic (F-S5 Phase B) resizes.
            constexpr int SHEET_SEED_SIZE_IU = 254000;
            for( auto item : sheets )
            {
                py::dict sheetDict = item.cast<py::dict>();

                const VECTOR2I seedPos( SEED_PITCH_IU * SHEET_SEED_COL,
                                        SEED_PITCH_IU * ( sheetIdx + 2 ) );

                // Transient seed coords — ELK F-S5 Phase B overrides.
                SCH_SHEET* sheet = new SCH_SHEET(
                        &frame->Schematic(), seedPos,
                        VECTOR2I( SHEET_SEED_SIZE_IU, SHEET_SEED_SIZE_IU ) );

                // Attach a fresh SCH_SCREEN so RecalculateConnections'
                // rule-area scan doesn't crash iterating a null child
                // r-tree.  SCH_SHEET's ctor leaves m_screen = nullptr;
                // SetScreen() bumps the screen's reference count.  The
                // child schematic itself is empty for now — F-S5 Phase
                // B + the child-sheet emit pass populate it later.
                sheet->SetScreen( new SCH_SCREEN( &frame->Schematic() ) );

                if( sheetDict.contains( "ref" ) )
                {
                    const std::string ref =
                            sheetDict[ "ref" ].cast<std::string>();
                    sheet->SetName( wxString::FromUTF8( ref.c_str() ) );
                }

                if( sheetDict.contains( "definition_filename" ) )
                {
                    const std::string fn =
                            sheetDict[ "definition_filename" ].cast<std::string>();
                    const wxString wxFn = wxString::FromUTF8( fn.c_str() );
                    sheet->SetFileName( wxFn );

                    // The SCH_SHEET carries the (relative) filename for the
                    // s-expr reference, but the child SCH_SCREEN itself must
                    // also carry an absolute on-disk filename or SCH_EDIT_FRAME
                    // ::SaveProject SKIPS it: PrepareSaveAsFiles /
                    // implicit-save iterate SCH_SCREENS and `continue` past any
                    // screen whose GetFileName() is not wxFileName::IsOk()
                    // (files-io.cpp ~1382).  Without this the child .kicad_sch
                    // is never written, so kicad-cli sch erc reads an absent /
                    // empty child sheet → the child hier-labels + parts vanish
                    // and every cross-sheet pin reports pin_not_connected /
                    // hier_label_mismatch.  Resolve against the root screen's
                    // directory so the child lands beside the parent file.
                    if( SCH_SCREEN* childScreen = sheet->GetScreen() )
                    {
                        wxFileName childFn( wxFn );
                        if( !childFn.IsAbsolute() )
                        {
                            wxFileName rootFn( screen->GetFileName() );
                            childFn.MakeAbsolute( rootFn.GetPath() );
                        }
                        childScreen->SetFileName( childFn.GetFullPath() );
                    }
                }

                frame->AddToScreen( sheet, screen );

                // Sheet pins from port_map.  Each (port_name, parent_net)
                // becomes one SCH_SHEET_PIN whose text == port_name (the
                // child-side port identifier).  parent_net is consumed
                // by the C++ resolution pass elsewhere — it never enters
                // SCH_SHEET_PIN text, because closure invariant #9
                // forbids string-name cross-sheet matching.
                if( sheetDict.contains( "port_map" ) )
                {
                    py::dict portMap =
                            sheetDict[ "port_map" ].cast<py::dict>();
                    int portIdx = 0;
                    for( auto portItem : portMap )
                    {
                        const std::string portName =
                                py::str( portItem.first ).cast<std::string>();

                        // Transient seed: distribute along the left
                        // edge.  ELK reassigns side + position in F-S6's
                        // sheet_pin_topology writeback.
                        const VECTOR2I pinSeedPos(
                                seedPos.x,
                                seedPos.y + portIdx * ( SEED_PITCH_IU / 2 ) );

                        SCH_SHEET_PIN* pin = new SCH_SHEET_PIN(
                                sheet, pinSeedPos,
                                wxString::FromUTF8( portName.c_str() ) );

                        // Transient seed side; ELK owns final side.
                        pin->SetSide( SHEET_SIDE::LEFT );

                        sheet->AddPin( pin );

                        // M4 closure: drop a transient SCH_LABEL with
                        // parent_net text at the sheet pin's seed coord
                        // so the CONNECTION_GRAPH's subgraph merger sees
                        // the sheet pin item as part of the parent_net
                        // subgraph.  Without this, the sheet pin lives
                        // in its own port_name subgraph and the
                        // projection's boundary_kiids never list it for
                        // parent_net — the builder then has no edge
                        // endpoint to route to.  Removed by the writeback
                        // (pre-existing SCH_LABEL cleanup).
                        if( portItem.second && !portItem.second.is_none() )
                        {
                            const std::string parentNet =
                                    py::str( portItem.second ).cast<std::string>();
                            if( !parentNet.empty() )
                            {
                                SCH_LABEL* netLabel = new SCH_LABEL(
                                        pinSeedPos,
                                        wxString::FromUTF8( parentNet.c_str() ) );
                                frame->AddToScreen( netLabel, screen );
                            }
                        }

                        // F-S4b: bidirectional KIID cross-link between
                        // parent SCH_SHEET_PIN and child-side
                        // SCH_HIERLABEL.  The child screen was attached
                        // earlier via sheet->SetScreen(); we add a
                        // matching SCH_HIERLABEL to that screen here so
                        // F-S4b's propagateToNeighbors KIID lookup
                        // resolves cleanly.  Without this child-side
                        // counterpart, the parent's m_matchedEndpoint
                        // would point at nothing and ERC would fire
                        // ERCE_UNMATCHED_HIER_REFERENCE.
                        //
                        // KIID assignment: each side's m_matchedEndpoint
                        // is the OTHER side's m_Uuid.  F-S4b's matcher
                        // accepts either direction (label->Matched ==
                        // pin->Uuid || pin->Matched == label->Uuid).
                        SCH_SCREEN* childScreen = sheet->GetScreen();
                        if( childScreen )
                        {
                            SCH_HIERLABEL* hlabel = new SCH_HIERLABEL(
                                    pinSeedPos,
                                    wxString::FromUTF8( portName.c_str() ),
                                    SCH_HIER_LABEL_T );
                            // Reverse cross-link
                            hlabel->SetMatchedEndpoint( pin->m_Uuid );
                            pin->SetMatchedEndpoint( hlabel->m_Uuid );
                            // hier_label_mismatch ERC fires when the
                            // sheet pin's shape doesn't match the child
                            // hier label's shape.  Default-constructed
                            // SCH_SHEET_PIN has L_INPUT shape; default
                            // SCH_HIERLABEL likewise.  Set both
                            // explicitly to BIDI so the contract is
                            // visible at every emit site.
                            pin->SetShape( L_BIDI );
                            hlabel->SetShape( L_BIDI );
                            childScreen->Append( hlabel );
                        }
                        else
                        {
                            // Fallback: no child screen → empty link
                            const KIID matchKiid;
                            pin->SetMatchedEndpoint( matchKiid );
                        }
                        ++portIdx;
                    }
                }

                // M4 closure: materialize the child sub-circuit's parts
                // on the child SCH_SCREEN (same shape as the parent's
                // parts loop, just targeting childScreen instead of the
                // root screen).  Each child part drops transient seed
                // labels at its pin world coords; those labels carry
                // child-local net names (e.g. "IN", "OUT", "VCC", "GND")
                // and merge with the SCH_HIERLABELs already on the
                // child screen via name fusion in CONNECTION_GRAPH.  The
                // cross-sheet match to the parent's SHEET_PIN is via
                // the KIID m_matchedEndpoint set above (F-S4b typed
                // walk), not via the name.  Writeback removes the
                // transient labels after layout.
                SCH_SCREEN* childScreenForParts = sheet->GetScreen();
                if( childScreenForParts && sheetDict.contains( "child_parts" ) )
                {
                    py::list childParts =
                            sheetDict[ "child_parts" ].cast<py::list>();
                    int childSeedIdx = 0;
                    for( auto cpItem : childParts )
                    {
                        py::dict cpart = cpItem.cast<py::dict>();
                        SCH_SYMBOL* csym = compose_make_symbol( frame, cpart );
                        if( !csym )
                            continue;

                        const int crow = childSeedIdx / 8;
                        const int ccol = childSeedIdx % 8;
                        const VECTOR2I cseedPos(
                                SEED_PITCH_IU * ( ccol + 2 ),
                                SEED_PITCH_IU * ( crow + 2 ) );
                        csym->SetPosition( cseedPos );

                        // Add to child screen directly (not the root
                        // screen via frame->AddToScreen, which would
                        // place the child's symbol on the parent
                        // sheet).
                        childScreenForParts->Append( csym );

                        if( cpart.contains( "connections" )
                            && cpart.contains( "kicad_pin_map" ) )
                        {
                            py::dict cconns =
                                    cpart[ "connections" ].cast<py::dict>();
                            py::dict cpinMap =
                                    cpart[ "kicad_pin_map" ].cast<py::dict>();

                            for( auto connItem : cconns )
                            {
                                const std::string portName =
                                        py::str( connItem.first ).cast<std::string>();
                                const std::string netName =
                                        py::str( connItem.second ).cast<std::string>();
                                if( netName.empty() )
                                    continue;
                                if( !cpinMap.contains( portName.c_str() ) )
                                    continue;
                                const std::string pinNum =
                                        cpinMap[ portName.c_str() ].cast<std::string>();

                                const wxString wxPinNum =
                                        wxString::FromUTF8( pinNum.c_str() );
                                SCH_PIN* fp = nullptr;
                                for( SCH_PIN* p : csym->GetPins() )
                                {
                                    if( p->GetNumber() == wxPinNum )
                                    {
                                        fp = p;
                                        break;
                                    }
                                }
                                if( !fp )
                                    continue;

                                const VECTOR2I pinWorld = fp->GetPosition();
                                const wxString labelText =
                                        wxString::FromUTF8( netName.c_str() );

                                SCH_LABEL* clabel =
                                        new SCH_LABEL( pinWorld, labelText );
                                childScreenForParts->Append( clabel );
                            }
                        }

                        ++childSeedIdx;
                    }
                }

                ++sheetsPlaced;
                ++sheetIdx;
            }
        }

        report[ "sheets_placed" ] = sheetsPlaced;

        // Run ELK layout into the same transaction (Fork 5 (a)).
        // The adapter signature change is part of this F-S3
        // change set; legacy signature is gone.
        klicad::auto_layout::LayoutReport layout =
                klicad::auto_layout::runElkLayout( txn );

        report[ "wires_emitted" ]    = layout.wires_emitted;
        report[ "total_wirelength" ] = layout.total_wirelength;
        report[ "crossings" ]        = layout.crossings;
        report[ "bends" ]            = layout.bends;
        // labels_placed is on ComposeReport but not LayoutReport;
        // the writeback tracks it implicitly via emitted SCH_LABEL
        // items.  TODO: extend LayoutReport with labels_placed.

        if( !layout.ok )
        {
            report[ "error" ] = std::string( "ELK layout failed" );
            return report;
        }

        // Commit drives SCH_COMMIT::Push: refresh, dirty flag,
        // CONNECTION_GRAPH rebuild all fire here.
        txn.commit( wxT( "KliCAD: compose" ) );

        // Persist.
        try
        {
            frame->SaveProject( /*aSaveAs*/ false );
        }
        catch( const std::exception& ex )
        {
            report[ "error" ] = std::string( "SaveProject failed: " ) + ex.what();
            return report;
        }

        if( frame->GetCanvas() )
            frame->GetCanvas()->Refresh();

        report[ "ok" ] = true;
    }
    catch( const std::exception& ex )
    {
        report[ "error" ] = std::string( ex.what() );
    }

    return report;
}

}  // anonymous namespace


void klicad_register_schematic_compose_bindings( py::module_& m )
{
    m.def( "compose", &schematic_compose,
           py::arg( "sch_path" ), py::arg( "program" ),
           "F-S3 single-IPC compose entry.  `program` is a dict with "
           "shape mirroring klicad::auto_layout::SchematicProgram "
           "(parts/sheets/nets).  Materializes parts in one "
           "SCH_COMMIT, runs ELK + connectivity-correct writeback "
           "(F-S1d), commits and saves.  Returns a ComposeReport dict." );
}
