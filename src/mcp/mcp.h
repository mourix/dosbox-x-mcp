/*
 *  DOSBox-X MCP (Model Context Protocol) server.
 *
 *  Additive to the mourix/dosbox-x-mcp fork. The whole module is compiled only
 *  when configured with --enable-mcp (which requires --enable-debug); see
 *  docs/MCP_BUILD_PLAN.md. Slice 0 ships only this scaffolding stub to prove the
 *  build pipeline and one-command verification end to end.
 */

#ifndef DOSBOX_MCP_H
#define DOSBOX_MCP_H

#include "config.h"

#if C_MCP

/* Compile-time version of the MCP module. Bumped as slices land. */
#define MCP_VERSION "0.0.0-slice0"

/* Returns the MCP module version string (see MCP_VERSION). */
const char *MCP_Version(void);

#endif /* C_MCP */

#endif /* DOSBOX_MCP_H */
