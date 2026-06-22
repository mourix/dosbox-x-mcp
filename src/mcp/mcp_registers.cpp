/*
 *  DOSBox-X MCP — emulator-state bridge for read_registers (Slice 3).
 *
 *  The single place the register tool touches core emulator globals
 *  (cpu_regs / Segs / cpu, via include/regs.h + include/cpu.h). It only *reads*
 *  them into a plain RegisterSnapshot; the formatting lives in the pure protocol
 *  layer (mcp_protocol.cpp). No core files are edited — this reuses the same
 *  globals/macros the debugger's DrawRegisters reads (debug.cpp:1146), so it adds
 *  nothing to the core-edit manifest. Must run on the emulator thread while parked.
 *
 *  Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"
#include "regs.h"
#include "cpu.h"

namespace mcp {

void snapshot_registers(RegisterSnapshot &s) {
    s.eax = (uint32_t)reg_eax;
    s.ebx = (uint32_t)reg_ebx;
    s.ecx = (uint32_t)reg_ecx;
    s.edx = (uint32_t)reg_edx;
    s.esi = (uint32_t)reg_esi;
    s.edi = (uint32_t)reg_edi;
    s.ebp = (uint32_t)reg_ebp;
    s.esp = (uint32_t)reg_esp;
    s.eip = (uint32_t)reg_eip;

    s.cs = SegValue(cs);
    s.ds = SegValue(ds);
    s.es = SegValue(es);
    s.fs = SegValue(fs);
    s.gs = SegValue(gs);
    s.ss = SegValue(ss);

    s.eflags   = (uint32_t)reg_flags;
    s.pmode    = cpu.pmode;
    s.code_big = cpu.code.big;
    s.vm86     = (reg_flags & FLAG_VM) != 0;
    s.cpl      = (unsigned)cpu.cpl;
}

} // namespace mcp

#endif /* C_MCP */
