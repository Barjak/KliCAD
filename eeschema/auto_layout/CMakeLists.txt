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

target_include_directories( klicad_auto_layout
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/eeschema
    )

# OGDF target is created by the add_subdirectory at top-level
# (KliCAD CMakeLists.txt).  PUBLIC so eeschema_kiface_objects
# transitively gets the OGDF include path when it links us.
#
# common: KliCAD core library carrying transitive include paths
# for math/, kimath/, thirdparty/nlohmann_json, dynamic_bitset,
# and the rest of KliCAD's vendored headers.  PRIVATE link —
# klicad_auto_layout's public header uses forward declarations of
# SCH_SCREEN/SCH_SYMBOL/SCH_PIN/LIB_SYMBOL only; the .cpp
# implementations are the only TUs that pull in
# <sch_screen.h>/<sch_symbol.h> and need the full transitive
# include set.
target_link_libraries( klicad_auto_layout
    PUBLIC  OGDF
    PRIVATE common
    )

# Use the same C++ settings + warning suppressions as eeschema.
# wxWidgets headers come transitively via eeschema's compile
# environment; if the library ever needs wx directly, add
# ${wxWidgets_INCLUDE_DIRS} to target_include_directories.
