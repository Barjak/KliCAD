/*
 * KliCAD subsystem binding: DRC custom-rules (.kicad_dru) + length report.
 *
 * Exposes the custom DRC rules text and a structured length report as
 * klicad_native_drc_rules.*  The rules text is the canonical .kicad_dru
 * sidecar file beside the board: PCB_BASE_EDIT_FRAME::GetDesignRulesPath()
 * (mirrors PROJECT::AbsolutePath(board_basename + .kicad_dru) — see
 * PCB_BASE_EDIT_FRAME::GetDesignRulesPath in pcb_base_edit_frame.cpp and
 * the API_HANDLER_PCB::handleGet/SetCustomDesignRules counterparts).
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer
 * runs after py::initialize_interpreter, and PyImport_AppendInittab
 * refuses post-init.
 *
 * Frame discovery: we cannot dynamic_cast<PCB_EDIT_FRAME*> across the
 * kiface boundary (typeinfo lives in _pcbnew.kiface.bundle), so we
 * identify via EDA_BASE_FRAME::GetFrameType() == FRAME_PCB_EDITOR and
 * then static_cast.  Same caveat as bindings_pcb_state.cpp.
 *
 * Validation strategy: parsing with a WX_STRING_REPORTER attached
 * collects per-line errors as messages without throwing PARSE_ERROR.
 * We also catch IO_ERROR from the lexer for malformed top-level syntax.
 *
 * set_custom_rules writes the .kicad_dru file and re-inits the DRC
 * engine + clearance cache so subsequent EvalRules calls see the new
 * rules immediately.
 *
 * get_length_report mirrors DRC_TEST_PROVIDER_MATCHED_LENGTH (we don't
 * have access to its private m_report) — it walks copper items, groups
 * by (rule, netcode), and runs LENGTH_DELAY_CALCULATION::CalculateLengthDetails
 * with the same PATH_OPTIMISATIONS used by the test provider.  Each
 * entry's target/tolerance bounds come from the matching LENGTH_CONSTRAINT
 * MINOPTMAX<int> (nm).  Returned lengths are in mm; bounds that aren't
 * set are returned as null in the dict.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <reporter.h>
#include <ki_exception.h>
#include <wildcards_and_files_ext.h>

#include <board.h>
#include <board_connected_item.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_track.h>
#include <netinfo.h>

#include <drc/drc_rule.h>
#include <drc/drc_rule_parser.h>
#include <drc/drc_engine.h>
#include <tools/drc_tool.h>
#include <tool/tool_manager.h>

#include <connectivity/connectivity_data.h>
#include <connectivity/from_to_cache.h>
#include <length_delay_calculation/length_delay_calculation.h>

#include <wx/arrstr.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/window.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

constexpr double NM_TO_MM = 1.0 / 1e6;


// Per-TU helpers — unique vs find_live_kiway_for_pcb_state / *_pcb_actions /
// *_footprint_editor / *_schematic_state / etc.
KIWAY* find_live_kiway_for_drc_rules()
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


PCB_EDIT_FRAME* find_pcb_edit_frame_for_drc_rules()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_PCB_EDITOR )
            return static_cast<PCB_EDIT_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live PCB_EDIT_FRAME, spawning pcbnew if necessary.  Throws
// on any failure so the caller doesn't have to null-check.
PCB_EDIT_FRAME* require_pcb_frame_for_drc_rules()
{
    if( PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_drc_rules() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_drc_rules();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(drc_rules needs to spawn the pcbnew frame)" );
    }

    kiway->Player( FRAME_PCB_EDITOR, true );

    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_drc_rules();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain PCB_EDIT_FRAME after "
            "KIWAY::Player(FRAME_PCB_EDITOR, true)" );
    }

    return frame;
}


// The .kicad_dru sidecar is the canonical custom-rule store for a BOARD.
// The frame helper handles project-relative resolution.
wxString resolve_rules_path( PCB_EDIT_FRAME* aFrame )
{
    wxString path = aFrame->GetDesignRulesPath();

    if( path.IsEmpty() )
    {
        throw std::runtime_error(
            "PCB_EDIT_FRAME has no design-rules path (board not yet saved?)" );
    }

    return path;
}


// Parse a rules text blob with a string reporter attached.  Returns
// (parsed_rules, errors).  Catches both per-line REPORTER errors and
// IO_ERROR thrown by the lexer for top-level syntax breakage.
struct ParseOutcome
{
    std::vector<std::shared_ptr<DRC_RULE>> rules;
    std::vector<std::string>               errors;
};

ParseOutcome parse_rules_text( const wxString& aText )
{
    ParseOutcome out;

    WX_STRING_REPORTER reporter;

    try
    {
        DRC_RULES_PARSER parser( aText, wxT( "klicad_native_drc_rules" ) );
        parser.Parse( out.rules, &reporter );
    }
    catch( const IO_ERROR& ioe )
    {
        out.errors.push_back( std::string( ioe.What().utf8_str() ) );
    }
    catch( const std::exception& ex )
    {
        out.errors.push_back( std::string( "parser exception: " ) + ex.what() );
    }
    catch( ... )
    {
        out.errors.push_back( "parser threw unknown exception" );
    }

    if( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) )
    {
        // The reporter accumulates all messages into one wxString
        // (newline-separated).  Split into individual error lines so
        // callers can iterate them; drop empties.
        wxString blob = reporter.GetMessages();
        wxArrayString lines = wxSplit( blob, '\n' );

        for( const wxString& line : lines )
        {
            if( !line.IsEmpty() )
                out.errors.push_back( std::string( line.utf8_str() ) );
        }
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// get_custom_rules
// ──────────────────────────────────────────────────────────────────────────
py::object drc_rules_get_custom_rules()
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_drc_rules();
    wxString        path  = resolve_rules_path( frame );

    if( !wxFileName::IsFileReadable( path ) )
        return py::str( "" );

    wxFFile file( path, "r" );

    if( !file.IsOpened() )
        throw std::runtime_error( std::string( "failed to open rules file: " )
                                  + std::string( path.utf8_str() ) );

    wxString content;
    file.ReadAll( &content );
    file.Close();

    return py::str( std::string( content.utf8_str() ) );
}


// ──────────────────────────────────────────────────────────────────────────
// validate_custom_rules
// ──────────────────────────────────────────────────────────────────────────
py::dict drc_rules_validate_custom_rules( const std::string& rules_text )
{
    wxString     text = wxString::FromUTF8( rules_text.c_str() );
    ParseOutcome outcome = parse_rules_text( text );

    py::dict result;
    result[ "ok" ]         = outcome.errors.empty();
    result[ "rule_count" ] = static_cast<int>( outcome.rules.size() );
    result[ "errors" ]     = py::cast( outcome.errors );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// set_custom_rules
// ──────────────────────────────────────────────────────────────────────────
py::dict drc_rules_set_custom_rules( const std::string& rules_text )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_drc_rules();
    wxString        path  = resolve_rules_path( frame );

    wxString     text    = wxString::FromUTF8( rules_text.c_str() );
    ParseOutcome outcome = parse_rules_text( text );

    if( !outcome.errors.empty() )
    {
        py::dict result;
        result[ "ok" ]     = false;
        result[ "errors" ] = py::cast( outcome.errors );
        return result;
    }

    // Atomic-ish write: open + write + close, then poke the DRC engine
    // so live evaluation picks up the change.
    {
        wxFFile file( path, "w" );

        if( !file.IsOpened() )
        {
            std::vector<std::string> errs{ std::string( "failed to open rules file for writing: " )
                                           + std::string( path.utf8_str() ) };
            py::dict result;
            result[ "ok" ]     = false;
            result[ "errors" ] = py::cast( errs );
            return result;
        }

        if( !file.Write( text ) )
        {
            file.Close();
            std::vector<std::string> errs{ "failed to write rules file" };
            py::dict result;
            result[ "ok" ]     = false;
            result[ "errors" ] = py::cast( errs );
            return result;
        }

        file.Close();
    }

    // Re-init the DRC engine so EvalRules picks up the new rules.  Match
    // PCB_EDIT_FRAME's own load path (silently swallow PARSE_ERROR — we
    // already validated above with WX_STRING_REPORTER, this is belt-and-
    // suspenders).
    if( TOOL_MANAGER* toolMgr = frame->GetToolManager() )
    {
        if( DRC_TOOL* drcTool = toolMgr->GetTool<DRC_TOOL>() )
        {
            if( DRC_ENGINE* engine = drcTool->GetDRCEngine().get() )
            {
                try
                {
                    engine->InitEngine( wxFileName( path ) );
                }
                catch( const PARSE_ERROR& )
                {
                    // Swallowed: pre-write validation should have caught
                    // this.  If we hit it here it means InitEngine's
                    // codepath disagrees with parse_rules_text — surface
                    // through the engine's own log reporter next run.
                }
                catch( ... )
                {
                    // Same.
                }
            }
        }
    }

    if( BOARD* board = frame->GetBoard() )
        board->IncrementTimeStamp();

    std::vector<std::string> noErrors;
    py::dict result;
    result[ "ok" ]     = true;
    result[ "errors" ] = py::cast( noErrors );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_length_report
// ──────────────────────────────────────────────────────────────────────────
//
// Mirrors DRC_TEST_PROVIDER_MATCHED_LENGTH::runInternal but exposes the
// raw per-net numbers (no DRC_ITEM violation reporting).  We can't peek
// at the test provider's private m_report directly, so we recompute.
//
py::list drc_rules_get_length_report( const std::string& net_filter )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_drc_rules();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    TOOL_MANAGER* toolMgr = frame->GetToolManager();
    DRC_TOOL*     drcTool = toolMgr ? toolMgr->GetTool<DRC_TOOL>() : nullptr;
    DRC_ENGINE*   engine  = drcTool ? drcTool->GetDRCEngine().get() : nullptr;

    if( !engine )
        throw std::runtime_error( "DRC engine unavailable on PCB_EDIT_FRAME" );

    std::shared_ptr<FROM_TO_CACHE> ftCache = board->GetConnectivity()->GetFromToCache();

    if( ftCache )
        ftCache->Rebuild( board );

    wxString filter = wxString::FromUTF8( net_filter.c_str() );

    // Group copper items by (rule, netcode).
    std::map<DRC_RULE*, std::set<BOARD_CONNECTED_ITEM*>> itemSets;

    auto considerItem =
            [&]( BOARD_ITEM* item )
            {
                DRC_CONSTRAINT c =
                        engine->EvalRules( LENGTH_CONSTRAINT, item, nullptr, item->GetLayer() );

                if( c.IsNull() )
                    return;

                BOARD_CONNECTED_ITEM* citem = dynamic_cast<BOARD_CONNECTED_ITEM*>( item );

                if( !citem )
                    return;

                itemSets[ c.GetParentRule() ].insert( citem );
            };

    for( PCB_TRACK* t : board->Tracks() )
        considerItem( t );

    for( FOOTPRINT* fp : board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
            considerItem( pad );
    }

    LENGTH_DELAY_CALCULATION* calc = board->GetLengthCalculation();

    py::list out;

    if( !calc )
        return out;

    for( const auto& [rule, ruleItems] : itemSets )
    {
        std::map<int, std::set<BOARD_CONNECTED_ITEM*>> netMap;

        for( BOARD_CONNECTED_ITEM* item : ruleItems )
            netMap[ item->GetNetCode() ].insert( item );

        // Resolve the LENGTH_CONSTRAINT once per rule for target/tolerance
        // reporting.  Falls back to no bounds if the rule has been
        // mutated and no longer carries a length constraint (shouldn't
        // happen — itemSets came from LENGTH_CONSTRAINT evaluation —
        // but defensive).
        std::optional<DRC_CONSTRAINT> lenConstraint = rule->FindConstraint( LENGTH_CONSTRAINT );

        for( const auto& [netCode, netItems] : netMap )
        {
            NETINFO_ITEM* netinfo = board->GetNetInfo().GetNetItem( netCode );
            wxString      netname = netinfo ? netinfo->GetNetname() : wxString();

            if( !filter.IsEmpty() && netname != filter )
                continue;

            std::vector<LENGTH_DELAY_CALCULATION_ITEM> lengthItems;
            lengthItems.reserve( netItems.size() );

            for( BOARD_CONNECTED_ITEM* item : netItems )
            {
                LENGTH_DELAY_CALCULATION_ITEM li = calc->GetLengthCalculationItem( item );

                if( li.Type() != LENGTH_DELAY_CALCULATION_ITEM::TYPE::UNKNOWN )
                    lengthItems.emplace_back( li );
            }

            constexpr PATH_OPTIMISATIONS opts = {
                .OptimiseVias        = true,
                .MergeTracks         = true,
                .OptimiseTracesInPads = true,
                .InferViaInPad       = false
            };

            LENGTH_DELAY_STATS details = calc->CalculateLengthDetails(
                    lengthItems, opts, nullptr, nullptr,
                    LENGTH_DELAY_LAYER_OPT::NO_LAYER_DETAIL,
                    LENGTH_DELAY_DOMAIN_OPT::NO_DELAY_DETAIL );

            double total_nm = static_cast<double>( details.TrackLength )
                              + static_cast<double>( details.ViaLength )
                              + static_cast<double>( details.PadToDieLength );

            double total_mm = total_nm * NM_TO_MM;

            py::dict entry;
            entry[ "net_name" ] = std::string( netname.utf8_str() );
            entry[ "length_mm" ] = total_mm;

            // The DRC schema models LENGTH_CONSTRAINT as MINOPTMAX<int>
            // in nm.  Min/Max are hard bounds; Opt is the target.  We
            // expose target_min/max as Min/Max and tolerance_min/max as
            // the same (one schema knob); within_tolerance is whether
            // total falls inside [Min, Max].
            bool within = true;

            auto set_bound =
                    [&]( const char* key, bool has, int value_nm )
                    {
                        if( has )
                            entry[ key ] = value_nm * NM_TO_MM;
                        else
                            entry[ key ] = py::none();
                    };

            if( lenConstraint )
            {
                const MINOPTMAX<int>& v = lenConstraint->GetValue();
                set_bound( "target_min_mm",    v.HasMin(), v.HasMin() ? v.Min() : 0 );
                set_bound( "target_max_mm",    v.HasMax(), v.HasMax() ? v.Max() : 0 );
                set_bound( "tolerance_min_mm", v.HasMin(), v.HasMin() ? v.Min() : 0 );
                set_bound( "tolerance_max_mm", v.HasMax(), v.HasMax() ? v.Max() : 0 );

                if( v.HasMin() && total_nm < v.Min() )
                    within = false;
                if( v.HasMax() && total_nm > v.Max() )
                    within = false;
            }
            else
            {
                entry[ "target_min_mm" ]    = py::none();
                entry[ "target_max_mm" ]    = py::none();
                entry[ "tolerance_min_mm" ] = py::none();
                entry[ "tolerance_max_mm" ] = py::none();
            }

            entry[ "within_tolerance" ] = within;

            out.append( entry );
        }
    }

    return out;
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h for why
// PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_drc_rules_bindings( py::module_& m )
{
    m.doc() = "KliCAD DRC custom-rules binding — read/write/validate the "
              ".kicad_dru sidecar file alongside the active board and "
              "produce a structured per-net length report.  The rules text "
              "syntax matches the in-tree DRC_RULES_PARSER (kicad_dru "
              "format).  Lengths returned in millimeters.";

    m.def( "get_custom_rules", &drc_rules_get_custom_rules,
           R"DOC(Read the current custom DRC rules text from the BOARD's
.kicad_dru sidecar file (PCB_BASE_EDIT_FRAME::GetDesignRulesPath).

Returns the file contents as a string, or '' if the file doesn't exist.
Raises RuntimeError if the board hasn't been saved yet (no path) or
the file exists but cannot be opened.
)DOC" );

    m.def( "set_custom_rules", &drc_rules_set_custom_rules,
           py::arg( "rules_text" ),
           R"DOC(Validate `rules_text` with DRC_RULES_PARSER, then write it
to the BOARD's .kicad_dru sidecar.  Re-inits the DRC engine so live
constraint evaluation picks up the new rules immediately.

Returns {ok: bool, errors: list[str]}.  On parse failure, the file is
NOT written and ok=False with the per-line error messages.  On file-
write failure, ok=False with a single I/O error message.
)DOC" );

    m.def( "validate_custom_rules", &drc_rules_validate_custom_rules,
           py::arg( "rules_text" ),
           R"DOC(Parse `rules_text` with DRC_RULES_PARSER without applying
it.  Returns {ok: bool, rule_count: int, errors: list[str]}.
)DOC" );

    m.def( "get_length_report", &drc_rules_get_length_report,
           py::arg( "net_filter" ) = std::string(),
           R"DOC(Compute a per-net length report against the active BOARD
using DRC_ENGINE rule evaluation + LENGTH_DELAY_CALCULATION (mirrors
DRC_TEST_PROVIDER_MATCHED_LENGTH internals).  Only nets covered by a
LENGTH_CONSTRAINT-bearing rule are returned.

net_filter: if non-empty, only return entries whose net_name matches
exactly.

Returns a list of dicts, each with:
    net_name (str)
    length_mm (float)         — total = track + via + pad-to-die
    target_min_mm (float|None)
    target_max_mm (float|None)
    tolerance_min_mm (float|None)
    tolerance_max_mm (float|None)
    within_tolerance (bool)   — total inside [Min, Max] of the rule

Bounds come from the matching rule's LENGTH_CONSTRAINT MINOPTMAX<int>;
unbounded sides are returned as None.  target_*_mm and tolerance_*_mm
currently track the same Min/Max (the DRC schema doesn't split them).
)DOC" );
}
