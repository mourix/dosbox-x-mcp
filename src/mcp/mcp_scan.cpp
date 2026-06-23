/*
 *  DOSBox-X MCP — emulator-state bridge for the memory scanner (Slice 11):
 *  scan_start / scan_filter / scan_results.
 *
 *  Like the Slice 6 breakpoint bridge, the scanner store is debugger-private (the
 *  single global MEMFINDInstance and its file-private MEMFinder struct are not
 *  exported), so the actual snapshot/filter/walk lives in debug.cpp behind the
 *  MCP_Scan* functions (the single Slice 11 core edit, in the
 *  docs/MCP_BUILD_PLAN.md manifest). This bridge only marshals the parsed request
 *  into those calls and reads back plain out-params into the pure layer's structs;
 *  the param parsing and JSON formatting stay pure in mcp_protocol.cpp
 *  (unit-tested without a boot).
 *
 *  Must run on the emulator thread while parked. Compiled only under
 *  --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"

/* Defined in src/debug/debug.cpp under #if C_MCP (the Slice 11 core edit).
 * Declared here, matching the definitions, to avoid pulling the ncurses-laden
 * debug headers into the MCP build. opType matches the pure ScanOp value. */
extern int  MCP_ScanStart(uint16_t seg, uint32_t ofs, uint32_t range, uint8_t size);
extern long MCP_ScanFilter(int opType, bool usePrev, uint32_t value);
extern bool MCP_ScanState(uint16_t *seg, uint32_t *ofs, uint32_t *baseLinear,
                          uint8_t *size, uint32_t *range, uint32_t *matches,
                          uint32_t *iterations);
extern int  MCP_ScanResults(uint32_t start, int max, uint32_t *total,
                            uint16_t *seg_arr, uint32_t *ofs_arr,
                            uint32_t *lin_arr, uint32_t *val_arr);

namespace mcp {

namespace {
/* Read the scanner state into st (active=false when no scan is in progress). */
void read_state(ScanState &st) {
    uint16_t seg = 0; uint32_t ofs = 0, base = 0, range = 0, matches = 0, iters = 0;
    uint8_t  size = 1;
    st.active = MCP_ScanState(&seg, &ofs, &base, &size, &range, &matches, &iters);
    st.seg = seg; st.off = ofs; st.base_linear = base; st.width = (int)size;
    st.range = range; st.matches = matches; st.iterations = iters;
}
} // namespace

int scan_start(const ScanStartRequest &req, ScanState &st) {
    int rc = MCP_ScanStart(req.seg, req.off, req.range, (uint8_t)req.width);
    read_state(st);
    return rc;
}

long scan_filter(const ScanFilterRequest &req, ScanState &st) {
    long rc = MCP_ScanFilter((int)req.op, req.use_prev, req.value);
    read_state(st);
    return rc;
}

void scan_state(ScanState &st) {
    read_state(st);
}

uint32_t scan_results(uint32_t start, size_t max, std::vector<ScanMatch> &out) {
    out.clear();
    std::vector<uint16_t> seg(max);
    std::vector<uint32_t> ofs(max), lin(max), val(max);
    uint32_t total = 0;
    int filled = MCP_ScanResults(start, (int)max, &total,
                                 seg.data(), ofs.data(), lin.data(), val.data());
    if (filled > 0) {
        out.reserve((size_t)filled);
        for (int i = 0; i < filled; i++) {
            ScanMatch m;
            m.seg = seg[i]; m.off = ofs[i]; m.lin = lin[i]; m.value = val[i];
            out.push_back(m);
        }
    }
    return total;
}

} // namespace mcp

#endif /* C_MCP */
