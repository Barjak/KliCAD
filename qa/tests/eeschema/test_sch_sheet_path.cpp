/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file
 * Test suite for #SCH_SHEET_PATH and #SCH_SHEET_LIST
 */

#include <qa_utils/uuid_test_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>
#include "eeschema_test_utils.h"

// Code under test
#include <sch_sheet_path.h>

#include <wildcards_and_files_ext.h>
#include <eeschema_helpers.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <schematic.h>

#include <sstream>

class TEST_SCH_SHEET_PATH_FIXTURE
{
public:
    TEST_SCH_SHEET_PATH_FIXTURE() : m_schematic( nullptr )
    {
        for( unsigned i = 0; i < 4; ++i )
        {
            m_sheets.emplace_back( nullptr, VECTOR2I( i, i ) );

            std::ostringstream ss;
            ss << "Sheet" << i;
            m_sheets[i].GetField( FIELD_T::SHEET_NAME )->SetText( ss.str() );
            m_sheets[i].SetParent( &m_schematic );
        }

        // 0->1->2
        m_linear.push_back( &m_sheets[0] );
        m_linear.push_back( &m_sheets[1] );
        m_linear.push_back( &m_sheets[2] );
    }

    SCHEMATIC      m_schematic;
    SCH_SHEET_PATH m_empty_path;

    /**
     * We look at sheet 2 in the hierarchy:
     * Sheets: 0 -> 1 -> 2
     */
    SCH_SHEET_PATH m_linear;

    /// handy store of SCH_SHEET objects
    std::vector<SCH_SHEET> m_sheets;
};


/**
 * Declare the test suite
 */
BOOST_FIXTURE_TEST_SUITE( SchSheetPath, TEST_SCH_SHEET_PATH_FIXTURE )


/**
 * Check properties of an empty SCH_SHEET_PATH
 */
BOOST_AUTO_TEST_CASE( Empty )
{
    BOOST_CHECK_EQUAL( m_empty_path.size(), 0 );

    BOOST_CHECK_THROW( m_empty_path.at( 0 ), std::out_of_range );

    // Sheet paths with no SCH_SCHEET object are illegal.
    // CHECK_WX_ASSERT( m_empty_path.GetPageNumber() );

    // These accessors return nullptr when empty (i.e. they don't crash)
    BOOST_CHECK_EQUAL( m_empty_path.Last(), nullptr );
    BOOST_CHECK_EQUAL( m_empty_path.LastScreen(), nullptr );

    BOOST_CHECK_EQUAL( m_empty_path.PathAsString(), "/" );
    BOOST_CHECK_EQUAL( m_empty_path.PathHumanReadable(), "/" );
}


/**
 * Check properties of a non-empty SCH_SHEET_PATH
 */
BOOST_AUTO_TEST_CASE( NonEmpty )
{
    BOOST_CHECK_EQUAL( m_linear.size(), 3 );

    BOOST_CHECK_EQUAL( m_linear.at( 0 ), &m_sheets[0] );
    BOOST_CHECK_EQUAL( m_linear.at( 1 ), &m_sheets[1] );
    BOOST_CHECK_EQUAL( m_linear.at( 2 ), &m_sheets[2] );

    BOOST_CHECK_EQUAL( m_linear.Last(), &m_sheets[2] );
    BOOST_CHECK_EQUAL( m_linear.LastScreen(), nullptr );

    // don't know what the uuids will be, but we know the format: /<8-4-4-4-12>/<8-4-4-4-12>/
    BOOST_CHECK_PREDICATE(
            KI_TEST::IsUUIDPathWithLevels, ( m_linear.PathAsString().ToStdString() )( 2 ) );

    // Sheet0 is the root sheet and isn't in the path
    BOOST_CHECK_EQUAL( m_linear.PathHumanReadable(), "/Sheet1/Sheet2/" );
}


BOOST_AUTO_TEST_CASE( Compare )
{
    SCH_SHEET_PATH otherEmpty;

    BOOST_CHECK( m_empty_path == otherEmpty );

    BOOST_CHECK( m_empty_path != m_linear );
}


