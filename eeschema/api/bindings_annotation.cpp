/*
 * KliCAD subsystem binding: schematic ANNOTATION (Pattern B, eeschema kiface).
 *
 * Exposes programmatic reference-designator assignment, clearing, summary, and
 * PCB->SCH back-annotation as klicad_native_annotation.*  This unblocks the
 * priority-1 use case: assigning ref-des in bulk from Python and pulling
 * pin/footprint/value changes from a pcbnew-exported netlist back into the
 * schematic without GUI interaction.
 *
 *   annotate(scope, order, start_number, sort_by_first_letter)
 *       Wraps SCH_EDIT_FRAME::AnnotateSymbols.  Always uses
 *       INCREMENTAL_BY_REF, reset=true, regroup_units=true,
 *       repair_timestamps=true, SYMBOL_FILTER_NON_POWER.
 *
 *   clear_annotation(scope)
 *       Wraps SCH_EDIT_FRAME::DeleteAnnotation (recursive=true).
 *
 *   get_annotation_summary()
 *       Walks the hierarchy and returns counts plus
 *       next_refs_by_prefix derived from the live REFDES_TRACKER.
 *
 *   back_annotate_from_netlist(netlist_path)
 *       Reads the pcbnew netlist file, runs BACK_ANNOTATE::BackAnnotateSymbols.
 *       All process_* flags default on (footprints, values, references,
 *       net names, attributes, other fields, unit swaps, pin swaps); the
 *       dry_run flag is off.  Set relink_footprints=false (matches the
 *       "update PCB->SCH" UI default).
 *
 * Pattern B: PYBIND11_EMBEDDED_MODULE is NOT used.  The eeschema kiface is
 * dlopen'd after Py_Initialize; appendinittab is locked.  Instead, this TU
 * exposes klicad_register_annotation_bindings(py::module_&) which the kiface
 * register table calls against a runtime-created module.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <reporter.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_sheet_path.h>
#include <sch_reference_list.h>
#include <sch_commit.h>

#include <refdes_tracker.h>
#include <backannotate.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/wfstream.h>
#include <wx/window.h>
#include <wx/txtstrm.h>

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// ────────────────────────────────────────────────────────────────────────────
// Frame discovery (mirrors bindings_schematic_state.cpp; names per-TU so we
// don't ODR-clash with sibling bindings that also walk wxTopLevelWindows).
// ────────────────────────────────────────────────────────────────────────────

KIWAY* find_live_kiway_for_annotation()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
        {
            if( holder->HasKiway() )
                return &holder->Kiway();
        }
    }
    return nullptr;
}


SCH_EDIT_FRAME* find_sch_frame_for_annotation()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_SCH )
            return static_cast<SCH_EDIT_FRAME*>( base );
    }
    return nullptr;
}


SCH_EDIT_FRAME* require_sch_frame_for_annotation()
{
    if( SCH_EDIT_FRAME* frame = find_sch_frame_for_annotation() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_annotation();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(annotation needs to spawn the eeschema frame)" );
    }

    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* frame = find_sch_frame_for_annotation();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain SCH_EDIT_FRAME after KIWAY::Player(FRAME_SCH, true)" );
    }

    return frame;
}


// ────────────────────────────────────────────────────────────────────────────
// Enum coercion
// ────────────────────────────────────────────────────────────────────────────

ANNOTATE_SCOPE_T parse_scope( const std::string& aScope )
{
    if( aScope == "all" )
        return ANNOTATE_ALL;

    if( aScope == "current_sheet" )
        return ANNOTATE_CURRENT_SHEET;

    throw std::invalid_argument(
        "scope must be one of: 'all', 'current_sheet' (got '" + aScope + "')" );
}


ANNOTATE_ORDER_T parse_order( const std::string& aOrder )
{
    if( aOrder == "x_then_y" )
        return SORT_BY_X_POSITION;

    if( aOrder == "y_then_x" )
        return SORT_BY_Y_POSITION;

    throw std::invalid_argument(
        "order must be one of: 'x_then_y', 'y_then_x' (got '" + aOrder + "')" );
}


// ────────────────────────────────────────────────────────────────────────────
// annotate
// ────────────────────────────────────────────────────────────────────────────

py::dict annotation_annotate( const std::string& scope,
                              const std::string& order,
                              int                start_number,
                              bool               sort_by_first_letter )
{
    SCH_EDIT_FRAME*  frame = require_sch_frame_for_annotation();
    ANNOTATE_SCOPE_T scopeEnum = parse_scope( scope );
    ANNOTATE_ORDER_T orderEnum = parse_order( order );

    (void) sort_by_first_letter;  // Reserved for future use; the current
                                  // AnnotateSymbols API doesn't expose this
                                  // flag directly — first-letter sort is
                                  // implicit in the prefix grouping.

    WX_STRING_REPORTER reporter;
    SCH_COMMIT         commit( frame );

    {
        py::gil_scoped_release nogil;

        frame->AnnotateSymbols( &commit,
                                scopeEnum,
                                orderEnum,
                                INCREMENTAL_BY_REF,
                                /*aRecursive*/ true,
                                start_number,
                                /*aResetAnnotation*/ true,
                                /*aRegroupUnits*/ true,
                                /*aRepairTimestamps*/ true,
                                reporter,
                                SYMBOL_FILTER_NON_POWER );

        commit.Push( wxT( "KliCAD: annotate" ) );
    }

    py::dict result;
    result[ "ok" ]       = true;
    result[ "scope" ]    = scope;
    result[ "order" ]    = order;
    result[ "messages" ] = std::string( reporter.GetMessages().utf8_str() );
    return result;
}


