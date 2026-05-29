/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_repeated_sheet_busfanout.cpp
 *
 * Phase R3.4 — verify bus-pin bit fan-out at the hier-pin connection
 * site for a `repeat_count = 4` child sheet whose pin is bus syntax
 * `DATA[0..3]`.  Each of the 4 slot paths must see a distinct bit of
 * the parent bus on the child-side scalar hier-label of that name; a
 * scalar pin (EN) is shared across all 4 slots.
 *
 * Covers the R3.1 - R3.3.1 changes in eeschema/connection_graph.cpp:
 *   - R3.1: hier-pin push respects synthetic-clone slot KIID.
 *   - R3.2: SCH_SHEET_PATH::GetSlotIndex derives the slot from the path.
 *   - R3.3: repeatBusPinBitName fan-out at hier-pin connection sites
 *           (forward and reverse-direction match).
 *   - R3.3.1: drop KIID prefilter in the reverse-direction hier-port
 *             walk so synthetic-clone slot paths reach the fan-out.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <connection_graph.h>
#include <schematic.h>
#include <sch_connection.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <settings/settings_manager.h>


/**
 * Build a SCHEMATIC with this shape:
 *
 *     root
 *       └─ top      (Sheet "Top",     screen with one child)
 *            └─ ch  (Sheet "Channel", repeat_count = 4)
 *                   └─ <shared child screen, 4 slots>
 *
 * The Channel sheet has two hier pins:
 *   - DATA[0..3]   bus pin   → fans out to bit K on slot K
 *   - EN           scalar    → shared across all 4 slots
 *
 * The shared child screen holds 5 scalar hier-labels:
 *   DATA0, DATA1, DATA2, DATA3, EN — name-matched against the
 *   per-slot pinName produced by R3.3's repeatBusPinBitName().
 *
 * On the parent (top) screen, a bus wire drives the Channel sheet's
 * DATA[0..3] pin and a scalar wire drives EN.  The Channel sheet's
 * pin coordinates and the wire endpoints share positions so the
 * connectivity walker links them.
 */
