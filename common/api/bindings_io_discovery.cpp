/*
 * KliCAD subsystem binding: IO plugin discovery.
 *
 * Module: klicad_native_io_discovery
 *
 *   list_sch_formats()              -> [dict, ...]
 *   list_pcb_formats()              -> [dict, ...]
 *   list_all_formats()              -> [dict, ...]
 *   format_for_extension(ext)       -> dict | None
 *
 * Each dict carries:
 *   {
 *     name:        str        (matches SCH_IO_MGR::ShowType / PCB_IO_MGR::ShowType)
 *     enum_value:  int        (corresponding SCH_FILE_T or PCB_FILE_T enum integer)
 *     enum_name:   str        (the C++ enumerator spelling, eg "SCH_KICAD" / "KICAD_SEXP")
 *     extensions:  list[str]  (canonical file extensions, lowercase-as-declared)
 *     can_read:    bool
 *     can_write:   bool
 *     kind:        str        ("sch" or "pcb")
 *   }
 *
 * Implementation note — option (b) from the brief:
 *
 *   The authoritative enumerations live in
 *     eeschema/sch_io/sch_io_mgr.h   (SCH_IO_MGR::SCH_FILE_T)
 *     pcbnew/pcb_io/pcb_io_mgr.h     (PCB_IO_MGR::PCB_FILE_T)
 *   together with the per-plugin GetSchematicFileDesc() / GetBoardFileDesc()
 *   methods returning IO_BASE::IO_FILE_DESC with extension lists.
 *
 *   Those headers live INSIDE the respective kiface trees and are NOT on
 *   libkicommon's include path — pulling them in from common/ would either
 *   require new include directories or, worse, link references to symbols
 *   that only resolve inside a kiface (SCH_IO* / PCB_IO* concrete plugins).
 *
 *   This binding is Pattern A (libkicommon-resident, available before any
 *   kiface is loaded) — so we cannot depend on kiface symbols.  We therefore
 *   hand-curate the enumeration here, mirroring the upstream tables.  The
 *   trade-off: if a new IO plugin is added upstream, this file needs an
 *   entry.  In exchange we get a discovery API that works the instant the
 *   Python interpreter comes up, with no need to spawn the schematic or PCB
 *   editor frames first.
 *
 *   Sources of truth at time of writing:
 *     - SCH_IO_MGR::SCH_FILE_T enumerator order: eeschema/sch_io/sch_io_mgr.h
 *     - SCH_IO_MGR::ShowType() strings:          eeschema/sch_io/sch_io_mgr.cpp
 *     - Schematic extensions:                    eeschema/sch_io/<plugin>/sch_io_*.h
 *                                                (GetSchematicFileDesc / GetLibraryDesc)
 *     - PCB_IO_MGR::PCB_FILE_T enumerator order: pcbnew/pcb_io/pcb_io_mgr.h
 *     - PCB_IO_MGR::ShowType() strings:          pcbnew/pcb_io/pcb_io_mgr.cpp
 *                                                (and per-plugin REGISTER_PLUGIN names)
 *     - PCB extensions:                          pcbnew/pcb_io/<plugin>/pcb_io_*.h/.cpp
 *
 * Skipped:
 *   - SCH_NESTED_TABLE / NESTED_TABLE (library-table indirection, not a file format)
 *   - SCH_FILE_UNKNOWN / PCB_FILE_UNKNOWN / FILE_TYPE_NONE (sentinel)
 *   - PCB plugin names whose canonical ShowType() is supplied at REGISTER_PLUGIN
 *     time inside the plugin TU: enumerated values still mirrored here using
 *     the names produced by ShowType() (verified by reading each plugin's
 *     registration in pcbnew/pcb_io/<plugin>/pcb_io_*.cpp).
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Locally-mirrored enumerator integer values.  These must stay in lock-step with
// the corresponding header enumerations.  Kept in this file so libkicommon never
// has to pull in eeschema / pcbnew kiface headers.
//
// SCH_FILE_T is declared via DEFINE_ENUM_VECTOR, which is a sequential enum class
// starting at 0 in declaration order.

enum SchFileT
{
    IODISC_SCH_KICAD = 0,
    IODISC_SCH_LEGACY,
    IODISC_SCH_ALTIUM,
    IODISC_SCH_CADSTAR_ARCHIVE,
    IODISC_SCH_DATABASE,
    IODISC_SCH_EAGLE,
    IODISC_SCH_EASYEDA,
    IODISC_SCH_EASYEDAPRO,
    IODISC_SCH_GEDA,
    IODISC_SCH_LTSPICE,
    IODISC_SCH_HTTP,
    IODISC_SCH_PADS,
    // SCH_FILE_UNKNOWN, SCH_NESTED_TABLE intentionally omitted
};

// PCB_FILE_T is a plain enum with an explicit PCB_FILE_UNKNOWN = 0.
enum PcbFileT
{
    IODISC_PCB_FILE_UNKNOWN     = 0,
    IODISC_PCB_KICAD_SEXP       = 1,
    IODISC_PCB_LEGACY           = 2,
    IODISC_PCB_ALLEGRO          = 3,
    IODISC_PCB_ALTIUM_CIRCUIT_MAKER   = 4,
    IODISC_PCB_ALTIUM_CIRCUIT_STUDIO  = 5,
    IODISC_PCB_ALTIUM_DESIGNER  = 6,
    IODISC_PCB_CADSTAR_PCB_ARCHIVE = 7,
    IODISC_PCB_EAGLE            = 8,
    IODISC_PCB_EASYEDA          = 9,
    IODISC_PCB_EASYEDAPRO       = 10,
    IODISC_PCB_FABMASTER        = 11,
    IODISC_PCB_GEDA_PCB         = 12,
    IODISC_PCB_PCAD             = 13,
    IODISC_PCB_SOLIDWORKS_PCB   = 14,
    IODISC_PCB_IPC2581          = 15,
    IODISC_PCB_ODBPP            = 16,
    IODISC_PCB_PADS             = 17,
    IODISC_PCB_SPRINT_LAYOUT    = 18,
    // FILE_TYPE_NONE, NESTED_TABLE intentionally omitted
};


struct FormatEntry
{
    const char*              kind;        // "sch" or "pcb"
    int                      enum_value;
    const char*              enum_name;
    const char*              name;        // ShowType() spelling
    std::vector<const char*> extensions;  // union of board/sch file + library extensions
    bool                     can_read;
    bool                     can_write;
};


// SCH table.  Extensions are the union of GetSchematicFileDesc and GetLibraryDesc
// (so format_for_extension("lib") can match the relevant plugin etc).
//
// can_read = true everywhere a plugin exposes a reader.  KiCad sexpr is the only
// one with a full writer in upstream; the rest are import-only.  HTTP / Database
// are network/proxy formats: read-only for the underlying schematic, library-only.
const std::vector<FormatEntry>& sch_table()
{
    static const std::vector<FormatEntry> kSch = {
        // KiCad sexpr
        { "sch", IODISC_SCH_KICAD, "SCH_KICAD", "KiCad",
          { "kicad_sch", "kicad_sym" }, true, true },

        // Legacy KiCad
        { "sch", IODISC_SCH_LEGACY, "SCH_LEGACY", "Legacy",
          { "sch", "lib" }, true, false },

        // Altium
        { "sch", IODISC_SCH_ALTIUM, "SCH_ALTIUM", "Altium",
          { "SchDoc", "SchLib", "IntLib" }, true, false },

        // CADSTAR Schematic Archive
        { "sch", IODISC_SCH_CADSTAR_ARCHIVE, "SCH_CADSTAR_ARCHIVE",
          "CADSTAR Schematic Archive",
          { "csa", "lib" }, true, false },

        // KiCad database library (libraries only; no schematic format)
        { "sch", IODISC_SCH_DATABASE, "SCH_DATABASE", "Database",
          { "kicad_dbl" }, true, false },

        // Eagle
        { "sch", IODISC_SCH_EAGLE, "SCH_EAGLE", "EAGLE",
          { "sch", "lbr" }, true, false },

        // EasyEDA (JLCEDA) Std
        { "sch", IODISC_SCH_EASYEDA, "SCH_EASYEDA", "EasyEDA (JLCEDA) Std",
          { "json" }, true, false },

        // EasyEDA (JLCEDA) Pro
        { "sch", IODISC_SCH_EASYEDAPRO, "SCH_EASYEDAPRO", "EasyEDA (JLCEDA) Pro",
          { "epro", "zip", "elibz" }, true, false },

        // gEDA / Lepton EDA
        { "sch", IODISC_SCH_GEDA, "SCH_GEDA", "gEDA / Lepton EDA",
          { "sch" }, true, false },

        // LTspice (schematic-only; library is suppressed upstream)
        { "sch", IODISC_SCH_LTSPICE, "SCH_LTSPICE", "LTspice",
          { "asc" }, true, false },

        // HTTP library (network proxy; libraries only)
        { "sch", IODISC_SCH_HTTP, "SCH_HTTP", "HTTP",
          { "kicad_httplib" }, true, false },

        // PADS Logic
        { "sch", IODISC_SCH_PADS, "SCH_PADS", "PADS Logic",
          { "asc", "txt" }, true, false },
    };
    return kSch;
}


// PCB table.  Extensions are union of GetBoardFileDesc and GetLibraryDesc.
// ODB++ and IPC-2581 are write-only (export formats); others read-only except
// KiCad sexpr (full read+write).  Legacy KiCad PCB is read-only (no longer
// written) — its GetBoardFileDesc upstream is currently mis-labelled "Eagle
// XML PCB files" but takes ".brd" / ".kicad_pcb"-era data.
const std::vector<FormatEntry>& pcb_table()
{
    static const std::vector<FormatEntry> kPcb = {
        // KiCad s-expression PCB
        { "pcb", IODISC_PCB_KICAD_SEXP, "KICAD_SEXP", "KiCad",
          { "kicad_pcb", "kicad_mod" }, true, true },

        // Legacy KiCad PCB
        { "pcb", IODISC_PCB_LEGACY, "LEGACY", "Legacy",
          { "brd", "mod", "emp" }, true, false },

        // Allegro PCB
        { "pcb", IODISC_PCB_ALLEGRO, "ALLEGRO", "Cadence Allegro",
          { "brd" }, true, false },

        // Altium Circuit Maker
        { "pcb", IODISC_PCB_ALTIUM_CIRCUIT_MAKER, "ALTIUM_CIRCUIT_MAKER",
          "Altium Circuit Maker", { "CMPcbDoc" }, true, false },

        // Altium Circuit Studio
        { "pcb", IODISC_PCB_ALTIUM_CIRCUIT_STUDIO, "ALTIUM_CIRCUIT_STUDIO",
          "Altium Circuit Studio", { "CSPcbDoc" }, true, false },

        // Altium Designer
        { "pcb", IODISC_PCB_ALTIUM_DESIGNER, "ALTIUM_DESIGNER",
          "Altium Designer",
          { "PcbDoc", "PcbLib", "IntLib" }, true, false },

        // CADSTAR PCB Archive
        { "pcb", IODISC_PCB_CADSTAR_PCB_ARCHIVE, "CADSTAR_PCB_ARCHIVE",
          "CADSTAR PCB Archive",
          { "cpa" }, true, false },

        // Eagle PCB
        { "pcb", IODISC_PCB_EAGLE, "EAGLE", "Eagle",
          { "brd", "lbr" }, true, false },

        // EasyEDA (JLCEDA) Std
        { "pcb", IODISC_PCB_EASYEDA, "EASYEDA", "EasyEDA (JLCEDA) Std",
          { "json", "zip" }, true, false },

        // EasyEDA (JLCEDA) Pro
        { "pcb", IODISC_PCB_EASYEDAPRO, "EASYEDAPRO", "EasyEDA (JLCEDA) Pro",
          { "epro", "zip", "elibz" }, true, false },

        // Fabmaster
        { "pcb", IODISC_PCB_FABMASTER, "FABMASTER", "Fabmaster",
          { "txt", "fab" }, true, false },

        // gEDA PCB
        { "pcb", IODISC_PCB_GEDA_PCB, "GEDA_PCB", "gEDA / Lepton EDA",
          { "pcb", "fp" }, true, false },

        // P-CAD
        { "pcb", IODISC_PCB_PCAD, "PCAD", "P-Cad",
          { "pcb" }, true, false },

        // Solidworks PCB
        { "pcb", IODISC_PCB_SOLIDWORKS_PCB, "SOLIDWORKS_PCB", "Solidworks PCB",
          { "SWPcbDoc" }, true, false },

        // IPC-2581 (export only)
        { "pcb", IODISC_PCB_IPC2581, "IPC2581", "IPC-2581",
          { "xml", "ipc2581" }, false, true },

        // ODB++ (export only)
        { "pcb", IODISC_PCB_ODBPP, "ODBPP", "ODB++",
          { "zip" }, false, true },

        // PADS PCB (ASCII)
        { "pcb", IODISC_PCB_PADS, "PADS", "PADS ASCII",
          { "asc" }, true, false },

        // Sprint Layout
        { "pcb", IODISC_PCB_SPRINT_LAYOUT, "SPRINT_LAYOUT", "Sprint Layout",
          { "lay6", "lay", "lmk" }, true, false },
    };
    return kPcb;
}


py::dict io_discovery_entry_to_dict( const FormatEntry& aEntry )
{
    py::dict d;
    d["kind"]       = py::str( aEntry.kind );
    d["name"]       = py::str( aEntry.name );
    d["enum_value"] = py::int_( aEntry.enum_value );
    d["enum_name"]  = py::str( aEntry.enum_name );

    py::list exts;
    for( const char* e : aEntry.extensions )
        exts.append( py::str( e ) );
    d["extensions"] = exts;

    d["can_read"]   = py::bool_( aEntry.can_read );
    d["can_write"]  = py::bool_( aEntry.can_write );
    return d;
}


py::list io_discovery_list_sch()
{
    py::list out;
    for( const FormatEntry& e : sch_table() )
        out.append( io_discovery_entry_to_dict( e ) );
    return out;
}


py::list io_discovery_list_pcb()
{
    py::list out;
    for( const FormatEntry& e : pcb_table() )
        out.append( io_discovery_entry_to_dict( e ) );
    return out;
}


py::list io_discovery_list_all()
{
    py::list out;
    for( const FormatEntry& e : sch_table() )
        out.append( io_discovery_entry_to_dict( e ) );
    for( const FormatEntry& e : pcb_table() )
        out.append( io_discovery_entry_to_dict( e ) );
    return out;
}


std::string io_discovery_normalise_ext( const std::string& aExt )
{
    std::string s = aExt;
    if( !s.empty() && s.front() == '.' )
        s.erase( s.begin() );
    // case-insensitive compare downstream — keep raw for return
    return s;
}


py::object io_discovery_format_for_extension( const std::string& aExt )
{
    std::string normalised = io_discovery_normalise_ext( aExt );
    if( normalised.empty() )
        return py::none();

    auto eq_icase = []( const std::string& a, const char* b ) {
        if( a.size() != std::char_traits<char>::length( b ) )
            return false;
        for( size_t i = 0; i < a.size(); ++i )
        {
            if( std::tolower( static_cast<unsigned char>( a[i] ) )
                != std::tolower( static_cast<unsigned char>( b[i] ) ) )
                return false;
        }
        return true;
    };

    // Scan SCH first, then PCB.  First match wins.  Callers wanting the full
    // set of candidates can iterate list_all_formats() themselves.
    for( const FormatEntry& e : sch_table() )
        for( const char* ext : e.extensions )
            if( eq_icase( normalised, ext ) )
                return io_discovery_entry_to_dict( e );

    for( const FormatEntry& e : pcb_table() )
        for( const char* ext : e.extensions )
            if( eq_icase( normalised, ext ) )
                return io_discovery_entry_to_dict( e );

    return py::none();
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_io_discovery, m )
{
    m.doc() = "KliCAD IO plugin discovery — enumerate the schematic and PCB file "
              "formats this build of KiCad knows about, with their canonical "
              "extensions and read/write capability.  Pattern A binding "
              "(libkicommon-resident) — always importable, does not require any "
              "kiface to be loaded.  Plugin tables are hand-curated to mirror "
              "SCH_IO_MGR::SCH_FILE_T and PCB_IO_MGR::PCB_FILE_T because their "
              "headers are not on libkicommon's include path.";

    m.def( "list_sch_formats", &io_discovery_list_sch,
           R"DOC(Return the list of schematic IO formats.

Each entry is a dict:
    {
      kind:        "sch",
      name:        str,        # matches SCH_IO_MGR::ShowType()
      enum_value:  int,        # matches SCH_IO_MGR::SCH_FILE_T integer value
      enum_name:   str,        # C++ enumerator name (e.g. "SCH_KICAD")
      extensions:  [str, ...], # union of schematic + library extensions
      can_read:    bool,
      can_write:   bool,
    }
)DOC" );

    m.def( "list_pcb_formats", &io_discovery_list_pcb,
           R"DOC(Return the list of PCB IO formats.

Each entry is a dict with the same shape as list_sch_formats(), but with
kind="pcb" and enum_value matching PCB_IO_MGR::PCB_FILE_T.
)DOC" );

    m.def( "list_all_formats", &io_discovery_list_all,
           R"DOC(Return list_sch_formats() concatenated with list_pcb_formats().)DOC" );

    m.def( "format_for_extension", &io_discovery_format_for_extension,
           py::arg( "ext" ),
           R"DOC(Look up the IO format that handles a given file extension.

`ext` may include or omit the leading dot ("kicad_pcb" or ".kicad_pcb").
Matching is case-insensitive.

Returns the matching format dict (see list_sch_formats / list_pcb_formats),
or None if no plugin claims the extension.  SCH formats are searched first.
Some extensions (e.g. "zip", "txt", "asc", "json", "brd", "lbr") are
claimed by multiple plugins — this helper returns only the first hit; use
list_all_formats() yourself if you need the full candidate set.
)DOC" );
}
