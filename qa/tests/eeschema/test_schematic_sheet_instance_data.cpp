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
 * @file test_schematic_sheet_instance_data.cpp
 *
 * Verifies the new SCHEMATIC-owned per-sheet-instance data API
 * introduced in P3a of the SCH_SHEET_INSTANCE refactor.  No
 * consumers route through this storage yet (P3b/c add dual-write
 * and read-flip); these tests pin the API shape and its lifetime
 * semantics so the migration can proceed against a known-good
 * substrate.
 *
 * Properties verified:
 *
 *   - Find returns nullptr for absent paths.
 *   - GetOrCreate is idempotent and stamps m_Path on insertion.
 *   - Set/Get page-number round-trip works and creates a record on
 *     first write.
 *   - Remove clears just the targeted record.
 *   - Storage survives RefreshHierarchy / ClearRepeatCloneCache
 *     cycles — this is the structural property the larger refactor
 *     ships, exercised here at the smallest scope.
 */

#include <boost/test/unit_test.hpp>

#include <kiid.h>
#include <sch_sheet.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>


struct SCHEMATIC_INSTANCE_DATA_FIXTURE
{
    SCHEMATIC_INSTANCE_DATA_FIXTURE() :
            m_schematic( nullptr )
    {
        m_top = new SCH_SHEET( &m_schematic );
        SCH_SCREEN* topScreen = new SCH_SCREEN( &m_schematic );
        const_cast<KIID&>( m_top->m_Uuid ) = topScreen->GetUuid();
        m_top->SetScreen( topScreen );
        m_top->SetName( "Top" );
        m_top->SetFileName( "top.kicad_sch" );

        m_schematic.SetTopLevelSheets( { m_top } );
    }

    SCHEMATIC  m_schematic;
    SCH_SHEET* m_top;
};


BOOST_FIXTURE_TEST_SUITE( SchematicSheetInstanceData,
                          SCHEMATIC_INSTANCE_DATA_FIXTURE )


BOOST_AUTO_TEST_CASE( FindReturnsNullForAbsentPath )
{
    KIID_PATH bogus;
    bogus.push_back( KIID() );

    BOOST_CHECK( m_schematic.FindSheetInstanceData( bogus ) == nullptr );
}


BOOST_AUTO_TEST_CASE( GetOrCreateInsertsAndStampsPath )
{
    KIID_PATH p;
    p.push_back( KIID() );

    SCH_SHEET_INSTANCE_DATA& rec = m_schematic.GetOrCreateSheetInstanceData( p );

    BOOST_CHECK( rec.m_Path == p );
    BOOST_CHECK( m_schematic.FindSheetInstanceData( p ) == &rec );
}


BOOST_AUTO_TEST_CASE( GetOrCreateIsIdempotent )
{
    KIID_PATH p;
    p.push_back( KIID() );

    SCH_SHEET_INSTANCE_DATA& first  = m_schematic.GetOrCreateSheetInstanceData( p );
    SCH_SHEET_INSTANCE_DATA& second = m_schematic.GetOrCreateSheetInstanceData( p );

    // Same reference — map iterator-stability for std::map is the
    // requirement we depend on.
    BOOST_CHECK_EQUAL( &first, &second );
}


BOOST_AUTO_TEST_CASE( PageNumberRoundTrip )
{
    KIID_PATH p;
    p.push_back( KIID() );

    BOOST_CHECK_EQUAL( m_schematic.GetSheetInstancePageNumber( p ),
                       wxString( wxEmptyString ) );

    m_schematic.SetSheetInstancePageNumber( p, "42" );

    BOOST_CHECK_EQUAL( m_schematic.GetSheetInstancePageNumber( p ),
                       wxString( "42" ) );
}


BOOST_AUTO_TEST_CASE( SetPageNumberCreatesRecordOnFirstWrite )
{
    KIID_PATH p;
    p.push_back( KIID() );

    BOOST_REQUIRE( m_schematic.FindSheetInstanceData( p ) == nullptr );

    m_schematic.SetSheetInstancePageNumber( p, "7" );

    BOOST_CHECK( m_schematic.FindSheetInstanceData( p ) != nullptr );
    BOOST_CHECK_EQUAL( m_schematic.FindSheetInstanceData( p )->m_PageNumber,
                       wxString( "7" ) );
}


BOOST_AUTO_TEST_CASE( RemoveDropsOnlyTargetedPath )
{
    KIID_PATH p1, p2;
    p1.push_back( KIID() );
    p2.push_back( KIID() );

    m_schematic.SetSheetInstancePageNumber( p1, "a" );
    m_schematic.SetSheetInstancePageNumber( p2, "b" );

    m_schematic.RemoveSheetInstanceData( p1 );

    BOOST_CHECK( m_schematic.FindSheetInstanceData( p1 ) == nullptr );
    BOOST_CHECK( m_schematic.FindSheetInstanceData( p2 ) != nullptr );
    BOOST_CHECK_EQUAL( m_schematic.FindSheetInstanceData( p2 )->m_PageNumber,
                       wxString( "b" ) );
}


BOOST_AUTO_TEST_CASE( DataSurvivesRefreshHierarchyCycle )
{
    // The structural property this refactor ships: per-instance data
    // is independent of the SCH_SHEET clone lifetime.  Write data,
    // force RefreshHierarchy (which clears the clone cache), confirm
    // the data is still there.
    KIID_PATH p;
    p.push_back( m_top->m_Uuid );
    p.push_back( KIID() );

    m_schematic.SetSheetInstancePageNumber( p, "99" );

    m_schematic.RefreshHierarchy();
    m_schematic.RefreshHierarchy();
    m_schematic.RefreshHierarchy();

    BOOST_CHECK_EQUAL( m_schematic.GetSheetInstancePageNumber( p ),
                       wxString( "99" ) );
}


BOOST_AUTO_TEST_SUITE_END()
