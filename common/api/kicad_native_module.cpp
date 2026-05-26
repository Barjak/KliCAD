/*
 * Local fork addition: the klicad_native pybind11 module.
 *
 * This is the *single Python-side import* through which scripted clients
 * reach into KiCad's C++ object model.  The intent is for every IPC handler /
 * subsystem to add its own bindings here (or in a per-subsystem TU that gets
 * compiled into kicommon / the relevant kiface) so that
 *
 *     from klicad_native import ...
 *
 * grows automatically with each new bound class.  No proto schemas required.
 *
 * The smoke-test build only exposes two trivial entries (echo, version) to
 * prove the binding + embedded interpreter + RunPython IPC pipeline works
 * end-to-end.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <build_version.h>
#include <string>

namespace py = pybind11;

PYBIND11_EMBEDDED_MODULE( klicad_native, m )
{
    m.doc() = "KiCad native bindings (local fork). Single import point for "
              "all scripted access to KiCad's C++ object model.";

    m.def( "echo",
           []( const std::string& s ) { return s; },
           py::arg( "s" ),
           "Return the input string unchanged. Smoke-test sanity probe." );

    m.def( "version",
           []() { return std::string( GetMajorMinorPatchVersion().mb_str() ); },
           "KiCad major.minor.patch version string (touches real KiCad code "
           "to prove the binding can call into libkicommon)." );
}
