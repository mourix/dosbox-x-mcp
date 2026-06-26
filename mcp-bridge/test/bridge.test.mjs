// bridge.test.mjs — end-to-end MCP integration test. Spawns the bridge over
// stdio (which in turn spawns a MANAGED headless emulator parked at reset via
// --break-start), then drives it with the official MCP client SDK:
//   initialize -> tools/list -> tools/call (ping, read_registers) -> error case.
//
// Skips cleanly if the dosbox-x binary has not been built yet, so `npm test`
// works in a checkout without a build; scripts/mcp-check.sh runs it right after
// building the --enable-mcp binary.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { TOOL_NAMES } from "../tools.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, "..", "..");
const BRIDGE = path.join(HERE, "..", "bridge.mjs");
const BINARY = path.join(REPO_ROOT, "src", "dosbox-x");

// Booting headless dosbox-x then completing the MCP handshake can take a while.
const TIMEOUT = 180000;

test("bridge end-to-end over MCP stdio (managed emulator)", { timeout: TIMEOUT }, async (t) => {
  if (!existsSync(BINARY)) {
    t.skip(`dosbox-x binary not built at ${BINARY}; build it first (scripts/mcp-check.sh)`);
    return;
  }

  const transport = new StdioClientTransport({
    command: process.execPath, // node
    args: [BRIDGE],
    env: { ...process.env, DOSBOX_MCP_MODE: "headless", DOSBOX_MCP_BREAK_START: "1" },
    stderr: "inherit",
  });

  const client = new Client({ name: "bridge-test", version: "0.0.0" }, { capabilities: {} });

  try {
    await client.connect(transport, { timeout: TIMEOUT });

    // tools/list — exactly the 26 catalog tools, each with a schema.
    const listed = await client.listTools(undefined, { timeout: TIMEOUT });
    const names = listed.tools.map((x) => x.name).sort();
    assert.deepEqual(names, [...TOOL_NAMES].sort(), "tools/list must match the catalog");
    for (const tool of listed.tools) {
      assert.ok(tool.description, `${tool.name} has a description`);
      assert.equal(typeof tool.inputSchema, "object", `${tool.name} has an inputSchema`);
    }

    // ping (any-state) — parked under --break-start.
    const ping = await client.callTool({ name: "ping", arguments: {} }, undefined, { timeout: TIMEOUT });
    assert.equal(ping.isError, undefined, "ping is not an error");
    assert.equal(ping.structuredContent.pong, true, "ping returns pong:true");
    assert.equal(ping.structuredContent.state, "parked", "guest is parked under --break-start");

    // read_registers (parked-state) — works while parked.
    const regs = await client.callTool({ name: "read_registers", arguments: {} }, undefined, { timeout: TIMEOUT });
    assert.ok(!regs.isError, "read_registers is not an error while parked");
    for (const field of ["eax", "cs", "eip", "eflags"]) {
      assert.ok(field in regs.structuredContent, `read_registers returns ${field}`);
    }
    // Pretty-printed JSON text block is always present too.
    assert.equal(regs.content[0].type, "text");

    // Error path: a run-state tool while parked -> -32001 mismatch -> isError,
    // carrying the current state so the model can react.
    const wrong = await client.callTool({ name: "read_screen", arguments: {} }, undefined, { timeout: TIMEOUT });
    assert.equal(wrong.isError, true, "run-state tool while parked is an error");
    assert.match(wrong.content[0].text, /parked/, "error text reports the current state");
  } finally {
    await client.close();
  }
});
