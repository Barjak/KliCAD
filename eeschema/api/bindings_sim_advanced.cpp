/*
 * KliCAD binding: advanced SPICE simulator surface — klicad_native_sim_advanced.
 *
 * Layered on klicad_native_simulator.  Pattern B (kiface-resident).  Same kiface
 * as bindings_simulator.cpp; see its header for the no-PYBIND11_EMBEDDED_MODULE
 * rationale.  Surface: save/load_workbook, parameter_sweep, add/list/set/remove_
 * tuner, measure, list_measurements, fft, set/get_simulation_parameter.
 *
 * C++ API gaps:
 *   remove_tuner   — SIMULATOR_FRAME has no public RemoveTuner accessor
 *                    (private on m_ui).  Raises RuntimeError until added.
 *   measure(raw)   — ngspice `meas` writes results to SendChar (uncaptured);
 *                    high-level forms assign to a named vector then GetRealVector.
 *                    Raw passthrough returns value=None.
 *   get_simulation_parameter — ngspice `print $name` is uncaptured; we mirror
 *                    set_ values in a process-local cache and return None for
 *                    parameters never set here.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>

#include <sch_edit_frame.h>
#include <sch_symbol.h>
#include <sch_reference_list.h>
#include <sch_sheet_path.h>
#include <schematic.h>

#include <sim/simulator_frame.h>
#include <sim/spice_simulator.h>
#include <sim/sim_types.h>
#include <sim/spice_value.h>
#include <sim/sim_library.h>
#include <sim/sim_library_spice.h>
#include <sim/sim_model.h>
#include <reporter.h>
#include <tuner_slider.h>

#include <wx/filename.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace
{

KIWAY* find_live_kiway_for_sim_adv()
{
    for( wxWindow* w : wxTopLevelWindows )
        if( KIWAY_HOLDER* holder = dynamic_cast<KIWAY_HOLDER*>( w ) )
            if( holder->HasKiway() )
                return &holder->Kiway();
    return nullptr;
}

SIMULATOR_FRAME* find_simulator_frame_adv()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );
        if( base && base->GetFrameType() == FRAME_SIMULATOR )
            return static_cast<SIMULATOR_FRAME*>( base );
    }
    return nullptr;
}

SIMULATOR_FRAME* require_simulator_frame_adv()
{
    if( SIMULATOR_FRAME* frame = find_simulator_frame_adv() )
        return frame;
    KIWAY* kiway = find_live_kiway_for_sim_adv();
    if( !kiway )
        throw std::runtime_error( "no live KIWAY — is KiCad's GUI running?" );
    KIWAY_PLAYER* sch    = kiway->Player( FRAME_SCH, true );
    KIWAY_PLAYER* player = kiway->Player( FRAME_SIMULATOR, true, sch );
    if( !player )
        throw std::runtime_error( "failed to spawn FRAME_SIMULATOR" );
    SIMULATOR_FRAME* frame = find_simulator_frame_adv();
    if( !frame )
        throw std::runtime_error( "FRAME_SIMULATOR not in wxTopLevelWindows after spawn" );
    return frame;
}

std::shared_ptr<SPICE_SIMULATOR> require_spice_simulator_adv( SIMULATOR_FRAME* aFrame )
{
    std::shared_ptr<SPICE_SIMULATOR> sim = aFrame->GetSimulator();
    if( !sim )
        throw std::runtime_error( "SIMULATOR_FRAME::GetSimulator() returned null" );
    return sim;
}

// Walk schematic hierarchy and return the (symbol, sheet_path) matching aRef.
std::pair<SCH_SYMBOL*, SCH_SHEET_PATH> find_symbol_by_ref( SCHEMATIC& aSch,
                                                           const wxString& aRef )
{
    SCH_REFERENCE_LIST refs;
    aSch.Hierarchy().GetSymbols( refs, SYMBOL_FILTER_ALL, true );

    for( size_t i = 0; i < refs.GetCount(); ++i )
    {
        SCH_REFERENCE& r = refs[ static_cast<int>( i ) ];

        if( r.GetRef() == aRef
            || ( r.GetSymbol() && r.GetSymbol()->GetRef( &r.GetSheetPath() ) == aRef ) )
        {
            return { r.GetSymbol(), r.GetSheetPath() };
        }
    }

    return { nullptr, SCH_SHEET_PATH() };
}

// Walk the SIMULATOR_FRAME widget tree for TUNER_SLIDER instances via
// dynamic_cast.  TUNER_SLIDER has no wxRTTI macros so GetClassInfo() returns
// its wxPanel base — dynamic_cast is the reliable identification.
std::vector<TUNER_SLIDER*> collect_tuner_widgets( SIMULATOR_FRAME* aFrame )
{
    std::vector<TUNER_SLIDER*> out;
    std::function<void( wxWindow* )> walk = [ & ]( wxWindow* w )
    {
        if( !w )
            return;
        if( TUNER_SLIDER* t = dynamic_cast<TUNER_SLIDER*>( w ) )
            out.push_back( t );
        for( wxWindow* child : w->GetChildren() )
            walk( child );
    };
    walk( aFrame );
    return out;
}


py::dict sim_adv_save_workbook( const std::string& aPath )
{
    SIMULATOR_FRAME* frame = require_simulator_frame_adv();
    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = frame->SaveWorkbook( wxString::FromUTF8( aPath ) );
    }
    py::dict result;
    result[ "ok" ]   = ok;
    result[ "path" ] = aPath;
    return result;
}

py::dict sim_adv_load_workbook( const std::string& aPath )
{
    SIMULATOR_FRAME* frame = require_simulator_frame_adv();
    if( !wxFileName::FileExists( wxString::FromUTF8( aPath ) ) )
        throw std::runtime_error( "workbook file does not exist: " + aPath );
    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = frame->LoadWorkbook( wxString::FromUTF8( aPath ) );
    }
    py::dict result;
    result[ "ok" ]   = ok;
    result[ "path" ] = aPath;
    return result;
}

std::string build_spice_command_adv( const std::string& aKind, const py::dict& aParams,
                                     SIM_TYPE* aOutType )
{
    auto need = [&]( const char* k ) -> std::string
    {
        if( !aParams.contains( k ) )
            throw std::invalid_argument( std::string( "missing analysis_args key '" ) + k + "'" );
        return py::str( aParams[ k ] ).cast<std::string>();
    };

    auto opt = [&]( const char* k, const std::string& def ) -> std::string
    {
        return aParams.contains( k ) ? py::str( aParams[ k ] ).cast<std::string>() : def;
    };

    if( aKind == "tran" )
    {
        std::string cmd = ".tran " + need( "step" ) + " " + need( "stop" );
        std::string start = opt( "start", std::string() );

        if( !start.empty() )
            cmd += " " + start;

        if( aParams.contains( "uic" ) && py::cast<bool>( aParams[ "uic" ] ) )
            cmd += " uic";

        *aOutType = ST_TRAN;
        return cmd;
    }

    if( aKind == "ac" )
    {
        *aOutType = ST_AC;
        return ".ac " + opt( "type", "dec" ) + " " + need( "npoints" ) + " "
               + need( "fstart" ) + " " + need( "fstop" );
    }

    if( aKind == "dc" )
    {
        *aOutType = ST_DC;
        return ".dc " + need( "source" ) + " " + need( "start" ) + " "
               + need( "stop" ) + " " + need( "step" );
    }

    if( aKind == "op" )
    {
        *aOutType = ST_OP;
        return ".op";
    }

    if( aKind == "noise" )
    {
        *aOutType = ST_NOISE;
        return ".noise " + need( "output" ) + " " + need( "src" ) + " "
               + opt( "type", "dec" ) + " " + need( "npoints" ) + " "
               + need( "fstart" ) + " " + need( "fstop" );
    }

    throw std::invalid_argument(
        "analysis must be 'tran'|'ac'|'dc'|'op'|'noise' (got '" + aKind + "')" );
}

std::string format_value( double aValue )
{
    char buf[ 64 ];
    int  n = std::snprintf( buf, sizeof( buf ), "%.12g", aValue );

    if( n < 0 || static_cast<size_t>( n ) >= sizeof( buf ) )
        return std::to_string( aValue );

    return std::string( buf );
}

py::dict sim_adv_parameter_sweep( const std::string& aParameter, double aStart, double aStop,
                                  double aStep, const std::string& aAnalysis,
                                  const py::dict& aAnalysisArgs )
{
    if( aStep == 0.0 )
        throw std::invalid_argument( "step must be non-zero" );

    if( ( aStop > aStart && aStep < 0.0 ) || ( aStop < aStart && aStep > 0.0 ) )
        throw std::invalid_argument( "step sign must match stop-start direction" );

    SIMULATOR_FRAME*                 frame = require_simulator_frame_adv();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator_adv( frame );

    SIM_TYPE    simType = ST_UNKNOWN;
    std::string analysisCmd = build_spice_command_adv( aAnalysis, aAnalysisArgs, &simType );

    std::vector<double>      values;
    std::vector<std::string> plots;
    py::list                 errors;

    const double span   = aStop - aStart;
    const int    nSteps = static_cast<int>( std::floor( span / aStep + 1e-9 ) ) + 1;

    for( int i = 0; i < nSteps; ++i )
    {
        double v = aStart + i * aStep;

        if( ( aStep > 0 && v > aStop ) || ( aStep < 0 && v < aStop ) )
            break;

        values.push_back( v );

        const std::string alterCmd = "alter " + aParameter + " = " + format_value( v );

        bool alterOk;
        bool runOk;

        {
            py::gil_scoped_release nogil;
            alterOk = sim->Command( alterCmd );
            runOk   = sim->Command( analysisCmd );
        }

        plots.push_back( sim->CurrentPlotName().ToStdString() );

        if( !alterOk )
            errors.append( "alter rejected for value " + format_value( v ) );
        else if( !runOk )
            errors.append( "analysis rejected for value " + format_value( v ) );
        else
            errors.append( py::none() );
    }

    py::dict result;
    result[ "parameter" ] = aParameter;
    result[ "analysis" ]  = aAnalysis;
    result[ "values" ]    = values;
    result[ "plots" ]     = plots;
    result[ "errors" ]    = errors;
    return result;
}

py::dict sim_adv_add_tuner( const std::string& aRef )
{
    SIMULATOR_FRAME* frame    = require_simulator_frame_adv();
    SCH_EDIT_FRAME*  schFrame = frame->GetSchematicFrame();

    if( !schFrame )
        throw std::runtime_error( "SIMULATOR_FRAME has no associated SCH_EDIT_FRAME" );

    SCHEMATIC& sch = schFrame->Schematic();

    if( !sch.IsValid() )
        throw std::runtime_error( "schematic is not valid (no project loaded?)" );

    wxString ref   = wxString::FromUTF8( aRef );
    auto     found = find_symbol_by_ref( sch, ref );

    if( !found.first )
        throw std::runtime_error( "symbol '" + aRef + "' not found in schematic" );

    {
        py::gil_scoped_release nogil;
        frame->AddTuner( found.second, found.first );
    }

    py::dict result;
    result[ "ok" ]  = true;
    result[ "ref" ] = aRef;
    return result;
}

py::list sim_adv_list_tuners()
{
    SIMULATOR_FRAME* frame = require_simulator_frame_adv();
    py::list out;
    for( TUNER_SLIDER* t : collect_tuner_widgets( frame ) )
    {
        py::dict d;
        d[ "ref" ]       = t->GetSymbolRef().ToStdString();
        d[ "value" ]     = t->GetValue().ToString().ToStdString();
        d[ "min" ]       = t->GetMin().ToString().ToStdString();
        d[ "max" ]       = t->GetMax().ToString().ToStdString();
        d[ "window_id" ] = static_cast<long>( t->GetId() );
        out.append( d );
    }
    return out;
}

py::dict sim_adv_set_tuner_value( const std::string& aRef, const std::string& aValue )
{
    SIMULATOR_FRAME* frame = require_simulator_frame_adv();
    std::shared_ptr<SPICE_SIMULATOR> sim = require_spice_simulator_adv( frame );
    wxString ref   = wxString::FromUTF8( aRef );
    wxString value = wxString::FromUTF8( aValue );

    // (a) Push to ngspice via `alter` so the next analysis run sees the new
    //     value immediately, even if no GUI slider exists for this ref.
    bool alterOk;
    {
        py::gil_scoped_release nogil;
        alterOk = sim->Command( "alter " + aRef + " = " + aValue );
    }

    // (b) If a matching TUNER_SLIDER exists, drive its SetValue() directly.
    bool widgetDriven = false;
    for( TUNER_SLIDER* t : collect_tuner_widgets( frame ) )
    {
        if( t->GetSymbolRef() != ref )
            continue;
        try
        {
            widgetDriven = t->SetValue( SPICE_VALUE( value ) );
        }
        catch( ... )
        {
            // SPICE_VALUE throws KI_PARAM_ERROR on unparseable input — swallow
            // since the alter path may still have succeeded.
        }
        break;
    }

    py::dict result;
    result[ "ok" ]            = alterOk;
    result[ "ref" ]           = aRef;
    result[ "value" ]         = aValue;
    result[ "altered" ]       = alterOk;
    result[ "widget_synced" ] = widgetDriven;
    return result;
}

py::dict sim_adv_remove_tuner( const std::string& /*aRef*/ )
{
    throw std::runtime_error(
        "remove_tuner not yet implemented — SIMULATOR_FRAME has no public "
        "RemoveTuner accessor (the method lives on the private m_ui).  "
        "Workaround: close the tuner via the GUI's X button." );
}

