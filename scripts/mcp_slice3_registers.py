#!/usr/bin/env python3
"""Integration Test #3 (Slice 3): read_registers at a stop.

Stdlib-only. Boots dosbox-x headless with ``-break-start`` so the CPU is parked
in the debugger at machine startup (sdlmain.cpp:10155), then over JSON-RPC:

  * ``ping``           -> confirms the dispatcher reports state "parked";
  * ``read_registers`` -> returns the full register set, asserts the shape
    (all GP/segment registers + flags + mode), that values are fixed-width hex,
    that the reply is bounded, and that the guest is in real mode at reset.

A parked-class request is only serviceable because ``-break-start`` parks the
CPU; this is the first tool that reads live emulator state. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 3).
"""

import os
import re
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port

HEX32 = re.compile(r"^0x[0-9a-f]{8}$")
HEX16 = re.compile(r"^0x[0-9a-f]{4}$")

GP32 = ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip"]
SEG16 = ["cs", "ds", "es", "fs", "gs", "ss"]
FLAGS = ["CF", "PF", "AF", "ZF", "SF", "TF", "IF", "DF", "OF", "IOPL"]
MODES = {"real", "pr16", "pr32", "vm86"}


def wait_for_parked(client, timeout=30.0):
    """Poll ping until the dispatcher reports the CPU is parked."""
    deadline = time.time() + timeout
    rid = 1000
    while time.time() < deadline:
        rid += 1
        resp = client.call("ping", rid)
        if resp and resp.get("result", {}).get("state") == "parked":
            return True
        time.sleep(0.2)
    return False


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice3_cap_")

    with DosboxHarness(capture_dir=capture_dir,
                       extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.start()
        sock = wait_for_port(port, h.proc)
        if sock is None:
            sys.stderr.write(h.output())
            fail("MCP server never accepted a connection on 127.0.0.1:%d" % port)

        client = RpcClient(sock)

        # The CPU must be parked (break-start) before a parked-class tool works.
        if not wait_for_parked(client):
            sys.stderr.write(h.output())
            fail("guest never reached the parked (debugger) state under -break-start")
        print("OK: guest parked at startup (state=parked)")

        resp = client.call("read_registers", 1)
        if not resp or "result" not in resp:
            fail("read_registers: unexpected reply: %r" % resp)
        reg = resp["result"]

        for name in GP32:
            v = reg.get(name)
            if not isinstance(v, str) or not HEX32.match(v):
                fail("read_registers: %s is not 32-bit hex: %r" % (name, v))
        for name in SEG16:
            v = reg.get(name)
            if not isinstance(v, str) or not HEX16.match(v):
                fail("read_registers: %s is not 16-bit hex: %r" % (name, v))
        if not isinstance(reg.get("eflags"), str) or not HEX32.match(reg["eflags"]):
            fail("read_registers: eflags is not 32-bit hex: %r" % reg.get("eflags"))

        flags = reg.get("flags")
        if not isinstance(flags, dict) or any(f not in flags for f in FLAGS):
            fail("read_registers: flags object incomplete: %r" % flags)
        for f in FLAGS:
            if not isinstance(flags[f], int):
                fail("read_registers: flag %s is not an int: %r" % (f, flags[f]))

        if reg.get("mode") not in MODES:
            fail("read_registers: unexpected mode: %r" % reg.get("mode"))
        # At -break-start the machine is at reset: real mode, ring 0.
        if reg.get("mode") != "real":
            fail("read_registers: expected real mode at reset, got %r" % reg.get("mode"))
        if reg.get("cpl") != 0:
            fail("read_registers: expected cpl 0 at reset, got %r" % reg.get("cpl"))

        # Response bound (guardrail #3): well under the 64 KiB ceiling.
        size = len(repr(resp))
        if size > 64 * 1024:
            fail("read_registers: reply exceeds payload ceiling (%d bytes)" % size)

        print("OK: read_registers -> cs=%s eip=%s mode=%s cpl=%d"
              % (reg["cs"], reg["eip"], reg["mode"], reg["cpl"]))

        client.close()

    print("PASS: Slice 3 read_registers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
