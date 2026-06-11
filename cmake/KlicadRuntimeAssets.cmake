# KliCAD runtime-asset auto-discovery — see
# ~/projects/KliCAD_development/GOAL.md F-B2.
#
# The kicad launcher loads several runtime assets at startup that are
# NOT linked into the binary: every *_kiface MODULE, the schema .json
# files copied into ${CMAKE_BINARY_DIR}/schemas/, the bitmap archive,
# etc.  Building `kicad` via `make kicad` (or any sub-target traversal
# that pulls kicad in) must produce these alongside the launcher, or
# the GUI pops a startup modal that hangs IPC.
#
# History: a hand-curated `add_dependencies( kicad <list> )` block at
# kicad/CMakeLists.txt:39-47 fixed the 2026-05-31 episode, but missed
# the PCM `schema_build_copy` target — recurrence on 2026-06-02.  A
# hand-curated list is a structural-fix-shaped lookup table that goes
# stale.  This module replaces it with an auto-registration mechanism.
#
# Usage in subdirectory CMakeLists.txt:
#
#     add_custom_target( my_schema_copy ALL ... )
#     klicad_register_runtime_asset( my_schema_copy )
#
# Usage in kicad/CMakeLists.txt (called exactly once, after all
# subdirectories have been added):
#
#     klicad_apply_runtime_asset_deps( kicad )
#
# Invariant: any new runtime-loaded asset is registered at its
# definition site.  `grep -rn klicad_register_runtime_asset
# CMakeLists.txt` enumerates every runtime asset in the tree — the
# auditable inventory.

# Global property carrying the list of registered runtime-asset
# targets.  Subdirectories append; the kicad target reads.
define_property( GLOBAL
    PROPERTY KLICAD_RUNTIME_ASSETS
    BRIEF_DOCS "Targets whose outputs the kicad launcher loads at runtime."
    FULL_DOCS  "Auto-collected list of runtime-asset targets.  Each \
entry is the name of an add_library / add_custom_target whose \
output is loaded by the running kicad binary at startup (kifaces, \
schema-json copies, bitmap archives).  Consumed by \
klicad_apply_runtime_asset_deps() to wire add_dependencies() on \
the kicad exe in one place." )

function( klicad_register_runtime_asset target_name )
    if( NOT TARGET ${target_name} )
        message( WARNING
            "klicad_register_runtime_asset: '${target_name}' is not a "
            "known target at call site — registration skipped." )
        return()
    endif()
    set_property( GLOBAL APPEND PROPERTY KLICAD_RUNTIME_ASSETS ${target_name} )
endfunction()

function( klicad_apply_runtime_asset_deps exe_target )
    if( NOT TARGET ${exe_target} )
        message( FATAL_ERROR
            "klicad_apply_runtime_asset_deps: '${exe_target}' is not a "
            "known target.  Call must come after add_executable()." )
    endif()

    get_property( assets GLOBAL PROPERTY KLICAD_RUNTIME_ASSETS )
    if( NOT assets )
        message( WARNING
            "klicad_apply_runtime_asset_deps: KLICAD_RUNTIME_ASSETS "
            "property is empty.  Either no subdirectory has called "
            "klicad_register_runtime_asset, or this function ran "
            "before the registering subdirectories were added." )
        return()
    endif()

    # Dedupe defensively (a target registered twice would otherwise
    # appear twice in the dep list — harmless but noisy).
    list( REMOVE_DUPLICATES assets )

    add_dependencies( ${exe_target} ${assets} )

    # Emit the discovered list at configure time so build-tree
    # consumers (the launcher's pre-flight asset list, F-B3; future
    # CI audits) can read it.  Single source of truth.
    set( asset_list_file "${CMAKE_BINARY_DIR}/klicad-runtime-assets.list" )
    string( REPLACE ";" "\n" asset_list_body "${assets}" )
    file( WRITE "${asset_list_file}" "${asset_list_body}\n" )

    message( STATUS
        "KliCAD runtime assets bound to ${exe_target}: ${assets}" )
endfunction()