std::string make_measure_handle()
{
    static int s_counter = 0;
    return "klicad_meas_" + std::to_string( ++s_counter );
}

struct MeasureTranslation
{
    bool        recognized;
    std::string command;
    std::string resultVar;
    std::string kind;
};

MeasureTranslation translate_measure_expr( const std::string& aExpr, SIM_TYPE aSimType )
{
    auto trim = []( std::string s ) -> std::string
    {
        size_t a = s.find_first_not_of( " \t\r\n" );
        size_t b = s.find_last_not_of( " \t\r\n" );
        return ( a == std::string::npos ) ? std::string() : s.substr( a, b - a + 1 );
    };

    std::string expr = trim( aExpr );

    if( expr.empty() )
        return { false, "", "", "" };

    // Raw passthrough.
    if( expr.size() >= 5 && expr.substr( 0, 5 ) == "meas " )
        return { true, expr, std::string(), "raw" };

    size_t paren = expr.find( '(' );

    if( paren == std::string::npos || expr.back() != ')' )
        return { false, "", "", "" };

    std::string fn   = trim( expr.substr( 0, paren ) );
    std::string args = trim( expr.substr( paren + 1, expr.size() - paren - 2 ) );

    if( fn.empty() || args.empty() )
        return { false, "", "", "" };

    // Split args on top-level commas.
    std::vector<std::string> argv;
    std::string              cur;
    int                      depth = 0;

    for( char c : args )
    {
        if( c == '(' )       depth++;
        else if( c == ')' )  depth--;
        else if( c == ',' && depth == 0 )
        {
            argv.push_back( trim( cur ) );
            cur.clear();
            continue;
        }
        cur.push_back( c );
    }

    if( !cur.empty() )
        argv.push_back( trim( cur ) );

    std::string fnL = fn;
    std::transform( fnL.begin(), fnL.end(), fnL.begin(),
                    []( unsigned char c ) { return std::tolower( c ); } );

    const char* analysis = ( aSimType == ST_AC )    ? "ac"
                         : ( aSimType == ST_DC )    ? "dc"
                         : ( aSimType == ST_NOISE ) ? "noise"
                                                    : "tran";

    std::string handle = make_measure_handle();

    if( ( fnL == "max" || fnL == "min" || fnL == "avg" || fnL == "rms"
          || fnL == "pp" || fnL == "integ" ) && argv.size() == 1 )
    {
        return { true,
                 std::string( "meas " ) + analysis + " " + handle + " " + fnL + " " + argv[ 0 ],
                 handle, fnL };
    }

    if( ( fnL == "rise_time" || fnL == "fall_time" ) && argv.size() == 1 )
    {
        // Hard-coded 10%/90% thresholds; callers needing other values
        // should use raw "meas tran ..." passthrough.
        const char* edge = ( fnL == "rise_time" ) ? "rise=1" : "fall=1";
        const char* vlo  = ( fnL == "rise_time" ) ? "0.1" : "0.9";
        const char* vhi  = ( fnL == "rise_time" ) ? "0.9" : "0.1";

        std::string cmd = std::string( "meas " ) + analysis + " " + handle
                          + " trig " + argv[ 0 ] + " val=" + vlo + " " + edge
                          + " targ " + argv[ 0 ] + " val=" + vhi + " " + edge;
        return { true, cmd, handle, fnL };
    }

    // settling_time intentionally not translated — requires the final value
    // in scope, which we can't infer without a prior reduction.
    return { false, "", "", "" };
}

