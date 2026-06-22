#!/usr/bin/env python3
"""Integration Test #5 (Slice 5): execution control — step / step_over /
continue / break.

Stdlib-only. Two phases so the assertions are race-free:

  Phase A — parked-class tools, against a guest parked with ``-break-start``:
    step (trace into)
      * each call advances CS:EIP by one instruction and stays parked
        (``resumed`` false, ``state`` "parked");
      * the reported cs:eip agrees with a follow-up ``read_registers``.
    step_over
      * returns a well-formed stop report and ends parked, CS:EIP advanced.
    continue (parked -> running)
      * ``resumed`` true, ``state`` "running"; ``ping`` then reports "running".
      * the guest really executes: once it is parked again (it re-parks itself
        on a BIOS interrupt, or we ``break`` it), CS:EIP differs from the point
        ``continue`` released — proving instructions ran while free-running.
    mode-mismatch fast-reject (never blocks)
      * ``break`` (run-class) while parked -> -32001 carrying state "parked";
      * ``step`` (parked-class) while running -> -32001 carrying state "running".

  Phase B — ``break``'s positive path, against a fresh guest with **no**
  ``-break-start`` (so it is genuinely free-running):
      * ``ping`` reports "running"; ``break`` is accepted (run-class while
        running) and the guest re-parks, after which ``read_registers`` works.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 5).
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice3_registers import wait_for_parked

CEILING = 64 * 1024
HEX16 = re.compile(r"^0x[0-9a-f]{4}$")
HEX32 = re.compile(r"^0x[0-9a-f]{8}$")


def assert_bounded(resp, what):
    if len(repr(resp)) > CEILING:
        fail("%s: reply exceeds payload ceiling" % what)


def expect_result(resp, what):
    if not resp or "result" not in resp:
        fail("%s: unexpected reply: %r" % (what, resp))
    assert_bounded(resp, what)
    return resp["result"]


def expect_error(resp, what, code):
    if not resp or "error" not in resp:
        fail("%s: expected an error, got: %r" % (what, resp))
    if resp["error"].get("code") != code:
        fail("%s: expected error code %d, got %r" % (what, code, resp["error"]))
    return resp["error"]


def regs(client, rid):
    r = expect_result(client.call("read_registers", rid), "read_registers")
    return int(r["cs"], 16), int(r["eip"], 16)


def ping_state(client, rid):
    r = expect_result(client.call("ping", rid), "ping")
    return r.get("state")


def wait_for_state(client, want, timeout=15.0):
    deadline = time.time() + timeout
    rid = 5000
    while time.time() < deadline:
        rid += 1
        if ping_state(client, rid) == want:
            return True
        time.sleep(0.1)
    return False


def ensure_parked(client, timeout=15.0):
    """Bring the guest back to parked, whether it re-parks itself (a BIOS
    interrupt re-enters the debugger) or needs an explicit ``break``. Robust to
    the race between the two. Returns True once parked."""
    deadline = time.time() + timeout
    rid = 7000
    while time.time() < deadline:
        rid += 1
        if ping_state(client, rid) == "parked":
            return True
        rid += 1
        client.call("break", rid)   # accepted if still running; else a no-op reject
        time.sleep(0.05)
    return ping_state(client, 7999) == "parked"


def check_exec(res, what, op, resumed, state):
    if res.get("op") != op:
        fail("%s: op %r != %r" % (what, res.get("op"), op))
    if res.get("resumed") is not resumed:
        fail("%s: resumed %r != %r" % (what, res.get("resumed"), resumed))
    if res.get("state") != state:
        fail("%s: state %r != %r" % (what, res.get("state"), state))
    if not HEX16.match(res.get("cs", "")):
        fail("%s: cs not 16-bit hex: %r" % (what, res.get("cs")))
    if not HEX32.match(res.get("eip", "")):
        fail("%s: eip not 32-bit hex: %r" % (what, res.get("eip")))
    if not isinstance(res.get("ran"), int):
        fail("%s: ran is not an int: %r" % (what, res.get("ran")))


def connect(h, port):
    h.start()
    sock = wait_for_port(port, h.proc)
    if sock is None:
        sys.stderr.write(h.output())
        fail("MCP server never accepted a connection on 127.0.0.1:%d" % port)
    return RpcClient(sock)


def phase_a(client, h):
    """Parked-class tools + continue + mismatch fast-rejects."""
    if not wait_for_parked(client):
        sys.stderr.write(h.output())
        fail("guest never reached the parked state under -break-start")
    print("OK: guest parked at startup (state=parked)")

    rid = 0

    # step: single-step advances CS:EIP and stays parked
    cs0, eip0 = regs(client, 1)
    prev = (cs0, eip0)
    for _ in range(3):
        rid += 1
        res = expect_result(client.call("step", rid), "step")
        check_exec(res, "step", "step", False, "parked")
        here = (int(res["cs"], 16), int(res["eip"], 16))
        if here == prev:
            fail("step: CS:EIP did not advance (still %04x:%08x)" % prev)
        rid += 1
        if regs(client, rid) != here:
            fail("step: reported cs:eip disagrees with read_registers")
        prev = here
    print("OK: step advanced CS:EIP %04x:%08x -> %04x:%08x (stayed parked)"
          % (cs0, eip0, prev[0], prev[1]))

    # step_over: well-formed, ends parked, CS:EIP advanced
    before = prev
    rid += 1
    res = expect_result(client.call("step_over", rid), "step_over")
    if res.get("op") != "step_over":
        fail("step_over: bad op %r" % res.get("op"))
    if not ensure_parked(client):
        sys.stderr.write(h.output())
        fail("step_over: guest never returned to parked")
    after = regs(client, 9000)
    if after == before:
        fail("step_over: CS:EIP did not advance")
    print("OK: step_over -> parked, CS:EIP %04x:%08x -> %04x:%08x"
          % (before[0], before[1], after[0], after[1]))

    # mode-mismatch: break while parked is fast-rejected
    err = expect_error(client.call("break", 100), "break while parked", -32001)
    if err.get("data", {}).get("state") != "parked":
        fail("break-while-parked: error did not carry state=parked: %r" % err)
    print("OK: break while parked -> fast-reject (-32001, state=parked)")

    # continue: release the guest to free-run
    released = regs(client, 200)
    rid += 1
    res = expect_result(client.call("continue", rid), "continue")
    check_exec(res, "continue", "continue", True, "running")
    if not wait_for_state(client, "running"):
        sys.stderr.write(h.output())
        fail("continue: guest never reported running")
    print("OK: continue -> running (released from %04x:%08x)"
          % (released[0], released[1]))

    # mode-mismatch: step while running is fast-rejected
    err = expect_error(client.call("step", 101), "step while running", -32001)
    if err.get("data", {}).get("state") != "running":
        fail("step-while-running: error did not carry state=running: %r" % err)
    print("OK: step while running -> fast-reject (-32001, state=running)")

    # The guest must have executed instructions while free-running: bring it back
    # to parked and confirm CS:EIP moved on from where continue released it.
    if not ensure_parked(client):
        sys.stderr.write(h.output())
        fail("continue: guest never came back to parked")
    stopped = regs(client, 300)
    if stopped == released:
        fail("continue: CS:EIP unchanged (guest did not execute while free)")
    print("OK: continue ran instructions; re-parked at %04x:%08x" % stopped)


def phase_b(client, h):
    """break's positive path against a genuinely free-running guest."""
    if not wait_for_state(client, "running"):
        sys.stderr.write(h.output())
        fail("free-running guest never reported running")
    print("OK: fresh guest free-running (state=running)")

    res = expect_result(client.call("break", 1), "break")
    if res.get("op") != "break":
        fail("break: bad op %r" % res.get("op"))
    if not wait_for_state(client, "parked"):
        sys.stderr.write(h.output())
        fail("break: guest never parked")
    cs, eip = regs(client, 2)
    print("OK: break -> parked at %04x:%08x (read_registers works at the stop)"
          % (cs, eip))


def main():
    # Phase A: parked-class tools, continue, mismatches (-break-start).
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice5a_cap_")
    with DosboxHarness(capture_dir=cap, extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        client = connect(h, port)
        phase_a(client, h)
        client.close()

    # Phase B: break on a free-running guest (no -break-start).
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice5b_cap_")
    with DosboxHarness(capture_dir=cap,
                       extra_env={"MCP_PORT": str(port)}) as h:
        client = connect(h, port)
        phase_b(client, h)
        client.close()

    print("PASS: Slice 5 execution control")
    return 0


if __name__ == "__main__":
    sys.exit(main())
