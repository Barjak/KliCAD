/*
 * KliCAD: kiface-side pybind11 binding registration.
 *
 * The cvpcb kiface is dlopen'd lazily, well after KICAD_API_SERVER::Start
 * has called py::initialize_interpreter.  PYBIND11_EMBEDDED_MODULE's static
 * initializer calls PyImport_AppendInittab, which CPython forbids post-init
 * — so EMBEDDED_MODULE can't be used inside kiface code.
 *
 * Instead, each kiface-resident binding TU exposes a
 *     void klicad_register_<name>_bindings( pybind11::module_& m );
 * function.  IFACE::OnKifaceStart calls klicad_register_cvpcb_bindings()
 * (defined in klicad_kiface_register.cpp), which:
 *   1. Acquires the GIL.
 *   2. Creates each klicad_native_<name> module via the Python C API and
 *      inserts it into sys.modules (bypassing inittab entirely).
 *   3. Calls each register function on the freshly-created module.
 *
 * To add a new kiface-resident binding:
 *   - Write your bindings_<name>.cpp with
 *         void klicad_register_<name>_bindings( pybind11::module_& m ) { ... }
 *     and add the function declaration to this header.
 *   - Add the .cpp to CVPCB_SRCS in cvpcb/CMakeLists.txt.
 *   - Add one entry to klicad_register_cvpcb_bindings() in the .cpp.
 */

#ifndef KLICAD_KIFACE_REGISTER_H
#define KLICAD_KIFACE_REGISTER_H

#include <pybind11/embed.h>

// Per-binding register functions (one per kiface-resident bindings_*.cpp).
void klicad_register_cvpcb_bindings_impl( pybind11::module_& m );


// Called once from IFACE::OnKifaceStart.  Idempotent (does nothing on
// second call).  Safe to call even if the embedded Python interpreter
// isn't up — it'll just no-op.
void klicad_register_cvpcb_bindings();

#endif // KLICAD_KIFACE_REGISTER_H