py::dict sim_adv_measure( const std::string& aExpr )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame_adv();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator_adv( frame );

    MeasureTranslation trans = translate_measure_expr( aExpr, frame->GetCurrentSimType() );

    if( !trans.recognized )
    {
        throw std::runtime_error(
            "measure expression '" + aExpr + "' not recognized.  Supported: "
            "max/min/avg/rms/pp/integ(signal), rise_time/fall_time(signal), "
            "or a raw 'meas <analysis> ...' line.  settling_time is not yet implemented." );
    }

    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = sim->Command( trans.command );
    }

    py::dict result;
    result[ "ok" ]         = ok;
    result[ "expression" ] = aExpr;
    result[ "command" ]    = trans.command;
    result[ "kind" ]       = trans.kind;

    if( trans.resultVar.empty() )
    {
        result[ "value" ]      = py::none();
        result[ "result_var" ] = py::none();
        return result;
    }

    std::vector<double> v;
    {
        py::gil_scoped_release nogil;
        v = sim->GetRealVector( trans.resultVar );
    }

    result[ "result_var" ] = trans.resultVar;
    result[ "value" ]      = v.empty() ? py::object( py::none() )
                                       : py::object( py::cast( v.front() ) );
    return result;
}

py::list sim_adv_list_measurements()
{
    py::list out;
    for( const char* name : { "max", "min", "avg", "rms", "pp", "integ",
                              "rise_time", "fall_time" } )
        out.append( std::string( name ) );
    return out;
}

