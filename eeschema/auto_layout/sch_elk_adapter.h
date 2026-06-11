/**
 * @file sch_elk_adapter.h
 *
 * @brief Adapter from KliCAD's SCH_SCREEN to the JVM-free C++ ELK
 *        Layered library at ~/projects/elk-cpp/.
 *
 * This adapter is the new layout backend tracked by GOAL.md gates
 * 1-5; it replaces sch_ogdf_adapter as gates are met.  The shape
 * mirrors the OGDF adapter (SCH_SCREEN → graph → layout → SCH_LINE
 * writeback) so callers can A/B test backends.
 *
 * Gate 1 (link gate) state: this header just declares the entry
 * point and a stub adapter.  No layout logic yet — the goal at
 * gate 1 is only to prove `libelk_core.a` links into the eeschema
 * kiface and that `#include "elk/elk.h"` resolves.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

class SCH_EDIT_FRAME;

namespace klicad::auto_layout {

struct LayoutReport;
class SchLayoutTransaction;

/**
 * Run ELK Layered into an existing transaction.  GOAL.md F-S3 +
 * Fork 5 (a): the layout adapter does not own a transaction; this
 * is the canonical (and now sole) entry point used by `compose()`.
 * The caller commits.
 */
LayoutReport runElkLayout( SchLayoutTransaction& aTxn );

}  // namespace klicad::auto_layout
