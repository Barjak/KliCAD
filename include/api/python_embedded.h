/*
 * Local fork addition: in-process Python interpreter lifecycle.
 *
 * The embedded interpreter is initialized once per KiCad process when the
 * always-on IPC API server starts, and torn down when the server stops.  Its
 * sole job is to host the klicad_native pybind11 module so that scripted
 * clients can drive KiCad through a single RunPython IPC command instead of
 * requiring per-method proto definitions.
 *
 * All Python execution happens on KiCad's main thread (the IPC server marshals
 * requests via wxEvtHandler) so we don't release the GIL after init — every
 * RunPython handler acquires it via pybind11::gil_scoped_acquire.
 */

#ifndef KICAD_PYTHON_EMBEDDED_H
#define KICAD_PYTHON_EMBEDDED_H

#include <kicommon.h>
#include <string>

class KICOMMON_API EMBEDDED_PYTHON
{
public:
    EMBEDDED_PYTHON();
    ~EMBEDDED_PYTHON();

    EMBEDDED_PYTHON( const EMBEDDED_PYTHON& ) = delete;
    EMBEDDED_PYTHON& operator=( const EMBEDDED_PYTHON& ) = delete;

    // Idempotent.  Calls pybind11::initialize_interpreter and primes the
    // shared __main__ namespace.  Safe to call from any thread; only the
    // first call does work.
    bool Init();

    // Idempotent.  Calls pybind11::finalize_interpreter.
    void Finalize();

    bool IsInitialized() const { return m_initialized; }

    // The version string the embedded interpreter reports (sys.version).
    // Empty if not initialized.
    std::string PythonVersion() const;

private:
    bool m_initialized;
};

#endif // KICAD_PYTHON_EMBEDDED_H