py::dict sim_adv_fft( const std::string& aVector, const py::dict& aOptions )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame_adv();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator_adv( frame );

    std::string window  = "hanning";
    int         nPoints = 0;

    if( aOptions.contains( "window" ) )
    {
        std::string w = py::str( aOptions[ "window" ] ).cast<std::string>();
        std::transform( w.begin(), w.end(), w.begin(),
                        []( unsigned char c ) { return std::tolower( c ); } );
        const std::map<std::string, std::string> map = {
            { "rect", "rectangular" }, { "rectangular", "rectangular" },
            { "hann", "hanning" },     { "hanning", "hanning" },
            { "hamming", "hamming" },  { "bartlet", "bartlet" }, { "bartlett", "bartlet" },
            { "blackman", "blackman" }, { "gaussian", "gaussian" }, { "flattop", "flattop" } };
        auto it = map.find( w );
        if( it == map.end() )
            throw std::invalid_argument( "unsupported FFT window: '" + w + "'" );
        window = it->second;
    }

    if( aOptions.contains( "n_points" ) )
        nPoints = py::cast<int>( aOptions[ "n_points" ] );

    std::string preFftPlot = sim->CurrentPlotName().ToStdString();

    bool ok;
    {
        py::gil_scoped_release nogil;

        sim->Command( "set specwindow = " + window );

        if( nPoints > 0 )
            sim->Command( "set fft_points = " + std::to_string( nPoints ) );

        ok = sim->Command( "fft " + aVector );
    }

    std::string postFftPlot = sim->CurrentPlotName().ToStdString();

    if( postFftPlot == preFftPlot )
    {
        py::gil_scoped_release nogil;
        sim->Command( "setplot spec1" );
        postFftPlot = sim->CurrentPlotName().ToStdString();
    }

    std::vector<double> freq;
    std::vector<double> mag;
    std::vector<double> phase;

    {
        py::gil_scoped_release nogil;
        freq  = sim->GetRealVector( "frequency" );
        mag   = sim->GetGainVector( aVector );
        phase = sim->GetPhaseVector( aVector );
    }

    py::dict result;
    result[ "ok" ]        = ok;
    result[ "vector" ]    = aVector;
    result[ "window" ]    = window;
    result[ "plot" ]      = postFftPlot;
    result[ "freq" ]      = freq;
    result[ "magnitude" ] = mag;
    result[ "phase" ]     = phase.empty() ? py::object( py::none() )
                                          : py::object( py::cast( phase ) );
    return result;
}

