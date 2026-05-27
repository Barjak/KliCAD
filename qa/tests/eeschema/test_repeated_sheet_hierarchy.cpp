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
 * @file test_repeated_sheet_hierarchy.cpp
 *
 * Phase R2 — verify BuildSheetList materializes synthetic-clone
 * SCH_SHEET children when a child sheet's repeat_count > 1.  N peers
 * appear in the Hierarchy() list; all share the template's screen;
 * each has a distinct Last()->m_Uuid drawn from m_repeatInstances
 * (slot 0 == template's own m_Uuid, slots 1..N-1 == m_repeatInstances).
 */

#include <boost/test/unit_test.hpp>

#include <sch_sheet.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>


/**
 * Build a SCHEMATIC with this shape:
 *
 *     virtual_root
 *       └─ top (Sheet "Top",     screen with N children)
 *           └─ ch  (Sheet "Channel", repeat_count = N)
 *                  └─ <empty screen — N synthetic clones share it>
 *
 * Returned by reference so the caller can inspect; ownership stays
 * with the SCHEMATIC.
 */
struct REPEATED_SHEET_FIXTURE
{
    REPEATED_SHEET_FIXTURE() :
            m_schematic( nullptr )
    {
        m_top = new SCH_SHEET( &m_schematic );
        SCH_SCREEN* topScreen = new SCH_SCREEN( &m_schematic );
        const_cast<KIID&>( m_top->m_Uuid ) = topScreen->GetUuid();
        m_top->SetScreen( topScreen );
        m_top->SetName( "Top" );
        m_top->SetFileName( "top.kicad_sch" );

        m_channel = new SCH_SHEET( &m_schematic );
        m_channelScreen = new SCH_SCREEN( &m_schematic );
        const_cast<KIID&>( m_channel->m_Uuid ) = m_channelScreen->GetUuid();
        m_channel->SetScreen( m_channelScreen );
        m_channel->SetName( "Channel" );
        m_channel->SetFileName( "channel.kicad_sch" );

        // The child sheet sits inside the top's screen — this is how
        // children appear in a real schematic.
        topScreen->Append( m_channel );

        m_schematic.SetTopLevelSheets( { m_top } );
    }

    /// Configure the channel as a repeat_count=N sheet.  Pre-allocates
    /// N-1 stable KIIDs into m_repeatInstances; slot 0 is the channel
    /// sheet's own m_Uuid.
    void SetRepeat( int aN )
    {
        m_channel->SetRepeatCount( aN );

        std::vector<KIID> slots;
        slots.reserve( aN - 1 );

        for( int i = 1; i < aN; ++i )
            slots.emplace_back();

        m_channel->SetRepeatInstances( slots );
    }

    SCHEMATIC   m_schematic;
    SCH_SHEET*  m_top;
    SCH_SHEET*  m_channel;
    SCH_SCREEN* m_channelScreen;
};


BOOST_FIXTURE_TEST_SUITE( RepeatedSheetHierarchy, REPEATED_SHEET_FIXTURE )


BOOST_AUTO_TEST_CASE( DefaultSingleInstanceUnchanged )
{
    // repeat_count = 1 (default) — Hierarchy() returns one entry for
    // top + one entry for the (single) channel.  This guarantees R2
    // is invisible to pre-multi-channel schematics.
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();
    BOOST_CHECK_EQUAL( hierarchy.size(), 2 );
}


BOOST_AUTO_TEST_CASE( ExpandsToNPaths )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();

    // 1 (top) + 4 (channel slots 0..3) = 5 paths
    BOOST_CHECK_EQUAL( hierarchy.size(), 5 );
}


BOOST_AUTO_TEST_CASE( CloneScreenIsShared )
{
    // Every channel slot reads the same LastScreen() — that's the
    // core invariant: N peers SHARE the body, they don't duplicate it.
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();

    SCH_SCREEN* sharedScreen = nullptr;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() == 2 && path.Last()->GetFileName() == "channel.kicad_sch" )
        {
            if( !sharedScreen )
                sharedScreen = path.LastScreen();
            else
                BOOST_CHECK_EQUAL( path.LastScreen(), sharedScreen );
        }
    }

    BOOST_CHECK_EQUAL( sharedScreen, m_channelScreen );
}


BOOST_AUTO_TEST_CASE( ClonesHaveDistinctKIIDs )
{
    // Every channel slot has a distinct Last()->m_Uuid — that's what
    // distinguishes the N paths from one another for refdes
    // annotation and netlist export.
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();

    std::set<KIID> seenKIIDs;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() == 2 && path.Last()->GetFileName() == "channel.kicad_sch" )
        {
            BOOST_CHECK( seenKIIDs.insert( path.Last()->m_Uuid ).second );
        }
    }

    BOOST_CHECK_EQUAL( seenKIIDs.size(), 4 );
}


BOOST_AUTO_TEST_CASE( SlotZeroIsTheOnCanvasSheet )
{
    // Slot 0 of the expansion uses the on-canvas template SCH_SHEET
    // directly — not a synthetic clone.  This preserves identity for
    // any downstream consumer that compares against the template's
    // pointer (e.g., selection state, drag handles).
    SetRepeat( 3 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();

    bool foundTemplate = false;
    int  syntheticCount = 0;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() == 2 && path.Last()->GetFileName() == "channel.kicad_sch" )
        {
            if( path.Last() == m_channel )
                foundTemplate = true;
            else if( path.Last()->IsSynthetic() )
                ++syntheticCount;
        }
    }

    BOOST_CHECK( foundTemplate );
    BOOST_CHECK_EQUAL( syntheticCount, 2 );  // N - 1 = 3 - 1
}


BOOST_AUTO_TEST_CASE( SyntheticClonesPointAtTemplate )
{
    // GetTemplate() on a synthetic clone returns the on-canvas sheet
    // — needed for the hierarchy navigator's rename guard, which
    // forwards SetName writes to the template instead of letting them
    // hit the transient clone (where they would silently revert on
    // the next RefreshHierarchy).
    SetRepeat( 3 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_LIST hierarchy = m_schematic.Hierarchy();

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() == 2 && path.Last()->IsSynthetic() )
            BOOST_CHECK_EQUAL( path.Last()->GetTemplate(), m_channel );
    }
}


BOOST_AUTO_TEST_CASE( ShrinkInvalidatesOldClones )
{
    // After shrinking repeat_count and refreshing, the hierarchy
    // collapses — the schematic's clone cache is cleared first so
    // stale clones from the previous walk don't leak.
    SetRepeat( 8 );
    m_schematic.RefreshHierarchy();
    BOOST_REQUIRE_EQUAL( m_schematic.Hierarchy().size(), 9 );

    SetRepeat( 2 );
    m_schematic.RefreshHierarchy();
    BOOST_CHECK_EQUAL( m_schematic.Hierarchy().size(), 3 );  // top + 2 channels
}


BOOST_AUTO_TEST_SUITE_END()
