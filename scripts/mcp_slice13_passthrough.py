#!/usr/bin/env python3
"""Integration Test #13 (Slice 13): debugger_command passthrough.

Stdlib-only. Boots dosbox-x headless with ``-break-start`` so the CPU is parked
in the debugger (the parked-class context ``debugger_command`` runs in), then
over JSON-RPC drives the escape hatch that runs an arbitrary debugger command
through ``ParseCommand`` and returns its captured ``DEBUG_ShowMsg`` output:

  * ``EV 10+20``  -> a recognised command with deterministic output. Asserts
    ``recognized: true``, the captured ``output`` echoes the expression and its
    evaluated value (0x10 + 0x20 = 0x30), and ``truncated: false``.
  * an unknown command -> ``recognized: false`` (ParseCommand did not match it).
  * empty params       -> ``-32602`` invalid-params (no ``command`` field).
  * the reply is bounded under the 64 KiB ceiling.

Reused by ``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 13).
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice3_registers import wait_for_parked


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice13_cap_")

    with DosboxHarness(capture_dir=capture_dir,
                       extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.start()
        sock = wait_for_port(port, h.proc)
        if sock is None:
            sys.stderr.write(h.output())
            fail("MCP server never accepted a connection on 127.0.0.1:%d" % port)

        client = RpcClient(sock)

        # debugger_command is parked-class: the CPU must be parked first.
        if not wait_for_parked(client):
            sys.stderr.write(h.output())
            fail("guest never reached the parked (debugger) state under -break-start")
        print("OK: guest parked at startup (state=parked)")

        # 1) A recognised command with deterministic output. EV echoes the
        #    expression and its evaluated hex value: 0x10 + 0x20 = 0x30.
        resp = client.call("debugger_command", 1, {"command": "EV 10+20"})
        if not resp or "result" not in resp:
            fail("debugger_command EV: unexpected reply: %r" % resp)
        res = resp["result"]
        if res.get("recognized") is not True:
            fail("debugger_command EV: expected recognized=true, got %r" % res.get("recognized"))
        if res.get("truncated") is not False:
            fail("debugger_command EV: expected truncated=false, got %r" % res.get("truncated"))
        out = res.get("output", "")
        if "EV of" not in out or "30" not in out:
            fail("debugger_command EV: output missing expected text: %r" % out)
        if res.get("command") != "EV 10+20":
            fail("debugger_command EV: command not echoed: %r" % res.get("command"))
        # Response bound (guardrail #3).
        if len(repr(resp)) > 64 * 1024:
            fail("debugger_command EV: reply exceeds payload ceiling")
        print("OK: EV 10+20 -> recognized, output=%r" % out)

        # 2) An unknown command: ParseCommand does not match it.
        resp = client.call("debugger_command", 2, {"command": "ZZNOTACOMMAND"})
        if not resp or "result" not in resp:
            fail("debugger_command unknown: unexpected reply: %r" % resp)
        if resp["result"].get("recognized") is not False:
            fail("debugger_command unknown: expected recognized=false, got %r"
                 % resp["result"].get("recognized"))
        print("OK: unknown command -> recognized=false")

        # 3) Missing command field -> invalid params.
        resp = client.call("debugger_command", 3, {})
        if not resp or "error" not in resp:
            fail("debugger_command empty: expected an error, got %r" % resp)
        if resp["error"].get("code") != -32602:
            fail("debugger_command empty: expected -32602, got %r" % resp["error"].get("code"))
        print("OK: missing command -> -32602 invalid params")

        client.close()

    print("PASS: Slice 13 debugger_command passthrough")
    return 0


if __name__ == "__main__":
    sys.exit(main())
