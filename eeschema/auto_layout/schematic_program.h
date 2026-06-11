/**
 * @file schematic_program.h
 *
 * Typed IPC carrier for klicad-python's compose_schematic flow
 * (GOAL.md F-S3).  Pure data — no methods, no pybind11 macros
 * here; the bindings live in eeschema/api/bindings_schematic_compose.cpp.
 *
 * Mirrors klipy.circuit._compose.SchematicProgram field-for-field.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <map>
#include <string>
#include <vector>

namespace klicad::auto_layout {

enum class ProgramNetKind
{
    SIGNAL,
    POWER,
    HIER_PORT,
    GROUND,
};

enum class ProgramMode
{
    REPLACE,
    DIFF,
    STRICT,
};


/** One part to instantiate as an SCH_SYMBOL.  No positions; ELK
 *  decides.  Field setting is data-driven via extra_fields. */
struct ProgramPart
{
    std::string ref;                                       ///< e.g. "R1"
    std::string lib_id;                                    ///< e.g. "Device:R"
    std::string value;                                     ///< e.g. "10k"
    std::string footprint;                                 ///< KiCad fp hint, may be ""
    std::map<std::string, std::string> kicad_pin_map;      ///< port_name → kicad_pin_number
    std::map<std::string, std::string> connections;        ///< port_name → net_name
    std::map<std::string, std::string> extra_fields;       ///< Sim.* / Klicad.SpecSrc / ...
};


/** One sheet instance.  H2 + R5 multi-channel are carried here via
 *  repeat_count + repeat_instances. */
struct ProgramSheet
{
    std::string ref;                                       ///< e.g. "U_amp1"
    std::string definition_filename;                       ///< child .kicad_sch basename
    std::map<std::string, std::string> port_map;           ///< port_name → parent net
    int repeat_count = 1;                                  ///< R5 multi-channel
    std::vector<std::string> repeat_instances;             ///< pre-existing slot KIIDs
    std::map<std::string, std::string> extra_fields;
};


/** One declared net.  `kind` controls whether the C++ adapter places
 *  a power symbol (POWER / GROUND) or emits a label per the F-S3
 *  per-named-net policy. */
struct ProgramNet
{
    std::string    name;
    ProgramNetKind kind = ProgramNetKind::SIGNAL;
    bool           expect_external = false;
};


/** Whole-schematic program shipped from klicad-python to KliCAD via
 *  klicad_native_schematic_compose.compose(sch_path, program). */
struct SchematicProgram
{
    std::string                sch_path;       ///< absolute path of .kicad_sch
    ProgramMode                mode = ProgramMode::DIFF;
    std::vector<ProgramPart>   parts;
    std::vector<ProgramSheet>  sheets;
    std::vector<ProgramNet>    nets;
};


/** Return shape of compose().  Counters surface to klicad-python so
 *  the caller can sanity-check what happened. */
struct ComposeReport
{
    bool        ok                 = false;
    std::string error;                              ///< empty on ok
    int         parts_placed       = 0;
    int         parts_kept         = 0;
    int         parts_removed      = 0;
    int         sheets_placed      = 0;
    int         sheets_kept        = 0;
    int         sheets_removed     = 0;
    int         labels_placed      = 0;
    int         wires_emitted      = 0;
    int         crossings          = 0;
    int         bends              = 0;
    double      total_wirelength   = 0.0;
};

}  // namespace klicad::auto_layout