std::map<std::string, std::string>& sim_param_cache()
{
    static std::map<std::string, std::string> s_cache;
    return s_cache;
}

py::dict sim_adv_set_simulation_parameter( const std::string& aName, const std::string& aValue )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame_adv();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator_adv( frame );
    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = sim->Command( "set " + aName + " = " + aValue );
    }
    if( ok )
        sim_param_cache()[ aName ] = aValue;
    py::dict result;
    result[ "ok" ]    = ok;
    result[ "name" ]  = aName;
    result[ "value" ] = aValue;
    return result;
}

py::object sim_adv_get_simulation_parameter( const std::string& aName )
{
    (void) require_simulator_frame_adv();
    auto it = sim_param_cache().find( aName );
    if( it == sim_param_cache().end() )
        return py::none();
    py::dict result;
    result[ "name" ]   = aName;
    result[ "value" ]  = it->second;
    result[ "cached" ] = true;
    return std::move( result );
}

// ──────────────────────────────────────────────────────────────────────────
// parse_subckt_lib — parse a SPICE .lib file via KiCad's SIM_LIBRARY_SPICE
// + SPICE_LIBRARY_PARSER and return {name: {pin_count, pin_names}}.
// ──────────────────────────────────────────────────────────────────────────
py::dict sim_adv_parse_subckt_lib( const std::string& aPath )
{
    py::dict result;
    result[ "path" ] = aPath;

    wxString path = wxString::FromUTF8( aPath.c_str() );

    if( !wxFileName::Exists( path ) )
    {
        result[ "ok" ]       = false;
        result[ "models" ]   = py::dict();
        result[ "messages" ] = py::list();
        result[ "error" ]    = std::string( "file not found" );
        return result;
    }

    WX_STRING_REPORTER reporter;
    SIM_LIBRARY_SPICE  library( /*aForceFullParse*/ true );

    bool ok = true;
    try
    {
        library.ReadFile( path, reporter );
    }
    catch( const std::exception& e )
    {
        ok = false;
        result[ "error" ] = std::string( e.what() );
    }

    py::dict models;
    for( const SIM_LIBRARY::MODEL& mdl : library.GetModels() )
    {
        py::dict model_info;
        model_info[ "pin_count" ] = mdl.model.GetPinCount();

        py::list pin_names;
        for( const std::string& pn : mdl.model.GetPinNames() )
            pin_names.append( pn );

        model_info[ "pin_names" ] = pin_names;
        models[ py::str( mdl.name ) ] = model_info;
    }

    py::list messages;
    if( reporter.HasMessage() )
    {
        wxString text = reporter.GetMessages();
        wxArrayString lines = wxSplit( text, '\n' );
        for( const wxString& line : lines )
        {
            if( !line.IsEmpty() )
                messages.append( std::string( line.ToUTF8() ) );
        }
    }

    result[ "ok" ] = ok && !reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR );
    result[ "models" ]   = models;
    result[ "messages" ] = messages;
    return result;
}


} // anon