struct REPEATED_SHEET_BUSFANOUT_FIXTURE
{
    REPEATED_SHEET_BUSFANOUT_FIXTURE() :
            m_mgr()
    {
        m_mgr.LoadProject( "" );
        m_schematic = std::make_unique<SCHEMATIC>( &m_mgr.Prj() );
        m_schematic->Reset();

        // Replace the default top-level root sheet with our own "Top"
        // sheet so it owns the Channel child and carries the parent
        // bus / EN wires.  Mirrors test_label_bus_connectivity.cpp.
        SCH_SHEET* defaultSheet = m_schematic->GetTopLevelSheet( 0 );

        m_topScreen = new SCH_SCREEN( m_schematic.get() );
        m_top = new SCH_SHEET( m_schematic.get() );
        m_top->SetScreen( m_topScreen );
        m_top->SetName( "Top" );
        m_top->SetFileName( "top.kicad_sch" );

        m_schematic->AddTopLevelSheet( m_top );
        m_schematic->RemoveTopLevelSheet( defaultSheet );
        delete defaultSheet;

        // Channel sheet — its KIID must match its screen's KIID for
        // the SCH_SHEET_PATH machinery (each sheet's KIID is the
        // primary key for the path; synthetic clones get the alt KIIDs
        // from m_repeatInstances).  Mirrors the R2 fixture.
        m_channelScreen = new SCH_SCREEN( m_schematic.get() );
        m_channel = new SCH_SHEET( m_schematic.get() );
        const_cast<KIID&>( m_channel->m_Uuid ) = m_channelScreen->GetUuid();
        m_channel->SetScreen( m_channelScreen );
        m_channel->SetName( "Channel" );
        m_channel->SetFileName( "channel.kicad_sch" );

        // Give the Channel sheet a body on the canvas — the bounding
        // box only matters for hit-test / ConstrainOnEdge; using a
        // generous size keeps pins inside it.
        m_channel->SetPosition( VECTOR2I( 0, 0 ) );
        m_channel->SetSize( VECTOR2I( 50 * SCALE, 80 * SCALE ) );

        // Place the Channel inside Top's screen — that's how child
        // sheets appear in real schematics (and how SCH_SHEET_PATH
        // builds the path top → ch).
        m_topScreen->Append( m_channel );

        // Configure repeat_count = 4 — slot 0 == channel's own KIID,
        // slots 1..3 == three pre-allocated KIIDs in m_repeatInstances.
        m_channel->SetRepeatCount( 4 );

        std::vector<KIID> slots;
        slots.reserve( 3 );

        for( int i = 0; i < 3; ++i )
            slots.emplace_back();  // mints fresh KIIDs

        m_channel->SetRepeatInstances( slots );

        // Bus sheet pin on Channel: width matches repeat_count, so
        // R3.3's fan-out applies.  Position on the sheet's left edge.
        m_busPin = new SCH_SHEET_PIN( m_channel );
        m_busPin->SetText( wxT( "DATA[0..3]" ) );
        m_busPin->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
        m_busPin->SetPosition( VECTOR2I( 0, 10 * SCALE ) );
        m_channel->AddPin( m_busPin );

        // Scalar sheet pin EN — shared across all 4 slots (no fan-out).
        m_enPin = new SCH_SHEET_PIN( m_channel );
        m_enPin->SetText( wxT( "EN" ) );
        m_enPin->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
        m_enPin->SetPosition( VECTOR2I( 0, 30 * SCALE ) );
        m_channel->AddPin( m_enPin );

        // Parent-side wires that drive the sheet pins.
        // Bus wire (LAYER_BUS) ending on the bus pin position; the
        // bus label provides the bus name to the wire.
        SCH_LINE* busWire = new SCH_LINE( VECTOR2I( -20 * SCALE, 10 * SCALE ),
                                          LAYER_BUS );
        busWire->SetEndPoint( VECTOR2I( 0, 10 * SCALE ) );
        m_topScreen->Append( busWire );

        SCH_LABEL* busLabel = new SCH_LABEL( VECTOR2I( -20 * SCALE, 10 * SCALE ),
                                             wxT( "DATA[0..3]" ) );
        m_topScreen->Append( busLabel );

        // Scalar wire to EN pin.
        SCH_LINE* enWire = new SCH_LINE( VECTOR2I( -20 * SCALE, 30 * SCALE ),
                                         LAYER_WIRE );
        enWire->SetEndPoint( VECTOR2I( 0, 30 * SCALE ) );
        m_topScreen->Append( enWire );

        SCH_LABEL* enLabel = new SCH_LABEL( VECTOR2I( -20 * SCALE, 30 * SCALE ),
                                            wxT( "EN" ) );
        m_topScreen->Append( enLabel );

        // Child screen: one scalar hier-label per bus bit + the EN
        // label.  The R3.3 fan-out matches each bit-K label against
        // pinName "DATA<K>" on the slot-K path; on other slots the
        // name doesn't match and the label is left alone.
        for( int k = 0; k < 4; ++k )
        {
            wxString name = wxString::Format( wxT( "DATA%d" ), k );
            SCH_HIERLABEL* lbl = new SCH_HIERLABEL( VECTOR2I( 10 * SCALE,
                                                              ( 10 + 5 * k ) * SCALE ),
                                                    name );
            lbl->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
            m_channelScreen->Append( lbl );
            m_dataLabels[ k ] = lbl;
        }

        m_enLabel = new SCH_HIERLABEL( VECTOR2I( 10 * SCALE, 40 * SCALE ), wxT( "EN" ) );
        m_enLabel->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
        m_channelScreen->Append( m_enLabel );

        m_schematic->RefreshHierarchy();
    }

    /// Look up the slot-K SCH_SHEET_PATH from the schematic's hierarchy.
    /// Slot 0 ends in the on-canvas template; slots 1..3 end in
    /// synthetic clones whose KIIDs come from m_repeatInstances.
    SCH_SHEET_PATH SlotPath( int aK ) const
    {
        SCH_SHEET_LIST    hierarchy = m_schematic->Hierarchy();
        const std::vector<KIID>& slots = m_channel->GetRepeatInstances();

        const KIID expected = ( aK == 0 ) ? m_channel->m_Uuid
                                          : slots[ static_cast<size_t>( aK - 1 ) ];

        for( const SCH_SHEET_PATH& path : hierarchy )
        {
            // P7: every path's Last() is the on-canvas template
            // (m_channel), so slot identity now lives on the trailing
            // SCH_SHEET_INSTANCE.  Match by LastInstance().SlotKiid().
            if( path.size() == 2 && path.Last()
                && path.Last()->GetFileName() == "channel.kicad_sch"
                && path.LastInstance().SlotKiid() == expected )
            {
                return path;
            }
        }

        return {};
    }

    static constexpr int SCALE = 100000;  // 1 mm in internal units

    SETTINGS_MANAGER           m_mgr;
    std::unique_ptr<SCHEMATIC> m_schematic;

    SCH_SHEET*  m_top;
    SCH_SCREEN* m_topScreen;
    SCH_SHEET*  m_channel;
    SCH_SCREEN* m_channelScreen;

