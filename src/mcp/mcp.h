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
#define MCP_VERSION "0.1.0-slice1"

/* Returns the MCP module version string (see MCP_VERSION). */
const char *MCP_Version(void);

/* The emulator-thread "run-class" service point. Called once per frame from
 * GFX_Events() (the single core call site, see the core-edit manifest in
 * docs/MCP_BUILD_PLAN.md). Slice 2 grows this into the run-class queue drain.
 *
 * Slice 1 uses it only for an env-gated self-test: when
 * MCP_SELFTEST_SCREENSHOT is set, it requests one screenshot after the guest
 * has had time to boot, so the integration harness can verify that the capture
 * path produces a PNG under the dummy video driver. It is a no-op otherwise. */
void MCP_GFXFrameService(void);

#endif /* C_MCP */

#endif /* DOSBOX_MCP_H */
