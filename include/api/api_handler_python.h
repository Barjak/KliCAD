/*
 * Local fork addition: API handler that exposes the embedded Python
 * interpreter via the RunPython IPC command.
 *
 * Registered alongside API_HANDLER_COMMON during KICAD_API_SERVER::Start so
 * that the command is reachable from every entry point that registers the
 * common handler.
 */

#ifndef KICAD_API_HANDLER_PYTHON_H
#define KICAD_API_HANDLER_PYTHON_H

#include <api/api_handler.h>
#include <api/common/commands/base_commands.pb.h>

class EMBEDDED_PYTHON;

class KICOMMON_API API_HANDLER_PYTHON : public API_HANDLER
{
public:
    explicit API_HANDLER_PYTHON( EMBEDDED_PYTHON* aPython );

private:
    HANDLER_RESULT<kiapi::common::commands::RunPythonResponse> handleRunPython(
            const HANDLER_CONTEXT<kiapi::common::commands::RunPython>& aCtx );

    EMBEDDED_PYTHON* m_python;
};

#endif // KICAD_API_HANDLER_PYTHON_H
