/*
 *  DOSBox-X MCP — lifecycle bridge (Slice 12): reset / quit.
 *
 *  reset/quit can't be performed inline in the dispatcher: the action is a C++
 *  throw (int(3) reboot / int(0) kill switch — the same signals the menu Reset
 *  and the quit path raise) that must propagate all the way up to
 *  DOSBOX_RunMachine's catch (gui/sdlmain.cpp), and it must fire only *after* the
 *  success reply has been written back to the client (otherwise quit tears the
 *  process down before the socket flushes). So the dispatcher records a pending
 *  op here and returns a normal reply; MCP_LifecycleService — called once per
 *  frame from MCP_GFXFrameService (the existing core call site), after the queue
 *  drain — lets a few frames pass (the reply flushes) and then performs the throw.
 *
 *  No new core edit: the throw originates in this MCP module at the existing
 *  MCP_GFXFrameService call site, and a reset arriving while the CPU is parked
 *  first disengages the debugger by reusing the Slice 5 core edit MCP_DebugExec
 *  (continue), so DOSBOX_RunMachine re-enters the normal loop after the reboot
 *  instead of the debugger loop. Both fields are touched only on the emulator
 *  thread (the dispatcher and the frame service), so no synchronization is needed.
 *
 *  Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md (Slice 12).
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"

/* debug.cpp (the Slice 5 core edit): mode 2 = continue (disengage the debugger
 * and release the guest). Reused to leave a clean normal-loop state before a
 * reset throw when the CPU is parked. */
extern int32_t MCP_DebugExec(int mode, bool *resumed);

/* debug.cpp: true while the CPU is parked in DEBUG_Loop. */
bool IsDebuggerActive(void);

namespace {

/* Pending lifecycle action: -1 none, otherwise a LifecycleOp value. Set by the
 * dispatcher, consumed by the frame service — both on the emulator thread. */
int g_pending = -1;

/* Frames to let elapse between the request and the throw, so the success reply
 * is written to the client first. A handful of frame ticks is a few ms — far
 * more than the server thread needs to flush the small reply after the drain
 * notifies it, in either execution state (parked DEBUG_Loop spins at ~1 kHz). */
int g_grace = 0;
const int kGraceFrames = 3;

} // namespace

namespace mcp {

void lifecycle_request(LifecycleOp op) {
    g_pending = (int)op;
    g_grace   = kGraceFrames;
}

} // namespace mcp

/* Called from MCP_GFXFrameService after the queue drain. A no-op until a reset/
 * quit is pending; then it counts down the grace frames and performs the throw,
 * which propagates up to DOSBOX_RunMachine. */
void MCP_LifecycleService(void) {
    if (g_pending < 0) return;
    if (g_grace > 0) { g_grace--; return; }

    int op = g_pending;
    g_pending = -1;

    if (op == (int)mcp::LIFE_RESET) {
        /* Disengage the debugger first if parked: the reboot path does not reset
         * the active loop, so without this the machine would re-enter the
         * debugger loop after rebooting. */
        if (IsDebuggerActive()) {
            bool resumed = false;
            MCP_DebugExec(2, &resumed);
        }
        throw int(3);   /* reboot the system (caught in DOSBOX_RunMachine) */
    } else {            /* LIFE_QUIT */
        throw int(0);   /* kill switch -> clean shutdown + process exit */
    }
}

#endif /* C_MCP */