// ────────────────────────────────────────────────────────────────────────────
// clear_annotation
// ────────────────────────────────────────────────────────────────────────────

py::dict annotation_clear( const std::string& scope )
{
    SCH_EDIT_FRAME*  frame = require_sch_frame_for_annotation();
    ANNOTATE_SCOPE_T scopeEnum = parse_scope( scope );

    WX_STRING_REPORTER reporter;

    {
        py::gil_scoped_release nogil;
        frame->DeleteAnnotation( scopeEnum, /*aRecursive*/ true, reporter );
    }

    py::dict result;
    result[ "ok" ]       = true;
    result[ "scope" ]    = scope;
    result[ "messages" ] = std::string( reporter.GetMessages().utf8_str() );
    return result;
}


// ────────────────────────────────────────────────────────────────────────────
// get_annotation_summary
// ────────────────────────────────────────────────────────────────────────────

py::dict annotation_get_summary()
{
    SCH_EDIT_FRAME* frame = require_sch_frame_for_annotation();
    SCHEMATIC&      sch   = frame->Schematic();

    int total       = 0;
    int annotated   = 0;
    int unannotated = 0;

    std::map<std::string, int> max_seen_by_prefix;

    if( sch.IsValid() )
    {
        SCH_SHEET_LIST     sheets = sch.Hierarchy();
        SCH_REFERENCE_LIST refs;
        sheets.GetSymbols( refs, SYMBOL_FILTER_ALL );

        for( size_t i = 0; i < refs.GetCount(); i++ )
        {
            SCH_REFERENCE&  ref    = refs[ i ];
            SCH_SYMBOL*     symbol = ref.GetSymbol();
            SCH_SHEET_PATH* sheet  = &ref.GetSheetPath();

            ++total;

            if( symbol->IsAnnotated( sheet ) )
            {
                ++annotated;

                wxString fullRef = symbol->GetRef( sheet, true );  // include num

                // Split into prefix + trailing digits.
                size_t splitAt = fullRef.length();

                while( splitAt > 0 && wxIsdigit( fullRef[ splitAt - 1 ] ) )
                    --splitAt;

                if( splitAt < fullRef.length() && splitAt > 0 )
                {
                    std::string prefix( fullRef.Left( splitAt ).utf8_str() );
                    long        n = 0;
                    fullRef.Mid( splitAt ).ToLong( &n );

                    int& cur = max_seen_by_prefix[ prefix ];

                    if( static_cast<int>( n ) > cur )
                        cur = static_cast<int>( n );
                }
            }
            else
            {
                ++unannotated;
            }
        }
    }

    // Derive next-available per prefix.  Prefer the live REFDES_TRACKER if
    // the project has one (its GetNextRefDes is non-const-mutating; we
    // therefore use the max-seen+1 heuristic for read-only summary).
    py::dict next_refs_by_prefix;

    for( const auto& [ prefix, maxN ] : max_seen_by_prefix )
        next_refs_by_prefix[ py::str( prefix ) ] = maxN + 1;

    py::dict result;
    result[ "total_symbols" ]       = total;
    result[ "annotated" ]           = annotated;
    result[ "unannotated" ]         = unannotated;
    result[ "next_refs_by_prefix" ] = next_refs_by_prefix;
    return result;
}


// ────────────────────────────────────────────────────────────────────────────
// back_annotate_from_netlist
// ────────────────────────────────────────────────────────────────────────────