BOOST_AUTO_TEST_CASE( SheetListGetOrdinalPath )
{
    // The "complex_hierarchy" test project has a root sheet with two sheets that reference the
    // same file.
    std::unique_ptr<SCHEMATIC> schematic;
    wxFileName fn( wxString::Format( wxS( "%snetlists/complex_hierarchy" ),
                                     KI_TEST::GetEeschemaTestDataDir() ),
                   wxS( "complex_hierarchy" ), FILEEXT::ProjectFileExtension );

    schematic.reset( EESCHEMA_HELPERS::LoadSchematic( fn.GetFullPath(), false, false, nullptr ) );

    SCH_SHEET_LIST hierarchy = schematic->Hierarchy();
    BOOST_CHECK_EQUAL( hierarchy.size(), 3 );

    // A null pointer should always result in an empty return value.
    BOOST_CHECK( !hierarchy.GetOrdinalPath( nullptr ) );

    // The root sheet is a single instance.  It's always ordinal.
    BOOST_CHECK( hierarchy.GetOrdinalPath( schematic->RootScreen() ).value() == hierarchy.at( 0 ) );

    // The shared schematic with the lowest page number is the ordinal sheet path.
    SCH_SHEET* sheet = hierarchy.at( 1 ).Last();
    BOOST_CHECK( hierarchy.GetOrdinalPath( sheet->GetScreen() ).value() == hierarchy.at( 1 ) );

    // The shared sheet with a higher page number is not the ordinal sheet path.
    sheet = hierarchy.at( 2 ).Last();
    BOOST_CHECK( hierarchy.GetOrdinalPath( sheet->GetScreen() ).value() == hierarchy.at( 1 ) );
}


/**
 * Test sheet path page number properties.
 */
BOOST_AUTO_TEST_CASE( SheetPathPageProperties )
{
    // BOOST_CHECK_EQUAL( m_linear.GetPageNumber(), wxEmptyString );

    // Add new instance to sheet object.
    // BOOST_CHECK( m_linear.Last()->AddInstance( m_linear.Path() ) );
    // m_linear.SetPageNumber( "1" );
    // BOOST_CHECK_EQUAL( m_linear.GetPageNumber(), "1" );
    // m_linear.SetPageNumber( "i" );
    // BOOST_CHECK_EQUAL( m_linear.GetPageNumber(), "i" );
}


/**
 * Test PathHumanReadable with sheet names containing slashes.
 * This tests the fix for GitLab issue #21878 where slashes in sheet names
 * caused incorrect netclass pattern matching.
 */
BOOST_AUTO_TEST_CASE( PathHumanReadableWithSlashes )
{
    SCH_SHEET_PATH pathWithSlash;
    SCHEMATIC schematicLocal( nullptr );
    std::vector<SCH_SHEET> sheets;

    for( unsigned i = 0; i < 3; ++i )
    {
        sheets.emplace_back( nullptr, VECTOR2I( i, i ) );
        sheets[i].SetParent( &schematicLocal );
    }

    sheets[0].GetField( FIELD_T::SHEET_NAME )->SetText( "Root" );
    sheets[1].GetField( FIELD_T::SHEET_NAME )->SetText( "Power/Supply" );
    sheets[2].GetField( FIELD_T::SHEET_NAME )->SetText( "SubSheet" );

    pathWithSlash.push_back( &sheets[0] );
    pathWithSlash.push_back( &sheets[1] );
    pathWithSlash.push_back( &sheets[2] );

    // Without escaping, the path contains the literal '/' in the sheet name
    wxString unescaped = pathWithSlash.PathHumanReadable( true, false, false );
    BOOST_CHECK_EQUAL( unescaped, "/Power/Supply/SubSheet/" );

    // With escaping, the '/' in the sheet name becomes "{slash}"
    wxString escaped = pathWithSlash.PathHumanReadable( true, false, true );
    BOOST_CHECK_EQUAL( escaped, "/Power{slash}Supply/SubSheet/" );

    // The escaped version should be unambiguous since '/' only means path separator
    // and "{slash}" means a literal slash character in the sheet name.

    // Test with stripping trailing separator
    wxString escapedNoTrail = pathWithSlash.PathHumanReadable( true, true, true );
    BOOST_CHECK_EQUAL( escapedNoTrail, "/Power{slash}Supply/SubSheet" );
}


/**
 * Test SCH_SHEET_PATH::GetSlotIndex() — derive the multi-channel slot
 * of the path's last segment.  P7: slot identity lives on the path's
 * trailing SCH_SHEET_INSTANCE (template_kiid + slot_kiid), not on the
 * SCH_SHEET pointer.  Scenarios:
 *
 *   (a) non-repeated parent → -1
 *   (b) template-slot SCH_SHEET_INSTANCE on a repeat>1 sheet → 0
 *   (c) slot K>0 SCH_SHEET_INSTANCE (slot_kiid in m_repeatInstances) → K
 *
 * Plus data-corruption guard (slot_kiid not on the template's instance
 * list) and the empty-path sentinel.  GetSlotIndex resolves the
 * template via SCHEMATIC::ResolveSheetTemplate, so the fixture wires
 * the sheets up to m_schematic via SetTopLevelSheets + RefreshHierarchy.
 */
