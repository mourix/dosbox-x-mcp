// tcp-client.mjs — newline-delimited JSON-RPC 2.0 client for the DOSBox-X MCP
// emulator endpoint (the raw TCP server in src/mcp/). One JSON object per line:
//   request  {"jsonrpc":"2.0","id":N,"method":"<tool>","params":{...}}
//   response {"jsonrpc":"2.0","id":N,"result":{...}}  | {...,"error":{code,message,data}}
//
// The emulator binds 127.0.0.1 only and accepts a SINGLE client at a time, so
// the bridge is that one client and issues calls SEQUENTIALLY (the emulator
// processes one request at a time anyway). Each call has a client-side timeout
// slightly above the server's 5 s queued-request timeout so a hung emulator
// surfaces as an error rather than a stuck MCP session.

import net from "node:net";

// A JSON-RPC error surfaced by the emulator (carries the protocol code + data,
// e.g. -32001 mode-mismatch with data.state = "running"|"parked").
export class RpcError extends Error {
  constructor(code, message, data) {
    super(message || `JSON-RPC error ${code}`);
    this.name = "RpcError";
    this.code = code;
    this.data = data;
  }
}

export class TcpJsonRpcClient {
  constructor({ host = "127.0.0.1", port, callTimeoutMs = 8000 } = {}) {
    if (!port) throw new Error("TcpJsonRpcClient requires a port");
    this.host = host;
    this.port = port;
    this.callTimeoutMs = callTimeoutMs;
    this.socket = null;
    this.buf = "";
    this.nextId = 1;
    this.pending = new Map(); // id -> { resolve, reject, timer }
    this.queue = Promise.resolve(); // serializes calls
    this.closed = false;
  }

  connect({ connectTimeoutMs = 8000 } = {}) {
    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host: this.host, port: this.port });
      const onError = (err) => { sock.destroy(); reject(err); };
      const timer = setTimeout(
        () => onError(new Error(`connect to ${this.host}:${this.port} timed out`)),
        connectTimeoutMs);
      sock.once("error", onError);
      sock.once("connect", () => {
        clearTimeout(timer);
        sock.removeListener("error", onError);
        sock.setEncoding("utf8");
        sock.on("data", (chunk) => this._onData(chunk));
        sock.on("error", (err) => this._onClose(err));
        sock.on("close", () => this._onClose(new Error("connection closed")));
        this.socket = sock;
        resolve(this);
      });
    });
  }

  _onData(chunk) {
    this.buf += chunk;
    let nl;
    while ((nl = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, nl).trim();
      this.buf = this.buf.slice(nl + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch { continue; }
      const entry = this.pending.get(msg.id);
      if (!entry) continue;
      this.pending.delete(msg.id);
      clearTimeout(entry.timer);
      if (msg.error) {
        entry.reject(new RpcError(msg.error.code, msg.error.message, msg.error.data));
      } else {
        entry.resolve(msg.result);
      }
    }
  }

  _onClose(err) {
    if (this.closed) return;
    this.closed = true;
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(err instanceof Error ? err : new Error(String(err)));
    }
    this.pending.clear();
  }

  // Issue one JSON-RPC call. Serialized behind any in-flight call so the single
  // client never has two requests outstanding.
  call(method, params) {
    this.queue = this.queue.then(
      () => this._send(method, params),
      () => this._send(method, params)); // run even if a prior call rejected
    return this.queue;
  }

  _send(method, params) {
    return new Promise((resolve, reject) => {
      if (this.closed || !this.socket) {
        reject(new Error("not connected to the emulator"));
        return;
      }
      const id = this.nextId++;
      const req = { jsonrpc: "2.0", id, method };
      if (params !== undefined) req.params = params;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`call '${method}' timed out after ${this.callTimeoutMs} ms`));
      }, this.callTimeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.write(JSON.stringify(req) + "\n", (err) => {
        if (err) {
          this.pending.delete(id);
          clearTimeout(timer);
          reject(err);
        }
      });
    });
  }

  close() {
    this.closed = true;
    if (this.socket) { this.socket.destroy(); this.socket = null; }
  }
}

// Poll until the emulator accepts a TCP connection on host:port (used by the
// managed launcher to wait for boot), or time out.
export async function waitForPort({ host = "127.0.0.1", port, timeoutMs = 60000,
  intervalMs = 200 } = {}) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const ok = await new Promise((resolve) => {
      const sock = net.createConnection({ host, port });
      const done = (v) => { sock.destroy(); resolve(v); };
      sock.once("connect", () => done(true));
      sock.once("error", () => done(false));
      setTimeout(() => done(false), intervalMs);
    });
    if (ok) return true;
    await new Promise((r) => setTimeout(r, intervalMs));
  }
  return false;
}
