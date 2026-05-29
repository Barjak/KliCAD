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
 * @file test_sheet_path_lifetime.cpp
 *
 * Regression gate for the SCH_SHEET_PATH KIID-storage refactor.
 *
 * Background: before the refactor, SCH_SHEET_PATH stored
 * std::vector<SCH_SHEET*>.  Long-lived holders (e.g. CONNECTION_SUBGRAPH::
 * m_sheet) cached paths across RefreshHierarchy() boundaries.  Each
 * RefreshHierarchy() called ClearRepeatCloneCache() — freeing every
 * synthetic SCH_SHEET clone — and then re-minted clones at new
 * addresses via BuildSheetListSortedByPageNumbers().  Any cached path
 * whose Last() was a clone became dangling.
 *
 * Under ASan this surfaced as a heap-use-after-free on
 * SCH_SHEET::IsSynthetic() called from resolveHierPinPushTarget()
 * (connection_graph.cpp:2914) during the next buildConnectionGraph()
 * pass.  Three prior fixes (KliCAD 66b0239127, e7ecca48ac, 35f1fa2a35)
 * proximately patched specific symptoms; the structural cause was
 * SCH_SHEET_PATH encoding identity as address rather than as KIID.
 *
 * Refactor (refactor/kiid-sheet-path): SCH_SHEET_PATH stores
 * std::vector<KIID>; pointer access requires explicit resolution
 * against the live SCHEMATIC.  Cached paths survive arbitrary
 * RefreshHierarchy() churn because identity is by-value, not by-
 * address.
 *
 * This file is the canonical regression: it exercises the exact
 * lifetime cycle that produced the original UAF, plus the related
 * "stale path stored in a long-lived map" pattern from
 * CONNECTION_GRAPH's m_sheet_to_subgraphs_map.
 *
 * Pre-refactor expected behavior under ASan:
 *   PathSurvivesRefreshHierarchyCycle      → heap-use-after-free
 *   StaleMapKeyAfterCacheClear             → heap-use-after-free
 *   ClonePointerStableWithinSingleRefresh  → pass (documents the
 *                                              proximate-fix invariant
 *                                              from R1 that the
 *                                              structural refactor
 *                                              renders moot)
 *   OrderingStableAcrossRefresh            → fails (operator< is
 *                                              pointer-based, see
 *                                              auditor C1)
 *   PageNumberSurvivesRefresh              → fails (write goes to
 *                                              the soon-to-be-freed
 *                                              clone, auditor I3)
 *   ConnectionMapPathReadsSurviveRefresh   → heap-use-after-free
 *
 * Post-refactor expected behavior under ASan: all six pass cleanly.
 */

#include <boost/test/unit_test.hpp>

#include <set>
#include <unordered_map>
#include <vector>

#include <sch_sheet.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>


/**
 * Shared fixture: a SCHEMATIC carrying a top sheet whose child
 * "Channel" can be configured with arbitrary repeat_count via
 * SetRepeat().  See test_repeated_sheet_hierarchy.cpp for the same
 * shape used by the R2 unit tests.
 */
struct SHEET_PATH_LIFETIME_FIXTURE
{
    SHEET_PATH_LIFETIME_FIXTURE() :
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

        topScreen->Append( m_channel );

        m_schematic.SetTopLevelSheets( { m_top } );
    }

    void SetRepeat( int aN )
    {
        m_channel->SetRepeatCount( aN );

        std::vector<KIID> slots;
        slots.reserve( aN - 1 );

        for( int i = 1; i < aN; ++i )
            slots.emplace_back();

        m_channel->SetRepeatInstances( slots );
    }

    /// Find a path in the current hierarchy that ends in a synthetic
    /// clone (i.e. one of slots 1..N-1).  Returns by-value copy so
    /// the caller can hold it across a subsequent refresh.
    SCH_SHEET_PATH CaptureSyntheticSlotPath()
    {
        for( const SCH_SHEET_PATH& path : m_schematic.Hierarchy() )
        {
            if( path.size() == 2 && path.Last()->IsSynthetic() )
                return path;
        }

        BOOST_FAIL( "no synthetic-clone path in hierarchy" );
        return {};
    }

    SCHEMATIC   m_schematic;
    SCH_SHEET*  m_top;
    SCH_SHEET*  m_channel;
    SCH_SCREEN* m_channelScreen;
};