BOOST_AUTO_TEST_CASE( GetSlotIndex )
{
    // (a) Empty path — sentinel.
    BOOST_CHECK_EQUAL( m_empty_path.GetSlotIndex(), -1 );

    // (a) Non-repeated parent — the m_linear fixture has repeat_count
    // == 1 (default) on every sheet, so Last() is a vanilla
    // single-instance sheet.
    BOOST_CHECK_EQUAL( m_linear.GetSlotIndex(), -1 );

    // Build a small parent/template hierarchy for (b) and (c) and
    // register it with a SCHEMATIC so ResolveSheetTemplate finds the
    // template by KIID:
    //
    //   parent (repeat=1)
    //     └─ template  (repeat=4)
    //
    // The path of "slot K" (K > 0) is push_back(parent),
    // push_back_slot(tmpl, kK) — both segments point at the
    // on-canvas template; slot K's identity lives on the trailing
    // SCH_SHEET_INSTANCE.
    SCHEMATIC   sch( nullptr );
    SCH_SHEET*  parent = new SCH_SHEET( &sch );
    SCH_SCREEN* parentScreen = new SCH_SCREEN( &sch );
    const_cast<KIID&>( parent->m_Uuid ) = parentScreen->GetUuid();
    parent->SetScreen( parentScreen );
    parent->GetField( FIELD_T::SHEET_NAME )->SetText( "Parent" );

    SCH_SHEET*  tmpl = new SCH_SHEET( &sch );
    SCH_SCREEN* tmplScreen = new SCH_SCREEN( &sch );
    const_cast<KIID&>( tmpl->m_Uuid ) = tmplScreen->GetUuid();
    tmpl->SetScreen( tmplScreen );
    tmpl->GetField( FIELD_T::SHEET_NAME )->SetText( "Channel" );
    tmpl->SetRepeatCount( 4 );

    const KIID k1, k2, k3;
    tmpl->SetRepeatInstances( { k1, k2, k3 } );

    parentScreen->Append( tmpl );
    sch.SetTopLevelSheets( { parent } );
    sch.RefreshHierarchy();

    // (b) Template-slot SCH_SHEET_INSTANCE on a repeat>1 sheet → 0.
    SCH_SHEET_PATH slot0;
    slot0.push_back( parent );
    slot0.push_back( tmpl );
    BOOST_CHECK_EQUAL( slot0.GetSlotIndex(), 0 );

    // (c) Slot K>0 → K.  push_back_slot stamps the trailing instance
    // with template_kiid=tmpl->m_Uuid + slot_kiid=kK.
    SCH_SHEET_PATH slot1;
    slot1.push_back( parent );
    slot1.push_back_slot( tmpl, k1 );
    BOOST_CHECK_EQUAL( slot1.GetSlotIndex(), 1 );

    SCH_SHEET_PATH slot2;
    slot2.push_back( parent );
    slot2.push_back_slot( tmpl, k2 );
    BOOST_CHECK_EQUAL( slot2.GetSlotIndex(), 2 );

    SCH_SHEET_PATH slot3;
    slot3.push_back( parent );
    slot3.push_back_slot( tmpl, k3 );
    BOOST_CHECK_EQUAL( slot3.GetSlotIndex(), 3 );

    // Data-corruption guard — a slot_kiid that's NOT on the
    // template's instance list (e.g., template shrunk after the
    // path was constructed).  GetSlotIndex returns -1.
    const KIID strayKiid;
    SCH_SHEET_PATH orphanPath;
    orphanPath.push_back( parent );
    orphanPath.push_back_slot( tmpl, strayKiid );
    BOOST_CHECK_EQUAL( orphanPath.GetSlotIndex(), -1 );

    // Non-repeated sheet (repeat_count==1) directly under a parent —
    // still -1 (single instance, no expansion).
    SCH_SHEET*  plain = new SCH_SHEET( &sch );
    SCH_SCREEN* plainScreen = new SCH_SCREEN( &sch );
    const_cast<KIID&>( plain->m_Uuid ) = plainScreen->GetUuid();
    plain->SetScreen( plainScreen );
    plain->GetField( FIELD_T::SHEET_NAME )->SetText( "Plain" );
    parentScreen->Append( plain );
    sch.RefreshHierarchy();

    SCH_SHEET_PATH plainPath;
    plainPath.push_back( parent );
    plainPath.push_back( plain );
    BOOST_CHECK_EQUAL( plainPath.GetSlotIndex(), -1 );
}