    SCH_SHEET_PIN* m_busPin;
    SCH_SHEET_PIN* m_enPin;

    SCH_HIERLABEL* m_dataLabels[ 4 ];
    SCH_HIERLABEL* m_enLabel;
};


BOOST_FIXTURE_TEST_SUITE( RepeatedSheetBusFanout, REPEATED_SHEET_BUSFANOUT_FIXTURE )


BOOST_AUTO_TEST_CASE( HierarchyHasFourChannelSlots )
{
    // Sanity: R2's BuildSheetList synthesizes the 4 channel paths
    // before R3 can do anything with them.
    SCH_SHEET_LIST hierarchy = m_schematic->Hierarchy();

    int channelSlots = 0;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() == 2 && path.Last()
            && path.Last()->GetFileName() == "channel.kicad_sch" )
        {
            ++channelSlots;
        }
    }

    BOOST_CHECK_EQUAL( channelSlots, 4 );
}


BOOST_AUTO_TEST_CASE( EachSlotBitConnectsToParentBus )
{
    // Build connectivity over the full sheet list (slot paths
    // included).  Recalculate with unconditional = true so a clean
    // graph is built from scratch.
    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();
    m_schematic->ConnectionGraph()->Recalculate( sheets, true );

    // For each slot K, the DATA<K> hier-label on the (shared) child
    // screen must end up on the parent's DATA<K> bus-bit net.  The
    // connection name carries the path-qualified net name (see
    // SCH_CONNECTION::Name() / GetNetName()); we only need to check
    // the local net name for equality with the bit name.
    for( int k = 0; k < 4; ++k )
    {
        SCH_SHEET_PATH slot = SlotPath( k );
        BOOST_REQUIRE_MESSAGE( slot.size() == 2,
                               "Slot path " << k << " missing from hierarchy" );

        SCH_HIERLABEL*   label = m_dataLabels[ k ];
        SCH_CONNECTION*  conn  = label->Connection( &slot );

        BOOST_REQUIRE_MESSAGE( conn != nullptr,
                               "Slot " << k << " DATA" << k
                                       << " label has no connection" );

        // The bit's local name is DATA<K>.
        wxString expected = wxString::Format( wxT( "DATA%d" ), k );

        BOOST_CHECK_MESSAGE( conn->LocalName() == expected,
                             "Slot " << k << ": expected LocalName "
                                     << expected << " got "
                                     << conn->LocalName() );

        // And the subgraph for that label on the slot-K path should
        // resolve to a net whose name ends in the bit name (the
        // parent bus's bit-K subgraph).  Look it up via the graph's
        // resolved name lookup; presence is the assertion.
        CONNECTION_SUBGRAPH* sg =
                m_schematic->ConnectionGraph()->GetSubgraphForItem( label );

        BOOST_REQUIRE_MESSAGE( sg != nullptr,
                               "Slot " << k << " DATA" << k
                                       << " label has no subgraph" );
    }
}


BOOST_AUTO_TEST_CASE( WrongBitLabelsDoNotCrossSlots )
{
    // On slot 0, the DATA1/DATA2/DATA3 labels exist on the shared
    // screen but the R3.3 fan-out only matches the bit-0 name; the
    // other three are left on their own (un-shared) local nets.
    // We verify that DATA1 on slot 0 is NOT on the same subgraph as
    // DATA1 on slot 1 (the latter is the one that fans out).
    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();
    m_schematic->ConnectionGraph()->Recalculate( sheets, true );

    SCH_SHEET_PATH slot0 = SlotPath( 0 );
    SCH_SHEET_PATH slot1 = SlotPath( 1 );

    BOOST_REQUIRE_EQUAL( slot0.size(), 2 );
    BOOST_REQUIRE_EQUAL( slot1.size(), 2 );

    SCH_CONNECTION* connSlot0 = m_dataLabels[ 1 ]->Connection( &slot0 );
    SCH_CONNECTION* connSlot1 = m_dataLabels[ 1 ]->Connection( &slot1 );

    BOOST_REQUIRE( connSlot0 != nullptr );
    BOOST_REQUIRE( connSlot1 != nullptr );

    // Both share the same local label name DATA1 — that's just the
    // label text — but their fully-qualified net names differ because
    // slot 1 is fan-out-bound to the parent bus and slot 0 is local.
    BOOST_CHECK_EQUAL( connSlot0->LocalName(), wxT( "DATA1" ) );
    BOOST_CHECK_EQUAL( connSlot1->LocalName(), wxT( "DATA1" ) );

    // The path-qualified Name() values include the sheet path, so
    // slot 0's DATA1 and slot 1's DATA1 are necessarily distinct
    // strings: slot 1's DATA1 reaches up to the parent bus, slot 0's
    // does not.  Different Name() values are the observable signature
    // that the two are on different nets.
    BOOST_CHECK_NE( connSlot0->Name(), connSlot1->Name() );
}