py::dict annotation_back_annotate_from_netlist( const std::string& netlist_path )
{
    SCH_EDIT_FRAME* frame = require_sch_frame_for_annotation();

    wxString wxPath = wxString::FromUTF8( netlist_path.c_str() );

    if( !wxFileName::FileExists( wxPath ) )
    {
        throw std::runtime_error(
            "netlist_path does not exist: " + netlist_path );
    }

    // Slurp the netlist file into a std::string.
    std::ifstream in( netlist_path, std::ios::binary );

    if( !in )
        throw std::runtime_error( "failed to open netlist: " + netlist_path );

    std::ostringstream buf;
    buf << in.rdbuf();
    std::string netlist_text = buf.str();

    WX_STRING_REPORTER reporter;
    bool               ok = false;

    {
        py::gil_scoped_release nogil;

        BACK_ANNOTATE backAnno( frame,
                                reporter,
                                /*aRelinkFootprints*/ false,
                                /*aProcessFootprints*/ true,
                                /*aProcessValues*/ true,
                                /*aProcessReferences*/ true,
                                /*aProcessNetNames*/ true,
                                /*aProcessAttributes*/ true,
                                /*aProcessOtherFields*/ true,
                                /*aPreferUnitSwaps*/ true,
                                /*aPreferPinSwaps*/ true,
                                /*aDryRun*/ false );

        ok = backAnno.BackAnnotateSymbols( netlist_text );
    }

    py::dict result;
    result[ "ok" ]           = ok;
    result[ "netlist_path" ] = netlist_path;
    result[ "messages" ]     = std::string( reporter.GetMessages().utf8_str() );
    return result;
}

} // anon


// Registered at kiface-load time via klicad_register_eeschema_bindings().
void klicad_register_annotation_bindings( py::module_& m )
{
    m.doc() = "KliCAD schematic annotation binding — programmatic reference "
              "designator assignment, clearing, summary, and PCB->SCH "
              "back-annotation against the running SCH_EDIT_FRAME.  Wraps "
              "SCH_EDIT_FRAME::AnnotateSymbols / DeleteAnnotation and "
              "BACK_ANNOTATE::BackAnnotateSymbols under the hood.";

    m.def( "annotate", &annotation_annotate,
           py::arg( "scope" )                = std::string( "all" ),
           py::arg( "order" )                = std::string( "x_then_y" ),
           py::arg( "start_number" )         = 0,
           py::arg( "sort_by_first_letter" ) = false,
           R"DOC(Annotate symbols in the schematic.

scope:        'all' | 'current_sheet'
order:        'x_then_y' (SORT_BY_X_POSITION) | 'y_then_x' (SORT_BY_Y_POSITION)
start_number: lowest ref-des number to assign (0 means use prefix-default,
              i.e. R1, R2, ...).
sort_by_first_letter: reserved (currently a no-op; reference grouping is
              already prefix-keyed).

Uses INCREMENTAL_BY_REF, resets existing annotation, regroups units,
repairs duplicate timestamps, and filters out power symbols.  Wraps the
mutation in a SCH_COMMIT pushed as "KliCAD: annotate" for undo.

Returns {ok, scope, order, messages}.  Spawns eeschema if not already up.
)DOC" );

    m.def( "clear_annotation", &annotation_clear,
           py::arg( "scope" ) = std::string( "all" ),
           R"DOC(Clear annotation (reference designators) from symbols.

scope: 'all' | 'current_sheet' (recursive into sub-sheets).

Returns {ok, scope, messages}.  Spawns eeschema if not already up.
)DOC" );

    m.def( "get_annotation_summary", &annotation_get_summary,
           R"DOC(Summarize annotation state of the schematic.

Returns {
    total_symbols:       int,
    annotated:           int,
    unannotated:         int,
    next_refs_by_prefix: {prefix(str) -> next_available_number(int)},
}

next_refs_by_prefix is derived from the currently-annotated symbols (max
seen + 1 per prefix); prefixes with no annotated symbols are omitted.
)DOC" );

    m.def( "back_annotate_from_netlist", &annotation_back_annotate_from_netlist,
           py::arg( "netlist_path" ),
           R"DOC(Apply PCB->SCH back-annotation from a pcbnew-exported netlist file.

Reads the netlist file and runs BACK_ANNOTATE::BackAnnotateSymbols with
all process_* flags enabled (footprints, values, references, net names,
attributes, other fields, unit swaps, pin swaps).  relink_footprints is
off and dry_run is off — changes are applied to the live schematic.

Returns {ok, netlist_path, messages}.  Raises RuntimeError if the file
doesn't exist or can't be read.
)DOC" );
}