BOOST_FIXTURE_TEST_SUITE( SheetPathLifetime, SHEET_PATH_LIFETIME_FIXTURE )


/**
 * THE SMOKING GUN.  Cache a SCH_SHEET_PATH whose Last() points at
 * a synthetic clone, force a RefreshHierarchy() cycle that frees the
 * clone (ClearRepeatCloneCache) and remints a new one at a different
 * address, then dereference the cached path.
 *
 * Pre-refactor: heap-use-after-free on Last()->IsSynthetic().
 * Post-refactor: Last(m_schematic) resolves by KIID and returns the
 * NEW clone for the same logical slot.
 *
 * This mirrors exactly the CONNECTION_SUBGRAPH::m_sheet UAF we
 * caught under ASan during the wire-stub multi-channel trigger.
 */
BOOST_AUTO_TEST_CASE( PathSurvivesRefreshHierarchyCycle )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET_PATH cached = CaptureSyntheticSlotPath();
    KIID            slotKiid = cached.Last()->m_Uuid;
    KIID            templateKiid = cached.Last()->GetTemplate()->m_Uuid;

    // Force ClearRepeatCloneCache() + remint at new addresses.  This
    // is exactly the sequence that fires from any SCH_COMMIT::Push
    // touching a multi-channel sheet's contents.
    m_schematic.RefreshHierarchy();

    // The cached path must still answer correctly when consulted via
    // the safe API.  Pre-refactor: cached.Last() returns a dangling
    // SCH_SHEET* and any deref is heap-use-after-free.  Post-refactor:
    // cached.LastInstance() reads from m_instances (which holds
    // identity by KIID, not by address) and the value remains valid.
    SCH_SHEET_INSTANCE last = cached.LastInstance();

    BOOST_CHECK( last.SlotKiid() == slotKiid  );
    BOOST_CHECK( last.TemplateKiid() == templateKiid  );
    BOOST_CHECK( !last.IsTemplateSlot() );  // synthetic slot K>0

    // The instance can be resolved against SCHEMATIC to recover a
    // live SCH_SHEET* for the freshly-minted clone of the same slot.
    SCH_SHEET* resolved = m_schematic.ResolveSheetTemplate( last );
    BOOST_CHECK( resolved != nullptr );
    BOOST_CHECK( resolved->m_Uuid == templateKiid  );
}


/**
 * Equivalent stress on the "path is a map key" pattern from
 * CONNECTION_GRAPH::m_sheet_to_subgraphs_map (connection_graph.h:847)
 * and friends.  Insert with one address generation; look up after
 * a remint cycle.  Pre-refactor: hash() may collide on KIID but
 * operator==() walks the pointer vector; the lookup either misses
 * silently (lost subgraph) or hits and dereferences a dangling
 * SCH_SHEET*.  Post-refactor: hash and equality are pure KIID
 * sequences, lookup is stable.
 */
