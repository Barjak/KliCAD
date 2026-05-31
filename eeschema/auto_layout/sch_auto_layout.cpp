/**
 * @file sch_auto_layout.cpp
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sch_auto_layout.h"

namespace klicad::auto_layout {

LayoutReport runAutoLayout( SCH_SCREEN& aScreen )
{
	SchOgdfAdapter adapter( aScreen );
	return adapter.run();
}

}  // namespace klicad::auto_layout
