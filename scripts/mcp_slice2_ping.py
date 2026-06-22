#!/usr/bin/env python3
"""Integration Test #2 (Slice 2): TCP JSON-RPC round trip.

Stdlib-only. Boots dosbox-x headless with the MCP server enabled (MCP_PORT),
connects over newline-delimited JSON-RPC and asserts:

  * ``ping``        -> pong + current execution state ("running");
  * ``server_info`` -> reports the localhost-only bind (127.0.0.1);
  * a parked-class request while the guest free-runs is fast-rejected with the
    current state (mode-mismatch), never blocking;
  * a second concurrent client is refused (single-client transport).

Reused by ``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 2).
"""

import json
import os
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for_port(port, proc, timeout=45.0):
    """Poll until the MCP server accepts a connection, or time out."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            c = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            return c
        except OSError:
            if proc is not None and proc.poll() is not None:
                return None
            time.sleep(0.2)
    return None


class RpcClient:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(8.0)
        self.buf = b""

    def call(self, method, req_id, params=None):
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        return self.read_line()

    def read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice2_cap_")

    with DosboxHarness(capture_dir=capture_dir,
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.start()
        sock = wait_for_port(port, h.proc)
        if sock is None:
            sys.stderr.write(h.output())
            fail("MCP server never accepted a connection on 127.0.0.1:%d" % port)

        client = RpcClient(sock)

        # 1. ping
        resp = client.call("ping", 1)
        if not resp or resp.get("id") != 1 or "result" not in resp:
            fail("ping: unexpected reply: %r" % resp)
        if resp["result"].get("pong") is not True:
            fail("ping: missing pong: %r" % resp)
        if resp["result"].get("state") != "running":
            fail("ping: expected state 'running', got %r" % resp["result"].get("state"))
        print("OK: ping -> pong, state=running")

        # 2. server_info: localhost-only bind invariant
        resp = client.call("server_info", 2)
        if not resp or "result" not in resp:
            fail("server_info: unexpected reply: %r" % resp)
        info = resp["result"]
        if info.get("bind") != "127.0.0.1":
            fail("server_info: bind is not localhost: %r" % info.get("bind"))
        if info.get("single_client") is not True:
            fail("server_info: single_client not advertised: %r" % info)
        print("OK: server_info -> bind=127.0.0.1, version=%s" % info.get("version"))

        # 3. mode-mismatch fast-reject (parked-class request while free-running)
        resp = client.call("read_registers", 3)
        if not resp or "error" not in resp:
            fail("mode-mismatch: expected an error reply, got: %r" % resp)
        err = resp["error"]
        if err.get("code") != -32001:
            fail("mode-mismatch: expected code -32001, got %r" % err.get("code"))
        if not isinstance(err.get("data"), dict) or err["data"].get("state") != "running":
            fail("mode-mismatch: error did not carry current state: %r" % err)
        print("OK: parked request while running -> fast-reject with state=running")

        # 4. single-client: a second connection is refused
        sock2 = socket.create_connection(("127.0.0.1", port), timeout=4.0)
        client2 = RpcClient(sock2)
        refused = client2.read_line()  # server sends a busy error then closes
        if not refused or "error" not in refused or refused["error"].get("code") != -32002:
            fail("single-client: second connection was not refused: %r" % refused)
        client2.close()
        print("OK: second client refused (single-client transport)")

        # The first client must still work after the refusal.
        resp = client.call("ping", 4)
        if not resp or resp.get("id") != 4:
            fail("first client broke after second-client refusal: %r" % resp)
        print("OK: first client still healthy after refusal")

        client.close()

    print("PASS: Slice 2 JSON-RPC round trip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
