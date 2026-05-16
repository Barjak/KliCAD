/*
 * Local fork addition: API handler exposing the embedded Python interpreter.
 * See include/api/api_handler_python.h for the contract.
 */

#include <api/api_handler_python.h>
#include <api/python_embedded.h>

#include <api/api_utils.h>  // traceApi
#include <wx/log.h>

#include <pybind11/embed.h>
#include <pybind11/eval.h>

namespace py = pybind11;

using kiapi::common::commands::RunPython;
using kiapi::common::commands::RunPythonResponse;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;


// Python-side runner installed into __main__ on first use.  Splits the code
// into an exec body + optional trailing expression so that scripts ending in
// an expression return repr(value) without the caller needing to wrap it.
// Captures stdout / stderr / traceback into a dict the C++ caller reads.
static const char* k_runner_source = R"PY(
import ast as __kc_ast
import io as __kc_io
import sys as __kc_sys
import traceback as __kc_traceback

def __kc_run_python(code):
    out = {'ok': True, 'stdout': '', 'stderr': '', 'result_repr': '', 'exception_traceback': ''}
    stdout_buf = __kc_io.StringIO()
    stderr_buf = __kc_io.StringIO()
    saved_out, saved_err = __kc_sys.stdout, __kc_sys.stderr
    __kc_sys.stdout, __kc_sys.stderr = stdout_buf, stderr_buf
    ns = __import__('__main__').__dict__
    try:
        tree = __kc_ast.parse(code, mode='exec')
        last_expr = None
        if tree.body and isinstance(tree.body[-1], __kc_ast.Expr):
            last_expr = tree.body.pop().value
        if tree.body:
            exec(compile(tree, '<run_python>', 'exec'), ns)
        if last_expr is not None:
            value = eval(compile(__kc_ast.Expression(last_expr), '<run_python>', 'eval'), ns)
            if value is not None:
                out['result_repr'] = repr(value)
    except BaseException:
        out['ok'] = False
        out['exception_traceback'] = __kc_traceback.format_exc()
    finally:
        __kc_sys.stdout, __kc_sys.stderr = saved_out, saved_err
        out['stdout'] = stdout_buf.getvalue()
        out['stderr'] = stderr_buf.getvalue()
    return out
)PY";


API_HANDLER_PYTHON::API_HANDLER_PYTHON( EMBEDDED_PYTHON* aPython ) :
        API_HANDLER(),
        m_python( aPython )
{
    registerHandler<RunPython, RunPythonResponse>( &API_HANDLER_PYTHON::handleRunPython );
}


HANDLER_RESULT<RunPythonResponse> API_HANDLER_PYTHON::handleRunPython(
        const HANDLER_CONTEXT<RunPython>& aCtx )
{
    RunPythonResponse response;

    if( !m_python || !m_python->IsInitialized() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "embedded Python interpreter is not initialized" );
        return tl::unexpected( e );
    }

    try
    {
        py::gil_scoped_acquire gil;
        py::object main_ns = py::module_::import( "__main__" ).attr( "__dict__" );

        // Lazily install the runner the first time.
        if( !main_ns.contains( "__kc_run_python" ) )
            py::exec( k_runner_source, main_ns );

        py::dict result = main_ns[ "__kc_run_python" ](
                py::str( aCtx.Request.code() ) ).cast<py::dict>();

        response.set_ok( result[ "ok" ].cast<bool>() );
        response.set_stdout( result[ "stdout" ].cast<std::string>() );
        response.set_stderr( result[ "stderr" ].cast<std::string>() );
        response.set_result_repr( result[ "result_repr" ].cast<std::string>() );
        response.set_exception_traceback(
                result[ "exception_traceback" ].cast<std::string>() );
    }
    catch( const py::error_already_set& e )
    {
        response.set_ok( false );
        response.set_exception_traceback(
                std::string( "C++ pybind11 error: " ) + e.what() );
    }
    catch( const std::exception& e )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_UNKNOWN );
        err.set_error_message( std::string( "RunPython internal error: " ) + e.what() );
        return tl::unexpected( err );
    }

    return response;
}
