/*
 * KliCAD subsystem binding: klicad_native_diff — semantic .kicad_sch /
 * .kicad_pcb file diff, keyed by item UUID (KIID).
 *
 *   diff_sch / diff_pcb (path_a, path_b)         -> full added/removed/modified
 *   diff_summary_sch / diff_summary_pcb (...)    -> counts only; faster
 *
 * Return shape:
 *   { ok, summary{added_count, removed_count, modified_count, total_a,
 *     total_b, unchanged, by_type{type: {added, removed, modified, total_a,
 *     total_b}}}, unchanged_count, added[], removed[], modified[], anomalies[] }
 *
 * Items keyed by `(uuid "...")` payload.  Items without a uuid (file headers,
 * lib_symbols cache, etc.) are skipped.
 *
 * Why we parse the s-expr text directly instead of going through SCH_IO_MGR /
 * PCB_IO_MGR as the design brief originally suggested: those managers and
 * every concrete IO plugin (SCH_IO_KICAD_SEXPR, PCB_IO_KICAD_SEXPR ...) live
 * INSIDE the eeschema / pcbnew kifaces.  pcb_io_mgr.cpp is compiled into the
 * static pcbcommon helper that links only into pcbnew kiface; sch_io_mgr.cpp
 * is compiled only into eeschema kiface.  This Pattern A binding lives in
 * libkicommon shared and cannot reference them at link time (same constraint
 * bindings_io_discovery.cpp documents).  The .kicad_sch / .kicad_pcb formats
 * carry every diffable item with an explicit `(uuid "...")` token — exactly
 * the stable key the diff needs.  No kiface needs to be loaded; the binding
 * is importable the instant the embedded interpreter starts.  No PROJECT
 * needs to be conjured either (SCH_IO_MGR::Load typically wants one).
 *
 * Detailed summaries:
 *   PCB:  footprint, segment, arc (track), via, zone, gr_line, gr_arc,
 *         gr_circle, gr_rect, gr_text, dimension.
 *   SCH:  symbol, wire, bus, junction, label, global_label,
 *         hierarchical_label, netclass_flag, directive_label, no_connect,
 *         sheet, text, text_box, bus_entry.
 * Counted-only (uuid-tracked, body-compared, empty summary_repr):
 *   PCB gr_poly/target/group/image/embedded_file;
 *   SCH image/rectangle/polyline/arc/circle/embedded_file.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// -------- s-expression parser --------

struct DiffSNode
{
    bool                                    is_list = false;
    bool                                    quoted  = false;
    std::string                             atom;
    std::vector<std::shared_ptr<DiffSNode>> children;
};

class DiffSexprParser
{
public:
    explicit DiffSexprParser( const std::string& aText ) : m_text( aText ), m_pos( 0 ) {}

    std::shared_ptr<DiffSNode> parse_one()
    {
        skip_ws();
        return ( m_pos >= m_text.size() ) ? nullptr : parse_node();
    }

private:
    const std::string& m_text;
    size_t             m_pos;

    void skip_ws()
    {
        while( m_pos < m_text.size() )
        {
            char c = m_text[m_pos];
            if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) ++m_pos;
            else if( c == '#' )
                while( m_pos < m_text.size() && m_text[m_pos] != '\n' ) ++m_pos;
            else break;
        }
    }

    std::shared_ptr<DiffSNode> parse_node()
    {
        skip_ws();
        if( m_pos >= m_text.size() )
            throw std::runtime_error( "unexpected end of s-expression input" );
        char c = m_text[m_pos];
        if( c == '(' )
        {
            ++m_pos;
            auto node = std::make_shared<DiffSNode>();
            node->is_list = true;
            while( true )
            {
                skip_ws();
                if( m_pos >= m_text.size() )
                    throw std::runtime_error( "unterminated list in s-expression" );
                if( m_text[m_pos] == ')' ) { ++m_pos; return node; }
                node->children.push_back( parse_node() );
            }
        }
        if( c == '"' ) return parse_string();
        return parse_atom();
    }

    std::shared_ptr<DiffSNode> parse_string()
    {
        ++m_pos;
        std::string out;
        while( m_pos < m_text.size() )
        {
            char c = m_text[m_pos++];
            if( c == '"' )
            {
                auto n = std::make_shared<DiffSNode>();
                n->quoted = true; n->atom = std::move( out );
                return n;
            }
            if( c == '\\' && m_pos < m_text.size() )
            {
                char e = m_text[m_pos++];
                switch( e )
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += e; break;
                }
            }
            else out += c;
        }
        throw std::runtime_error( "unterminated string in s-expression" );
    }

    std::shared_ptr<DiffSNode> parse_atom()
    {
        std::string out;
        while( m_pos < m_text.size() )
        {
            char c = m_text[m_pos];
            if( c == ' ' || c == '\t' || c == '\n' || c == '\r'
                || c == '(' || c == ')' || c == '"' ) break;
            out += c; ++m_pos;
        }
        auto n = std::make_shared<DiffSNode>();
        n->atom = std::move( out );
        return n;
    }
};

// Canonical re-serialisation (used for fallback "raw body changed" compare).
void diff_serialize( const DiffSNode& aNode, std::string& aOut )
{
    if( aNode.is_list )
    {
        aOut += '(';
        bool first = true;
        for( const auto& c : aNode.children )
        {
            if( !first ) aOut += ' ';
            first = false;
            diff_serialize( *c, aOut );
        }
        aOut += ')';
    }
    else if( aNode.quoted )
    {
        aOut += '"';
        for( char c : aNode.atom )
        {
            if( c == '"' ) aOut += "\\\"";
            else if( c == '\\' ) aOut += "\\\\";
            else aOut += c;
        }
        aOut += '"';
    }
    else aOut += aNode.atom;
}

std::string diff_serialize( const DiffSNode& aNode )
{
    std::string out; diff_serialize( aNode, out ); return out;
}

// -------- tree helpers --------

bool diff_is_list_head( const DiffSNode& aNode, const char* aHead )
{
    if( !aNode.is_list || aNode.children.empty() ) return false;
    const DiffSNode& h = *aNode.children[0];
    return !h.is_list && h.atom == aHead;
}

const DiffSNode* diff_find_child( const DiffSNode& aNode, const char* aHead )
{
    if( !aNode.is_list ) return nullptr;
    for( const auto& c : aNode.children )
        if( diff_is_list_head( *c, aHead ) ) return c.get();
    return nullptr;
}

std::string diff_atom_at( const DiffSNode& aList, size_t aIdx )
{
    if( !aList.is_list || aIdx >= aList.children.size() ) return std::string();
    const DiffSNode& n = *aList.children[aIdx];
    return n.is_list ? std::string() : n.atom;
}

std::string diff_extract_uuid( const DiffSNode& aItem )
{
    const DiffSNode* u = diff_find_child( aItem, "uuid" );
    return u ? diff_atom_at( *u, 1 ) : std::string();
}

std::string diff_property_value( const DiffSNode& aItem, const char* aName )
{
    if( !aItem.is_list ) return std::string();
    for( const auto& c : aItem.children )
    {
        if( !diff_is_list_head( *c, "property" ) || c->children.size() < 3 ) continue;
        if( diff_atom_at( *c, 1 ) == aName ) return diff_atom_at( *c, 2 );
    }
    return std::string();
}

// -------- summary builders --------

py::dict diff_xy_dict( const DiffSNode* aList )
{
    py::dict d;
    if( aList && aList->children.size() >= 3 )
    { d["x"] = diff_atom_at( *aList, 1 ); d["y"] = diff_atom_at( *aList, 2 ); }
    return d;
}

py::dict diff_kv_at( const DiffSNode& aItem )
{
    py::dict d;
    const DiffSNode* at = diff_find_child( aItem, "at" );
    if( at && at->children.size() >= 3 )
    {
        d["x"] = diff_atom_at( *at, 1 ); d["y"] = diff_atom_at( *at, 2 );
        if( at->children.size() >= 4 ) d["rot"] = diff_atom_at( *at, 3 );
    }
    return d;
}

py::list diff_xy_list( const DiffSNode& aList )
{
    py::list out;
    if( !aList.is_list ) return out;
    for( const auto& c : aList.children )
    {
        if( !diff_is_list_head( *c, "xy" ) || c->children.size() < 3 ) continue;
        py::list pt; pt.append( diff_atom_at( *c, 1 ) ); pt.append( diff_atom_at( *c, 2 ) );
        out.append( pt );
    }
    return out;
}

py::dict diff_pair_dict( const DiffSNode* aList, const char* aKa, const char* aKb )
{
    py::dict d;
    if( aList && aList->children.size() >= 3 )
    { d[aKa] = diff_atom_at( *aList, 1 ); d[aKb] = diff_atom_at( *aList, 2 ); }
    return d;
}

// Set s[key] = (child "name") atom-1, but only if the child is present.
// Absent != present-with-empty-string, so we skip rather than write blank;
// keeps the diff stable when (net ...) etc. is omitted on one side.
void diff_put_atom1( py::dict& s, const DiffSNode& aItem, const char* aKey, const char* aName )
{
    if( const DiffSNode* c = diff_find_child( aItem, aName ) ) s[aKey] = diff_atom_at( *c, 1 );
}

// PCB ----

py::dict diff_summary_footprint( const DiffSNode& aItem )
{
    py::dict s;
    s["lib_id"]   = diff_atom_at( aItem, 1 );
    s["ref"]      = diff_property_value( aItem, "Reference" );
    s["value"]    = diff_property_value( aItem, "Value" );
    s["position"] = diff_kv_at( aItem );
    diff_put_atom1( s, aItem, "layer", "layer" );
    return s;
}

py::dict diff_summary_segment( const DiffSNode& aItem )
{
    py::dict s;
    s["start"] = diff_xy_dict( diff_find_child( aItem, "start" ) );
    s["end"]   = diff_xy_dict( diff_find_child( aItem, "end" ) );
    diff_put_atom1( s, aItem, "width", "width" );
    diff_put_atom1( s, aItem, "layer", "layer" );
    diff_put_atom1( s, aItem, "net",   "net"   );
    return s;
}

py::dict diff_summary_arc_track( const DiffSNode& aItem )
{
    py::dict s = diff_summary_segment( aItem );
    s["mid"] = diff_xy_dict( diff_find_child( aItem, "mid" ) );
    return s;
}

py::dict diff_summary_via( const DiffSNode& aItem )
{
    py::dict s;
    s["at"] = diff_xy_dict( diff_find_child( aItem, "at" ) );
    diff_put_atom1( s, aItem, "size",  "size"  );
    diff_put_atom1( s, aItem, "drill", "drill" );
    if( const DiffSNode* layers = diff_find_child( aItem, "layers" ) )
    {
        py::list ls;
        for( size_t i = 1; i < layers->children.size(); ++i )
            ls.append( diff_atom_at( *layers, i ) );
        s["layers"] = ls;
    }
    diff_put_atom1( s, aItem, "net", "net" );
    return s;
}

py::dict diff_summary_zone( const DiffSNode& aItem )
{
    py::dict s;
    diff_put_atom1( s, aItem, "net",      "net"      );
    diff_put_atom1( s, aItem, "net_name", "net_name" );
    diff_put_atom1( s, aItem, "layer",    "layer"    );
    return s;
}

py::dict diff_summary_gr_line( const DiffSNode& aItem )
{
    py::dict s;
    s["start"] = diff_xy_dict( diff_find_child( aItem, "start" ) );
    s["end"]   = diff_xy_dict( diff_find_child( aItem, "end" ) );
    if( const DiffSNode* sk = diff_find_child( aItem, "stroke" ) )
        diff_put_atom1( s, *sk, "width", "width" );
    diff_put_atom1( s, aItem, "layer", "layer" );
    return s;
}

py::dict diff_summary_gr_arc( const DiffSNode& aItem )
{
    py::dict s = diff_summary_gr_line( aItem );
    s["mid"] = diff_xy_dict( diff_find_child( aItem, "mid" ) );
    return s;
}

py::dict diff_summary_gr_circle( const DiffSNode& aItem )
{
    py::dict s;
    s["center"] = diff_xy_dict( diff_find_child( aItem, "center" ) );
    s["end"]    = diff_xy_dict( diff_find_child( aItem, "end" ) );
    if( const DiffSNode* sk = diff_find_child( aItem, "stroke" ) )
        diff_put_atom1( s, *sk, "width", "width" );
    diff_put_atom1( s, aItem, "layer", "layer" );
    return s;
}

py::dict diff_summary_gr_text( const DiffSNode& aItem )
{
    py::dict s;
    s["text"] = diff_atom_at( aItem, 1 );
    s["at"]   = diff_kv_at( aItem );
    diff_put_atom1( s, aItem, "layer", "layer" );
    return s;
}

py::dict diff_summary_dimension( const DiffSNode& aItem )
{
    py::dict s;
    diff_put_atom1( s, aItem, "type",  "type"  );
    diff_put_atom1( s, aItem, "layer", "layer" );
    return s;
}

// SCH ----

py::dict diff_summary_sch_symbol( const DiffSNode& aItem )
{
    py::dict s;
    diff_put_atom1( s, aItem, "lib_id", "lib_id" );
    s["ref"]      = diff_property_value( aItem, "Reference" );
    s["value"]    = diff_property_value( aItem, "Value" );
    s["position"] = diff_kv_at( aItem );
    return s;
}

py::dict diff_summary_wire( const DiffSNode& aItem )
{
    py::dict s;
    if( const DiffSNode* p = diff_find_child( aItem, "pts" ) ) s["pts"] = diff_xy_list( *p );
    return s;
}

py::dict diff_summary_junction( const DiffSNode& aItem )
{
    py::dict s; s["at"] = diff_xy_dict( diff_find_child( aItem, "at" ) ); return s;
}

py::dict diff_summary_label( const DiffSNode& aItem )
{
    py::dict s; s["text"] = diff_atom_at( aItem, 1 ); s["at"] = diff_kv_at( aItem ); return s;
}

py::dict diff_summary_sheet( const DiffSNode& aItem )
{
    py::dict s;
    s["sheet_name"] = diff_property_value( aItem, "Sheetname" );
    s["file_name"]  = diff_property_value( aItem, "Sheetfile" );
    s["at"]         = diff_xy_dict( diff_find_child( aItem, "at" ) );
    s["size"]       = diff_pair_dict( diff_find_child( aItem, "size" ), "w", "h" );
    return s;
}

py::dict diff_summary_bus_entry( const DiffSNode& aItem )
{
    py::dict s;
    s["at"]   = diff_xy_dict( diff_find_child( aItem, "at" ) );
    s["size"] = diff_xy_dict( diff_find_child( aItem, "size" ) );
    return s;
}

// -------- type dispatch tables --------

using DiffSummarizer = py::dict (*)( const DiffSNode& );

struct DiffTypeSpec
{
    const char*    name;
    DiffSummarizer summarize;  // nullptr -> counted-only, body-compared
};

const std::vector<DiffTypeSpec>& diff_pcb_types()
{
    static const std::vector<DiffTypeSpec> kTypes = {
        { "footprint",     &diff_summary_footprint },
        { "segment",       &diff_summary_segment   },
        { "arc",           &diff_summary_arc_track },
        { "via",           &diff_summary_via       },
        { "zone",          &diff_summary_zone      },
        { "gr_line",       &diff_summary_gr_line   },
        { "gr_arc",        &diff_summary_gr_arc    },
        { "gr_circle",     &diff_summary_gr_circle },
        { "gr_rect",       &diff_summary_gr_line   },
        { "gr_poly",       nullptr                 },
        { "gr_text",       &diff_summary_gr_text   },
        { "dimension",     &diff_summary_dimension },
        { "target",        nullptr                 },
        { "group",         nullptr                 },
        { "image",         nullptr                 },
        { "embedded_file", nullptr                 },
    };
    return kTypes;
}

const std::vector<DiffTypeSpec>& diff_sch_types()
{
    static const std::vector<DiffTypeSpec> kTypes = {
        { "symbol",             &diff_summary_sch_symbol },
        { "wire",               &diff_summary_wire       },
        { "bus",                &diff_summary_wire       },
        { "junction",           &diff_summary_junction   },
        { "label",              &diff_summary_label      },
        { "global_label",       &diff_summary_label      },
        { "hierarchical_label", &diff_summary_label      },
        { "netclass_flag",      &diff_summary_label      },
        { "directive_label",    &diff_summary_label      },
        { "no_connect",         &diff_summary_junction   },
        { "sheet",              &diff_summary_sheet      },
        { "text",               &diff_summary_label      },
        { "text_box",           &diff_summary_label      },
        { "bus_entry",          &diff_summary_bus_entry  },
        { "image",              nullptr                  },
        { "rectangle",          nullptr                  },
        { "polyline",           nullptr                  },
        { "arc",                nullptr                  },
        { "circle",             nullptr                  },
        { "embedded_file",      nullptr                  },
    };
    return kTypes;
}

const DiffTypeSpec* diff_lookup_type( const std::vector<DiffTypeSpec>& aSpecs,
                                      const std::string&               aName )
{
    for( const DiffTypeSpec& t : aSpecs )
        if( aName == t.name ) return &t;
    return nullptr;
}

// -------- item indexing --------

struct DiffItem
{
    std::string         type;
    std::string         uuid;
    const DiffSNode*    node = nullptr;
    const DiffTypeSpec* spec = nullptr;
    std::string         raw_body;
};

void diff_index_top_level( const DiffSNode&                  aRoot,
                           const std::vector<DiffTypeSpec>&  aSpecs,
                           std::map<std::string, DiffItem>&  aOut,
                           std::set<std::string>&            aAnomalies )
{
    // The file's root is (kicad_pcb ...) or (kicad_sch ...).  Iterate its
    // direct children — diffable items live one level deep.  Nested blocks
    // like (lib_symbols ...) / (net N "name") / (instances ...) are file
    // metadata / caches and aren't user-addressable as discrete items.
    if( !aRoot.is_list ) return;
    for( const auto& child : aRoot.children )
    {
        if( !child->is_list || child->children.empty() ) continue;
        const DiffSNode& head = *child->children[0];
        if( head.is_list ) continue;

        const std::string&  typeName = head.atom;
        const DiffTypeSpec* spec     = diff_lookup_type( aSpecs, typeName );
        std::string         uuid     = diff_extract_uuid( *child );

        if( uuid.empty() )
        {
            // No uuid -> not addressable.  If it's a typed item we know about
            // but it's missing a uuid, flag it; otherwise it's silent metadata.
            if( spec ) aAnomalies.insert( "uuidless:" + typeName );
            continue;
        }

        DiffItem item;
        item.type     = typeName;
        item.uuid     = uuid;
        item.node     = child.get();
        item.spec     = spec;
        item.raw_body = diff_serialize( *child );

        if( aOut.find( uuid ) != aOut.end() )
            aAnomalies.insert( "uuid_collision:" + typeName );
        aOut[uuid] = std::move( item );
    }
}

// -------- file loading --------

std::string diff_slurp( const std::string& aPath )
{
    std::ifstream f( aPath, std::ios::binary );
    if( !f ) throw std::runtime_error( "diff: cannot open file: " + aPath );
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::shared_ptr<DiffSNode> diff_parse_file( const std::string& aPath,
                                            const char*        aExpectedRoot )
{
    std::string body = diff_slurp( aPath );
    DiffSexprParser p( body );
    auto root = p.parse_one();
    if( !root )
        throw std::runtime_error( "diff: empty file: " + aPath );
    if( !diff_is_list_head( *root, aExpectedRoot ) )
    {
        std::string actual = root->is_list && !root->children.empty()
                                 ? root->children[0]->atom
                                 : std::string( "<not-a-list>" );
        throw std::runtime_error( std::string( "diff: expected (" ) + aExpectedRoot
                                  + " ...) at root of " + aPath + ", got (" + actual + " ...)" );
    }
    return root;
}

// -------- diff core --------

struct DiffByTypeCounts
{
    int added = 0, removed = 0, modified = 0, total_a = 0, total_b = 0;
};

py::dict diff_item_summary( const DiffItem& aItem )
{
    if( aItem.spec && aItem.spec->summarize )
        return aItem.spec->summarize( *aItem.node );
    return py::dict();
}

py::dict diff_item_entry( const DiffItem& aItem )
{
    py::dict d;
    d["uuid"]         = aItem.uuid;
    d["type"]         = aItem.type;
    d["summary_repr"] = diff_item_summary( aItem );
    return d;
}

py::list diff_changed_fields( const py::dict& aBefore, const py::dict& aAfter )
{
    py::list out;
    std::set<std::string> keys;
    for( auto kv : aBefore ) keys.insert( py::str( kv.first ).cast<std::string>() );
    for( auto kv : aAfter )  keys.insert( py::str( kv.first ).cast<std::string>() );
    for( const std::string& k : keys )
    {
        py::str pk( k );
        bool hb = aBefore.contains( pk );
        bool ha = aAfter.contains( pk );
        if( hb && ha )
        {
            if( !aBefore[pk].equal( aAfter[pk] ) ) out.append( pk );
        }
        else { out.append( pk ); }
    }
    return out;
}

struct DiffResult
{
    py::list                                added;
    py::list                                removed;
    py::list                                modified;
    int                                     unchanged = 0;
    std::map<std::string, DiffByTypeCounts> by_type;
};

DiffResult diff_compute( const std::map<std::string, DiffItem>& aA,
                         const std::map<std::string, DiffItem>& aB,
                         bool                                   aDetailed )
{
    DiffResult r;
    for( const auto& [uuid, it] : aA ) r.by_type[it.type].total_a += 1;
    for( const auto& [uuid, it] : aB ) r.by_type[it.type].total_b += 1;

    for( const auto& [uuid, ia] : aA )
    {
        auto bit = aB.find( uuid );
        if( bit == aB.end() )
        {
            r.by_type[ia.type].removed += 1;
            if( aDetailed ) r.removed.append( diff_item_entry( ia ) );
            continue;
        }

        const DiffItem& ib = bit->second;
        if( ia.raw_body == ib.raw_body && ia.type == ib.type )
        {
            r.unchanged += 1;
            continue;
        }

        r.by_type[ia.type].modified += 1;
        if( aDetailed )
        {
            py::dict before  = diff_item_summary( ia );
            py::dict after   = diff_item_summary( ib );
            py::list changed = diff_changed_fields( before, after );
            // Whole-body diff fallback when no surfaced field changed (i.e. the
            // mod was in a field we don't summarise, like (stroke ...) on wire).
            if( py::len( changed ) == 0 ) changed.append( py::str( "body" ) );

            py::dict entry;
            entry["uuid"]           = ia.uuid;
            entry["type"]           = ia.type;
            entry["before"]         = before;
            entry["after"]          = after;
            entry["changed_fields"] = changed;
            if( ia.type != ib.type ) entry["type_changed_to"] = ib.type;
            r.modified.append( entry );
        }
    }

    for( const auto& [uuid, ib] : aB )
    {
        if( aA.find( uuid ) == aA.end() )
        {
            r.by_type[ib.type].added += 1;
            if( aDetailed ) r.added.append( diff_item_entry( ib ) );
        }
    }
    return r;
}

py::dict diff_build_summary( const DiffResult&                      aR,
                             const std::map<std::string, DiffItem>& aA,
                             const std::map<std::string, DiffItem>& aB )
{
    py::dict summary;
    int totalAdded = 0, totalRemoved = 0, totalModified = 0;
    py::dict byType;
    for( const auto& [type, c] : aR.by_type )
    {
        py::dict td;
        td["added"]    = c.added;
        td["removed"]  = c.removed;
        td["modified"] = c.modified;
        td["total_a"]  = c.total_a;
        td["total_b"]  = c.total_b;
        byType[py::str( type )] = td;
        totalAdded    += c.added;
        totalRemoved  += c.removed;
        totalModified += c.modified;
    }
    summary["added_count"]    = totalAdded;
    summary["removed_count"]  = totalRemoved;
    summary["modified_count"] = totalModified;
    summary["total_a"]        = static_cast<int>( aA.size() );
    summary["total_b"]        = static_cast<int>( aB.size() );
    summary["unchanged"]      = aR.unchanged;
    summary["by_type"]        = byType;
    return summary;
}

py::dict diff_run( const std::string&               aPathA,
                   const std::string&               aPathB,
                   const char*                      aExpectedRoot,
                   const std::vector<DiffTypeSpec>& aSpecs,
                   bool                             aDetailed )
{
    auto rootA = diff_parse_file( aPathA, aExpectedRoot );
    auto rootB = diff_parse_file( aPathB, aExpectedRoot );

    std::map<std::string, DiffItem> mapA, mapB;
    std::set<std::string>           anomalies;
    diff_index_top_level( *rootA, aSpecs, mapA, anomalies );
    diff_index_top_level( *rootB, aSpecs, mapB, anomalies );

    DiffResult r = diff_compute( mapA, mapB, aDetailed );

    py::dict out;
    out["ok"]              = true;
    out["summary"]         = diff_build_summary( r, mapA, mapB );
    out["unchanged_count"] = r.unchanged;
    if( aDetailed )
    {
        out["added"]    = r.added;
        out["removed"]  = r.removed;
        out["modified"] = r.modified;
    }
    if( !anomalies.empty() )
    {
        py::list a;
        for( const std::string& s : anomalies ) a.append( py::str( s ) );
        out["anomalies"] = a;
    }
    return out;
}

py::dict diff_sch( const std::string& aA, const std::string& aB )
{
    return diff_run( aA, aB, "kicad_sch", diff_sch_types(), true );
}

py::dict diff_pcb( const std::string& aA, const std::string& aB )
{
    return diff_run( aA, aB, "kicad_pcb", diff_pcb_types(), true );
}

py::dict diff_summary_sch( const std::string& aA, const std::string& aB )
{
    return diff_run( aA, aB, "kicad_sch", diff_sch_types(), false );
}

py::dict diff_summary_pcb( const std::string& aA, const std::string& aB )
{
    return diff_run( aA, aB, "kicad_pcb", diff_pcb_types(), false );
}

} // anon

PYBIND11_EMBEDDED_MODULE( klicad_native_diff, m )
{
    m.doc() = "KliCAD semantic file-diff binding — compare two .kicad_sch or "
              ".kicad_pcb files at the item level, keyed by UUID (KIID).  "
              "Pattern A (libkicommon-resident): parses the s-expressions "
              "directly, no kiface load required and no PROJECT needed.";

    m.def( "diff_sch", &diff_sch, py::arg( "path_a" ), py::arg( "path_b" ),
           "Diff two .kicad_sch files. Returns "
           "{ok, summary{added_count,removed_count,modified_count,total_a,"
           "total_b,unchanged,by_type:{type:{added,removed,modified,total_a,"
           "total_b}}}, unchanged_count, added[{uuid,type,summary_repr}], "
           "removed[...], modified[{uuid,type,before,after,changed_fields,"
           "[type_changed_to]}], [anomalies]}. Items keyed by (uuid \"...\"); "
           "uuidless items (headers, lib_symbols cache) skipped. "
           "Detailed types: symbol, wire, bus, junction, no_connect, label, "
           "global_label, hierarchical_label, netclass_flag, directive_label, "
           "text, text_box, sheet, bus_entry. Other types are uuid-tracked but "
           "summary_repr is empty; their modifications still surface as "
           "changed_fields=[\"body\"]. Raises RuntimeError on missing/unreadable "
           "files or wrong root form." );

    m.def( "diff_pcb", &diff_pcb, py::arg( "path_a" ), py::arg( "path_b" ),
           "Diff two .kicad_pcb files. Same return shape as diff_sch(). "
           "Detailed types: footprint, segment, arc (track), via, zone, "
           "gr_line, gr_arc, gr_circle, gr_rect, gr_text, dimension. Other "
           "types (gr_poly, target, group, image, embedded_file) uuid-tracked "
           "with empty summary_repr; modifications via changed_fields=[\"body\"]." );

    m.def( "diff_summary_sch", &diff_summary_sch, py::arg( "path_a" ), py::arg( "path_b" ),
           "Like diff_sch() but skips per-item lists -- returns only "
           "{ok, summary, unchanged_count, [anomalies]}." );

    m.def( "diff_summary_pcb", &diff_summary_pcb, py::arg( "path_a" ), py::arg( "path_b" ),
           "Like diff_pcb() but counts-only." );
}