/**
 * F1: PathHumanReadable must append a `:K` slot suffix when a path's
 * trailing SCH_SHEET_INSTANCE points at slot K > 0 of a repeat>1
 * template (K = position of slot_kiid within m_repeatInstances + 1).
 * Without this disambiguation, all slots of the same template share
 * their SHEET_NAME field and produce identical human-readable paths,
 * which downstream collapses N PCB channels onto one rule area.  The
 * template-slot path (K = 0) stays un-suffixed so existing single-
 * instance hierarchies are unaffected.
 */
BOOST_AUTO_TEST_CASE( PathHumanReadableSyntheticSlotSuffix )
{
    // parent (repeat=1)
    //   └─ template "Channel" (repeat=4)
    //       slot 0 = template-slot SCH_SHEET_INSTANCE — no suffix
    //       slot 1 = push_back_slot(tmpl, k1)         → "Channel:1"
    //       slot 2 = push_back_slot(tmpl, k2)         → "Channel:2"
    //       slot 3 = push_back_slot(tmpl, k3)         → "Channel:3"
    SCHEMATIC   sch( nullptr );
    SCH_SHEET*  parent = new SCH_SHEET( &sch );
    SCH_SCREEN* parentScreen = new SCH_SCREEN( &sch );
    const_cast<KIID&>( parent->m_Uuid ) = parentScreen->GetUuid();
    parent->SetScreen( parentScreen );
    parent->GetField( FIELD_T::SHEET_NAME )->SetText( "Parent" );

    SCH_SHEET*  tmpl = new SCH_SHEET( &sch );
    SCH_SCREEN* tmplScreen = new SCH_SCREEN( &sch );
    const_cast<KIID&>( tmpl->m_Uuid ) = tmplScreen->GetUuid();
    tmpl->SetScreen( tmplScreen );
    tmpl->GetField( FIELD_T::SHEET_NAME )->SetText( "Channel" );
    tmpl->SetRepeatCount( 4 );

    const KIID k1, k2, k3;
    tmpl->SetRepeatInstances( { k1, k2, k3 } );

    parentScreen->Append( tmpl );
    sch.SetTopLevelSheets( { parent } );
    sch.RefreshHierarchy();

    // Slot 0: template-slot — no suffix (preserves the behavior for
    // single-instance hierarchies).
    SCH_SHEET_PATH slot0;
    slot0.push_back( parent );
    slot0.push_back( tmpl );
    BOOST_CHECK_EQUAL( slot0.PathHumanReadable( true, false, false ), "/Channel/" );

    // Slots 1..N-1: `:K` suffix on the trailing segment.  Parent
    // segment remains untouched (parent is repeat_count==1).
    SCH_SHEET_PATH slot1;
    slot1.push_back( parent );
    slot1.push_back_slot( tmpl, k1 );
    BOOST_CHECK_EQUAL( slot1.PathHumanReadable( true, false, false ), "/Channel:1/" );

    SCH_SHEET_PATH slot2;
    slot2.push_back( parent );
    slot2.push_back_slot( tmpl, k2 );
    BOOST_CHECK_EQUAL( slot2.PathHumanReadable( true, false, false ), "/Channel:2/" );

    SCH_SHEET_PATH slot3;
    slot3.push_back( parent );
    slot3.push_back_slot( tmpl, k3 );
    BOOST_CHECK_EQUAL( slot3.PathHumanReadable( true, false, false ), "/Channel:3/" );

    // Trailing-separator stripping still works with the suffix.
    BOOST_CHECK_EQUAL( slot2.PathHumanReadable( true, true, false ), "/Channel:2" );

    // Non-multi-channel hierarchies are untouched: m_linear's
    // segments are all repeat_count==1, so the output matches the
    // pre-F1 baseline.
    BOOST_CHECK_EQUAL( m_linear.PathHumanReadable(), "/Sheet1/Sheet2/" );

    // Data-corruption guard — slot_kiid NOT on the template's
    // instance list emits the bare name with no suffix (matches
    // GetSlotIndex's -1 sentinel; safer than fabricating a slot
    // number).
    const KIID strayKiid;
    SCH_SHEET_PATH orphanPath;
    orphanPath.push_back( parent );
    orphanPath.push_back_slot( tmpl, strayKiid );
    BOOST_CHECK_EQUAL( orphanPath.PathHumanReadable( true, false, false ), "/Channel/" );
}


BOOST_AUTO_TEST_SUITE_END()
