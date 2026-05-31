# Port-constrained auto-layout adapter — see
# ~/projects/KliCAD_development/research/c5-adapter-design.md.
#
# Builds a small static lib that the eeschema kiface links.  All
# OGDF symbols are encapsulated in this directory; eeschema code
# outside auto_layout/ doesn't see the OGDF API.

add_library( klicad_auto_layout STATIC
    sch_ogdf_adapter.cpp
    sch_auto_layout.cpp
    )

target_include_directories( klicad_auto_layout PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    )

# OGDF: forked at ../../../ogdf with our port-constraint extensions
# (branch feature/port-constraints).  Build it as a sibling
# subdirectory so headers + the libOGDF target are visible.
#
# The actual add_subdirectory call lives in the top-level
# KliCAD CMakeLists.txt to avoid double-include if anything else
# in the eeschema tree later needs OGDF.

target_link_libraries( klicad_auto_layout PUBLIC
    OGDF
    eeschema_kiface_objects   # for SCH_SCREEN etc. headers; check exact target name in top CMakeLists
    )

# ASan flags inherited from top-level CFLAGS so the static lib
# matches the kiface's runtime; no special handling needed here
# as long as the top build is configured with -fsanitize=address.
