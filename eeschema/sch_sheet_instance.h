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
 * @file sch_sheet_instance.h
 *
 * `SCH_SHEET_INSTANCE` is the per-path value identifying ONE appearance
 * of a sheet in a hierarchy.  It is a pure value type with no owned
 * resources, no back-pointers, and no lifetime entanglement with any
 * other object in the schematic.
 *
 * Motivation: prior to the v3 refactor (see commit a31aae36a5's
 * regression suite), `SCH_SHEET` was doing two jobs.  As a *template*
 * it represented the on-canvas sheet the user drew — name, filename,
 * screen, position.  As an *instance* it represented a particular
 * appearance in a hierarchy path, which for multi-channel slots
 * required materializing a synthetic clone owned by
 * `SCHEMATIC::m_repeatClones`.  That cache was freed on every
 * `RefreshHierarchy` (`ClearRepeatCloneCache`) and re-minted at new
 * addresses, leaving every long-lived `SCH_SHEET_PATH` consumer
 * holding dangling pointers.
 *
 * Separating *template* from *instance* into two distinct types lets
 * each type tell the truth about its lifetime:
 *
 *   SCH_SHEET           — owned by its parent screen; stable for the
 *                         schematic's lifetime; pointer identity is
 *                         meaningful.
 *
 *   SCH_SHEET_INSTANCE  — pure value (`KIID template_kiid, KIID
 *                         slot_kiid`); copyable, hashable, totally
 *                         orderable; never owns memory; never
 *                         dangles.  Resolves to a `SCH_SHEET*` on
 *                         demand against a live `SCHEMATIC`.
 *
 * `SCH_SHEET_PATH` is `std::vector<SCH_SHEET_INSTANCE>`.  Long-lived
 * holders (CONNECTION_SUBGRAPH::m_sheet, SCH_REFERENCE::m_sheetPath,
 * the connection-map keys, etc.) survive arbitrary churn of the
 * underlying SCH_SHEET storage because identity is encoded by KIID,
 * not by address.
 *
 * Identity contract:
 *
 *   template_kiid  — the KIID of the on-canvas SCH_SHEET whose
 *                    screen is this instance's body.  Resolves
 *                    against SCHEMATIC's hierarchy.
 *
 *   slot_kiid      — distinguishes one of N peer appearances when
 *                    the template's `repeat_count > 1`.  For slot 0
 *                    of a single-instance sheet (the common case),
 *                    `slot_kiid == template_kiid`.  For slot 0 of a
 *                    multi-channel sheet, `slot_kiid == template_kiid`
 *                    as well (slot 0 is "the template's own
 *                    instance").  For slots 1..N-1 of a multi-channel
 *                    sheet, `slot_kiid` is drawn from the template's
 *                    `m_repeatInstances` vector.  Both KIIDs are
 *                    persisted in the .kicad_sch file format.
 */

#ifndef SCH_SHEET_INSTANCE_H
#define SCH_SHEET_INSTANCE_H

#include <functional>

#include <kiid.h>


/**
 * Per-path appearance of a sheet in the schematic hierarchy.
 *
 * Value type: hashable, totally orderable by `(template_kiid,
 * slot_kiid)` lex, trivially copyable.  Holds no SCH_SHEET pointer
 * because every consumer that wants one must resolve it against a
 * live SCHEMATIC, which closes the dangling-pointer class.
 */
class SCH_SHEET_INSTANCE
{
public:
    SCH_SHEET_INSTANCE() = default;

    SCH_SHEET_INSTANCE( const KIID& aTemplateKiid, const KIID& aSlotKiid ) :
            m_template_kiid( aTemplateKiid ),
            m_slot_kiid( aSlotKiid )
    {
    }

    /**
     * Construct an instance whose template_kiid and slot_kiid are
     * the same KIID — i.e. slot 0 of a non-multi-channel or of the
     * "first slot" of a multi-channel sheet.
     */
    static SCH_SHEET_INSTANCE OfTemplate( const KIID& aTemplateKiid )
    {
        return SCH_SHEET_INSTANCE( aTemplateKiid, aTemplateKiid );
    }

    const KIID& TemplateKiid() const { return m_template_kiid; }
    const KIID& SlotKiid()     const { return m_slot_kiid; }

    /**
     * @return true if this instance is slot 0 of its template
     *         (i.e. the instance whose slot_kiid equals its
     *         template_kiid).  Multi-channel templates have one
     *         slot-0 and N-1 non-slot-0 instances; non-multi-
     *         channel templates have only the slot-0 instance.
     */
    bool IsTemplateSlot() const { return m_template_kiid == m_slot_kiid; }

    bool operator==( const SCH_SHEET_INSTANCE& aOther ) const
    {
        return m_template_kiid == aOther.m_template_kiid
                && m_slot_kiid == aOther.m_slot_kiid;
    }

    bool operator!=( const SCH_SHEET_INSTANCE& aOther ) const
    {
        return !( *this == aOther );
    }

    /// Lex over (template_kiid, slot_kiid).  Stable across any
    /// SCH_SHEET pointer churn — see the lifetime contract above.
    bool operator<( const SCH_SHEET_INSTANCE& aOther ) const
    {
        if( m_template_kiid < aOther.m_template_kiid )
            return true;

        if( aOther.m_template_kiid < m_template_kiid )
            return false;

        return m_slot_kiid < aOther.m_slot_kiid;
    }

private:
    KIID m_template_kiid;
    KIID m_slot_kiid;
};


/// std::hash specialization so SCH_SHEET_INSTANCE can key unordered
/// containers.  Combines template and slot KIID hashes via the
/// standard boost-style XOR-shift mix.
namespace std
{
template<>
struct hash<SCH_SHEET_INSTANCE>
{
    std::size_t operator()( const SCH_SHEET_INSTANCE& aInstance ) const noexcept
    {
        std::size_t h = std::hash<KIID>{}( aInstance.TemplateKiid() );
        h ^= std::hash<KIID>{}( aInstance.SlotKiid() )
                + 0x9e3779b97f4a7c15ULL + ( h << 6 ) + ( h >> 2 );
        return h;
    }
};
} // namespace std


#endif // SCH_SHEET_INSTANCE_H
