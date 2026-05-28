/*
 * KliCAD subsystem binding: schematic ratsnest spec source.
 *
 * Exposes klicad_native_ratsnest.set_spec(netlist_string) which replaces the
 * schematic's ratsnest layer (SCH_RATSNEST_ITEM) with edges derived from an
 * external KiCad-sexpr netlist (the format produced by
 * `kicad-cli sch export netlist`).  This is the M2 entry point: the M1
 * builder (SCH_RATSNEST_BUILDER::BuildFrom) derives edges from the in-memory
 * CONNECTION_GRAPH; M2 lets a caller supply a *spec* netlist directly so the
 * ratsnest shows what the user *should* draw rather than what's already
 * connected on the canvas.
 *
 * Algorithm:
 *   1. Parse the netlist string via SEXPR::PARSER.
 *   2. Walk top-level (net (name "...") (node (ref "...") (pin "..."))) records.
 *   3. For each (ref, pin) in a net, look up the world-coordinate position of
 *      the pin on the schematic's placed symbols (walks the whole hierarchy).
 *   4. For each net with >= 2 resolved pins, append (N-1) chain edges
 *      between consecutive pins to the SCH_RATSNEST_ITEM.  Chain (not MST)
 *      because it's the simplest visually-acceptable spanning tree; the M1
 *      builder uses Kruskal+Delaunay over CONNECTION_GRAPH pins, but for
 *      spec-driven edges a chain is sufficient and easier to verify.
 *   5. Mark the ratsnest item dirty on the active SCH_VIEW so the canvas
 *      repaints.
 *
 * Pins listed in the spec netlist that don't exist on the current schematic
 * (e.g. a symbol that wasn't placed yet) are returned in the "missing_pins"
 * list; the binding does not throw on those.
 *
 * Pattern (follows BINDING_PATTERN.md and the bindings_schematic_state.cpp
 * template).  Kiface-resident: registered from klicad_kiface_register.cpp,
 * NOT via PYBIND11_EMBEDDED_MODULE — see klicad_kiface_register.h.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <api/klicad_kiface_register.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <eda_draw_frame.h>
#include <kiid.h>
#include <math/vector2d.h>

#include <sch_edit_frame.h>
#include <sch_base_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_sheet_path.h>
#include <sch_pin.h>
#include <sch_ratsnest_item.h>
#include <sch_view.h>

#include <view/view.h>
#include <view/view_item.h>

#include <sexpr/sexpr.h>
#include <sexpr/sexpr_parser.h>

#include <wx/string.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace
{

// ──────────────────────────────────────────────────────────────────────────
// Frame discovery — clone of the patterns used in bindings_schematic_state
// and bindings_hierarchy.  Each binding TU keeps its own copy so that no
// symbol resolution leaks across translation units (the eeschema kiface is
// dlopen'd lazily and the typeinfo is module-local).
// ──────────────────────────────────────────────────────────────────────────
KIWAY* find_live_kiway_for_ratsnest()
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


SCH_EDIT_FRAME* find_sch_edit_frame_for_ratsnest()
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
    if( SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_ratsnest() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_ratsnest();

    if( !kiway )
        throw std::runtime_error(
            "no live KIWAY — is KiCad's GUI running? "
            "(klicad_native_ratsnest needs an SCH_EDIT_FRAME)" );

    kiway->Player( FRAME_SCH, true );

    SCH_EDIT_FRAME* frame = find_sch_edit_frame_for_ratsnest();

    if( !frame )
        throw std::runtime_error(
            "failed to obtain SCH_EDIT_FRAME after KIWAY::Player(FRAME_SCH, true)" );

    return frame;
}


// ──────────────────────────────────────────────────────────────────────────
// Pin position index — build a single hash table from (ref, pin_number) to
// world-coordinate VECTOR2I for the entire schematic hierarchy.  Each spec
// net then becomes O(K) lookups instead of an O(NxK) walk per (ref, pin).
// ──────────────────────────────────────────────────────────────────────────
struct PinKey
{
    wxString ref;
    wxString pin;

    bool operator==( const PinKey& aOther ) const
    {
        return ref == aOther.ref && pin == aOther.pin;
    }
};


struct PinKeyHash
{
    std::size_t operator()( const PinKey& aKey ) const noexcept
    {
        std::size_t h1 = std::hash<std::string>{}( aKey.ref.ToStdString() );
        std::size_t h2 = std::hash<std::string>{}( aKey.pin.ToStdString() );
        return h1 ^ ( h2 + 0x9e3779b9 + ( h1 << 6 ) + ( h1 >> 2 ) );
    }
};


using PinPosIndex = std::unordered_map<PinKey, VECTOR2I, PinKeyHash>;


PinPosIndex build_pin_position_index( SCHEMATIC& aSch )
{
    PinPosIndex index;

    for( const SCH_SHEET_PATH& path : aSch.Hierarchy() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            wxString    ref = sym->GetRef( &path );

            for( SCH_PIN* pin : sym->GetPins( &path ) )
            {
                PinKey key{ ref, pin->GetNumber() };
                // First-wins: hierarchical instances of the same ref-des
                // would otherwise overwrite.  In a flat (or normalized
                // hierarchical) schematic refs are unique, so first-wins
                // is the same as last-wins.  Per-instance disambiguation
                // is M3 territory (see schematic-ratsnest.md §D.2).
                index.emplace( std::move( key ), pin->GetPosition() );
            }
        }
    }

    return index;
}


// ──────────────────────────────────────────────────────────────────────────
// Netlist sexpr walking.
//
// KiCad-cli `sch export netlist` emits a sexpr document of the form:
//
//   (export (version "E")
//     (design ...)
//     (components ...)
//     (libparts ...)
//     (nets
//       (net (code "1") (name "/SIG") (class "Default")
//            (node (ref "R1") (pin "1") (pintype "passive"))
//            (node (ref "C1") (pin "2") (pintype "passive")))
//       ...))
//
// We walk recursively for any (net ...) list (which we then identify by
// having a (name "...") child and one or more (node ...) children).  This
// tolerates the (nets ...) wrapper without depending on it.
// ──────────────────────────────────────────────────────────────────────────

// Read an atom child as a string regardless of whether it was serialized as a
// quoted string or a bare symbol.  KiCad's netlist writer quotes net names
// but emits component refs as bare symbols when they're identifier-safe.
bool sexpr_atom_to_string( const SEXPR::SEXPR* aNode, std::string& aOut )
{
    if( !aNode )
        return false;

    if( aNode->IsString() )
    {
        aOut = aNode->GetString();
        return true;
    }

    if( aNode->IsSymbol() )
    {
        aOut = aNode->GetSymbol();
        return true;
    }

    return false;
}


// Get the symbol-name head of a list, e.g. "net" for (net (code "1") ...).
// Returns empty string if @p aList isn't a list or its first child isn't a
// symbol atom.
std::string list_head( const SEXPR::SEXPR* aList )
{
    if( !aList || !aList->IsList() || aList->GetNumberOfChildren() == 0 )
        return std::string();

    const SEXPR::SEXPR* head = aList->GetChild( 0 );

    if( head && head->IsSymbol() )
        return head->GetSymbol();

    return std::string();
}


// Find the first child list of @p aParent whose head symbol equals @p aHead.
// Returns nullptr if not found.
const SEXPR::SEXPR* find_child( const SEXPR::SEXPR* aParent, const char* aHead )
{
    if( !aParent || !aParent->IsList() )
        return nullptr;

    for( std::size_t i = 0; i < aParent->GetNumberOfChildren(); ++i )
    {
        const SEXPR::SEXPR* c = aParent->GetChild( i );

        if( list_head( c ) == aHead )
            return c;
    }

    return nullptr;
}


// A single (ref, pin) pair extracted from a (node ...) sub-form.
struct SpecNode
{
    wxString ref;
    wxString pin;
};


// A single (net (name ...) (node ...) ...) record.
struct SpecNet
{
    wxString              name;
    std::vector<SpecNode> nodes;
};


// Try to parse @p aList as a (net ...) record.  Returns true and fills
// @p aOut on success; returns false otherwise.  Tolerant of unknown fields.
bool parse_net_record( const SEXPR::SEXPR* aList, SpecNet& aOut )
{
    if( list_head( aList ) != "net" )
        return false;

    const SEXPR::SEXPR* name_node = find_child( aList, "name" );

    if( !name_node || name_node->GetNumberOfChildren() < 2 )
        return false;

    std::string name_str;

    if( !sexpr_atom_to_string( name_node->GetChild( 1 ), name_str ) )
        return false;

    aOut.name = wxString::FromUTF8( name_str.c_str() );
    aOut.nodes.clear();

    for( std::size_t i = 0; i < aList->GetNumberOfChildren(); ++i )
    {
        const SEXPR::SEXPR* child = aList->GetChild( i );

        if( list_head( child ) != "node" )
            continue;

        const SEXPR::SEXPR* ref_node = find_child( child, "ref" );
        const SEXPR::SEXPR* pin_node = find_child( child, "pin" );

        if( !ref_node || ref_node->GetNumberOfChildren() < 2 )
            continue;
        if( !pin_node || pin_node->GetNumberOfChildren() < 2 )
            continue;

        std::string ref_str;
        std::string pin_str;

        if( !sexpr_atom_to_string( ref_node->GetChild( 1 ), ref_str ) )
            continue;
        if( !sexpr_atom_to_string( pin_node->GetChild( 1 ), pin_str ) )
            continue;

        SpecNode sn;
        sn.ref = wxString::FromUTF8( ref_str.c_str() );
        sn.pin = wxString::FromUTF8( pin_str.c_str() );
        aOut.nodes.push_back( std::move( sn ) );
    }

    return true;
}


// Recursively walk the sexpr tree, accumulating every (net ...) record we
// find.  KiCad's netlist puts them inside a (nets ...) wrapper but we don't
// depend on that — any depth works.
void collect_nets_recursive( const SEXPR::SEXPR* aNode, std::vector<SpecNet>& aOut )
{
    if( !aNode || !aNode->IsList() )
        return;

    SpecNet rec;

    if( parse_net_record( aNode, rec ) )
    {
        aOut.push_back( std::move( rec ) );
        // A (net ...) record's children are (code ...)/(name ...)/(node ...),
        // none of which are themselves (net ...) records; no need to recurse
        // into them.
        return;
    }

    for( std::size_t i = 0; i < aNode->GetNumberOfChildren(); ++i )
        collect_nets_recursive( aNode->GetChild( i ), aOut );
}


// ──────────────────────────────────────────────────────────────────────────
// set_spec — main entry point.
// ──────────────────────────────────────────────────────────────────────────
py::dict ratsnest_set_spec( const std::string& netlist_string )
{
    SCH_EDIT_FRAME* frame = require_sch_edit_frame();
    SCHEMATIC&      sch   = frame->Schematic();

    // 1. Parse netlist.
    std::vector<SpecNet> spec_nets;

    if( !netlist_string.empty() )
    {
        SEXPR::PARSER parser;
        std::unique_ptr<SEXPR::SEXPR> root;

        try
        {
            root = parser.Parse( netlist_string );
        }
        catch( const std::exception& e )
        {
            throw std::runtime_error(
                std::string( "set_spec: failed to parse netlist sexpr: " ) + e.what() );
        }
        catch( ... )
        {
            throw std::runtime_error( "set_spec: failed to parse netlist sexpr" );
        }

        collect_nets_recursive( root.get(), spec_nets );
    }

    // 2. Build pin-position index over the live schematic.
    PinPosIndex pin_index = build_pin_position_index( sch );

    // 3. Acquire (or lazily allocate) the schematic's ratsnest item, and
    // replace its edge list with spec-derived edges.
    SCH_RATSNEST_ITEM* rats = sch.GetRatsnestItem();

    if( !rats )
    {
        // SCHEMATIC lazily allocates m_ratsnest inside RecalculateConnections
        // (see schematic.cpp:2084).  In normal eeschema-frame use, project
        // load triggers RecalculateConnections so the item is always present
        // by the time set_spec is callable.  If we still don't have one, the
        // schematic is in a state where ratsnest rendering won't work
        // anyway — bail with a clear message.
        throw std::runtime_error(
            "set_spec: SCH_RATSNEST_ITEM not available on this SCHEMATIC; "
            "connectivity engine has not yet been initialized.  Open a "
            "schematic file or trigger an SCH_COMMIT to populate the "
            "ratsnest item before calling set_spec." );
    }

    rats->ClearEdges();

    int edges_placed   = 0;
    int nets_resolved  = 0;
    std::vector<std::string> missing_pins;

    for( const SpecNet& net : spec_nets )
    {
        // Collect resolved positions for this net's pins.  Pins not in the
        // index get logged to missing_pins.
        std::vector<VECTOR2I> resolved;
        resolved.reserve( net.nodes.size() );

        for( const SpecNode& node : net.nodes )
        {
            PinKey key{ node.ref, node.pin };
            auto   it = pin_index.find( key );

            if( it == pin_index.end() )
            {
                std::string missing = std::string( node.ref.utf8_str() )
                                      + "."
                                      + std::string( node.pin.utf8_str() );
                missing_pins.push_back( std::move( missing ) );
                continue;
            }

            resolved.push_back( it->second );
        }

        if( resolved.size() < 2 )
            continue;

        // Chain-pairwise: (N-1) edges connecting resolved[i] → resolved[i+1].
        // MST is a v2 polish (see schematic-ratsnest.md §B.3); the chain is
        // a valid spanning tree and avoids dragging Delaunay/Kruskal into a
        // path that's already covered by SCH_RATSNEST_BUILDER for the
        // self-rats case.
        for( std::size_t i = 1; i < resolved.size(); ++i )
        {
            rats->AddEdge( resolved[ i - 1 ], resolved[ i ], net.name );
            ++edges_placed;
        }

        ++nets_resolved;
    }

    // 4. Mark the ratsnest item dirty on the active VIEW so the canvas
    // repaints.  The item is registered as a permanent overlay in
    // SCH_VIEW::DisplaySheet (sch_view.cpp:175) — we just need to tell
    // the view its geometry changed.
    if( frame->GetCanvas() && frame->GetCanvas()->GetView() )
        frame->GetCanvas()->GetView()->Update( rats, KIGFX::GEOMETRY );

    if( frame->GetCanvas() )
        frame->GetCanvas()->Refresh();

    py::dict result;
    result[ "ok" ]            = true;
    result[ "edges_placed" ]  = edges_placed;
    result[ "nets_resolved" ] = nets_resolved;
    result[ "missing_pins" ]  = missing_pins;
    return result;
}

} // anon


void klicad_register_ratsnest_bindings( py::module_& m )
{
    m.doc() = "KliCAD schematic ratsnest spec source binding (kiface-loaded). "
              "Drives SCH_RATSNEST_ITEM with edges derived from an external "
              "KiCad-sexpr netlist, replacing the M1 connection-graph-derived "
              "self-rats with spec-driven rats.";

    m.def( "set_spec", &ratsnest_set_spec,
           py::arg( "netlist_string" ),
           R"DOC(Replace the schematic ratsnest layer with edges derived from a
KiCad-sexpr netlist string (the format produced by
`kicad-cli sch export netlist`).

For each (net ...) record in the netlist, the binding:
  1. Resolves each (node (ref R) (pin P)) to the world-coordinate position
     of that pin on the schematic's placed symbols.
  2. If 2+ pins resolve, appends (N-1) chain edges to the ratsnest layer.
  3. If a pin can't be found on the schematic (e.g. its symbol hasn't been
     placed yet), records "<ref>.<pin>" in the returned missing_pins list
     and continues without raising.

Returns a dict:
  ok            (bool): always True on a successful call.
  edges_placed  (int):  total number of ratsnest edges added.
  nets_resolved (int):  number of nets that contributed at least one edge.
  missing_pins  (list[str]): "<ref>.<pin>" entries for spec pins not found
                              on the schematic.

The ratsnest item is repainted automatically (SCH_VIEW::Update + canvas
Refresh).  Calling set_spec again replaces the previous edge list.
)DOC" );
}
