/*
 * KliCAD subsystem binding: GERBVIEW state (Pattern B, kiface-resident).
 *
 * Exposes the live GERBVIEW_FRAME's file-loading and layer-control surface
 * as klicad_native_gerbview.* — loading Gerber/Excellon/zip/job files,
 * clearing draw layers, querying the active layer, and enumerating loaded
 * files per layer.
 *
 * Pattern (per BINDING_PATTERN.md, Pattern B):
 *   1. Walk wxTopLevelWindows for the live GERBVIEW_FRAME (FRAME_GERBER),
 *      spawning it via KIWAY::Player(FRAME_GERBER, true) if missing.
 *   2. Cross-kiface-boundary identification: EDA_BASE_FRAME::GetFrameType()
 *      then static_cast<GERBVIEW_FRAME*>.  Avoid dynamic_cast because the
 *      typeinfo lives in _gerbview.kiface.bundle and may not be visible to
 *      libkicommon at link time.
 *   3. Return { ok, ... } as a py::dict; lists return py::list of dicts.
 *
 * NOT a JOB_*-backed binding — direct C++ against GERBVIEW_FRAME and
 * GERBER_FILE_IMAGE_LIST.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <eda_draw_frame.h>

#include <gerbview_frame.h>
#include <gerber_file_image.h>
#include <gerber_file_image_list.h>

#include <wx/string.h>
#include <wx/arrstr.h>
#include <wx/filename.h>
#include <wx/window.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// Per-TU helper name (unique vs existing bindings).
KIWAY* find_live_kiway_for_gerbview()
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


// Walk wxTopLevelWindows for a GERBVIEW_FRAME (FRAME_GERBER).  We can't
// dynamic_cast<GERBVIEW_FRAME*> safely across kiface boundaries — the
// typeinfo lives in _gerbview.kiface.bundle and may not be visible to
// libkicommon at link time.  Instead, use EDA_BASE_FRAME::GetFrameType()
// to identify FRAME_GERBER, then static_cast (safe given the identifier).
GERBVIEW_FRAME* find_gerbview_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_GERBER )
            return static_cast<GERBVIEW_FRAME*>( base );
    }
    return nullptr;
}


// Resolve a live GERBVIEW_FRAME, spawning gerbview if necessary.  Throws on
// any failure so the caller doesn't have to null-check.
GERBVIEW_FRAME* require_gerbview_frame()
{
    if( GERBVIEW_FRAME* frame = find_gerbview_frame() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_gerbview();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(gerbview state needs to spawn the gerbview frame)" );
    }

    // Spawn gerbview; this creates the GERBVIEW_FRAME with an empty layout.
    kiway->Player( FRAME_GERBER, true );

    GERBVIEW_FRAME* frame = find_gerbview_frame();

    if( !frame )
    {
        throw std::runtime_error(
            "failed to obtain GERBVIEW_FRAME after KIWAY::Player(FRAME_GERBER, true)" );
    }

    return frame;
}


// Refresh the gerbview canvas after a mutation so the user sees the change
// immediately.
void refresh_gerbview_canvas( GERBVIEW_FRAME* aFrame )
{
    if( aFrame && aFrame->GetCanvas() )
        aFrame->GetCanvas()->Refresh();
}


// ──────────────────────────────────────────────────────────────────────────
// load_gerber_files
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_load_gerber_files( const std::vector<std::string>& paths )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    py::list errors;
    int      loaded = 0;

    for( const std::string& p : paths )
    {
        wxString wxp = wxString::FromUTF8( p.c_str() );

        bool ok = false;

        try
        {
            ok = frame->LoadGerberFiles( wxp );
        }
        catch( const std::exception& ex )
        {
            errors.append( std::string( p ) + ": " + ex.what() );
            continue;
        }
        catch( ... )
        {
            errors.append( std::string( p ) + ": unknown exception" );
            continue;
        }

        if( ok )
            ++loaded;
        else
            errors.append( std::string( p ) + ": LoadGerberFiles returned false" );
    }

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]           = errors.size() == 0;
    result[ "loaded_count" ] = loaded;
    result[ "errors" ]       = errors;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// load_excellon_files
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_load_excellon_files( const std::vector<std::string>& paths )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    py::list errors;
    int      loaded = 0;

    for( const std::string& p : paths )
    {
        wxString wxp = wxString::FromUTF8( p.c_str() );

        bool ok = false;

        try
        {
            ok = frame->LoadExcellonFiles( wxp );
        }
        catch( const std::exception& ex )
        {
            errors.append( std::string( p ) + ": " + ex.what() );
            continue;
        }
        catch( ... )
        {
            errors.append( std::string( p ) + ": unknown exception" );
            continue;
        }

        if( ok )
            ++loaded;
        else
            errors.append( std::string( p ) + ": LoadExcellonFiles returned false" );
    }

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]           = errors.size() == 0;
    result[ "loaded_count" ] = loaded;
    result[ "errors" ]       = errors;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// load_zip_archive
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_load_zip_archive( const std::string& path )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    wxString wxp = wxString::FromUTF8( path.c_str() );

    py::list errors;
    int      loaded = 0;

    bool ok = false;

    try
    {
        ok = frame->LoadZipArchiveFile( wxp );
    }
    catch( const std::exception& ex )
    {
        errors.append( std::string( path ) + ": " + ex.what() );
    }
    catch( ... )
    {
        errors.append( std::string( path ) + ": unknown exception" );
    }

    if( ok )
        loaded = 1;
    else if( errors.size() == 0 )
        errors.append( std::string( path ) + ": LoadZipArchiveFile returned false" );

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]           = ok;
    result[ "loaded_count" ] = loaded;
    result[ "errors" ]       = errors;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// load_autodetect
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_load_autodetect( const std::vector<std::string>& paths )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    py::list errors;
    int      loaded = 0;

    for( const std::string& p : paths )
    {
        wxString wxp = wxString::FromUTF8( p.c_str() );

        bool ok = false;

        try
        {
            // LoadAutodetectedFiles handles a single file at a time; for a
            // list the user would normally invoke once per file.  If passed
            // an empty string LoadAutodetectedFiles opens a file picker —
            // we deliberately do not pass empty strings here.
            if( wxp.IsEmpty() )
            {
                errors.append( std::string( p ) + ": empty path" );
                continue;
            }

            ok = frame->LoadAutodetectedFiles( wxp );
        }
        catch( const std::exception& ex )
        {
            errors.append( std::string( p ) + ": " + ex.what() );
            continue;
        }
        catch( ... )
        {
            errors.append( std::string( p ) + ": unknown exception" );
            continue;
        }

        if( ok )
            ++loaded;
        else
            errors.append( std::string( p ) + ": LoadAutodetectedFiles returned false" );
    }

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]           = errors.size() == 0;
    result[ "loaded_count" ] = loaded;
    result[ "errors" ]       = errors;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// load_gerber_job
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_load_gerber_job( const std::string& path )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    wxString wxp = wxString::FromUTF8( path.c_str() );

    py::list errors;
    int      loaded = 0;

    bool ok = false;

    try
    {
        ok = frame->LoadGerberJobFile( wxp );
    }
    catch( const std::exception& ex )
    {
        errors.append( std::string( path ) + ": " + ex.what() );
    }
    catch( ... )
    {
        errors.append( std::string( path ) + ": unknown exception" );
    }

    if( ok )
        loaded = 1;
    else if( errors.size() == 0 )
        errors.append( std::string( path ) + ": LoadGerberJobFile returned false" );

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]           = ok;
    result[ "loaded_count" ] = loaded;
    result[ "errors" ]       = errors;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// clear_all_layers
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_clear_all_layers()
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    // Clear_DrawLayers( false ) skips the confirmation dialog (we don't want
    // a modal dialog popping up from a Python call).
    bool cleared = frame->Clear_DrawLayers( false );

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ] = cleared;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// clear_current_layer
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_clear_current_layer()
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    // query = false: skip confirmation dialog.
    frame->Erase_Current_DrawLayer( false );

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ] = true;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_active_layer
// ──────────────────────────────────────────────────────────────────────────
int gerbview_get_active_layer()
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();
    return frame->GetActiveLayer();
}


// ──────────────────────────────────────────────────────────────────────────
// set_active_layer
// ──────────────────────────────────────────────────────────────────────────
py::object gerbview_set_active_layer( int layer )
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();

    frame->SetActiveLayer( layer, /*doLayerWidgetUpdate*/ true );

    refresh_gerbview_canvas( frame );

    py::dict result;
    result[ "ok" ]    = true;
    result[ "layer" ] = layer;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// get_layer_count