BOOST_AUTO_TEST_CASE( StaleMapKeyAfterCacheClear )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    std::unordered_map<SCH_SHEET_PATH, int> map;

    int idx = 0;
    for( const SCH_SHEET_PATH& path : m_schematic.Hierarchy() )
    {
        SCH_SHEET_INSTANCE leaf = path.LastInstance();

        if( path.size() == 2 && !leaf.IsTemplateSlot() )
            map.emplace( path, idx++ );
    }

    const size_t before = map.size();
    BOOST_REQUIRE( before > 0 );

    // Force the cache cycle.
    m_schematic.RefreshHierarchy();

    // Re-look-up each synthetic-slot path in the FRESH hierarchy.
    // After the refactor every fresh path equals its pre-refresh
    // counterpart by KIID, so every lookup must hit.
    size_t hits = 0;

    for( const SCH_SHEET_PATH& fresh : m_schematic.Hierarchy() )
    {
        SCH_SHEET_INSTANCE leaf = fresh.LastInstance();

        if( fresh.size() == 2 && !leaf.IsTemplateSlot() )
        {
            if( map.find( fresh ) != map.end() )
                hits++;
        }
    }

    BOOST_CHECK_EQUAL( hits, before );
}


/**
 * Documents the proximate-fix invariant from R1: within a SINGLE
 * RefreshHierarchy() call, MintRepeatClone returns the SAME pointer
 * for the same (template, slotKIID) pair on subsequent walks (because
 * the clones are cached in m_repeatClones).  This is what the
 * proximate fixes around clone lifetime were quietly relying on.
 *
 * The structural refactor renders this invariant moot — consumers
 * no longer need pointer stability because they hold KIIDs — but
 * the test is preserved as a record of the contract R1 introduced,
 * and as a regression catch if the cache is ever re-engineered.
 */
BOOST_AUTO_TEST_CASE( ClonePointerStableWithinSingleRefresh )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    SCH_SHEET* firstLookup = nullptr;
    SCH_SHEET* secondLookup = nullptr;
    KIID slotKiid;

    for( const SCH_SHEET_PATH& path : m_schematic.Hierarchy() )
    {
        if( path.size() == 2 && path.Last()->IsSynthetic() )
        {
            firstLookup = path.Last();
            slotKiid = firstLookup->m_Uuid;
            break;
        }
    }

    BOOST_REQUIRE( firstLookup );

    for( const SCH_SHEET_PATH& path : m_schematic.Hierarchy() )
    {
        if( path.size() == 2 && path.Last()->IsSynthetic()
                && path.Last()->m_Uuid == slotKiid )
        {
            secondLookup = path.Last();
            break;
        }
    }

    BOOST_REQUIRE( secondLookup );
    BOOST_CHECK_EQUAL( firstLookup, secondLookup );
}


/**
 * Auditor C1: SCH_SHEET_PATH::operator<() currently delegates to
 * `m_sheets < d1.m_sheets`, a pointer-address-based lex compare.
 * After ClearRepeatCloneCache + remint, the synthetic-clone
 * pointers change.  Any container using default operator<
 * (std::set<SCH_SHEET_PATH>, std::map<SCH_SHEET_PATH, ...>) may
 * therefore land different paths in different positions across
 * refresh cycles.  Worse, the *current* ordering is itself
 * undefined-but-deterministic (per-allocator-run), so changes
 * are silent and tests don't catch them.
 *
 * This test pins the requirement: iteration order over the same
 * logical set of paths must be KIID-stable across refreshes.
 *
 * Pre-refactor: operator< is address-based; order shifts.
 * Post-refactor: operator< delegates to Cmp() (KIID-lex); stable.
 */
BOOST_AUTO_TEST_CASE( OrderingStableAcrossRefresh )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    // Capture the KIID sequences (by-value) of every synthetic
    // path in iteration order on a std::set<SCH_SHEET_PATH>.
    auto captureKiidSequences = [&]() {
        std::set<SCH_SHEET_PATH> ordered;

        for( const SCH_SHEET_PATH& p : m_schematic.Hierarchy() )
        {
            if( p.size() == 2 && p.Last()->IsSynthetic() )
                ordered.insert( p );
        }

        std::vector<std::vector<KIID>> kiidSeqs;
        for( const SCH_SHEET_PATH& p : ordered )
        {
            std::vector<KIID> seq;
            for( size_t i = 0; i < p.size(); ++i )
                seq.push_back( p.GetSheet( i )->m_Uuid );
            kiidSeqs.push_back( std::move( seq ) );
        }
        return kiidSeqs;
    };

    auto before = captureKiidSequences();
    BOOST_REQUIRE_EQUAL( before.size(), 3u );   // slots 1..3 are synthetic

    // Force the clone churn.
    m_schematic.RefreshHierarchy();

    auto after = captureKiidSequences();
    BOOST_REQUIRE_EQUAL( after.size(), 3u );

    // BOOST_CHECK_EQUAL_COLLECTIONS would need operator<< on the
    // element type (std::vector<KIID>) — assert by-value equality
    // directly, with a count check first so the message is meaningful.
    BOOST_REQUIRE_EQUAL( before.size(), after.size() );
    BOOST_CHECK( before == after );
}


