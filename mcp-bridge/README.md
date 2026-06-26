# dosbox-x-mcp-bridge

A small **stdio↔TCP bridge** that exposes the DOSBox-X debugger toolset as a
proper **MCP server** for Claude Code (or any MCP client).

## Why this exists

The `--enable-mcp` build of DOSBox-X serves a *flat, newline-delimited JSON-RPC*
endpoint over a raw **TCP socket** (methods like `ping`, `read_registers`,
`step`). Claude Code speaks the **MCP stdio transport** — also newline-delimited
JSON-RPC, but with an `initialize` handshake and `tools/list` / `tools/call`
envelopes. This bridge is the translation layer: it serves the 26-tool catalog
and forwards each `tools/call` to the emulator as a flat method call.

## Install

From the repo root:

```bash
scripts/mcp-install.sh            # project .mcp.json (committed)
scripts/mcp-install.sh --user     # …and personal config via `claude mcp add`
```

The installer checks the `src/dosbox-x` binary and Node ≥ 18, runs `npm install`
here, writes `.mcp.json`, and runs the smoke test. Then open the repo in Claude
Code (`/mcp` to connect).

## Lifecycle: attach or spawn

The bridge picks how to reach the emulator at runtime:

- **Managed (default):** no `DOSBOX_MCP_PORT` set → it spawns a headless emulator
  via `scripts/mcp-launch.sh` and owns its lifecycle (killed when the MCP session
  ends).
- **Attach:** `DOSBOX_MCP_PORT` set **and reachable** → it connects to that
  already-running emulator. Launch one yourself with
  `scripts/mcp-launch.sh --port N --mode visible` to *watch* the guest while
  Claude drives it.

## Configuration (env vars, set in `.mcp.json`)

| Var | Meaning |
|-----|---------|
| `DOSBOX_MCP_PORT` | Attach to this 127.0.0.1 port if reachable; else spawn managed. |
| `DOSBOX_MCP_HOST` | Emulator host (default `127.0.0.1`). |
| `DOSBOX_MCP_MODE` | Managed launch mode: `headless` (default) or `visible`. |
| `DOSBOX_MCP_MOUNT` | Directory mounted as `C:` for managed launches. |
| `DOSBOX_MCP_RUN` | Boot command(s) for managed launches (newline-separated). |
| `DOSBOX_MCP_BREAK_START` | `1` to park the CPU at reset (parked-class tools work immediately). |
| `DOSBOX_MCP_CONFIG` | A `dosbox-x.conf` to load for managed launches. |

## Files

- `bridge.mjs` — MCP stdio server + attach/spawn lifecycle wiring.
- `tools.mjs` — the 26-tool catalog (names, JSON-Schema params, state class). The
  client-facing single source of truth; a sync-guard test keeps it aligned with
  the C++ method table.
- `tcp-client.mjs` — newline-delimited JSON-RPC client to the emulator.
- `launcher.mjs` — managed-mode spawn of `scripts/mcp-launch.sh`.
- `test/` — `node --test` integration + sync-guard tests (`npm test`).

Tool *workflows* (attach → breakpoint → step → inspect) live in
`../docs/MCP_MANUAL.md`; the tool schemas here are the authoritative parameter
reference.
