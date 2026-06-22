/*
 *  DOSBox-X MCP — emulator-state bridge for read_registers (Slice 3).
 *
 *  The single place the register tools touch core emulator globals
 *  (cpu_regs / Segs / cpu, via include/regs.h + include/cpu.h). read_registers
 *  only *reads* them into a plain RegisterSnapshot; write_register (Slice 7)
 *  delegates to the debugger's own ChangeRegister so it inherits the exact
 *  longest-name-first register matching and per-register width masking the
 *  debugger's SR command uses. The formatting/parsing live in the pure protocol
 *  layer (mcp_protocol.cpp). No core files are edited — this reuses the same
 *  globals/macros the debugger's DrawRegisters reads (debug.cpp:1146) and the
 *  ChangeRegister global (debug.cpp:1728, external linkage, declared below like
 *  the GetAddress/DasmI386 globals in mcp_memory.cpp), so it adds nothing to the
 *  core-edit manifest. Must run on the emulator thread while parked.
 *
 *  Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"
#include "regs.h"
#include "cpu.h"

#include <cstdio>

/* Defined in src/debug/debug.cpp (external linkage, not in any installed header).
 * Parses "<REG> <value>" assignments and applies them, returning false if the
 * register name is not recognised. */
extern bool ChangeRegister(char* const str);

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

bool write_register(const RegWriteRequest &req) {
    /* Hand ChangeRegister exactly what the SR command would: the register name
     * followed by the value. A space separates them (GetHexValue skips it), and
     * the value is plain hex (GetHexValue auto-detects the base). ChangeRegister
     * returns false for an unrecognised register name. */
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %X", req.reg.c_str(), (unsigned)req.value);
    return ChangeRegister(buf);
}

} // namespace mcp

#endif /* C_MCP */
