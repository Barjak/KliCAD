/*
 * KliCAD subsystem binding: PCB board stackup (CRUD).
 *
 * Exposes BOARD::GetDesignSettings().GetStackupDescriptor() — the
 * BOARD_STACKUP descriptor with its BOARD_STACKUP_ITEM list — as
 * klicad_native_stackup.*  Read the layer list, list predefined
 * dielectric materials, and mutate per-layer thickness / material /
 * color.  rebuild_stackup() calls BOARD_STACKUP::SynchronizeWithBoard
 * which re-derives the list from BOARD_DESIGN_SETTINGS (adds missing
 * layers, removes disabled ones).
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * pcbnew/api/klicad_kiface_register.cpp::klicad_register_pcbnew_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer
 * runs after py::initialize_interpreter, and PyImport_AppendInittab
 * refuses post-init.
 *
 * Frame discovery mirrors bindings_pcb_state.cpp: walk
 * wxTopLevelWindows for EDA_BASE_FRAME::GetFrameType() ==
 * FRAME_PCB_EDITOR (we cannot dynamic_cast<PCB_EDIT_FRAME*> across the
 * kiface boundary), then static_cast.  Spawn via
 * KIWAY::Player(FRAME_PCB_EDITOR, true) if no PCB editor is up.
 *
 * Mutations wrap the BOARD in a BOARD_COMMIT — BOARD_STACKUP lives
 * inside BOARD_DESIGN_SETTINGS which is owned by the BOARD, so
 * commit.Modify(board) + commit.Push() is the correct undo unit.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <base_units.h>
#include <eda_base_frame.h>
#include <layer_ids.h>

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <pcb_edit_frame.h>
#include <pcb_base_frame.h>
#include <pcb_draw_panel_gal.h>

#include <board_stackup_manager/board_stackup.h>
#include <board_stackup_manager/dielectric_material.h>
#include <board_stackup_manager/stackup_predefined_prms.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// BOARD_STACKUP_ITEM stores thickness in PCB internal units (nanometers).
// Use pcbIUScale for round-trip conversion so this code keeps working if
// the IU constant ever changes.
inline double iu_to_mm_stackup( int iu )
{
    return pcbIUScale.IUTomm( iu );
}

inline int mm_to_iu_stackup( double mm )
{
    return pcbIUScale.mmToIU( mm );
}


// Per-TU helper name (unique vs find_live_kiway, find_live_kiway_for_*).
KIWAY* find_live_kiway_for_stackup()
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


// Walk wxTopLevelWindows for a PCB_EDIT_FRAME.  Identify via
// EDA_BASE_FRAME::GetFrameType() because dynamic_cast<PCB_EDIT_FRAME*>
// isn't safe across the kiface boundary.
PCB_EDIT_FRAME* find_pcb_edit_frame_for_stackup()
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
// std::runtime_error on any failure.
PCB_EDIT_FRAME* require_pcb_frame_for_stackup()
{
    if( PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_stackup() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_stackup();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(stackup needs to spawn the pcbnew frame)" );
    }

    kiway->Player( FRAME_PCB_EDITOR, true );

    PCB_EDIT_FRAME* frame = find_pcb_edit_frame_for_stackup();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain PCB_EDIT_FRAME after "
            "KIWAY::Player(FRAME_PCB_EDITOR, true)" );
    }

    return frame;
}


// Refresh the PCB canvas after a mutation so the user sees the change.
void refresh_pcb_canvas_for_stackup( PCB_EDIT_FRAME* aFrame )
{
    if( aFrame && aFrame->GetCanvas() )
        aFrame->GetCanvas()->Refresh();
}


// Stringify a BOARD_STACKUP_ITEM_TYPE for the Python side.  These are
// stable strings (used in dicts) — do not translate.
const char* stackup_item_type_name( BOARD_STACKUP_ITEM_TYPE aType )
{
    switch( aType )
    {
    case BS_ITEM_TYPE_COPPER:      return "copper";
    case BS_ITEM_TYPE_DIELECTRIC:  return "dielectric";
    case BS_ITEM_TYPE_SOLDERPASTE: return "solderpaste";
    case BS_ITEM_TYPE_SOLDERMASK:  return "soldermask";
    case BS_ITEM_TYPE_SILKSCREEN:  return "silkscreen";
    case BS_ITEM_TYPE_UNDEFINED:
    default:                       return "undefined";
    }
}


// Convert a BOARD_STACKUP_ITEM into a serializable py::dict.  EpsilonR
// and LossTangent are only meaningful for dielectric items (dielectric
// and soldermask) — for copper / silkscreen / solderpaste we still emit
// the numeric value (0.0 default), but the meaning is undefined.
py::dict stackup_item_to_dict( const BOARD_STACKUP_ITEM* aItem )
{
    py::dict d;

    d[ "name" ]         = std::string( aItem->GetLayerName().utf8_str() );
    d[ "type" ]         = stackup_item_type_name( aItem->GetType() );
    d[ "type_name" ]    = std::string( aItem->GetTypeName().utf8_str() );
    d[ "thickness_mm" ] = iu_to_mm_stackup( aItem->GetThickness() );
    d[ "material" ]     = std::string( aItem->GetMaterial().utf8_str() );
    d[ "color" ]        = std::string( aItem->GetColor().utf8_str() );

    if( aItem->HasEpsilonRValue() )
        d[ "dielectric_constant" ] = aItem->GetEpsilonR();
    else
        d[ "dielectric_constant" ] = 0.0;

    if( aItem->HasLossTangentValue() )
        d[ "dielectric_loss" ] = aItem->GetLossTangent();
    else
        d[ "dielectric_loss" ] = 0.0;

    // Bonus: expose the PCB layer id for items that map to a real
    // board layer (copper, silk, mask, paste).  Dielectric items
    // return UNDEFINED_LAYER here — useful for Python-side filtering.
    d[ "pcb_layer_id" ]  = static_cast<int>( aItem->GetBrdLayerId() );
    d[ "sublayer_count" ] = aItem->GetSublayersCount();

    return d;
}


// Find the first BOARD_STACKUP_ITEM whose LayerName matches.  The
// stackup uses BOARD::GetLayerName() output as its display name (see
// BOARD_STACKUP::SynchronizeWithBoard), so canonical names like
// "F.Cu" / "B.Cu" / "F.SilkS" / "F.Mask" work.  Dielectric layers
// don't have a board layer name — match against TypeName instead
// (e.g. "core", "prepreg") or formatted "Dielectric N".
BOARD_STACKUP_ITEM* find_stackup_layer( BOARD_STACKUP& aStackup, const std::string& aName )
{
    wxString wxname = wxString::FromUTF8( aName.c_str() );

    for( BOARD_STACKUP_ITEM* item : aStackup.GetList() )
    {
        if( item->GetLayerName() == wxname )
            return item;
    }

    // Fallback: dielectric items often have empty LayerName; allow the
    // caller to specify the formatted dielectric layer name produced
    // by FormatDielectricLayerName() ("Dielectric 1", "Dielectric 2"…).
    for( BOARD_STACKUP_ITEM* item : aStackup.GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC
            && item->FormatDielectricLayerName() == wxname )
        {
            return item;
        }
    }

    return nullptr;
}


// ──────────────────────────────────────────────────────────────────────────
// get_stackup
// ──────────────────────────────────────────────────────────────────────────
py::list stackup_get_stackup()
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_stackup();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    const BOARD_STACKUP& stackup = board->GetDesignSettings().GetStackupDescriptor();

    py::list out;

    for( const BOARD_STACKUP_ITEM* item : stackup.GetList() )
        out.append( stackup_item_to_dict( item ) );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_predefined_materials
// ──────────────────────────────────────────────────────────────────────────
py::list stackup_list_predefined_materials()
{
    // DIELECTRIC_SUBSTRATE_LIST seeds itself with the predefined
    // material table on construction (see dielectric_material.cpp).
    // We expose dielectric + soldermask + silkscreen as one flat list,
    // since callers typically want "what string can I pass to
    // set_layer_material()" regardless of layer kind.
    py::list out;

    auto append_list = [&out]( DIELECTRIC_SUBSTRATE_LIST::DL_MATERIAL_LIST_TYPE aType )
    {
        DIELECTRIC_SUBSTRATE_LIST list( aType );

        for( int i = 0; i < list.GetCount(); ++i )
        {
            if( DIELECTRIC_SUBSTRATE* s = list.GetSubstrate( i ) )
                out.append( std::string( s->m_Name.utf8_str() ) );
        }
    };

    append_list( DIELECTRIC_SUBSTRATE_LIST::DL_MATERIAL_DIELECTRIC );
    append_list( DIELECTRIC_SUBSTRATE_LIST::DL_MATERIAL_SOLDERMASK );
    append_list( DIELECTRIC_SUBSTRATE_LIST::DL_MATERIAL_SILKSCREEN );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_predefined_stackups
// ──────────────────────────────────────────────────────────────────────────
py::list stackup_list_predefined_stackups()
{
    // KiCad doesn't ship a fixed table of named preset stackups; the
    // closest is BOARD_STACKUP::BuildDefaultStackupList(nullptr, N)
    // which derives a default stackup for N copper layers.  Expose the
    // common copper-layer counts here so callers can pick one without
    // poking BOARD_DESIGN_SETTINGS first.
    py::list out;

    const int counts[] = { 1, 2, 4, 6, 8, 10, 12, 16, 32 };

    for( int n : counts )
    {
        BOARD_STACKUP probe;
        probe.BuildDefaultStackupList( nullptr, n );

        py::dict d;
        d[ "name" ]         = std::string( "default_" ) + std::to_string( n ) + "_layer";
        d[ "copper_layers" ] = n;
        d[ "layer_count" ]  = probe.GetCount();
        out.append( d );

        probe.RemoveAll();
    }

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// set_layer_thickness
// ──────────────────────────────────────────────────────────────────────────
py::dict stackup_set_layer_thickness( const std::string& layer_name, double thickness_mm )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_stackup();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    BOARD_STACKUP&      stackup = board->GetDesignSettings().GetStackupDescriptor();
    BOARD_STACKUP_ITEM* item    = find_stackup_layer( stackup, layer_name );

    py::dict result;

    if( !item )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "unknown stackup layer '" ) + layer_name + "'";
        return result;
    }

    if( !item->IsThicknessEditable() )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "thickness is not editable for layer '" )
                            + layer_name + "' (type "
                            + stackup_item_type_name( item->GetType() ) + ")";
        return result;
    }

    int new_iu = mm_to_iu_stackup( thickness_mm );

    {
        BOARD_COMMIT commit( frame );
        commit.Modify( board );
        item->SetThickness( new_iu );
        commit.Push( wxT( "KliCAD: set_layer_thickness" ) );
    }

    refresh_pcb_canvas_for_stackup( frame );

    result[ "ok" ]           = true;
    result[ "layer" ]        = layer_name;
    result[ "thickness_mm" ] = iu_to_mm_stackup( item->GetThickness() );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// set_layer_material
// ──────────────────────────────────────────────────────────────────────────
py::dict stackup_set_layer_material( const std::string& layer_name, const std::string& material )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_stackup();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    BOARD_STACKUP&      stackup = board->GetDesignSettings().GetStackupDescriptor();
    BOARD_STACKUP_ITEM* item    = find_stackup_layer( stackup, layer_name );

    py::dict result;

    if( !item )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "unknown stackup layer '" ) + layer_name + "'";
        return result;
    }

    if( !item->IsMaterialEditable() )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "material is not editable for layer '" )
                            + layer_name + "' (type "
                            + stackup_item_type_name( item->GetType() ) + ")";
        return result;
    }

    {
        BOARD_COMMIT commit( frame );
        commit.Modify( board );
        item->SetMaterial( wxString::FromUTF8( material.c_str() ) );
        commit.Push( wxT( "KliCAD: set_layer_material" ) );
    }

    refresh_pcb_canvas_for_stackup( frame );

    result[ "ok" ]       = true;
    result[ "layer" ]    = layer_name;
    result[ "material" ] = std::string( item->GetMaterial().utf8_str() );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// set_layer_color
// ──────────────────────────────────────────────────────────────────────────
py::dict stackup_set_layer_color( const std::string& layer_name, const std::string& color )
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_stackup();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    BOARD_STACKUP&      stackup = board->GetDesignSettings().GetStackupDescriptor();
    BOARD_STACKUP_ITEM* item    = find_stackup_layer( stackup, layer_name );

    py::dict result;

    if( !item )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "unknown stackup layer '" ) + layer_name + "'";
        return result;
    }

    if( !item->IsColorEditable() )
    {
        result[ "ok" ]    = false;
        result[ "error" ] = std::string( "color is not editable for layer '" )
                            + layer_name + "' (type "
                            + stackup_item_type_name( item->GetType() ) + ")";
        return result;
    }

    {
        BOARD_COMMIT commit( frame );
        commit.Modify( board );
        item->SetColor( wxString::FromUTF8( color.c_str() ) );
        commit.Push( wxT( "KliCAD: set_layer_color" ) );
    }

    refresh_pcb_canvas_for_stackup( frame );

    result[ "ok" ]    = true;
    result[ "layer" ] = layer_name;
    result[ "color" ] = std::string( item->GetColor().utf8_str() );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// rebuild_stackup
// ──────────────────────────────────────────────────────────────────────────
py::dict stackup_rebuild_stackup()
{
    PCB_EDIT_FRAME* frame = require_pcb_frame_for_stackup();
    BOARD*          board = frame->GetBoard();

    if( !board )
        throw std::runtime_error( "PCB_EDIT_FRAME has no active BOARD" );

    BOARD_DESIGN_SETTINGS& settings = board->GetDesignSettings();
    BOARD_STACKUP&         stackup  = settings.GetStackupDescriptor();

    bool changed;

    {
        BOARD_COMMIT commit( frame );
        commit.Modify( board );
        changed = stackup.SynchronizeWithBoard( &settings );
        commit.Push( wxT( "KliCAD: rebuild_stackup" ) );
    }

    refresh_pcb_canvas_for_stackup( frame );

    py::dict result;
    result[ "ok" ]          = true;
    result[ "changed" ]     = changed;
    result[ "layer_count" ] = stackup.GetCount();
    return result;
}

} // anon


// Registered at kiface-load time — see klicad_kiface_register.h for why
// PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_stackup_bindings( py::module_& m )
{
    m.doc() = "KliCAD board stackup binding — CRUD over "
              "BOARD::GetDesignSettings().GetStackupDescriptor() (the "
              "BOARD_STACKUP item list).  Read the full per-layer "
              "stackup, list predefined dielectric materials and "
              "default stackup templates, and mutate thickness / "
              "material / color on individual layers.  Mutations wrap "
              "in a BOARD_COMMIT for undo support.";

    m.def( "get_stackup", &stackup_get_stackup,
           R"DOC(Return the full BOARD_STACKUP item list on the active BOARD.

Each list entry is a dict with:
  name (str)               - LayerName as shown in the stackup manager
                             (e.g. 'F.Cu', 'F.SilkS', empty for many
                             dielectric items)
  type (str)               - 'copper', 'dielectric', 'solderpaste',
                             'soldermask', 'silkscreen', 'undefined'
  type_name (str)          - underlying KiCad type label (e.g. 'core',
                             'prepreg', 'copper', 'Top Solder Mask')
  thickness_mm (float)     - physical thickness, millimeters
  material (str)           - material name (e.g. 'FR4', 'Epoxy', or
                             'Not specified')
  color (str)              - color string (canonical name or '#rrggbbaa')
  dielectric_constant (float) - epsilon_r; 0.0 if not meaningful
  dielectric_loss (float)  - loss tangent (tanD); 0.0 if not meaningful
  pcb_layer_id (int)       - PCB_LAYER_ID enum value, or -1 (UNDEFINED_LAYER)
                             for dielectric items
  sublayer_count (int)     - dielectric sublayer count (>=1)
)DOC" );

    m.def( "list_predefined_materials", &stackup_list_predefined_materials,
           R"DOC(Return the union of predefined dielectric, soldermask, and silkscreen material names.

These are the strings BOARD_STACKUP_ITEM::SetMaterial accepts as
canonical material names — e.g. 'FR4', 'Polyimide', 'Epoxy', 'Liquid
Photo'.  Note: 'Not specified' appears in each sub-list (it's the
KiCad convention for "no material specified").  Callers may pass
arbitrary strings to set_layer_material; predefined names are simply
what the stackup editor offers in its dropdown.
)DOC" );

    m.def( "list_predefined_stackups", &stackup_list_predefined_stackups,
           R"DOC(Return a list of default stackup templates.

Each item: {name (str), copper_layers (int), layer_count (int)}.
KiCad doesn't ship named preset stackups; this enumerates the default
stackups BOARD_STACKUP::BuildDefaultStackupList produces for common
copper-layer counts (1/2/4/6/8/10/12/16/32).  Use it to inform a UI
chooser; rebuild_stackup() applies the active stackup setting back
onto the board.
)DOC" );

    m.def( "set_layer_thickness", &stackup_set_layer_thickness,
           py::arg( "layer_name" ),
           py::arg( "thickness_mm" ),
           R"DOC(Set the physical thickness of a stackup layer (millimeters).

layer_name: matches BOARD_STACKUP_ITEM::GetLayerName (canonical names
            like 'F.Cu', 'B.Cu', 'F.SilkS').  For dielectric layers,
            either the formatted name ('Dielectric 1', 'Dielectric 2',
            …) or the raw LayerName works.
thickness_mm: layer thickness in mm; converted to KiCad PCB internal
              units (nanometers).

Returns {ok: bool, layer: str, thickness_mm: float, error?: str}.
Returns ok=False with an 'error' field if the layer isn't found or
the layer type doesn't permit thickness editing.
)DOC" );

    m.def( "set_layer_material", &stackup_set_layer_material,
           py::arg( "layer_name" ),
           py::arg( "material" ),
           R"DOC(Set the material name on a stackup layer.

layer_name: see set_layer_thickness.
material: material string (predefined names from
          list_predefined_materials() are recommended but arbitrary
          strings are accepted).

Returns {ok: bool, layer: str, material: str, error?: str}.
Returns ok=False if the layer isn't found or its type doesn't permit
material editing (copper layers, paste layers).
)DOC" );

    m.def( "set_layer_color", &stackup_set_layer_color,
           py::arg( "layer_name" ),
           py::arg( "color" ),
           R"DOC(Set the color of a stackup layer.

layer_name: see set_layer_thickness.
color: a canonical color name acceptable in .gbrjob files (e.g. 'Green',
       'Red', 'White') OR an HTML hex string '#rrggbbaa'.

Returns {ok: bool, layer: str, color: str, error?: str}.
Returns ok=False if the layer isn't found or its type doesn't permit
color editing (copper / dielectric core layers).
)DOC" );

    m.def( "rebuild_stackup", &stackup_rebuild_stackup,
           R"DOC(Resynchronize the BOARD_STACKUP with the BOARD_DESIGN_SETTINGS.

Calls BOARD_STACKUP::SynchronizeWithBoard, which adds stackup items
for any newly-enabled board layers and removes items for disabled
layers.  Use after toggling copper layer counts or enabling/disabling
silk/mask/paste layers from elsewhere.

Returns {ok: bool, changed: bool, layer_count: int}.  'changed' is
True if SynchronizeWithBoard added or removed any items.
)DOC" );
}
