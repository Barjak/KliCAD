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
 * @file test_sch_sheet_instance.cpp
 *
 * Value-semantics unit tests for SCH_SHEET_INSTANCE.  The new type
 * is introduced as the per-path identity that replaces raw
 * SCH_SHEET* in SCH_SHEET_PATH storage.  Before adding any
 * consumers (P3 onward), pin down its invariants here:
 *
 *   - copyability, equality, ordering, hashability
 *   - lex order is over (template_kiid, slot_kiid) — stable across
 *     any SCH_SHEET pointer churn (this is the whole point)
 *   - usable as std::set and std::unordered_map key with default
 *     comparator + std::hash specialization
 *   - OfTemplate factory produces an instance whose template_kiid
 *     equals slot_kiid, identifying slot 0 of the template
 */

#include <boost/test/unit_test.hpp>

#include <set>
#include <unordered_map>
#include <vector>

#include <kiid.h>
#include <sch_sheet_instance.h>


BOOST_AUTO_TEST_SUITE( SchSheetInstance )


BOOST_AUTO_TEST_CASE( DefaultConstructorYieldsTwoNilKiids )
{
    SCH_SHEET_INSTANCE inst;

    BOOST_CHECK_EQUAL( inst.TemplateKiid(), KIID( 0 ) );
    BOOST_CHECK_EQUAL( inst.SlotKiid(),     KIID( 0 ) );
    BOOST_CHECK( inst.IsTemplateSlot() );  // niluuid == niluuid
}


BOOST_AUTO_TEST_CASE( ConstructorRetainsBothKiids )
{
    KIID t;
    KIID s;

    SCH_SHEET_INSTANCE inst( t, s );

    BOOST_CHECK_EQUAL( inst.TemplateKiid(), t );
    BOOST_CHECK_EQUAL( inst.SlotKiid(),     s );
}


BOOST_AUTO_TEST_CASE( OfTemplateFactorySetsBothToSame )
{
    KIID t;

    SCH_SHEET_INSTANCE inst = SCH_SHEET_INSTANCE::OfTemplate( t );

    BOOST_CHECK_EQUAL( inst.TemplateKiid(), t );
    BOOST_CHECK_EQUAL( inst.SlotKiid(),     t );
    BOOST_CHECK( inst.IsTemplateSlot() );
}


BOOST_AUTO_TEST_CASE( IsTemplateSlotDistinguishesSlotZero )
{
    KIID t;
    KIID s;   // different KIID

    SCH_SHEET_INSTANCE slot0( t, t );
    SCH_SHEET_INSTANCE slotK( t, s );

    BOOST_CHECK( slot0.IsTemplateSlot() );
    BOOST_CHECK( !slotK.IsTemplateSlot() );
}


BOOST_AUTO_TEST_CASE( Equality )
{
    KIID a;
    KIID b;

    SCH_SHEET_INSTANCE x( a, b );
    SCH_SHEET_INSTANCE y( a, b );
    SCH_SHEET_INSTANCE z( b, a );

    BOOST_CHECK( x == y );
    BOOST_CHECK( !( x != y ) );
    BOOST_CHECK( x != z );
}


BOOST_AUTO_TEST_CASE( CopyAndAssignAreValueSemantics )
{
    KIID a, b;

    SCH_SHEET_INSTANCE src( a, b );
    SCH_SHEET_INSTANCE copy = src;
    SCH_SHEET_INSTANCE assigned;
    assigned = src;

    BOOST_CHECK( src == copy );
    BOOST_CHECK( src == assigned );

    // Mutating the source value would require setters; type has
    // none.  The instance is effectively immutable after
    // construction — assert this property indirectly by replacing
    // the value via assignment from a different source.
    KIID c, d;
    SCH_SHEET_INSTANCE other( c, d );
    copy = other;

    BOOST_CHECK( copy == other );
    BOOST_CHECK( src != copy );   // original unchanged
}


BOOST_AUTO_TEST_CASE( LexOrderIsTotalAndKiidBased )
{
    // Construct three KIIDs with known ordering by sorting them.
    std::vector<KIID> ids = { KIID(), KIID(), KIID() };
    std::sort( ids.begin(), ids.end() );

    const KIID& low  = ids[0];
    const KIID& mid  = ids[1];
    const KIID& high = ids[2];

    SCH_SHEET_INSTANCE a( low,  low  );
    SCH_SHEET_INSTANCE b( low,  high );
    SCH_SHEET_INSTANCE c( mid,  low  );

    // Lex by (template, slot): a < b (same template, lower slot)
    BOOST_CHECK( a < b );
    BOOST_CHECK( !( b < a ) );

    // Lex by (template, slot): b < c (lower template wins regardless
    // of slot; b's template == low, c's template == mid > low)
    BOOST_CHECK( b < c );
    BOOST_CHECK( !( c < b ) );

    // Transitivity sanity
    BOOST_CHECK( a < c );
    BOOST_CHECK( !( c < a ) );
}


BOOST_AUTO_TEST_CASE( UsableAsStdSetKey )
{
    KIID t1, t2;

    SCH_SHEET_INSTANCE a( t1, t1 );
    SCH_SHEET_INSTANCE b( t1, t2 );
    SCH_SHEET_INSTANCE c( t2, t1 );

    std::set<SCH_SHEET_INSTANCE> s;
    s.insert( a );
    s.insert( b );
    s.insert( c );
    s.insert( a );   // duplicate; should be deduped

    BOOST_CHECK_EQUAL( s.size(), 3u );
    BOOST_CHECK( s.count( a ) == 1u );
    BOOST_CHECK( s.count( b ) == 1u );
    BOOST_CHECK( s.count( c ) == 1u );

    SCH_SHEET_INSTANCE notInserted( t2, t2 );
    BOOST_CHECK( s.count( notInserted ) == 0u );
}


BOOST_AUTO_TEST_CASE( UsableAsStdUnorderedMapKey )
{
    KIID t1, t2;

    SCH_SHEET_INSTANCE a( t1, t1 );
    SCH_SHEET_INSTANCE b( t1, t2 );

    std::unordered_map<SCH_SHEET_INSTANCE, int> m;
    m[a] = 1;
    m[b] = 2;

    BOOST_CHECK_EQUAL( m[a], 1 );
    BOOST_CHECK_EQUAL( m[b], 2 );
    BOOST_CHECK_EQUAL( m.size(), 2u );
}


BOOST_AUTO_TEST_CASE( HashIsStableAcrossEqualInstances )
{
    KIID t, s;

    SCH_SHEET_INSTANCE x( t, s );
    SCH_SHEET_INSTANCE y( t, s );  // equivalent value

    std::hash<SCH_SHEET_INSTANCE> hasher;

    BOOST_CHECK_EQUAL( hasher( x ), hasher( y ) );
}


BOOST_AUTO_TEST_CASE( HashSeparatesDistinctInstances )
{
    KIID a, b;

    SCH_SHEET_INSTANCE x( a, a );
    SCH_SHEET_INSTANCE y( a, b );
    SCH_SHEET_INSTANCE z( b, a );

    std::hash<SCH_SHEET_INSTANCE> hasher;

    // Different value → different hash (with overwhelming probability;
    // KIID is a UUID, collisions are cryptographically rare).  These
    // are stronger than the std::hash contract requires, but the
    // contract still permits this assertion for distinct UUID-derived
    // inputs.
    BOOST_CHECK( hasher( x ) != hasher( y ) );
    BOOST_CHECK( hasher( x ) != hasher( z ) );
    BOOST_CHECK( hasher( y ) != hasher( z ) );
}


BOOST_AUTO_TEST_SUITE_END()
