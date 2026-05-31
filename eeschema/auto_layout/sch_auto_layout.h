/**
 * @file sch_auto_layout.h
 *
 * @brief High-level entry point for the port-aware auto-layout
 *        pipeline.  Thin convenience over SchOgdfAdapter for the
 *        common "lay out an open schematic" case.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sch_ogdf_adapter.h"

class SCH_SCREEN;

namespace klicad::auto_layout {

/**
 * @brief Run the full pipeline on @p aScreen and return the
 *        layout report.
 *
 * Equivalent to:
 *
 *     SchOgdfAdapter adapter( aScreen );
 *     return adapter.run();
 *
 * Provided as a free function for the IPC binding's convenience
 * — no state to manage when the caller doesn't need to inspect
 * intermediate OGDF graph state.
 */
LayoutReport runAutoLayout( SCH_SCREEN& aScreen );

}  // namespace klicad::auto_layout
