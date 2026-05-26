/*
 * KliCAD subsystem binding: bitmap-to-component converter.
 *
 * Module: klicad_native_bitmap2component
 *
 *   convert(input_image, output_path, output_format, options={})
 *                                       -> dict   (STUB: raises NotImplementedError)
 *   list_supported_formats()            -> list[dict]
 *   list_layer_choices()                -> list[dict]    (footprint output mode)
 *
 * -----------------------------------------------------------------------------
 * Status: STUB binding.
 * -----------------------------------------------------------------------------
 *
 * The real conversion path requires:
 *   (1) the `potrace` static library (currently linked only into the
 *       `bitmap2component` executable target -- see
 *       `bitmap2component/CMakeLists.txt`), and
 *   (2) the `BITMAPCONV_INFO` implementation TU
 *       `bitmap2component/bitmap2component.cpp`, which is also built only
 *       into the `bitmap2component` executable.
 *
 * To turn this stub into a real wrapper we would have to extend
 * `common/CMakeLists.txt` to (a) link `potrace` into `kicommon` and
 * (b) add `bitmap2component/bitmap2component.cpp` to `KICOMMON_SRCS` (or
 * factor it into a small intermediate static lib).  The current task forbids
 * touching any CMakeLists, so we register a stub module instead:
 *
 *   * `list_supported_formats()` and `list_layer_choices()` return useful
 *     metadata immediately (hand-mirrored from the upstream tool so callers
 *     can build UIs / validate input without firing the conversion).
 *   * `convert(...)` raises `NotImplementedError` with a message pointing the
 *     caller at the standalone `bitmap2component` CLI bundled with this
 *     KiCad build.
 *
 * Pattern A (libkicommon-resident PYBIND11_EMBEDDED_MODULE) -- the module
 * imports cleanly the moment the embedded interpreter comes up.
 *
 * NOTE: when the build-side hookup eventually lands, the surface defined here
 * is what callers should keep using.  The only change will be that
 * `convert(...)` actually does work instead of raising.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// ---------------------------------------------------------------------------
// Format catalogue.  Mirrors `OUTPUT_FMT_ID` in
// `bitmap2component/bitmap2component.h`:
//   SYMBOL_FMT, SYMBOL_PASTE_FMT, FOOTPRINT_FMT, POSTSCRIPT_FMT,
//   DRAWING_SHEET_FMT
//
// SYMBOL_PASTE_FMT is the "headerless symbol body" sub-mode the UI exposes as
// a clipboard copy; we surface it under its own format name so the binding
// surface matches the CLI's --fmt switches once that CLI lands.
// ---------------------------------------------------------------------------

struct B2C_FormatEntry
{
    const char* name;            // user-facing format name accepted by convert()
    int         enum_value;      // OUTPUT_FMT_ID integer
    const char* enum_name;       // C++ enumerator spelling
    const char* default_ext;     // canonical output extension (no leading dot)
    const char* description;
};


const std::vector<B2C_FormatEntry>& b2c_format_table()
{
    static const std::vector<B2C_FormatEntry> kFormats = {
        { "footprint",      2 /* FOOTPRINT_FMT */,     "FOOTPRINT_FMT",
          "kicad_mod",
          "KiCad footprint (.kicad_mod)" },

        { "symbol",         0 /* SYMBOL_FMT */,        "SYMBOL_FMT",
          "kicad_sym",
          "KiCad symbol (.kicad_sym)" },

        { "symbol_paste",   1 /* SYMBOL_PASTE_FMT */,  "SYMBOL_PASTE_FMT",
          "kicad_sym",
          "KiCad symbol body for clipboard paste (no library header)" },

        { "pl_editor",      4 /* DRAWING_SHEET_FMT */, "DRAWING_SHEET_FMT",
          "kicad_wks",
          "KiCad drawing sheet (.kicad_wks)" },

        { "postscript",     3 /* POSTSCRIPT_FMT */,    "POSTSCRIPT_FMT",
          "ps",
          "PostScript (.ps)" },
    };
    return kFormats;
}


