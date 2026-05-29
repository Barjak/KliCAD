/*
 * KliCAD: schematic hierarchy + sheet navigation as klicad_native_hierarchy.*.
 * Pattern B (eeschema kiface-resident) — see klicad_kiface_register.h.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <base_units.h>
#include <eda_base_frame.h>
#include <eda_draw_frame.h>
#include <kiid.h>

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>

#include <wx/string.h>
#include <wx/window.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace
{

KIWAY* find_live_kiway_for_hierarchy()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( holder->HasKiway() )
                return &holder->Kiway();
    }
    return nullptr;
}


SCH_EDIT_FRAME* find_sch_edit_frame_for_hierarchy()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( base && base->GetFrameType() == FRAME_SCH )
            return static_cast<SCH_EDIT_FRAME*>( base );
    }
    return nullptr;
}


SCH_EDIT_FRAME* require_sch_edit_frame()
{
    if( SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_hierarchy() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_hierarchy();

    if( !kiway )
        throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );

    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_hierarchy();

    if( !frame )
        throw std::runtime_error( "failed to obtain SCH_EDIT_FRAME after Player(FRAME_SCH)" );

    return frame;
}


SCHEMATIC& require_loaded_schematic( SCH_EDIT_FRAME* aFrame )
{
    SCHEMATIC& sch = aFrame->Schematic();

    if( !sch.IsValid() )
        throw std::runtime_error( "no schematic loaded — open a project first" );

    return sch;
}


void refresh_sch_canvas( SCH_EDIT_FRAME* aFrame )
{
    if( aFrame && aFrame->GetCanvas() )
        aFrame->GetCanvas()->Refresh();
}


// Metadata dict used by list_sheets / get_current_sheet / walk_hierarchy.
py::dict describe_sheet_path( const SCH_SHEET_PATH& aPath )
{
    py::dict   d;
    SCH_SHEET* last = aPath.Last();
    size_t     size = aPath.size();

    if( !last )
    {
        d[ "uuid" ]        = std::string();
        d[ "name" ]        = std::string();
        d[ "page_number" ] = std::string();
        d[ "parent_uuid" ] = std::string();
        d[ "file_name" ]   = std::string();
        d[ "depth" ]       = 0;
        d[ "path_string" ] = std::string( "/" );
        d[ "path_human" ]  = std::string( "/" );
        d[ "path_kiids" ]  = py::list();
        return d;
    }

    d[ "uuid" ]        = last->m_Uuid.AsStdString();
    d[ "name" ]        = std::string( last->GetName().utf8_str() );
    d[ "page_number" ] = std::string( aPath.GetPageNumber().utf8_str() );
    d[ "file_name" ]   = std::string( last->GetFileName().utf8_str() );
    // P7: synthetic clones no longer exist.  Every SCH_SHEET reported
    // is an on-canvas template.  Per-path slot identity is in the
    // path's `kiids` list below — if a path's leaf KIID differs from
    // the leaf SCH_SHEET's m_Uuid, the path represents slot K>0 of
    // a multi-channel sheet.  is_synthetic is preserved as a boolean
    // for klicad-python wire compatibility but always reads false.
    d[ "is_synthetic" ] = false;
    // SCH_SHEET_PATH starts at the virtual root, so user depth = size-1.
    d[ "depth" ]       = ( size > 0 ) ? static_cast<int>( size - 1 ) : 0;

    if( size >= 2 && aPath.at( size - 2 ) )
        d[ "parent_uuid" ] = aPath.at( size - 2 )->m_Uuid.AsStdString();
    else
        d[ "parent_uuid" ] = std::string();

    d[ "path_string" ] = std::string( aPath.Path().AsString().utf8_str() );
    d[ "path_human" ]  = std::string(
            aPath.PathHumanReadable( true, false, false ).utf8_str() );

    py::list kiids;
    for( size_t i = 0; i < size; ++i )
        kiids.append( aPath.at( i ) ? aPath.at( i )->m_Uuid.AsStdString()
                                    : std::string() );
    d[ "path_kiids" ] = kiids;
    return d;
}


py::list hier_list_sheets()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );

    py::list out;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
        out.append( describe_sheet_path( path ) );

    return out;
}


py::dict hier_get_current_sheet()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    (void) require_loaded_schematic( frame );
    return describe_sheet_path( frame->GetCurrentSheet() );
}


// Descend into a child SCH_SHEET (by KIID) on the current screen.
py::dict hier_push_sheet( const std::string& child_uuid )
{
    SCH_EDIT_FRAME* frame  = require_sch_edit_frame();
    (void) require_loaded_schematic( frame );
    SCH_SCREEN*     screen = frame->GetScreen();

    if( !screen )
        throw std::runtime_error( "current sheet has no active SCH_SCREEN" );

    KIID         target( wxString::FromUTF8( child_uuid.c_str() ) );
    SCH_SHEET*   child = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
    {
        if( item->m_Uuid == target )
        {
            child = static_cast<SCH_SHEET*>( item );
            break;
        }
    }

    if( !child )
    {
        py::dict err;
        err[ "ok" ]    = false;
        err[ "error" ] = std::string( "no child SCH_SHEET with uuid '" )
                         + child_uuid + "' on the current sheet";
        return err;
    }

    SCH_SHEET_PATH newPath = frame->GetCurrentSheet();
    newPath.push_back( child );

    frame->SetCurrentSheet( newPath );
    frame->DisplayCurrentSheet();
    refresh_sch_canvas( frame );

    py::dict d = describe_sheet_path( newPath );
    d[ "ok" ]  = true;
    return d;
}


py::dict hier_pop_sheet()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    (void) require_loaded_schematic( frame );

    SCH_SHEET_PATH newPath = frame->GetCurrentSheet();

    if( newPath.size() <= 1 )
    {
        py::dict err;
        err[ "ok" ]    = false;
        err[ "error" ] = std::string( "already at hierarchy root — nothing to pop" );
        err[ "path" ]  = describe_sheet_path( newPath );
        return err;
    }

    newPath.pop_back();

    frame->SetCurrentSheet( newPath );
    frame->DisplayCurrentSheet();
    refresh_sch_canvas( frame );

    py::dict d = describe_sheet_path( newPath );
    d[ "ok" ]  = true;
    return d;
}


// Jump to an absolute KIID_PATH.  Accepts either the full path (final sheet
// included) or a parent-style path (matching SCH_SYMBOL_INSTANCE.m_Path).
py::dict hier_set_current_sheet( const std::string& path_str )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );

    KIID_PATH kpath = KIID_PATH( wxString::FromUTF8( path_str.c_str() ) );

    std::optional<SCH_SHEET_PATH> target =
            sch.Hierarchy().GetSheetPathByKIIDPath( kpath, true );

    if( !target )
        target = sch.Hierarchy().GetSheetPathByKIIDPath( kpath, false );

    if( !target )
    {
        py::dict err;
        err[ "ok" ]    = false;
        err[ "error" ] = std::string( "no sheet path matches '" ) + path_str + "'";
        return err;
    }

    frame->SetCurrentSheet( *target );
    frame->DisplayCurrentSheet();
    refresh_sch_canvas( frame );

    py::dict d = describe_sheet_path( *target );
    d[ "ok" ]  = true;
    return d;
}


std::string hier_get_sheet_path_string()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    (void) require_loaded_schematic( frame );

    return std::string(
            frame->GetCurrentSheet().PathHumanReadable( true, false, false ).utf8_str() );
}


// Recursive walker for walk_hierarchy.
py::dict walk_node( const SCH_SHEET_PATH& aPath, int aMaxDepth )
{
    py::dict node = describe_sheet_path( aPath );
    py::list children;
    int      curDepth = static_cast<int>( aPath.size() ) - 1;

    SCH_SCREEN* screen = aPath.LastScreen();

    if( ( aMaxDepth < 0 || curDepth < aMaxDepth ) && screen )
    {
        for( SCH_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
        {
            SCH_SHEET_PATH childPath = aPath;
            childPath.push_back( static_cast<SCH_SHEET*>( item ) );
            children.append( walk_node( childPath, aMaxDepth ) );
        }
    }

    node[ "children" ] = children;
    return node;
}


py::dict hier_walk_hierarchy( int max_depth )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );

    SCH_SHEET_PATH rootPath;
    rootPath.push_back( &sch.Root() );

    return walk_node( rootPath, max_depth );
}


// Count SCH_SHEET_PATHs whose LastScreen contains a SCH_SYMBOL with this UUID.
// Shared sub-schematics referenced from N parents contribute N.
int hier_count_instances( const std::string& symbol_uuid )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );
    KIID            target( wxString::FromUTF8( symbol_uuid.c_str() ) );
    int             count = 0;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();
        if( !screen ) continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            if( item->m_Uuid == target )
            {
                ++count;
                break;
            }
        }
    }
    return count;
}


py::list hier_list_sheet_pins( const std::string& sheet_uuid )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );

    KIID       target( wxString::FromUTF8( sheet_uuid.c_str() ) );
    SCH_SHEET* found = nullptr;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();
        if( !screen ) continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
        {
            if( item->m_Uuid == target )
            {
                found = static_cast<SCH_SHEET*>( item );
                break;
            }
        }
        if( found ) break;
    }

    if( !found )
        throw std::runtime_error( std::string( "no SCH_SHEET with uuid '" )
                                  + sheet_uuid + "' in the hierarchy" );

    py::list out;

    for( SCH_SHEET_PIN* pin : found->GetPins() )
    {
        if( !pin ) continue;

        py::dict d;
        d[ "uuid" ] = pin->m_Uuid.AsStdString();
        d[ "name" ] = std::string( pin->GetText().utf8_str() );
        VECTOR2I pos = pin->GetPosition();
        // Use schIUScale (100nm per IU) — not pcbIUScale (1nm per IU); this
        // file is on the schematic side.  Previous version divided by 1e6
        // and reported every pin position 100x too small.
        d[ "x_mm" ] = schIUScale.IUTomm( pos.x );
        d[ "y_mm" ] = schIUScale.IUTomm( pos.y );

        const char* side = "undefined";
        switch( pin->GetSide() )
        {
        case SHEET_SIDE::LEFT:   side = "left";   break;
        case SHEET_SIDE::RIGHT:  side = "right";  break;
        case SHEET_SIDE::TOP:    side = "top";    break;
        case SHEET_SIDE::BOTTOM: side = "bottom"; break;
        default: break;
        }
        d[ "side" ] = std::string( side );

        const char* shape = "unspecified";
        switch( pin->GetShape() )
        {
        case L_INPUT:    shape = "input";    break;
        case L_OUTPUT:   shape = "output";   break;
        case L_BIDI:     shape = "bidi";     break;
        case L_TRISTATE: shape = "tristate"; break;
        default: break;
        }
        d[ "shape" ] = std::string( shape );

        out.append( d );
    }

    return out;
}


// Re-stamp page numbers.  SetInitialPageNumbers asserts that all page
// numbers are empty first (see sch_sheet_path.cpp:1656), so we blank them.
py::dict hier_update_page_numbers()
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = require_loaded_schematic( frame );

    SCH_SHEET_LIST sheets = sch.Hierarchy();

    for( SCH_SHEET_PATH& path : sheets )
        path.SetPageNumber( wxEmptyString );

    sheets.SetInitialPageNumbers();

    frame->SetSheetNumberAndCount();
    frame->DisplayCurrentSheet();
    refresh_sch_canvas( frame );

    py::dict d;
    d[ "ok" ]          = true;
    d[ "sheet_count" ] = static_cast<int>( sheets.size() );
    return d;
}

} // anon


void klicad_register_hierarchy_bindings( py::module_& m )
{
    m.doc() = "KliCAD schematic hierarchy / sheet navigation binding.  "
              "Read SCH_SHEET_LIST, walk it as a tree, and drive the live "
              "SCH_EDIT_FRAME's active SCH_SHEET_PATH stack.";

    m.def( "list_sheets", &hier_list_sheets,
           "Every SCH_SHEET_PATH in the hierarchy, depth-first.  Each entry: "
           "{uuid, name, page_number, parent_uuid, file_name, depth, "
           "path_string, path_human, path_kiids: [uuid...]}." );

    m.def( "get_current_sheet", &hier_get_current_sheet,
           "Metadata dict for the currently-displayed sheet path (same shape "
           "as list_sheets() entries).  Tracks SCH_EDIT_FRAME::GetCurrentSheet()." );

    m.def( "push_sheet", &hier_push_sheet, py::arg( "uuid" ),
           "Descend into a child SCH_SHEET on the current screen by KIID.  "
           "Returns new sheet's metadata + ok=True, or ok=False + error on miss." );

    m.def( "pop_sheet", &hier_pop_sheet,
           "Ascend one level.  Returns ok=False + error if already at root, "
           "else the new current sheet's metadata + ok=True." );

    m.def( "set_current_sheet", &hier_set_current_sheet, py::arg( "path" ),
           "Jump to an absolute KIID_PATH (e.g. '/<root>/<sheet>/<sub>').  "
           "Accepts the full path or a parent path matching "
           "SCH_SYMBOL_INSTANCE.m_Path.  Returns resolved sheet metadata or "
           "ok=False + error if no match." );

    m.def( "get_sheet_path_string", &hier_get_sheet_path_string,
           "Human-readable form of the current path ('/root/sheet/...').  "
           "Backed by SCH_SHEET_PATH::PathHumanReadable." );

    m.def( "walk_hierarchy", &hier_walk_hierarchy, py::arg( "max_depth" ) = -1,
           "Full hierarchy as a nested dict.  Each node has the same fields "
           "as list_sheets entries plus children: [node...].  max_depth >= 0 "
           "caps recursion (0 = root only).  -1 = unlimited." );

    m.def( "count_instances", &hier_count_instances, py::arg( "symbol_uuid" ),
           "Count SCH_SHEET_PATHs containing a SCH_SYMBOL with this UUID.  "
           "In complex hierarchies a shared sub-schematic referenced from N "
           "sheets contributes N." );

    m.def( "list_sheet_pins", &hier_list_sheet_pins, py::arg( "sheet_uuid" ),
           "Hierarchical pins on a SCH_SHEET by KIID.  Each pin: "
           "{uuid, name, x_mm, y_mm, side, shape}.  "
           "side: left|right|top|bottom|undefined.  "
           "shape: input|output|bidi|tristate|unspecified.  "
           "Raises RuntimeError if no SCH_SHEET with that uuid exists." );

    m.def( "update_page_numbers", &hier_update_page_numbers,
           "Reassign all sheet page numbers via "
           "SCH_SHEET_LIST::SetInitialPageNumbers (blanks existing numbers "
           "first since SetInitialPageNumbers asserts they're empty).  Useful "
           "after add/remove-sheet edits.  Returns {ok: True, sheet_count: int}." );
}