void klicad_register_sim_advanced_bindings( py::module_& m )
{
    m.doc() = "KliCAD advanced SPICE simulator binding — workbook persistence, "
              "parameter sweeps, tuners, measurements, FFT, and ngspice "
              "parameter access.  Layered on top of klicad_native_simulator.";

    m.def( "save_workbook", &sim_adv_save_workbook, py::arg( "path" ),
           "Save the current SIMULATOR_FRAME workbook to a .kicad_wks file." );

    m.def( "load_workbook", &sim_adv_load_workbook, py::arg( "path" ),
           "Load a .kicad_wks workbook into the SIMULATOR_FRAME." );

    m.def( "parameter_sweep", &sim_adv_parameter_sweep,
           py::arg( "parameter" ), py::arg( "start" ), py::arg( "stop" ), py::arg( "step" ),
           py::arg( "analysis" ) = std::string( "tran" ),
           py::arg( "analysis_args" ) = py::dict(),
           "Sweep `parameter` over [start, stop] in step increments, running "
           "analysis ('tran'|'ac'|'dc'|'op'|'noise') with analysis_args at each "
           "value.  Returns {parameter, analysis, values, plots, errors}.  Pair "
           "with klicad_native_simulator.get_vector(name, plot=plots[i])." );

    m.def( "add_tuner", &sim_adv_add_tuner, py::arg( "component_ref" ),
           "Add a tuner slider for the symbol with the given hierarchical ref. "
           "Only tunable models (passives, sources, behavioural) accept tuners." );

    m.def( "list_tuners", &sim_adv_list_tuners,
           "Return [{ref, window_id}, ...] for the active tuner sliders." );

    m.def( "set_tuner_value", &sim_adv_set_tuner_value,
           py::arg( "ref" ), py::arg( "value" ),
           "Set a tuner value (drives both ngspice `alter` and the GUI slider). "
           "Returns {ok, ref, value, altered, widget_synced}." );

    m.def( "remove_tuner", &sim_adv_remove_tuner, py::arg( "component_ref" ),
           "Remove a tuner.  Currently raises — SIMULATOR_FRAME has no public "
           "RemoveTuner accessor (private on m_ui)." );

    m.def( "measure", &sim_adv_measure, py::arg( "expression" ),
           "Evaluate a measurement against the last sim.  High-level forms: "
           "max/min/avg/rms/pp/integ(<signal>), rise_time/fall_time(<signal>) "
           "(default 10%/90%).  Raw passthrough: 'meas tran <handle> ...'.  "
           "Returns {ok, expression, command, kind, result_var, value}; raw "
           "returns value=None — read result_var via simulator.get_vector." );

    m.def( "list_measurements", &sim_adv_list_measurements,
           "Return the high-level measurement names this binding recognizes." );

    m.def( "fft", &sim_adv_fft, py::arg( "vector_name" ),
           py::arg( "options" ) = py::dict(),
           "FFT a time-domain vector via ngspice `fft`.  options: {window: "
           "'rect'|'hann'|'hamming'|'bartlet'|'blackman'|'gaussian'|'flattop', "
           "n_points: int}.  Returns {ok, vector, window, plot, freq, magnitude, "
           "phase}.  Side effect: switches current plot to spec<N>." );

    m.def( "set_simulation_parameter", &sim_adv_set_simulation_parameter,
           py::arg( "name" ), py::arg( "value" ),
           "Set an ngspice option via `set name = value`.  Mirrored in a "
           "process-local cache so get_simulation_parameter can read it back." );

    m.def( "get_simulation_parameter", &sim_adv_get_simulation_parameter,
           py::arg( "name" ),
           "Read back a parameter previously set via set_simulation_parameter. "
           "Returns None for parameters never set here (ngspice's `print` output "
           "isn't captureable today)." );

    m.def( "parse_subckt_lib", &sim_adv_parse_subckt_lib, py::arg( "path" ),
           "Parse a SPICE .lib file via SIM_LIBRARY_SPICE + SPICE_LIBRARY_PARSER. "
           "Returns {ok, path, models: {name: {pin_count, pin_names: [...]}, ...}, "
           "messages: [...], error?: str}.  Authoritative source for .SUBCKT "
           "arity + pin names — Python clients should NOT reimplement this "
           "parsing locally (see klicad-python/CONTRIBUTING.md)." );
}
