/*
 *  DOSBox-X MCP (Model Context Protocol) server — Slice 0 scaffolding stub.
 *
 *  This translation unit exists so the --enable-mcp build wiring (configure.ac,
 *  src/Makefile.am, src/mcp/Makefile.am) links a real libmcp.a into dosbox-x.
 *  It carries no emulator/debugger logic yet; later slices build the TCP
 *  JSON-RPC server and debugger bridge here. See docs/MCP_BUILD_PLAN.md.
 */

#include "mcp.h"

#if C_MCP

const char *MCP_Version(void) {
    return MCP_VERSION;
}

#endif /* C_MCP */