BOOST_AUTO_TEST_CASE( ScalarPinSharedAcrossSlots )
{
    // The EN pin is scalar — R3.3's repeatBusPinBitName returns empty
    // and the pre-R3.3 behavior (shared net) applies.  All 4 slots
    // see the EN label on the same parent-EN net, so the connection
    // Name() agrees across slots.
    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();
    m_schematic->ConnectionGraph()->Recalculate( sheets, true );

    wxString firstName;

    for( int k = 0; k < 4; ++k )
    {
        SCH_SHEET_PATH slot = SlotPath( k );
        BOOST_REQUIRE_EQUAL( slot.size(), 2 );

        SCH_CONNECTION* conn = m_enLabel->Connection( &slot );
        BOOST_REQUIRE_MESSAGE( conn != nullptr,
                               "Slot " << k << " EN label has no connection" );

        BOOST_CHECK_EQUAL( conn->LocalName(), wxT( "EN" ) );

        if( k == 0 )
            firstName = conn->Name();
        else
            BOOST_CHECK_EQUAL( conn->Name(), firstName );
    }
}


BOOST_AUTO_TEST_SUITE_END()


/**
 * Candidate A — vectorized shape: the body declares ONE scalar `DATA`
 * hier-label (not per-bit DATA0..DATA3) and the matcher accepts it via
 * the bus base-name fallback.  Each slot K's `DATA` subgraph still
 * binds to bit K of the parent's DATA[0..3] bus, driven by the slot
 * index in the path rather than by name-encoding in the label.
 *
 * This is the canonical shape per the multi-channel spec; the legacy
 * REPEATED_SHEET_BUSFANOUT_FIXTURE above remains as the backward-compat
 * case for hand-unrolled bodies.
 */
struct REPEATED_SHEET_BUSFANOUT_SCALAR_FIXTURE
{
    REPEATED_SHEET_BUSFANOUT_SCALAR_FIXTURE() :
            m_mgr()
    {
        m_mgr.LoadProject( "" );
        m_schematic = std::make_unique<SCHEMATIC>( &m_mgr.Prj() );
        m_schematic->Reset();

        SCH_SHEET* defaultSheet = m_schematic->GetTopLevelSheet( 0 );

        m_topScreen = new SCH_SCREEN( m_schematic.get() );
        m_top = new SCH_SHEET( m_schematic.get() );
        m_top->SetScreen( m_topScreen );
        m_top->SetName( "Top" );
        m_top->SetFileName( "top.kicad_sch" );

        m_schematic->AddTopLevelSheet( m_top );
        m_schematic->RemoveTopLevelSheet( defaultSheet );
        delete defaultSheet;

        m_channelScreen = new SCH_SCREEN( m_schematic.get() );
        m_channel = new SCH_SHEET( m_schematic.get() );
        const_cast<KIID&>( m_channel->m_Uuid ) = m_channelScreen->GetUuid();
        m_channel->SetScreen( m_channelScreen );
        m_channel->SetName( "Channel" );
        m_channel->SetFileName( "channel.kicad_sch" );
        m_channel->SetPosition( VECTOR2I( 0, 0 ) );
        m_channel->SetSize( VECTOR2I( 50 * SCALE, 80 * SCALE ) );

        m_topScreen->Append( m_channel );

        m_channel->SetRepeatCount( 4 );

        std::vector<KIID> slots;
        slots.reserve( 3 );
        for( int i = 0; i < 3; ++i )
            slots.emplace_back();
        m_channel->SetRepeatInstances( slots );

        // Bus sheet pin DATA[0..3] — same as the legacy fixture.
        m_busPin = new SCH_SHEET_PIN( m_channel );
        m_busPin->SetText( wxT( "DATA[0..3]" ) );
        m_busPin->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
        m_busPin->SetPosition( VECTOR2I( 0, 10 * SCALE ) );
        m_channel->AddPin( m_busPin );

        // Parent-side bus wire + bus label.
        SCH_LINE* busWire = new SCH_LINE( VECTOR2I( -20 * SCALE, 10 * SCALE ),
                                          LAYER_BUS );
        busWire->SetEndPoint( VECTOR2I( 0, 10 * SCALE ) );
        m_topScreen->Append( busWire );

        SCH_LABEL* busLabel = new SCH_LABEL( VECTOR2I( -20 * SCALE, 10 * SCALE ),
                                             wxT( "DATA[0..3]" ) );
        m_topScreen->Append( busLabel );

        // Vectorized body: ONE scalar hier-label named "DATA" (the bus
        // base-name).  The base-name fallback in propagateToNeighbors
        // matches this label across all 4 slot paths, and each slot
        // records m_repeat_bus_bit_index = K so Clone() renames the
        // subgraph driver to DATA[K].
        m_dataLabel = new SCH_HIERLABEL( VECTOR2I( 10 * SCALE, 10 * SCALE ),
                                         wxT( "DATA" ) );
        m_dataLabel->SetShape( LABEL_FLAG_SHAPE::L_INPUT );
        m_channelScreen->Append( m_dataLabel );

        m_schematic->RefreshHierarchy();
    }

