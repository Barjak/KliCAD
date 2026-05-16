/*
 * Local fork addition: in-process Python interpreter lifecycle.
 * See include/api/python_embedded.h for the contract.
 */

#include <api/python_embedded.h>

#include <api/api_utils.h>  // traceApi
#include <wx/log.h>

#include <pybind11/embed.h>

namespace py = pybind11;


EMBEDDED_PYTHON::EMBEDDED_PYTHON() : m_initialized( false )
{
}


EMBEDDED_PYTHON::~EMBEDDED_PYTHON()
{
    Finalize();
}


bool EMBEDDED_PYTHON::Init()
{
    if( m_initialized )
        return true;

    try
    {
        // initialize_interpreter(init_signal_handlers=false) so embedded Python
        // does not steal SIGINT etc. from KiCad.
        py::initialize_interpreter( false );
        m_initialized = true;

        wxLogTrace( traceApi, "EmbeddedPython: initialized (%s)",
                    PythonVersion().c_str() );
        return true;
    }
    catch( const std::exception& e )
    {
        wxLogTrace( traceApi, "EmbeddedPython: init failed: %s", e.what() );
        return false;
    }
}


void EMBEDDED_PYTHON::Finalize()
{
    if( !m_initialized )
        return;

    try
    {
        py::finalize_interpreter();
    }
    catch( const std::exception& e )
    {
        wxLogTrace( traceApi, "EmbeddedPython: finalize failed: %s", e.what() );
    }

    m_initialized = false;
}


std::string EMBEDDED_PYTHON::PythonVersion() const
{
    if( !m_initialized )
        return {};

    try
    {
        py::gil_scoped_acquire gil;
        py::object sys = py::module_::import( "sys" );
        return sys.attr( "version" ).cast<std::string>();
    }
    catch( const std::exception& )
    {
        return {};
    }
}
