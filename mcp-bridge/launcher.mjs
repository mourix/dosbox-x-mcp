// launcher.mjs — managed lifecycle: spawn a headless DOSBox-X via
// scripts/mcp-launch.sh, wait for its MCP port to accept, and own teardown.
//
// Used when no DOSBOX_MCP_PORT is configured (the "managed" half of the
// attach-or-spawn lifecycle). Launch knobs come from DOSBOX_MCP_* env vars set
// in .mcp.json. IMPORTANT: the child's stdout/stderr are routed to the bridge's
// STDERR (process.stderr) — never stdout, which is the MCP stdio channel.

import { spawn } from "node:child_process";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { waitForPort } from "./tcp-client.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = path.resolve(HERE, "..");
const LAUNCH_SH = path.join(REPO_ROOT, "scripts", "mcp-launch.sh");

// Ask the OS for an unused TCP port (bind :0, read it back, release it). A tiny
// race window remains before mcp-launch.sh binds it, acceptable for local use.
function pickFreePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.once("error", reject);
    srv.listen(0, "127.0.0.1", () => {
      const { port } = srv.address();
      srv.close(() => resolve(port));
    });
  });
}

// Build mcp-launch.sh args from the DOSBOX_MCP_* environment.
function launchArgsFromEnv(env, port) {
  const args = ["--port", String(port), "--mode", env.DOSBOX_MCP_MODE || "headless"];
  if (env.DOSBOX_MCP_MOUNT) args.push("--mount", env.DOSBOX_MCP_MOUNT);
  if (env.DOSBOX_MCP_CONFIG) args.push("--config", env.DOSBOX_MCP_CONFIG);
  if (truthy(env.DOSBOX_MCP_BREAK_START)) args.push("--break-start");
  // DOSBOX_MCP_RUN may carry multiple commands separated by newlines.
  if (env.DOSBOX_MCP_RUN) {
    for (const cmd of env.DOSBOX_MCP_RUN.split("\n")) {
      if (cmd.trim()) args.push("--run", cmd);
    }
  }
  return args;
}

function truthy(v) {
  return v != null && /^(1|true|yes|on)$/i.test(String(v).trim());
}

export class ManagedEmulator {
  constructor({ env = process.env, log = () => {} } = {}) {
    this.env = env;
    this.log = log;
    this.proc = null;
    this.port = null;
  }

  // Spawn the emulator and resolve once its MCP port accepts connections.
  async start({ startupTimeoutMs = 60000 } = {}) {
    this.port = await pickFreePort();
    const args = launchArgsFromEnv(this.env, this.port);
    this.log(`launching: ${LAUNCH_SH} ${args.join(" ")}`);
    this.proc = spawn("bash", [LAUNCH_SH, ...args], {
      cwd: REPO_ROOT,
      env: this.env,
      // stdin ignored; child stdout+stderr -> bridge STDERR (keep MCP stdout clean).
      stdio: ["ignore", "inherit", "inherit"],
    });
    this.proc.on("exit", (code, signal) => {
      this.log(`emulator exited (code=${code} signal=${signal})`);
      this.proc = null;
    });

    const ok = await waitForPort({ port: this.port, timeoutMs: startupTimeoutMs });
    if (!ok) {
      this.stop();
      throw new Error(`emulator did not open MCP port ${this.port} within ${startupTimeoutMs} ms`);
    }
    this.log(`emulator ready on 127.0.0.1:${this.port}`);
    return this.port;
  }

  // SIGTERM, then SIGKILL after a grace period.
  stop({ graceMs = 5000 } = {}) {
    const p = this.proc;
    if (!p) return;
    this.proc = null;
    try { p.kill("SIGTERM"); } catch { /* already gone */ }
    const t = setTimeout(() => { try { p.kill("SIGKILL"); } catch { /* gone */ } }, graceMs);
    p.once("exit", () => clearTimeout(t));
  }
}