// ---------------------------------------------------------------------------
// Layer choices for footprint output.  Mirrors the wxChoice in
// `bitmap2component/bitmap2cmp_panel_base.cpp` (index -> layer spelling
// resolved in `bitmap2cmp_panel.cpp`).
//
// Index here is what the bitmap2component executable's UI passes around as
// the "selected layer" -- keep it stable so external scripts can pick by
// index OR by name.
// ---------------------------------------------------------------------------

struct B2C_LayerEntry
{
    int         index;        // UI index (matches m_layerCtrl->GetSelection())
    const char* name;         // canonical short name (accepted by convert(layer=...))
    const char* kicad_layer;  // the wxString layer spelling passed to ConvertBitmap()
    const char* display;      // human-friendly label (matches the panel's wxChoice text)
};


const std::vector<B2C_LayerEntry>& b2c_layer_table()
{
    static const std::vector<B2C_LayerEntry> kLayers = {
        { 0, "f_cu",        "F.Cu",      "F.Cu" },
        { 1, "silkscreen",  "F.SilkS",   "F.Silkscreen" },
        { 2, "f_mask",      "F.Mask",    "F.Mask" },
        { 3, "drawings",    "Dwgs.User", "User.Drawings" },
        { 4, "comments",    "Cmts.User", "User.Comments" },
        { 5, "eco1",        "Eco1.User", "User.Eco1" },
        { 6, "eco2",        "Eco2.User", "User.Eco2" },
        { 7, "fab",         "F.Fab",     "F.Fab" },
    };
    return kLayers;
}


py::dict b2c_format_to_dict( const B2C_FormatEntry& aEntry )
{
    py::dict d;
    d["name"]        = py::str( aEntry.name );
    d["enum_value"]  = py::int_( aEntry.enum_value );
    d["enum_name"]   = py::str( aEntry.enum_name );
    d["default_ext"] = py::str( aEntry.default_ext );
    d["description"] = py::str( aEntry.description );
    return d;
}


py::dict b2c_layer_to_dict( const B2C_LayerEntry& aEntry )
{
    py::dict d;
    d["index"]       = py::int_( aEntry.index );
    d["name"]        = py::str( aEntry.name );
    d["kicad_layer"] = py::str( aEntry.kicad_layer );
    d["display"]     = py::str( aEntry.display );
    return d;
}


py::list b2c_list_supported_formats()
{
    py::list out;
    for( const B2C_FormatEntry& e : b2c_format_table() )
        out.append( b2c_format_to_dict( e ) );
    return out;
}


py::list b2c_list_layer_choices()
{
    py::list out;
    for( const B2C_LayerEntry& e : b2c_layer_table() )
        out.append( b2c_layer_to_dict( e ) );
    return out;
}