/**
 * Auditor I3: SCH_SHEET_PATH::SetPageNumber routes through
 * `sheet->addInstance(...)` then `sheet->setPageNumber(...)` on the
 * sheet returned by Last() — which for a synthetic slot is a clone
 * that gets freed by the next ClearRepeatCloneCache().  Page-number
 * writes on multi-channel slots are silently lost.
 *
 * Pre-refactor: GetPageNumber returns empty after refresh.
 * Post-P5 (lifetime tightening / instance-data on SCHEMATIC):
 * GetPageNumber returns the previously-set value.
 *
 * This is the structural lifetime invariant the refactor must
 * preserve: per-instance data outlives any single refresh cycle
 * because it lives on SCHEMATIC keyed by (template_KIID, slot_KIID),
 * not on a freeable clone.
 */
BOOST_AUTO_TEST_CASE( PageNumberSurvivesRefresh )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    // Find slot 2's path (an arbitrary synthetic slot) and record
    // its slot KIID so we can re-find it after refresh.
    KIID slotKiid;
    {
        SCH_SHEET_PATH path = CaptureSyntheticSlotPath();
        path.SetPageNumber( "99" );
        slotKiid = path.Last()->m_Uuid;
    }

    m_schematic.RefreshHierarchy();

    // Re-find the same logical slot in the FRESH hierarchy.
    SCH_SHEET_PATH found;
    bool           gotPath = false;

    for( const SCH_SHEET_PATH& p : m_schematic.Hierarchy() )
    {
        if( p.size() == 2 && p.Last()->IsSynthetic()
                && p.Last()->m_Uuid == slotKiid )
        {
            found = p;
            gotPath = true;
            break;
        }
    }

    BOOST_REQUIRE( gotPath );
    BOOST_CHECK_EQUAL( found.GetPageNumber(), wxString( "99" ) );
}


/**
 * Round-trip on the CONNECTION_GRAPH map pattern.  Auditor W4: the
 * UAF caught under ASan was specifically on a CONNECTION_SUBGRAPH's
 * cached SCH_SHEET_PATH being dereferenced after the next refresh.
 * This test approximates that by storing paths as values in a
 * long-lived container, forcing the refresh cycle, then reading
 * back via a fresh-hierarchy lookup.
 *
 * Pre-refactor: the container holds paths whose m_sheets pointers
 * dangle.  Iteration that calls .Last() is UAF.
 * Post-refactor: the container holds paths whose KIID storage is
 * stable.  Iteration resolves to fresh SCH_SHEETs via SCHEMATIC.
 */
