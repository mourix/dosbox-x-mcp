#!/usr/bin/env bash
#
# scripts/mcp-install.sh — install the DOSBox-X MCP server for Claude Code.
#
# Wires the stdio<->TCP bridge (mcp-bridge/) up as an MCP server so the debugger
# toolset shows up in a Claude Code session. Repeatable (guardrail #4): it
#   1. checks the dosbox-x binary is built and node >= 18 is present,
#   2. installs the bridge's npm deps (the official MCP SDK),
#   3. writes/merges a project .mcp.json at the repo root (committed, shareable),
#   4. with --user, also registers the server in your personal config (claude mcp add),
#   5. runs the bridge smoke test end-to-end (unless --no-test).
#
# The bridge picks its own emulator lifecycle at runtime: it ATTACHES to a
# running emulator if DOSBOX_MCP_PORT is set and reachable, else SPAWNS a managed
# headless one. Launch defaults are baked into .mcp.json's env from the flags
# below.
#
# Usage:
#   scripts/mcp-install.sh [options]
#
# Options:
#   --user            also register the server in personal config via `claude mcp add`.
#   --mode MODE       managed launch mode: headless (default) | visible.
#   --mount DIR       default drive C: mount for managed launches.
#   --run "CMD"       default boot command for managed launches (repeatable).
#   --break-start     park the CPU at reset by default (parked-class tools work immediately).
#   --port N          attach to an emulator already listening on 127.0.0.1:N instead of spawning.
#   --no-test         skip the end-to-end smoke test.
#   -h, --help        show this help.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_DIR="$ROOT/mcp-bridge"
BRIDGE_JS="$BRIDGE_DIR/bridge.mjs"
BIN="$ROOT/src/dosbox-x"
MCP_JSON="$ROOT/.mcp.json"

fail() { printf 'mcp-install: %s\n' "$*" >&2; exit 1; }
log()  { printf 'mcp-install: %s\n' "$*" >&2; }

USER_SCOPE=0
RUN_TEST=1
MODE="headless"
MOUNT=""
BREAK_START=0
PORT=""
RUN_CMDS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --user)        USER_SCOPE=1; shift ;;
        --no-test)     RUN_TEST=0; shift ;;
        --mode)        MODE="${2:-}"; shift 2 ;;
        --mount)       MOUNT="${2:-}"; shift 2 ;;
        --run)         RUN_CMDS+=("${2:-}"); shift 2 ;;
        --break-start) BREAK_START=1; shift ;;
        --port)        PORT="${2:-}"; shift 2 ;;
        -h|--help)     sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             fail "unknown option: $1" ;;
    esac
done

# 1. Prerequisites.
[ -x "$BIN" ] || fail "dosbox-x binary not found at $BIN — build it first (scripts/mcp-check.sh, or ./build-debug)."
command -v node >/dev/null 2>&1 || fail "node not found — install Node.js >= 18."
NODE_MAJOR="$(node -p 'process.versions.node.split(".")[0]')"
[ "$NODE_MAJOR" -ge 18 ] || fail "Node.js >= 18 required (found $(node --version))."
log "prerequisites OK (binary present, node $(node --version))"

# 2. Install the bridge's npm deps.
log "installing bridge dependencies (npm) ..."
( cd "$BRIDGE_DIR" && npm install --no-audit --no-fund >/dev/null ) || fail "npm install failed in $BRIDGE_DIR"

# Assemble the default env baked into the registration (newline-joined --run).
RUN_JOINED=""
for cmd in "${RUN_CMDS[@]:-}"; do
    [ -n "$cmd" ] || continue
    RUN_JOINED="${RUN_JOINED:+$RUN_JOINED$'\n'}$cmd"
done

# 3. Write/merge the project .mcp.json (use node for a safe JSON merge).
# Use a path RELATIVE to the repo root so the committed file is portable across
# clones (Claude Code launches project servers from the project directory; the
# bridge resolves its own repo root internally, independent of cwd).
REL_BRIDGE="mcp-bridge/bridge.mjs"
log "writing project .mcp.json -> $MCP_JSON"
MCP_JSON="$MCP_JSON" BRIDGE_JS="$REL_BRIDGE" MODE="$MODE" MOUNT="$MOUNT" \
RUN_JOINED="$RUN_JOINED" BREAK_START="$BREAK_START" PORT="$PORT" node <<'NODE'
const fs = require("fs");
const file = process.env.MCP_JSON;
let doc = {};
if (fs.existsSync(file)) {
  try { doc = JSON.parse(fs.readFileSync(file, "utf8")); }
  catch { console.error("mcp-install: existing .mcp.json is not valid JSON; aborting"); process.exit(1); }
}
doc.mcpServers = doc.mcpServers || {};
const env = { DOSBOX_MCP_MODE: process.env.MODE || "headless" };
if (process.env.MOUNT) env.DOSBOX_MCP_MOUNT = process.env.MOUNT;
if (process.env.RUN_JOINED) env.DOSBOX_MCP_RUN = process.env.RUN_JOINED;
if (process.env.BREAK_START === "1") env.DOSBOX_MCP_BREAK_START = "1";
if (process.env.PORT) env.DOSBOX_MCP_PORT = process.env.PORT;
doc.mcpServers["dosbox-x"] = {
  command: "node",
  args: [process.env.BRIDGE_JS],
  env,
};
fs.writeFileSync(file, JSON.stringify(doc, null, 2) + "\n");
NODE

# 4. Optional user-scope registration.
if [ "$USER_SCOPE" = "1" ]; then
    if command -v claude >/dev/null 2>&1; then
        log "registering user-scope server via 'claude mcp add'"
        ENV_ARGS=(--env "DOSBOX_MCP_MODE=$MODE")
        [ -n "$MOUNT" ] && ENV_ARGS+=(--env "DOSBOX_MCP_MOUNT=$MOUNT")
        [ "$BREAK_START" = "1" ] && ENV_ARGS+=(--env "DOSBOX_MCP_BREAK_START=1")
        [ -n "$PORT" ] && ENV_ARGS+=(--env "DOSBOX_MCP_PORT=$PORT")
        [ -n "$RUN_JOINED" ] && ENV_ARGS+=(--env "DOSBOX_MCP_RUN=$RUN_JOINED")
        claude mcp remove --scope user dosbox-x >/dev/null 2>&1 || true
        claude mcp add --scope user "${ENV_ARGS[@]}" dosbox-x -- node "$BRIDGE_JS" \
            || fail "'claude mcp add' failed"
    else
        log "WARNING: --user given but 'claude' CLI not found on PATH; skipped user registration."
    fi
fi

# 5. Smoke test.
if [ "$RUN_TEST" = "1" ]; then
    log "running bridge smoke test (boots a headless emulator) ..."
    ( cd "$BRIDGE_DIR" && node --test test/*.test.mjs ) || fail "bridge smoke test failed"
fi

log "done. The 'dosbox-x' MCP server is registered in $MCP_JSON."
log "Open this repo in Claude Code and run /mcp (or restart the session) to connect."
