/**
 * @file layout_report.h
 *
 * Return-shape of the schematic auto-layout adapter.  Lives in its own
 * header so the OGDF subtree (`sch_ogdf_adapter.{h,cpp}`) can be
 * excised cleanly per GOAL.md M5.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace klicad::auto_layout {

struct LayoutReport
{
    bool   ok               = false;
    int    symbols_placed   = 0;
    int    wires_emitted    = 0;
    int    crossings        = 0;
    int    bends            = 0;
    double total_wirelength = 0.0;
};

}  // namespace klicad::auto_layout