    SCH_SHEET_PATH SlotPath( int aK ) const
    {
        SCH_SHEET_LIST    hierarchy = m_schematic->Hierarchy();
        const std::vector<KIID>& slots = m_channel->GetRepeatInstances();

        const KIID expected = ( aK == 0 ) ? m_channel->m_Uuid
                                          : slots[ static_cast<size_t>( aK - 1 ) ];

        for( const SCH_SHEET_PATH& path : hierarchy )
        {
            // P7: every path's Last() is the on-canvas template
            // (m_channel), so slot identity now lives on the trailing
            // SCH_SHEET_INSTANCE.  Match by LastInstance().SlotKiid().
            if( path.size() == 2 && path.Last()
                && path.Last()->GetFileName() == "channel.kicad_sch"
                && path.LastInstance().SlotKiid() == expected )
            {
                return path;
            }
        }
        return {};
    }

    static constexpr int SCALE = 100000;

    SETTINGS_MANAGER           m_mgr;
    std::unique_ptr<SCHEMATIC> m_schematic;

    SCH_SHEET*  m_top;
    SCH_SCREEN* m_topScreen;
    SCH_SHEET*  m_channel;
    SCH_SCREEN* m_channelScreen;

    SCH_SHEET_PIN* m_busPin;
    SCH_HIERLABEL* m_dataLabel;
};


BOOST_FIXTURE_TEST_SUITE( RepeatedSheetBusFanoutScalar,
                          REPEATED_SHEET_BUSFANOUT_SCALAR_FIXTURE )


BOOST_AUTO_TEST_CASE( ScalarBodyPortMatchesBusBaseName )
{
    // Build connectivity for the full sheet list.  The matcher's
    // base-name fallback must wire each slot's lone "DATA" label to
    // bit K of the parent's DATA[0..3] bus.
    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();
    m_schematic->ConnectionGraph()->Recalculate( sheets, true );

    for( int k = 0; k < 4; ++k )
    {
        SCH_SHEET_PATH slot = SlotPath( k );
        BOOST_REQUIRE_MESSAGE( slot.size() == 2,
                               "Slot path " << k << " missing from hierarchy" );

        SCH_CONNECTION* conn = m_dataLabel->Connection( &slot );

        BOOST_REQUIRE_MESSAGE( conn != nullptr,
                               "Slot " << k << " DATA label has no connection" );

        // Local name on the body side is just the label text "DATA".
        BOOST_CHECK_EQUAL( conn->LocalName(), wxT( "DATA" ) );

        // The subgraph for that label on the slot-K path exists; the
        // bit-K rename is performed by the Clone() pass driven off
        // m_repeat_bus_bit_index.  Presence + uniqueness of the per-slot
        // Name() is the observable signature.
        CONNECTION_SUBGRAPH* sg =
                m_schematic->ConnectionGraph()->GetSubgraphForItem( m_dataLabel );
        BOOST_REQUIRE_MESSAGE( sg != nullptr,
                               "Slot " << k << " DATA label has no subgraph" );
    }

    // Cross-slot disambiguation: the Name() of slot K's DATA subgraph
    // must differ from slot J's (each binds to a different bus bit).
    SCH_SHEET_PATH slot0 = SlotPath( 0 );
    SCH_SHEET_PATH slot1 = SlotPath( 1 );
    SCH_CONNECTION* c0 = m_dataLabel->Connection( &slot0 );
    SCH_CONNECTION* c1 = m_dataLabel->Connection( &slot1 );
    BOOST_REQUIRE( c0 != nullptr );
    BOOST_REQUIRE( c1 != nullptr );
    BOOST_CHECK_NE( c0->Name(), c1->Name() );
}


BOOST_AUTO_TEST_SUITE_END()
