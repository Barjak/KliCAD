/*
 * KliCAD eeschema kiface binding registration.  See header for rationale.
 */

#include "klicad_kiface_register.h"

#include <pybind11/embed.h>
#include <Python.h>

#include <api/api_utils.h> // traceApi
#include <wx/log.h>

#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;


namespace
{

bool s_eeschema_bindings_registered = false;


// Create a fresh module object, insert it into sys.modules under the given
// name, and return a pybind11 wrapper.  Bypasses PyImport_AppendInittab
// (which is locked after Py_Initialize).
py::module_ make_runtime_module( const char* aName, const char* aDoc )
{
    // PyModule_New returns a new module object with __name__ = aName but no
    // entry in sys.modules.  We add it ourselves so `import aName` works.
    PyObject* raw = PyModule_New( aName );

    if( !raw )
        throw std::runtime_error( std::string( "PyModule_New failed for " ) + aName );

    if( aDoc )
        PyModule_SetDocString( raw, aDoc );

    // Register in sys.modules so import sees it.
    PyObject* sys_modules = PyImport_GetModuleDict();
    PyDict_SetItemString( sys_modules, aName, raw );

    // Hand reference to pybind11 (it takes ownership of the +1 we hold).
    return py::reinterpret_steal<py::module_>( raw );
}

} // anon


void klicad_register_eeschema_bindings()
{
    if( s_eeschema_bindings_registered )
        return;

    if( !Py_IsInitialized() )
    {
        wxLogTrace( traceApi,
                    "KliCAD: skipping eeschema binding registration — "
                    "embedded Python isn't initialized" );
        return;
    }

    try
    {
        py::gil_scoped_acquire gil;

        // One entry per binding TU.  Keep alphabetized.
        struct Entry
        {
            const char* module_name;
            const char* doc;
            void ( *register_fn )( py::module_& );
        };

        const std::vector<Entry> entries = {
            { "klicad_native_annotation",
              "KliCAD schematic annotation binding (kiface-loaded).",
              &klicad_register_annotation_bindings },
            { "klicad_native_auto_layout",
              "KliCAD port-aware OGDF-based schematic auto-layout "
              "(kiface-loaded).  See HANDOFF.md Option C.",
              &klicad_register_auto_layout_bindings },
            { "klicad_native_hierarchy",
              "KliCAD schematic hierarchy / sheet navigation (kiface-loaded).",
              &klicad_register_hierarchy_bindings },
            { "klicad_native_ratsnest",
              "KliCAD schematic ratsnest spec source binding (kiface-loaded).",
              &klicad_register_ratsnest_bindings },
            { "klicad_native_sch_actions",
              "KliCAD schematic-editor TOOL_ACTION runner (kiface-loaded).",
              &klicad_register_sch_actions_bindings },
            { "klicad_native_schematic_state",
              "KliCAD direct SCHEMATIC state binding (kiface-loaded).",
              &klicad_register_schematic_state_bindings },
            { "klicad_native_sim_advanced",
              "KliCAD advanced SPICE (workbooks/sweeps/tuners/measure/fft).",
              &klicad_register_sim_advanced_bindings },
            { "klicad_native_simulator",
              "KliCAD SPICE simulator binding (kiface-loaded).",
              &klicad_register_simulator_bindings },
            { "klicad_native_spec_pane",
              "KliCAD eeschema spec-pane control (kiface-loaded).",
              &klicad_register_spec_pane_bindings },
            { "klicad_native_symbol_editor",
              "KliCAD symbol library editor (kiface-loaded).",
              &klicad_register_symbol_editor_bindings },
        };

        for( const Entry& e : entries )
        {
            py::module_ m = make_runtime_module( e.module_name, e.doc );
            e.register_fn( m );
            wxLogTrace( traceApi, "KliCAD: registered %s", e.module_name );
        }

        s_eeschema_bindings_registered = true;
    }
    catch( const py::error_already_set& e )
    {
        wxLogTrace( traceApi,
                    "KliCAD: eeschema binding registration raised Python "
                    "exception: %s", e.what() );
    }
    catch( const std::exception& e )
    {
        wxLogTrace( traceApi,
                    "KliCAD: eeschema binding registration failed: %s",
                    e.what() );
    }
}
