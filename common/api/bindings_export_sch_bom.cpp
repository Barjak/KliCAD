/*
 * KliCAD subsystem binding: schematic BOM export.
 *
 * Exposes JOB_EXPORT_SCH_BOM as
 *   klicad_native_export_sch_bom.run(schematic_path, output, **opts).
 *
 * Mirrors bindings_erc.cpp / bindings_export_sch_plot.cpp; see
 * BINDING_PATTERN.md for the per-subsystem recipe.
 *
 * The BOM job has a wide config surface (field columns, group-by,
 * sort, named presets, format presets, variants, delimiters).  We
 * surface every JOB_EXPORT_SCH_BOM member field as a kwarg with the
 * same defaults the CLI command uses (see command_sch_export_bom.cpp),
 * with one exception: the list-typed fields default to an empty list,
 * which the underlying handler interprets as "use the BOM preset's
 * own column list" — that's almost always what a user passing a
 * bom_preset_name actually wants.  A caller who wants the CLI's
 * historical defaults can pass them explicitly.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <jobs/job_export_sch_bom.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>
#include <reporter.h>

#include <wx/string.h>
#include <wx/window.h>

#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-subsystem unique helper name — must not collide with the matching
// helpers in bindings_drc.cpp (find_live_kiway), bindings_erc.cpp
// (find_live_kiway_for_erc), bindings_export_sch_plot.cpp
// (find_live_kiway_for_export_sch_plot), bindings_export_gerbers.cpp
// (find_live_kiway_for_export_gerbers), bindings_export_drill.cpp
// (find_live_kiway_for_export_drill), bindings_export_3d.cpp
// (find_live_kiway_for_export_3d), bindings_gerber_diff.cpp
// (find_live_kiway_for_gerber_diff), or bindings_render.cpp
// (find_live_kiway_for_render).
KIWAY* find_live_kiway_for_export_sch_bom()
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


// Convert a Python list[str] of wxString-equivalents (UTF-8 std::strings
// from pybind11/stl.h) into the std::vector<wxString> shape that
// JOB_EXPORT_SCH_BOM's list-typed members want.
std::vector<wxString> to_wxstring_vector( const std::vector<std::string>& aIn )
{
    std::vector<wxString> out;
    out.reserve( aIn.size() );

    for( const std::string& s : aIn )
        out.emplace_back( wxString::FromUTF8( s ) );

    return out;
}


py::object run_export_sch_bom( const std::string&              schematic_path,
                               const std::string&              output,
                               // Preset selection (matches CLI --preset / --fmt-preset)
                               const std::string&              bom_preset_name,
                               const std::string&              bom_format_preset_name,
                               // Format / delimiter options
                               const std::string&              field_delimiter,
                               const std::string&              string_delimiter,
                               const std::string&              ref_delimiter,
                               const std::string&              ref_range_delimiter,
                               bool                            keep_tabs,
                               bool                            keep_line_breaks,
                               // Field / grouping / sort options
                               const std::vector<std::string>& fields_ordered,
                               const std::vector<std::string>& fields_labels,
                               const std::vector<std::string>& fields_group_by,
                               const std::string&              sort_field,
                               bool                            sort_asc,
                               const std::string&              filter_string,
                               bool                            exclude_dnp,
                               bool                            group_symbols,
                               // Variant selection
                               const std::vector<std::string>& variant_names )
{
    KIWAY* kiway = find_live_kiway_for_export_sch_bom();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY available — is KiCad's GUI running? "
                                  "(BOM export needs the eeschema kiface and project state)" );

    // The schematic jobs handler expects the schematic editor frame to be
    // live when running in GUI mode with a project loaded.  Player(..., true)
    // creates the frame if missing; no-op if already up.
    kiway->Player( FRAME_SCH, true );

    JOB_EXPORT_SCH_BOM bomJob;

    bomJob.m_filename = wxString::FromUTF8( schematic_path );
    bomJob.SetConfiguredOutputPath( wxString::FromUTF8( output ) );

    // Named presets — these are looked up by name against the project's
    // stored BOM presets / format presets at dispatch time.  Empty string
    // means "use the schematic's current settings".
    bomJob.m_bomPresetName    = wxString::FromUTF8( bom_preset_name );
    bomJob.m_bomFmtPresetName = wxString::FromUTF8( bom_format_preset_name );

    // Format / delimiter options.
    bomJob.m_fieldDelimiter    = wxString::FromUTF8( field_delimiter );
    bomJob.m_stringDelimiter   = wxString::FromUTF8( string_delimiter );
    bomJob.m_refDelimiter      = wxString::FromUTF8( ref_delimiter );
    bomJob.m_refRangeDelimiter = wxString::FromUTF8( ref_range_delimiter );
    bomJob.m_keepTabs          = keep_tabs;
    bomJob.m_keepLineBreaks    = keep_line_breaks;

    // Field / grouping / sort options.
    bomJob.m_fieldsOrdered = to_wxstring_vector( fields_ordered );
    bomJob.m_fieldsLabels  = to_wxstring_vector( fields_labels );
    bomJob.m_fieldsGroupBy = to_wxstring_vector( fields_group_by );
    bomJob.m_sortField     = wxString::FromUTF8( sort_field );
    bomJob.m_sortAsc       = sort_asc;
    bomJob.m_filterString  = wxString::FromUTF8( filter_string );
    bomJob.m_excludeDNP    = exclude_dnp;
    bomJob.m_groupSymbols  = group_symbols;

    // Variants.  Empty list means "default variant only" per the JOB header.
    bomJob.m_variantNames = to_wxstring_vector( variant_names );

    WX_STRING_REPORTER reporter;

    int exitCode;
    {
        // Release the GIL across the long C++ dispatch so any re-entrant
        // Python callbacks (none today, but future bindings might) can run.
        py::gil_scoped_release nogil;
        exitCode = kiway->ProcessJob( KIWAY::FACE_SCH, &bomJob, &reporter );
    }

    py::list outputPaths;
    for( const JOB_OUTPUT& out : bomJob.GetOutputs() )
        outputPaths.append( std::string( out.m_outputPath.ToUTF8() ) );

    py::dict result;
    result[ "ok" ]           = ( exitCode == 0 );
    result[ "exit_code" ]    = exitCode;
    result[ "messages" ]     = reporter.GetMessages().ToStdString();
    result[ "output_paths" ] = outputPaths;
    return result;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_export_sch_bom, m )
{
    m.doc() = "KliCAD schematic BOM export binding (calls JOB_EXPORT_SCH_BOM "
              "under the hood). Returns a structured dict; never streams. "
              "Requires KiCad's GUI to be running (needs a live KIWAY).";

    m.def( "run", &run_export_sch_bom,
           py::arg( "schematic_path" ),
           py::arg( "output" ),
           // Preset selection
           py::arg( "bom_preset_name" )        = "",
           py::arg( "bom_format_preset_name" ) = "",
           // Format / delimiter options (defaults match the JOB ctor: empty
           // string => let the format preset / schematic settings decide).
           py::arg( "field_delimiter" )        = "",
           py::arg( "string_delimiter" )       = "",
           py::arg( "ref_delimiter" )          = "",
           py::arg( "ref_range_delimiter" )    = "",
           py::arg( "keep_tabs" )              = false,
           py::arg( "keep_line_breaks" )       = false,
           // Field / grouping / sort options
           py::arg( "fields_ordered" )         = std::vector<std::string>{},
           py::arg( "fields_labels" )          = std::vector<std::string>{},
           py::arg( "fields_group_by" )        = std::vector<std::string>{},
           py::arg( "sort_field" )             = "",
           py::arg( "sort_asc" )               = true,
           py::arg( "filter_string" )          = "",
           py::arg( "exclude_dnp" )            = false,
           py::arg( "group_symbols" )          = true,
           // Variants
           py::arg( "variant_names" )          = std::vector<std::string>{},
           R"DOC(Generate a Bill of Materials from a .kicad_sch file.

Args:
  schematic_path:          path to .kicad_sch
  output:                  output file path (typically .csv / .tsv; exact
                           shape governed by the format preset's delimiters)

  bom_preset_name:         named BOM preset stored in the project settings to
                           use as the base configuration.  Empty (default)
                           uses the schematic's current settings.  When set,
                           any field/group/sort kwargs left at their defaults
                           (empty list / empty string) inherit from the
                           preset; explicit kwargs override the preset.
  bom_format_preset_name:  named BOM format preset (delimiters, quoting,
                           etc.).  Empty (default) uses schematic settings.

  field_delimiter:         column separator (default: '' = preset default;
                           CLI uses ',')
  string_delimiter:        quote character (default: '' = preset default;
                           CLI uses '"')
  ref_delimiter:           separator between refdes in a grouped row
                           (default: '' = preset default; CLI uses ',')
  ref_range_delimiter:     range character for contiguous refs in a group
                           (default: '' = preset default; CLI uses '-';
                           set to empty to disable range collapsing)
  keep_tabs:               preserve tabs inside field values (default: False)
  keep_line_breaks:        preserve newlines inside field values
                           (default: False)

  fields_ordered:          list[str] of field names defining column order.
                           Default: [] (use preset).  CLI default when no
                           preset is given:
                           ['Reference','Value','Footprint','QUANTITY','DNP']
  fields_labels:           list[str] of column header labels parallel to
                           fields_ordered.  Default: [] (auto / preset).
  fields_group_by:         list[str] of field names to group rows by.
                           Default: [] (no extra grouping; preset rules
                           still apply if a preset is named).
  sort_field:              field name to sort rows by (default: '' = preset
                           default; CLI uses 'Reference')
  sort_asc:                sort ascending (default: True)
  filter_string:           substring filter applied to rows before output
                           (default: '' = no filter)
  exclude_dnp:             drop DNP-marked symbols entirely (default: False)
  group_symbols:           collapse identical symbols into one row with a
                           QUANTITY column (default: True)

  variant_names:           list[str] of design variant names to emit.
                           Default: [] = the default variant only.

Returns a dict:
  ok:           bool       True iff job exited cleanly (exit_code == 0)
  exit_code:    int        raw exit code from JOB dispatch
  messages:     str        reporter output (status / warnings from the run)
  output_paths: list[str]  files actually written, as reported by the JOB

Raises:
  RuntimeError     if no live KIWAY is available (KiCad GUI not running)
)DOC" );
}