// ──────────────────────────────────────────────────────────────────────────
int gerbview_get_layer_count()
{
    GERBVIEW_FRAME* frame = require_gerbview_frame();
    GERBER_FILE_IMAGE_LIST* images = frame->GetImagesList();

    if( !images )
        return 0;

    int count = 0;

    for( unsigned i = 0; i < images->ImagesMaxCount(); ++i )
    {
        if( images->GetGbrImage( i ) != nullptr )
            ++count;
    }

    return count;
}


// ──────────────────────────────────────────────────────────────────────────
// list_loaded_files
// ──────────────────────────────────────────────────────────────────────────
py::list gerbview_list_loaded_files()
{
    GERBVIEW_FRAME*         frame  = require_gerbview_frame();
    GERBER_FILE_IMAGE_LIST* images = frame->GetImagesList();

    py::list result;

    if( !images )
        return result;

    for( unsigned i = 0; i < images->ImagesMaxCount(); ++i )
    {
        GERBER_FILE_IMAGE* img = images->GetGbrImage( i );

        if( !img )
            continue;

        py::dict d;
        d[ "layer" ]      = static_cast<int>( i );
        d[ "filename" ]   = std::string( img->m_FileName.utf8_str() );
        d[ "image_name" ] = std::string( img->m_ImageName.utf8_str() );
        d[ "in_use" ]     = img->m_InUse;
        d[ "visible" ]    = frame->IsLayerVisible( static_cast<int>( i ) );
        d[ "is_x2" ]      = img->m_IsX2_file;
        result.append( d );
    }

    return result;
}


} // anon


