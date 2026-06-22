/*
 *  DOSBox-X MCP — emulator-state bridge for execution control (Slice 5):
 *  step / step_over / continue / break.
 *
 *  Unlike the read-only Slice 3/4 bridges (mcp_registers.cpp / mcp_memory.cpp)
 *  this one *drives* execution, so it cannot avoid mutating debugger state. To
 *  keep that mutation co-located with the debugger's own statics (debugging /
 *  exitLoop / mustCompleteInstruction / the static StepOver), the actual
 *  primitive lives in debug.cpp as MCP_DebugExec — the single Slice 5 core edit,
 *  listed in the docs/MCP_BUILD_PLAN.md manifest. This bridge only marshals the
 *  request enum into that call and reads back the resulting CS:EIP + execution
 *  state; the request classification and JSON formatting stay pure in
 *  mcp_protocol.cpp (unit-tested without a boot).
 *
 *  Must run on the emulator thread: parked for step/step_over/continue, running
 *  for break. Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"
#include "regs.h"

/* Defined in src/debug/debug.cpp under #if C_MCP (the one Slice 5 core edit).
 * mode 0=step 1=step_over 2=continue 3=break; returns the DEBUG_Run status and
 * sets *resumed when the guest was released to free-run. */
extern int32_t MCP_DebugExec(int mode, bool *resumed);

/* debug.cpp: true while the CPU is parked in DEBUG_Loop. */
bool IsDebuggerActive(void);

namespace mcp {

void exec_control(ExecOp op, ExecResult &out) {
    int mode = op == EXEC_STEP      ? 0 :
               op == EXEC_STEP_OVER ? 1 :
               op == EXEC_CONTINUE  ? 2 : 3;

    bool resumed = false;
    out.ran     = MCP_DebugExec(mode, &resumed);
    out.resumed = resumed;
    /* Read back the live CS:EIP. For a plain single step this is the new stop;
     * for a resumed op it is the point the guest was released from (the real
     * stop is observed later via ping/read_registers). */
    out.cs    = SegValue(cs);
    out.eip   = (uint32_t)reg_eip;
    out.state = IsDebuggerActive() ? STATE_PARKED : STATE_RUNNING;
}

} // namespace mcp

#endif /* C_MCP */
