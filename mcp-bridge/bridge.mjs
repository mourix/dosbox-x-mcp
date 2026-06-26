#!/usr/bin/env node
// bridge.mjs — MCP stdio server that exposes the DOSBox-X debugger toolset.
//
// Claude Code speaks MCP over stdio (newline-delimited JSON-RPC with an
// initialize handshake + tools/list / tools/call). The emulator speaks a flat,
// newline-delimited JSON-RPC over a raw TCP socket. This bridge translates
// between the two: it serves the tool catalog (tools.mjs) and forwards each
// tools/call to the emulator as a flat method call.
//
// Lifecycle (attach-or-spawn):
//   * DOSBOX_MCP_PORT set AND reachable -> ATTACH to that emulator (e.g. a
//     `mcp-launch.sh --mode visible` session you launched to watch the guest).
//   * otherwise -> MANAGED: spawn a headless emulator and own its lifecycle.
//
// Everything except the MCP protocol bytes goes to STDERR; stdout is reserved
// for the MCP channel.

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  ListToolsRequestSchema,
  CallToolRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

import { TOOLS, TOOL_NAMES } from "./tools.mjs";
import { TcpJsonRpcClient, RpcError, waitForPort } from "./tcp-client.mjs";
import { ManagedEmulator } from "./launcher.mjs";

const VERSION = "0.1.0";
const HOST = process.env.DOSBOX_MCP_HOST || "127.0.0.1";

function log(...args) {
  process.stderr.write(`[dosbox-x-mcp] ${args.join(" ")}\n`);
}

const TOOL_SET = new Set(TOOL_NAMES);

// Resolve a connected TCP client and a teardown hook, honoring attach-or-spawn.
async function connectEmulator() {
  const configuredPort = process.env.DOSBOX_MCP_PORT
    ? Number(process.env.DOSBOX_MCP_PORT) : 0;

  let managed = null;
  let port = configuredPort;

  if (configuredPort) {
    log(`DOSBOX_MCP_PORT=${configuredPort} set; probing for a running emulator...`);
    const reachable = await waitForPort({ host: HOST, port: configuredPort, timeoutMs: 2000 });
    if (reachable) {
      log(`attaching to emulator at ${HOST}:${configuredPort}`);
    } else {
      log(`no emulator at ${HOST}:${configuredPort}; falling back to a managed instance`);
      port = 0;
    }
  }

  if (!port) {
    managed = new ManagedEmulator({ env: process.env, log });
    port = await managed.start();
  }

  const client = new TcpJsonRpcClient({ host: HOST, port });
  await client.connect();

  return {
    client,
    port,
    async dispose() {
      try { client.close(); } catch { /* ignore */ }
      if (managed) managed.stop();
    },
  };
}

// Map an emulator call result/error onto an MCP CallTool result.
function okResult(result) {
  const out = {
    content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
  };
  // structuredContent must be a JSON object (not an array / scalar).
  if (result && typeof result === "object" && !Array.isArray(result)) {
    out.structuredContent = result;
  }
  return out;
}

function errResult(text) {
  return { content: [{ type: "text", text }], isError: true };
}

async function main() {
  const conn = await connectEmulator();

  const server = new Server(
    { name: "dosbox-x", version: VERSION },
    { capabilities: { tools: {} } },
  );

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: TOOLS.map(({ name, description, inputSchema }) => ({
      name, description, inputSchema,
    })),
  }));

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const { name } = request.params;
    const args = request.params.arguments || {};
    if (!TOOL_SET.has(name)) {
      return errResult(`unknown tool: ${name}`);
    }
    try {
      const result = await conn.client.call(name, args);
      return okResult(result);
    } catch (err) {
      if (err instanceof RpcError) {
        const state = err.data && err.data.state ? ` current state: ${err.data.state}.` : "";
        return errResult(`emulator error ${err.code}: ${err.message}.${state}`);
      }
      return errResult(`bridge error calling '${name}': ${err.message}`);
    }
  });

  // Tear everything down on transport close or a termination signal.
  let disposed = false;
  const shutdown = async (why) => {
    if (disposed) return;
    disposed = true;
    log(`shutting down (${why})`);
    await conn.dispose();
    process.exit(0);
  };
  server.onclose = () => shutdown("transport closed");
  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));

  const transport = new StdioServerTransport();
  await server.connect(transport);
  log(`ready: ${TOOL_NAMES.length} tools, emulator on ${HOST}:${conn.port}`);
}

main().catch((err) => {
  log(`fatal: ${err && err.stack ? err.stack : err}`);
  process.exit(1);
});
