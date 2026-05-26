/*
 * KliCAD subsystem binding: SPICE simulator (ngspice) — klicad_native_simulator.
 *
 * Exposes programmatic access to the running SIMULATOR_FRAME's SPICE_SIMULATOR:
 *   - generate_netlist_from_schematic()  — run NETLIST_EXPORTER_SPICE
 *   - load_netlist(text)                 — push SPICE source into ngspice
 *   - command(cmd)                       — raw ngspice command
 *   - run_analysis(kind, **params)       — typed analysis dispatch
 *   - stop()                             — abort an in-flight run
 *   - list_plots() / list_vectors()      — enumerate ngspice plots & vectors
 *   - get_vector(name)                   — fetch a vector + its X axis
 *   - get_state()                        — frame/simulator state snapshot
 *
 * Pattern B (kiface-resident).  Registered at kiface-load time from
 * eeschema/api/klicad_kiface_register.cpp::klicad_register_eeschema_bindings()
 * — DO NOT use PYBIND11_EMBEDDED_MODULE here: its static initializer runs
 * after py::initialize_interpreter, and PyImport_AppendInittab refuses
 * post-init.
 *
 * Frame discovery: walk wxTopLevelWindows for FRAME_SIMULATOR (then static_cast
 * to SIMULATOR_FRAME, mirroring bindings_schematic_state.cpp's pattern).
 *
 * Frame spawning: SIMULATOR_FRAME's ctor dereferences SCH_BASE_FRAME via its
 * parent — so we must spawn FRAME_SCH first, then FRAME_SIMULATOR with the
 * SCH frame as parent.  This mirrors what bindings_gui.cpp's show_frame()
 * does for FRAME_SIMULATOR.
 *
 * The simulator is accessed via SIMULATOR_FRAME::GetSimulator() which returns
 * a std::shared_ptr<SPICE_SIMULATOR>.  All vector / command / state methods
 * live on SPICE_SIMULATOR (the base class of NGSPICE).  We never down-cast to
 * NGSPICE — the SPICE_SIMULATOR virtual surface is sufficient.
 *
 * Returns structured py::dict / py::list only — no streaming, no protos.
 *
 * Limitations:
 *   - The `run_analysis` path issues a single ngspice command line; tuners,
 *     IBIS, workbooks, and the multi-tab notebook are deferred.
 *   - Stdout capture for `command()` returns an empty string today — ngspice's
 *     SendChar callback is wired through SIMULATOR_REPORTER inside the running
 *     frame, which routes to its console panel.  Re-routing through a
 *     per-call reporter to capture text would require swapping the reporter
 *     pointer under m_reporterMutex around each command, which races with
 *     background simulation threads.  Punted to a later iteration.
 *   - generate_netlist_from_schematic() requires the SIMULATOR_FRAME to be
 *     up (so we can reach its SCHEMATIC via the parent SCH_EDIT_FRAME).  A
 *     blank schematic produces an empty netlist with just .title / .end.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <frame_type.h>
#include <kiway.h>
#include <kiway_holder.h>
#include <kiway_player.h>

#include <eda_base_frame.h>
#include <reporter.h>
#include <richio.h>

#include <sch_edit_frame.h>
#include <schematic.h>

#include <sim/sim_tab.h>
#include <sim/simulator_frame.h>
#include <sim/spice_simulator.h>
#include <sim/sim_types.h>
#include <netlist_exporters/netlist_exporter_spice.h>

#include <wx/string.h>
#include <wx/window.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// ──────────────────────────────────────────────────────────────────────────
// Helpers — names chosen to be unique vs find_live_kiway, _for_erc, _for_gui,
// _for_sch_actions, _for_schematic_state, _for_symbol_editor.
// ──────────────────────────────────────────────────────────────────────────

KIWAY* find_live_kiway_for_simulator()
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


// Walk wxTopLevelWindows for FRAME_SIMULATOR.  Identify via
// EDA_BASE_FRAME::GetFrameType() then static_cast (same RTTI caveat as
// bindings_schematic_state.cpp — we're in the eeschema kiface so the
// SIMULATOR_FRAME typeinfo lives here, but the identifier path is symmetric
// with the other bindings_*.cpp).
SIMULATOR_FRAME* find_simulator_frame()
{
    for( wxWindow* w : wxTopLevelWindows )
    {
        EDA_BASE_FRAME* base = dynamic_cast<EDA_BASE_FRAME*>( w );

        if( !base )
            continue;

        if( base->GetFrameType() == FRAME_SIMULATOR )
            return static_cast<SIMULATOR_FRAME*>( base );
    }
    return nullptr;
}


// Find or spawn the SIMULATOR_FRAME.  SIMULATOR_FRAME's ctor needs an
// SCH_BASE_FRAME parent (see bindings_gui.cpp comment for FRAME_SIMULATOR),
// so we spawn FRAME_SCH first.
SIMULATOR_FRAME* require_simulator_frame()
{
    if( SIMULATOR_FRAME* frame = find_simulator_frame() )
        return frame;

    KIWAY* kiway = find_live_kiway_for_simulator();

    if( !kiway )
    {
        throw std::runtime_error(
            "no live KIWAY available — is KiCad's GUI running? "
            "(simulator needs to spawn FRAME_SCH + FRAME_SIMULATOR)" );
    }

    // FRAME_SIMULATOR's ctor dereferences its SCH parent unconditionally
    // (SIMULATOR_FRAME_UI ctor calls SCH_BASE_FRAME::eeconfig() on it).
    KIWAY_PLAYER* sch = kiway->Player( FRAME_SCH, true );

    KIWAY_PLAYER* player = kiway->Player( FRAME_SIMULATOR, true, sch );

    if( !player )
    {
        throw std::runtime_error(
            "failed to spawn FRAME_SIMULATOR via KIWAY::Player(FRAME_SIMULATOR, true)" );
    }

    SIMULATOR_FRAME* frame = find_simulator_frame();

    if( !frame )
    {
        throw std::runtime_error(
            "Player(FRAME_SIMULATOR) returned but the frame is not in wxTopLevelWindows" );
    }

    return frame;
}


// Resolve the SPICE_SIMULATOR for the live SIMULATOR_FRAME.  Throws on
// failure.  Holds a shared_ptr to keep ngspice alive across the call.
std::shared_ptr<SPICE_SIMULATOR> require_spice_simulator( SIMULATOR_FRAME* aFrame )
{
    std::shared_ptr<SPICE_SIMULATOR> sim = aFrame->GetSimulator();

    if( !sim )
        throw std::runtime_error( "SIMULATOR_FRAME::GetSimulator() returned null" );

    return sim;
}


// Map analysis-kind strings to (sim_type, command-line builder).  This is the
// minimum surface to drive ngspice from Python without re-implementing the
// DIALOG_SIM_COMMAND form.
//
//   'tran'    -> .tran STEP STOP [START] [UIC]
//   'ac'      -> .ac TYPE NPOINTS FSTART FSTOP        (type: dec/oct/lin)
//   'dc'      -> .dc SOURCE START STOP STEP
//   'op'      -> .op
//   'noise'   -> .noise V(out) src TYPE NPOINTS FSTART FSTOP
//
// Per-kind required params are pulled from the kwargs py::dict.  Missing keys
// raise std::invalid_argument.

std::string require_str_param( const py::dict& aParams, const char* aKey )
{
    if( !aParams.contains( aKey ) )
    {
        throw std::invalid_argument(
            std::string( "missing required param '" ) + aKey + "'" );
    }
    return py::str( aParams[ aKey ] ).cast<std::string>();
}


std::string optional_str_param( const py::dict& aParams, const char* aKey,
                                const std::string& aDefault )
{
    if( !aParams.contains( aKey ) )
        return aDefault;
    return py::str( aParams[ aKey ] ).cast<std::string>();
}


bool optional_bool_param( const py::dict& aParams, const char* aKey, bool aDefault )
{
    if( !aParams.contains( aKey ) )
        return aDefault;
    return py::cast<bool>( aParams[ aKey ] );
}


std::string build_spice_command( const std::string& aKind, const py::dict& aParams,
                                 SIM_TYPE* aOutType )
{
    if( aKind == "tran" )
    {
        std::string step  = require_str_param( aParams, "step" );
        std::string stop  = require_str_param( aParams, "stop" );
        std::string start = optional_str_param( aParams, "start", std::string() );
        bool        uic   = optional_bool_param( aParams, "uic", false );

        std::string cmd = ".tran " + step + " " + stop;

        if( !start.empty() )
            cmd += " " + start;

        if( uic )
            cmd += " uic";

        *aOutType = ST_TRAN;
        return cmd;
    }
    else if( aKind == "ac" )
    {
        std::string type   = optional_str_param( aParams, "type", "dec" );
        std::string npts   = require_str_param( aParams, "npoints" );
        std::string fstart = require_str_param( aParams, "fstart" );
        std::string fstop  = require_str_param( aParams, "fstop" );

        std::string cmd = ".ac " + type + " " + npts + " " + fstart + " " + fstop;
        *aOutType = ST_AC;
        return cmd;
    }
    else if( aKind == "dc" )
    {
        std::string source = require_str_param( aParams, "source" );
        std::string start  = require_str_param( aParams, "start" );
        std::string stop   = require_str_param( aParams, "stop" );
        std::string step   = require_str_param( aParams, "step" );

        std::string cmd = ".dc " + source + " " + start + " " + stop + " " + step;
        *aOutType = ST_DC;
        return cmd;
    }
    else if( aKind == "op" )
    {
        *aOutType = ST_OP;
        return ".op";
    }
    else if( aKind == "noise" )
    {
        std::string output = require_str_param( aParams, "output" );
        std::string src    = require_str_param( aParams, "src" );
        std::string type   = optional_str_param( aParams, "type", "dec" );
        std::string npts   = require_str_param( aParams, "npoints" );
        std::string fstart = require_str_param( aParams, "fstart" );
        std::string fstop  = require_str_param( aParams, "fstop" );

        std::string cmd = ".noise " + output + " " + src + " " + type + " " + npts
                          + " " + fstart + " " + fstop;
        *aOutType = ST_NOISE;
        return cmd;
    }

    throw std::invalid_argument(
        "kind must be one of: 'tran', 'ac', 'dc', 'op', 'noise' (got '" + aKind + "')" );
}


// SIM_TYPE -> short string for return dicts.
const char* sim_type_to_str( SIM_TYPE aType )
{
    switch( aType )
    {
    case ST_AC:    return "ac";
    case ST_DC:    return "dc";
    case ST_DISTO: return "disto";
    case ST_NOISE: return "noise";
    case ST_OP:    return "op";
    case ST_PZ:    return "pz";
    case ST_SENS:  return "sens";
    case ST_TF:    return "tf";
    case ST_TRAN:  return "tran";
    case ST_SP:    return "sp";
    case ST_FFT:   return "fft";
    default:       return "unknown";
    }
}


// ──────────────────────────────────────────────────────────────────────────
// get_state
// ──────────────────────────────────────────────────────────────────────────
py::dict sim_state_get_state()
{
    py::dict d;

    SIMULATOR_FRAME* frame = find_simulator_frame();
    d[ "has_frame" ] = ( frame != nullptr );

    if( !frame )
    {
        d[ "is_running" ]     = false;
        d[ "current_plot" ]   = std::string();
        d[ "sim_type" ]       = std::string( "none" );
        d[ "sim_finished" ]   = false;
        return d;
    }

    std::shared_ptr<SPICE_SIMULATOR> sim = frame->GetSimulator();
    d[ "has_simulator" ] = ( sim != nullptr );

    if( sim )
    {
        d[ "is_running" ]   = sim->IsRunning();
        d[ "current_plot" ] = sim->CurrentPlotName().ToStdString();
    }
    else
    {
        d[ "is_running" ]   = false;
        d[ "current_plot" ] = std::string();
    }

    d[ "sim_type" ]      = std::string( sim_type_to_str( frame->GetCurrentSimType() ) );
    d[ "sim_command" ]   = frame->GetCurrentSimCommand().ToStdString();
    d[ "sim_finished" ]  = frame->SimFinished();
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// generate_netlist_from_schematic
// ──────────────────────────────────────────────────────────────────────────
std::string sim_state_generate_netlist_from_schematic()
{
    SIMULATOR_FRAME* frame   = require_simulator_frame();
    SCH_EDIT_FRAME*  schFrame = frame->GetSchematicFrame();

    if( !schFrame )
        throw std::runtime_error( "SIMULATOR_FRAME has no associated SCH_EDIT_FRAME" );

    SCHEMATIC& sch = schFrame->Schematic();

    if( !sch.IsValid() )
        throw std::runtime_error( "schematic is not valid (no project loaded?)" );

    NETLIST_EXPORTER_SPICE exporter( &sch );
    WX_STRING_REPORTER     reporter;
    STRING_FORMATTER       formatter;

    bool ok = exporter.DoWriteNetlist( wxEmptyString,
                                       NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS,
                                       formatter, reporter );

    if( !ok && reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) )
    {
        throw std::runtime_error(
            "netlist generation reported errors: "
            + reporter.GetMessages().ToStdString() );
    }

    return formatter.GetString();
}


// ──────────────────────────────────────────────────────────────────────────
// load_netlist
// ──────────────────────────────────────────────────────────────────────────
py::dict sim_state_load_netlist( const std::string& aNetlistText )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = sim->LoadNetlist( aNetlistText );
    }

    py::dict result;
    result[ "ok" ] = ok;
    if( !ok )
        result[ "error" ] = std::string( "SPICE_SIMULATOR::LoadNetlist returned false" );
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// command
// ──────────────────────────────────────────────────────────────────────────
py::dict sim_state_command( const std::string& aCmd )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = sim->Command( aCmd );
    }

    py::dict result;
    result[ "ok" ]     = ok;
    result[ "output" ] = std::string();   // see file-header limitation note
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// run_analysis
// ──────────────────────────────────────────────────────────────────────────
// Goes through the same workbook flow as the GUI's "Run" button:
//   1. NewSimTab(cmd) — creates a SIM_PLOT_TAB (or SIM_NOPLOT_TAB for non-
//      plottables like .op) and makes it current.  This is the canvas
//      AddTrace later draws on.
//   2. Run the analysis:
//        from_schematic=True  -> frame->LoadSimulator(...) + sim->Run()
//          (regenerates the netlist from the live schematic via the
//          circuit model — same code path StartSimulation uses).
//        from_schematic=False -> Command("<bare-form>") on whatever
//          netlist is currently loaded (e.g. via prior load_netlist()).
//          Note: the leading '.' must be stripped — `.tran ...` is a
//          netlist directive that no-ops as an interactive command,
//          while `tran ...` is the interactive run form.
//   3. SetSpicePlotName on the tab so subsequent add_trace calls find
//      the correct ngspice plot.
//
// Earlier this binding called `sim->Command(".tran ...")` directly with
// no tab.  That returned ok=true but the analysis never actually ran
// (silent no-op on the dot-prefixed form), and even when fixed there
// was no GUI tab for traces to attach to.  This rewrite mirrors the
// click-Run-in-the-GUI flow.
py::dict sim_state_run_analysis( const std::string& aKind, const py::kwargs& aKwargs )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    py::dict params = aKwargs;

    SIM_TYPE    simType = ST_UNKNOWN;
    std::string cmd     = build_spice_command( aKind, params, &simType );

    bool from_schematic = optional_bool_param( params, "from_schematic", false );

    SIM_TAB* tab = frame->NewSimTab( wxString::FromUTF8( cmd ) );
    if( !tab )
        throw std::runtime_error( "SIMULATOR_FRAME::NewSimTab returned nullptr" );

    bool ok = false;

    if( from_schematic )
    {
        // Reuse the GUI's full pipeline: regenerate netlist from schematic
        // (LoadSimulator) and kick off the background simulation run.
        unsigned opts = static_cast<unsigned>( tab->GetSimOptions() );

        {
            py::gil_scoped_release nogil;
            ok = frame->LoadSimulator( tab->GetSimCommand(), opts );
            if( ok )
                ok = sim->Run();
        }
    }
    else
    {
        // Run against whatever netlist ngspice currently holds.  Strip the
        // leading '.' (build_spice_command emits the netlist-directive form;
        // Command() needs the interactive form).
        std::string interactive = cmd;
        if( !interactive.empty() && interactive[ 0 ] == '.' )
            interactive.erase( 0, 1 );

        {
            py::gil_scoped_release nogil;
            ok = sim->Command( interactive );
        }
    }

    tab->SetSpicePlotName( sim->CurrentPlotName() );

    py::dict result;
    result[ "ok" ]              = ok;
    result[ "kind" ]            = aKind;
    result[ "command" ]         = cmd;
    result[ "sim_type" ]        = std::string( sim_type_to_str( simType ) );
    result[ "plot_name" ]       = sim->CurrentPlotName().ToStdString();
    result[ "from_schematic" ]  = from_schematic;

    py::list vectors;
    for( const std::string& v : sim->AllVectors() )
        vectors.append( v );
    result[ "vectors" ]   = vectors;

    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// add_trace
// ──────────────────────────────────────────────────────────────────────────
// Thin wrapper around SIMULATOR_FRAME::AddVoltageTrace / AddCurrentTrace.
// What clicking "Add Signal" in the workbook does, exposed scriptable.
// Requires a current SIM_TAB — call run_analysis() (or NewSimTab via the
// workbook) first.
py::dict sim_state_add_trace( const std::string& aName, const std::string& aKind )
{
    SIMULATOR_FRAME* frame = require_simulator_frame();

    if( !frame->GetCurrentSimTab() )
    {
        py::dict d;
        d[ "ok" ]    = false;
        d[ "error" ] = std::string(
            "no current SIM_TAB — run an analysis first (run_analysis(...)) "
            "so there's a plot tab to attach to" );
        return d;
    }

    wxString name = wxString::FromUTF8( aName );

    {
        py::gil_scoped_release nogil;

        if( aKind == "voltage" || aKind == "v" )
            frame->AddVoltageTrace( name );
        else if( aKind == "current" || aKind == "i" )
            frame->AddCurrentTrace( name );
        else
        {
            py::gil_scoped_acquire gil;
            throw std::invalid_argument(
                "kind must be 'voltage'/'v' or 'current'/'i' (got '" + aKind + "')" );
        }
    }

    py::dict d;
    d[ "ok" ]   = true;
    d[ "name" ] = aName;
    d[ "kind" ] = aKind;
    return d;
}


// ──────────────────────────────────────────────────────────────────────────
// stop
// ──────────────────────────────────────────────────────────────────────────
py::dict sim_state_stop()
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    bool ok;
    {
        py::gil_scoped_release nogil;
        ok = sim->Stop();
    }

    py::dict result;
    result[ "ok" ] = ok;
    return result;
}


// ──────────────────────────────────────────────────────────────────────────
// list_plots
// ──────────────────────────────────────────────────────────────────────────
// SPICE_SIMULATOR doesn't expose an "all plots" accessor (only CurrentPlotName
// + AllVectors).  We approximate by running `setplot` and parsing — but that
// requires stdout capture which we don't have.  As a pragmatic minimum, we
// return [current_plot_name] so callers at least have a handle to the latest
// run; future iterations can wire ngSpice_AllPlots through SPICE_SIMULATOR.
py::list sim_state_list_plots()
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    py::list out;

    wxString current = sim->CurrentPlotName();

    if( !current.IsEmpty() )
        out.append( current.ToStdString() );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// list_vectors
// ──────────────────────────────────────────────────────────────────────────
py::list sim_state_list_vectors( const std::string& aPlot )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    // Switch plot if requested.  ngspice's `setplot <name>` rebinds the
    // "current plot" used by AllVectors / GetRealVector / etc.
    if( !aPlot.empty() )
    {
        py::gil_scoped_release nogil;
        sim->Command( "setplot " + aPlot );
    }

    py::list out;
    for( const std::string& v : sim->AllVectors() )
        out.append( v );

    return out;
}


// ──────────────────────────────────────────────────────────────────────────
// get_vector
// ──────────────────────────────────────────────────────────────────────────
//
// Returns:
//   { kind: 'real'|'complex',
//     xaxis: list[float],
//     xaxis_name: str,
//     data: list[float] | { real: list[float], imag: list[float] } }
//
// The vector is classified as complex if AC/SP/NOISE-style output (any
// non-zero imaginary component) is seen.  We probe via GetImaginaryVector;
// if it's empty OR all-zero, treat as real.  This is conservative but matches
// what the plot panel does today.
py::dict sim_state_get_vector( const std::string& aName, const std::string& aPlot )
{
    SIMULATOR_FRAME*                 frame = require_simulator_frame();
    std::shared_ptr<SPICE_SIMULATOR> sim   = require_spice_simulator( frame );

    if( !aPlot.empty() )
    {
        py::gil_scoped_release nogil;
        sim->Command( "setplot " + aPlot );
    }

    // Infer X axis name from current sim type.  GetXAxis is virtual on
    // SPICE_SIMULATOR and returns e.g. "time" / "frequency" / "v-sweep".
    SIM_TYPE simType = frame->GetCurrentSimType();
    wxString xaxisName = sim->GetXAxis( simType );

    std::vector<double>  xAxis;
    std::vector<double>  realData;
    std::vector<double>  imagData;

    {
        py::gil_scoped_release nogil;

        if( !xaxisName.IsEmpty() )
            xAxis = sim->GetRealVector( xaxisName.ToStdString() );

        realData = sim->GetRealVector( aName );
        imagData = sim->GetImaginaryVector( aName );
    }

    // Classify: if any imaginary value is non-zero treat as complex.
    bool isComplex = false;
    for( double v : imagData )
    {
        if( v != 0.0 )
        {
            isComplex = true;
            break;
        }
    }

    py::dict result;
    result[ "name" ]       = aName;
    result[ "xaxis_name" ] = xaxisName.ToStdString();
    result[ "xaxis" ]      = xAxis;

    if( isComplex )
    {
        result[ "kind" ] = std::string( "complex" );

        py::dict data;
        data[ "real" ] = realData;
        data[ "imag" ] = imagData;
        result[ "data" ] = data;
    }
    else
    {
        result[ "kind" ] = std::string( "real" );
        result[ "data" ] = realData;
    }

    return result;
}

} // anon


// Registered at kiface-load time — see header in bindings_schematic_state.cpp
// for why PYBIND11_EMBEDDED_MODULE can't be used inside a lazy-loaded kiface.
void klicad_register_simulator_bindings( py::module_& m )
{
    m.doc() = "KliCAD SPICE simulator binding — programmatic control of the "
              "ngspice instance owned by the running SIMULATOR_FRAME.  "
              "Generate netlists from the active schematic, run typed "
              "analyses (.tran/.ac/.dc/.op/.noise), and fetch result vectors. "
              "Spawns FRAME_SCH + FRAME_SIMULATOR on first call if needed.";

    m.def( "get_state", &sim_state_get_state,
           R"DOC(Return a state snapshot of the simulator subsystem.

Keys: has_frame, has_simulator, is_running, current_plot, sim_type
('tran'|'ac'|'dc'|'op'|'noise'|...), sim_command, sim_finished.

Safe to call even when no SIMULATOR_FRAME is open (returns has_frame=False).
)DOC" );

    m.def( "generate_netlist_from_schematic",
           &sim_state_generate_netlist_from_schematic,
           R"DOC(Generate a SPICE netlist from the running SCH_EDIT_FRAME.

Uses NETLIST_EXPORTER_SPICE with OPTION_DEFAULT_FLAGS (save all V/I/P/events,
adjust passive values, adjust include paths).  Returns the netlist as a
string (.title ... .end).

Requires a SIMULATOR_FRAME (so the SCH parent is reachable).  Raises
RuntimeError if no schematic is loaded or the exporter reports errors.
)DOC" );

    m.def( "load_netlist", &sim_state_load_netlist,
           py::arg( "netlist_text" ),
           R"DOC(Push a SPICE netlist into ngspice (SPICE_SIMULATOR::LoadNetlist).

Returns {ok: bool, error?: str}.  Spawns the simulator frame if needed.
)DOC" );

    m.def( "command", &sim_state_command,
           py::arg( "spice_cmd" ),
           R"DOC(Run a raw ngspice command (e.g. 'op', '.tran 1u 10m', 'setplot tran1').

Returns {ok: bool, output: str}.  Note: stdout capture is not yet wired
(output is always ""); ngspice's SendChar routes to the frame's console.
)DOC" );

    m.def( "run_analysis", &sim_state_run_analysis,
           py::arg( "kind" ) = std::string( "tran" ),
           R"DOC(Run a typed SPICE analysis.

kind: 'tran' | 'ac' | 'dc' | 'op' | 'noise'

Per-kind kwargs:
  tran:  step=str, stop=str, [start=str], [uic=bool]
  ac:    [type='dec'|'oct'|'lin'], npoints=str, fstart=str, fstop=str
  dc:    source=str, start=str, stop=str, step=str
  op:    (no params)
  noise: output=str ('V(out)'), src=str, [type='dec'|'oct'|'lin'],
         npoints=str, fstart=str, fstop=str

All numeric values are strings so SPICE engineering suffixes ('1u', '10m',
'1k') pass through verbatim.

Returns {ok, kind, command, sim_type, plot_name, vectors: list[str]}.
)DOC" );

    m.def( "stop", &sim_state_stop,
           R"DOC(Halt an in-flight simulation (SPICE_SIMULATOR::Stop).

Returns {ok: bool}.
)DOC" );

    m.def( "list_plots", &sim_state_list_plots,
           R"DOC(Return the list of plot names cached by ngspice.

Note: SPICE_SIMULATOR doesn't expose ngSpice_AllPlots today; this returns
just [current_plot_name] as a minimum-viable surface.  Use command('setplot')
to inspect the full list via the simulator console.
)DOC" );

    m.def( "list_vectors", &sim_state_list_vectors,
           py::arg( "plot" ) = std::string(),
           R"DOC(Return the vector (signal) names in the named plot.

plot: '' (default) -> current plot; otherwise calls `setplot <plot>` first.
)DOC" );

    m.def( "get_vector", &sim_state_get_vector,
           py::arg( "name" ),
           py::arg( "plot" ) = std::string(),
           R"DOC(Fetch a vector + its X axis from ngspice.

Returns:
  {
    name: str,
    kind: 'real' | 'complex',
    xaxis_name: str (e.g. 'time', 'frequency'),
    xaxis: list[float],
    data: list[float]                              # if kind == 'real'
        | {real: list[float], imag: list[float]}   # if kind == 'complex'
  }

Use plot='' for current plot, or pass an explicit plot name to switch first.
)DOC" );

    m.def( "add_trace", &sim_state_add_trace,
           py::arg( "name" ),
           py::arg( "kind" ) = std::string( "voltage" ),
           R"DOC(Add a signal trace to the current simulator plot tab.

What clicking "Add Signal" in the simulator workbook does, exposed scriptable.

name: net name for voltage traces (e.g. 'nc1') or device ref-des for current
      traces (e.g. 'R1').  No 'v(...)' or 'i(...)' wrapping — the underlying
      AddVoltageTrace / AddCurrentTrace handles that.

kind: 'voltage' (default) | 'v' | 'current' | 'i'

Requires a current SIM_TAB — call run_analysis(...) first so there's a plot
tab to attach to.

Returns {ok, name, kind} on success, or {ok: False, error} if no current tab.
)DOC" );
}