BOOST_AUTO_TEST_CASE( ConnectionMapPathReadsSurviveRefresh )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    // Long-lived container holding paths by value, just like
    // CONNECTION_GRAPH::m_sheet_to_subgraphs_map.
    std::unordered_map<SCH_SHEET_PATH, int> longLived;

    int idx = 0;
    for( const SCH_SHEET_PATH& p : m_schematic.Hierarchy() )
        longLived.emplace( p, idx++ );

    const size_t beforeSize = longLived.size();
    BOOST_REQUIRE( beforeSize > 0 );

    // Force the clone churn.
    m_schematic.RefreshHierarchy();

    // Iterate the long-lived container via the SAFE API
    // (LastInstance / ResolveSheetTemplate).  Pre-refactor: every
    // call to p.Last() through the stored path was UAF on the
    // synthetic-clone entries.  Post-refactor: LastInstance reads
    // from m_instances (KIID-valued), and resolution against the
    // live SCHEMATIC returns either a fresh clone or the template,
    // never a dangling pointer.
    size_t readable = 0;

    for( const auto& [p, _idx] : longLived )
    {
        SCH_SHEET_INSTANCE inst = p.LastInstance();

        if( m_schematic.ResolveSheetTemplate( inst ) )
            readable++;
    }

    BOOST_CHECK_EQUAL( readable, beforeSize );
}


/**
 * P4 additive API: LastInstance() / GetInstance(i) extract
 * SCH_SHEET_INSTANCE values from the current m_sheets storage.
 *
 * For a non-synthetic sheet: template_kiid == slot_kiid == sheet's
 * own m_Uuid.  For a synthetic-clone slot path: template_kiid is
 * the on-canvas template's m_Uuid, slot_kiid is the clone's
 * (drawn from the template's m_repeatInstances).
 */
BOOST_AUTO_TEST_CASE( LastInstanceSplitsTemplateFromSlot )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    int slotZeroFound = 0;
    int slotKFound    = 0;

    for( const SCH_SHEET_PATH& p : m_schematic.Hierarchy() )
    {
        if( p.size() != 2 )
            continue;

        SCH_SHEET*         leaf = p.Last();
        SCH_SHEET_INSTANCE inst = p.LastInstance();

        BOOST_REQUIRE( leaf );

        if( leaf->IsSynthetic() )
        {
            // Slot K > 0: clone's m_Uuid is the slot_kiid; the
            // template's m_Uuid is the template_kiid.
            BOOST_CHECK( inst.SlotKiid() == leaf->m_Uuid  );
            BOOST_CHECK( inst.TemplateKiid() == leaf->GetTemplate()->m_Uuid );
            BOOST_CHECK( !inst.IsTemplateSlot() );
            slotKFound++;
        }
        else if( leaf == m_channel )
        {
            // Slot 0: sheet IS the template; both KIIDs equal.
            BOOST_CHECK( inst.SlotKiid() == leaf->m_Uuid  );
            BOOST_CHECK( inst.TemplateKiid() == leaf->m_Uuid  );
            BOOST_CHECK( inst.IsTemplateSlot() );
            slotZeroFound++;
        }
    }

    BOOST_CHECK_EQUAL( slotZeroFound, 1 );
    BOOST_CHECK_EQUAL( slotKFound,    3 );   // slots 1..3 are synthetic
}


/**
 * P4: GetInstance(i) at each position along a path returns the
 * SCH_SHEET_INSTANCE that names that step.
 */
BOOST_AUTO_TEST_CASE( GetInstanceWalksThePath )
{
    SetRepeat( 4 );
    m_schematic.RefreshHierarchy();

    for( const SCH_SHEET_PATH& p : m_schematic.Hierarchy() )
    {
        BOOST_CHECK( p.GetInstance( p.size() )      == SCH_SHEET_INSTANCE() );
        BOOST_CHECK( p.GetInstance( p.size() + 7 )  == SCH_SHEET_INSTANCE() );

        for( size_t i = 0; i < p.size(); ++i )
        {
            SCH_SHEET*         step = p.GetSheet( i );
            SCH_SHEET_INSTANCE inst = p.GetInstance( i );

            BOOST_REQUIRE( step );
            BOOST_CHECK( inst.SlotKiid() == step->m_Uuid  );
            BOOST_CHECK( inst.TemplateKiid() == ( step->GetTemplate()
                                                          ? step->GetTemplate()->m_Uuid
                                                          : step->m_Uuid ) );
        }
    }
}


BOOST_AUTO_TEST_SUITE_END()