// Registered at kiface-load time — PYBIND11_EMBEDDED_MODULE can't be used
// inside a lazy-loaded kiface (see klicad_kiface_register.h).
void klicad_register_gerbview_bindings_impl( py::module_& m )
{
    m.doc() = "KliCAD direct GERBVIEW_FRAME state binding — programmatic "
              "loading of Gerber/Excellon/zip/job files and layer control "
              "against the running GERBVIEW_FRAME.  Spawns the gerbview "
              "frame on first call if it isn't already open.";

    m.def( "load_gerber_files", &gerbview_load_gerber_files,
           py::arg( "paths" ),
           R"DOC(Load a list of Gerber files into the gerbview frame.

Each path is loaded onto its own draw layer.
Returns {ok: bool, loaded_count: int, errors: list[str]}.
)DOC" );

    m.def( "load_excellon_files", &gerbview_load_excellon_files,
           py::arg( "paths" ),
           R"DOC(Load a list of Excellon (NC drill) files into the gerbview frame.

Returns {ok: bool, loaded_count: int, errors: list[str]}.
)DOC" );

    m.def( "load_zip_archive", &gerbview_load_zip_archive,
           py::arg( "path" ),
           R"DOC(Load a zip archive containing Gerber and/or drill files.

Returns {ok: bool, loaded_count: int, errors: list[str]}.
)DOC" );

    m.def( "load_autodetect", &gerbview_load_autodetect,
           py::arg( "paths" ),
           R"DOC(Load a list of files, autodetecting each one's type (Gerber, drill, zip, job).

Returns {ok: bool, loaded_count: int, errors: list[str]}.
)DOC" );

    m.def( "load_gerber_job", &gerbview_load_gerber_job,
           py::arg( "path" ),
           R"DOC(Load a Gerber job file (.gbrjob), then load the gerber files it references.

Returns {ok: bool, loaded_count: int, errors: list[str]}.
)DOC" );

    m.def( "clear_all_layers", &gerbview_clear_all_layers,
           R"DOC(Clear every draw layer (delete all loaded gerber/drill data).

Returns {ok: bool}.  Skips the interactive confirmation dialog.
)DOC" );

    m.def( "clear_current_layer", &gerbview_clear_current_layer,
           R"DOC(Erase the currently-active draw layer.

Returns {ok: bool}.  Skips the interactive confirmation dialog.
)DOC" );

    m.def( "get_active_layer", &gerbview_get_active_layer,
           "Return the 0-based index of the currently-active layer." );

    m.def( "set_active_layer", &gerbview_set_active_layer,
           py::arg( "layer" ),
           R"DOC(Set the active layer (0-based index).

Returns {ok: bool, layer: int}.
)DOC" );

    m.def( "get_layer_count", &gerbview_get_layer_count,
           "Return the number of layers with a loaded GERBER_FILE_IMAGE." );

    m.def( "list_loaded_files", &gerbview_list_loaded_files,
           R"DOC(Return per-layer info for every loaded image.

Each entry: {layer: int, filename: str, image_name: str, in_use: bool,
             visible: bool, is_x2: bool}.
)DOC" );
}