// ---------------------------------------------------------------------------
// convert() -- stub.
//
// We accept the full intended signature so callers can write their final
// code today; we raise NotImplementedError with a clear, actionable message.
// Once potrace + bitmap2component.cpp are linkable into libkicommon, replace
// the body with a real BITMAPCONV_INFO call and keep the signature stable.
// ---------------------------------------------------------------------------
py::dict b2c_convert( const std::string& aInputImage,
                      const std::string& aOutputPath,
                      const std::string& aOutputFormat,
                      py::object         aOptions )
{
    (void) aInputImage;
    (void) aOutputPath;
    (void) aOptions;

    // Validate the format name early so callers at least get a useful
    // KeyError-style failure if they passed garbage, before we hit the
    // NotImplementedError below.
    bool format_known = false;
    for( const B2C_FormatEntry& e : b2c_format_table() )
    {
        if( aOutputFormat == e.name )
        {
            format_known = true;
            break;
        }
    }

    if( !format_known )
    {
        std::string msg = "klicad_native_bitmap2component.convert: unknown "
                          "output_format '" + aOutputFormat + "'. "
                          "Call list_supported_formats() for the accepted set.";
        throw std::runtime_error( msg );
    }

    PyErr_SetString(
        PyExc_NotImplementedError,
        "klicad_native_bitmap2component.convert is not wired up in this build. "
        "The conversion code (BITMAPCONV_INFO from bitmap2component/) is not "
        "linkable into libkicommon because the potrace static library and the "
        "bitmap2component implementation TU are only attached to the "
        "standalone bitmap2component executable. "
        "Workaround: invoke the bundled `bitmap2component` CLI directly "
        "(it ships inside the KiCad app bundle), or have a maintainer extend "
        "common/CMakeLists.txt to link `potrace` into kicommon and add "
        "bitmap2component/bitmap2component.cpp to KICOMMON_SRCS, then rebuild. "
        "list_supported_formats() and list_layer_choices() are functional and "
        "can be used in the meantime to validate parameters."
    );
    throw py::error_already_set();
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_bitmap2component, m )
{
    m.doc() = "KliCAD bitmap-to-component binding (STUB). "
              "The metadata helpers list_supported_formats() and "
              "list_layer_choices() are live; convert() raises "
              "NotImplementedError until potrace + bitmap2component.cpp are "
              "linked into libkicommon.  See the top of "
              "common/api/bindings_bitmap2component.cpp for the rationale "
              "and the exact CMakeLists changes a maintainer would have to "
              "make to flip the stub into a real wrapper.";

    m.def( "list_supported_formats", &b2c_list_supported_formats,
           R"DOC(Return the list of output formats convert() can produce.

Each entry is a dict:
    {
      name:        str,    # pass this as output_format= to convert()
      enum_value:  int,    # underlying OUTPUT_FMT_ID integer
      enum_name:   str,    # C++ enumerator spelling
      default_ext: str,    # canonical output extension (no leading dot)
      description: str,    # one-line human-friendly description
    }

Mirrored from OUTPUT_FMT_ID in bitmap2component/bitmap2component.h.
)DOC" );

    m.def( "list_layer_choices", &b2c_list_layer_choices,
           R"DOC(Return the legal `layer=` choices for footprint output.

Each entry is a dict:
    {
      index:       int,    # legacy UI index (stable)
      name:        str,    # pass this as options={'layer': ...} to convert()
      kicad_layer: str,    # the underlying KiCad layer spelling (e.g. 'F.SilkS')
      display:     str,    # human label as shown in the bitmap2component GUI
    }

Only meaningful when output_format='footprint'.  Other output formats
ignore the layer option.
)DOC" );

    m.def( "convert", &b2c_convert,
           py::arg( "input_image" ),
           py::arg( "output_path" ),
           py::arg( "output_format" ),
           py::arg( "options" ) = py::dict(),
           R"DOC(Convert a bitmap image to a KiCad output file.

STUB: this entry point currently raises NotImplementedError.  The
signature is final; once the underlying C++ conversion path becomes
reachable from libkicommon, the body will start producing real output
without any caller-side change.

Args:
    input_image:  Path to the source raster image (PNG / JPG / BMP / etc.).
    output_path:  Path to write the produced file to.
    output_format: One of the names returned by list_supported_formats()
                   -- 'footprint', 'symbol', 'symbol_paste', 'pl_editor',
                   or 'postscript'.
    options:      Optional dict of conversion knobs:
                    threshold (int 0-255)  -- B/W threshold; default 127
                    negative  (bool)       -- invert the bitmap before tracing
                    layer     (str)        -- footprint-only; see
                                              list_layer_choices() for accepted
                                              names; default 'silkscreen'
                    dpi       (int|tuple)  -- output DPI (int = both axes;
                                              (dx, dy) tuple for asymmetric).
                                              Default 300.

Workaround until the stub is filled in: run the standalone
`bitmap2component` CLI from the KiCad bundle.
)DOC" );
}
