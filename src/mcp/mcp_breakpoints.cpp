/*
 *  DOSBox-X MCP — emulator-state bridge for breakpoints (Slice 6):
 *  breakpoint_list / breakpoint_add / breakpoint_delete.
 *
 *  Like the Slice 5 execution bridge, the breakpoint store is debugger-private
 *  (CBreakpoint and its static BPoints list are not exported), so the actual
 *  mutation/listing lives in debug.cpp behind the MCP_Breakpoint* functions (the
 *  single Slice 6 core edit, in the docs/MCP_BUILD_PLAN.md manifest). This bridge
 *  only marshals the parsed request / reads back plain out-params into the pure
 *  layer's structs; the param parsing and JSON formatting stay pure in
 *  mcp_protocol.cpp (unit-tested without a boot).
 *
 *  Must run on the emulator thread while parked. Compiled only under
 *  --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"

/* Defined in src/debug/debug.cpp under #if C_MCP (the Slice 6 core edit).
 * Declared here, matching the definitions, to avoid pulling the ncurses-laden
 * debug headers into the MCP build. `type` is the EBreakpoint value that BpType
 * mirrors; ah/al == -1 means "match all". */
extern int  MCP_BreakpointAdd(int type, uint16_t seg, uint32_t off,
                              uint8_t intnr, int ah, int al, bool once);
extern bool MCP_BreakpointDelete(int index);
extern int  MCP_BreakpointCount(void);
extern bool MCP_BreakpointGet(int index, int *type, uint16_t *seg, uint32_t *off,
                              int *intnr, int *ah, int *al, int *memvalue,
                              bool *once, bool *active);

namespace mcp {

int bp_add(const BpAddRequest &req) {
    return MCP_BreakpointAdd((int)req.type, req.seg, req.off, req.intnr,
                             req.ah, req.al, req.once);
}

bool bp_delete(int index) {
    return MCP_BreakpointDelete(index);
}

void bp_list(std::vector<BreakpointInfo> &out) {
    out.clear();
    int n = MCP_BreakpointCount();
    out.reserve(n > 0 ? (size_t)n : 0);
    for (int i = 0; i < n; i++) {
        int type = 0, intnr = 0, ah = -1, al = -1, memvalue = -1;
        uint16_t seg = 0; uint32_t off = 0;
        bool once = false, active = false;
        if (!MCP_BreakpointGet(i, &type, &seg, &off, &intnr, &ah, &al,
                               &memvalue, &once, &active))
            continue;
        BreakpointInfo bp;
        bp.index    = i;
        bp.type     = (BpType)type;
        bp.seg      = seg;
        bp.off      = off;
        bp.intnr    = (uint8_t)intnr;
        bp.ah       = ah;
        bp.al       = al;
        bp.memvalue = memvalue;
        bp.once     = once;
        bp.active   = active;
        out.push_back(bp);
    }
}

} // namespace mcp

#endif /* C_MCP */
