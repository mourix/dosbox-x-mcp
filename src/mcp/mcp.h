/*
 *  DOSBox-X MCP (Model Context Protocol) server.
 *
 *  Additive to the mourix/dosbox-x-mcp fork. The whole module is compiled only
 *  when configured with --enable-mcp (which requires --enable-debug); see
 *  docs/MCP_BUILD_PLAN.md.
 */

#ifndef DOSBOX_MCP_H
#define DOSBOX_MCP_H

#include "config.h"

#if C_MCP

/* Compile-time version of the MCP module. Bumped as slices land. */
#define MCP_VERSION "0.3.0-slice3"

/* Returns the MCP module version string (see MCP_VERSION). */
const char *MCP_Version(void);

/* The emulator-thread "run-class" service point. Called once per frame from
 * GFX_Events() (the single core call site, see the core-edit manifest in
 * docs/MCP_BUILD_PLAN.md). It lazily starts the TCP JSON-RPC server (when
 * MCP_PORT is set) and drains the request queue against the current execution
 * state, so all request handling stays single-threaded on the emulator thread.
 * Because GFX_Events() also runs inside DEBUG_Loop, this one drain services both
 * run-class and parked-class requests. It also runs the env-gated Slice 1
 * screenshot self-test (MCP_SELFTEST_SCREENSHOT); a no-op otherwise. */
void MCP_GFXFrameService(void);

#endif /* C_MCP */

#endif /* DOSBOX_MCP_H */
