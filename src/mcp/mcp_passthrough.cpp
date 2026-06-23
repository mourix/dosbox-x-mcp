/*
 *  DOSBox-X MCP — debugger_command passthrough bridge (Slice 13).
 *
 *  Runs an arbitrary debugger command through the built-in command interpreter
 *  (ParseCommand, debug.cpp) on the emulator thread while parked, capturing the
 *  text it emits via DEBUG_ShowMsg. Capture is driven by the single Slice 13
 *  core edit: a one-line hook in DEBUG_ShowMsg (debug_gui.cpp) that calls
 *  MCP_DebugCaptureLine for every emitted line. The hook is a no-op unless a
 *  passthrough is active, so it adds no behaviour to normal debugger output.
 *
 *  The capture is bounded to MCP_PASSTHROUGH_MAX (16 KiB) via the pure
 *  passthrough_append helper (unit-tested in tests/mcp_protocol_tests.cpp), so a
 *  chatty command can never firehose the client (guardrail #3).
 *
 *  Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "config.h"

#if C_MCP

#include <string>
#include <vector>

#include "mcp_protocol.h"

/* The debugger's command interpreter (debug.cpp, global linkage — also declared
 * in src/gui/sdl_gui.cpp). Returns true when the command was recognised. */
bool ParseCommand(char* str);

namespace {
/* Passthrough capture state. File-local so the C-linkage capture hook below and
 * the bridge share it; only ever touched on the emulator thread while parked, so
 * no synchronisation is needed (the dispatcher runs single-threaded there). */
bool        g_capturing = false;
std::string g_capture;
bool        g_truncated = false;
} // namespace

/* Called from DEBUG_ShowMsg (debug_gui.cpp, the Slice 13 core edit) for every
 * emitted line. A no-op unless a debugger_command passthrough is capturing.
 * Appends through the pure capped-append helper so the truncation matches what
 * the unit tests assert. Global linkage (the hook forward-declares it). */
void MCP_DebugCaptureLine(const char *line) {
    if (!g_capturing || line == nullptr) return;
    mcp::passthrough_append(g_capture, std::string(line), g_truncated);
}

namespace mcp {

bool run_debugger_command(const std::string &cmd, std::string &out, bool &truncated) {
    /* Arm capture, run the command, disarm — even if ParseCommand throws we must
     * not leave capture armed, so guard with a tiny RAII reset. */
    struct CaptureGuard {
        ~CaptureGuard() { g_capturing = false; }
    } guard;

    g_capturing = true;
    g_capture.clear();
    g_truncated = false;

    /* ParseCommand takes a mutable char* (it upper-cases a private copy, but the
     * signature is non-const), so hand it a writable buffer. */
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    bool recognized = ParseCommand(buf.data());

    g_capturing = false;
    out = g_capture;
    truncated = g_truncated;
    /* Drop the captured text now that it has been copied out (it can be 16 KiB). */
    g_capture.clear();
    return recognized;
}

} // namespace mcp

#endif /* C_MCP */
