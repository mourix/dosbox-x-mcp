#!/usr/bin/env python3
"""Integration Test #6 (Slice 6): breakpoints — list / add / delete.

Stdlib-only. Boots headless parked with ``-break-start`` and drives the three
breakpoint tools over TCP. Three phases:

  Phase 1 — list/add/delete mechanics (deterministic, no execution):
    * start from a clean slate (``breakpoint_delete all``); ``breakpoint_list``
      reports an empty, well-formed, bounded list;
    * ``breakpoint_add`` an exec bp at a fixed seg:off -> it appears in the list
      with the right type/seg/off;
    * ``breakpoint_add`` an INT bp (INT 21h, AH=4Ch) -> appears with int/ah and
      ``al`` rendered "*" (match-all);
    * ``breakpoint_delete`` by index removes exactly that one; the other remains;
    * ``breakpoint_delete all`` empties the list.

  Phase 2 — an exec breakpoint actually stops a continuing guest:
    Find a fall-through instruction (disassemble the current insn + its successor,
    plant a one-shot exec bp on the successor, ``continue``); on a branch/timeout
    retry from the next instruction. When ``continue`` re-parks exactly on the
    planted address, the breakpoint fired. Robust to control flow.

  Phase 3 — an INT breakpoint stops a continuing guest:
    Plant ``BPINT 10h`` (video BIOS, called during POST) and ``continue``; the
    guest re-parks when the BIOS issues INT 10h.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 6).
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


# -- small client helpers --------------------------------------------------

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


def regs(client):
    r = expect_result(client.call("read_registers", rid()), "read_registers")
    return int(r["cs"], 16), int(r["eip"], 16)


def ping_state(client):
    r = expect_result(client.call("ping", rid()), "ping")
    return r.get("state")


def bp_list(client):
    r = expect_result(client.call("breakpoint_list", rid()), "breakpoint_list")
    assert_bounded(r, "breakpoint_list")
    for key in ("breakpoints", "count", "total", "truncated"):
        if key not in r:
            fail("breakpoint_list: missing %r in %r" % (key, r))
    return r


def bp_add(client, **params):
    r = expect_result(client.call("breakpoint_add", rid(), params), "breakpoint_add")
    if not r.get("added"):
        fail("breakpoint_add(%r): not added: %r" % (params, r))
    return r


def bp_delete(client, **params):
    r = expect_result(client.call("breakpoint_delete", rid(), params), "breakpoint_delete")
    return r


def wait_for_state(client, want, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ping_state(client) == want:
            return True
        time.sleep(0.1)
    return False


def ensure_parked(client, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ping_state(client) == "parked":
            return True
        client.call("break", rid())     # accepted if running; rejected (no-op) if parked
        time.sleep(0.05)
    return ping_state(client) == "parked"


# -- phases ----------------------------------------------------------------

def phase1_mechanics(client, h):
    """list / add / delete are purely deterministic while parked."""
    if not wait_for_parked(client):
        sys.stderr.write(h.output())
        fail("guest never reached parked under -break-start")
    print("OK: guest parked at startup")

    bp_delete(client, all=True)
    lst = bp_list(client)
    if lst["total"] != 0:
        fail("after delete-all, list not empty: %r" % lst)
    if lst["truncated"]:
        fail("empty list reported truncated: %r" % lst)
    print("OK: clean slate, breakpoint_list empty + well-formed")

    # add an exec breakpoint at a fixed location
    bp_add(client, type="exec", seg="0x1234", off="0x5678")
    lst = bp_list(client)
    execbp = [b for b in lst["breakpoints"]
              if b["type"] == "exec" and b.get("seg") == "0x1234"
              and b.get("off") == "0x00005678"]
    if len(execbp) != 1:
        fail("exec bp not reflected in list: %r" % lst)
    if lst["total"] != 1:
        fail("expected total 1 after one add, got %r" % lst)
    print("OK: exec bp added and listed (%04x:%08x)" % (0x1234, 0x5678))

    # add an INT breakpoint (newest -> index 0)
    bp_add(client, type="int", int="0x21", ah="0x4c")
    lst = bp_list(client)
    intbp = [b for b in lst["breakpoints"] if b["type"] == "int"]
    if len(intbp) != 1:
        fail("int bp not reflected: %r" % lst)
    ib = intbp[0]
    if ib.get("int") != "0x21" or ib.get("ah") != "0x4c" or ib.get("al") != "*":
        fail("int bp fields wrong: %r" % ib)
    if lst["total"] != 2:
        fail("expected total 2 after two adds, got %r" % lst)
    print("OK: int bp added and listed (INT 21h AH=4C AL=*)")

    # delete the int bp by its index; the exec bp must survive
    bp_delete(client, index=ib["index"])
    lst = bp_list(client)
    if lst["total"] != 1:
        fail("expected total 1 after delete-by-index, got %r" % lst)
    if any(b["type"] == "int" for b in lst["breakpoints"]):
        fail("int bp survived delete-by-index: %r" % lst)
    if not any(b["type"] == "exec" for b in lst["breakpoints"]):
        fail("exec bp wrongly removed by delete-by-index: %r" % lst)
    print("OK: delete-by-index removed exactly the int bp")

    # bad delete params are rejected, never crash
    expect_error(client.call("breakpoint_delete", rid(), {}), "delete without index", -32602)
    print("OK: delete with no index/all -> -32602")

    bp_delete(client, all=True)
    if bp_list(client)["total"] != 0:
        fail("delete-all did not empty the list")
    print("OK: delete-all emptied the list")


def phase2_exec_stop(client, h):
    """An exec breakpoint stops a continuing guest (control-flow robust)."""
    if not ensure_parked(client):
        fail("phase2: could not park")
    bp_delete(client, all=True)

    # Step past the reset far-jump (F000:FFF0) into linear POST init code, where
    # most instructions fall through — so the search below converges immediately
    # instead of burning a timeout on the guaranteed-taken reset jump.
    for _ in range(4):
        client.call("step", rid())

    for attempt in range(40):
        cs, eip = regs(client)
        d = expect_result(client.call("disassemble", rid(),
                                      {"seg": cs, "off": eip, "count": 2}), "disassemble")
        insns = d.get("insns", [])
        if len(insns) < 2:
            client.call("step", rid())   # can't look ahead here; advance and retry
            continue
        target = int(insns[1]["off"], 16)

        bp_delete(client, all=True)
        bp_add(client, type="exec", seg=cs, off=target, once=True)
        expect_result(client.call("continue", rid()), "continue")

        if wait_for_state(client, "parked", timeout=4.0):
            cs2, eip2 = regs(client)
            if (cs2, eip2) == (cs, target):
                print("OK: exec bp fired — continue stopped at %04x:%08x "
                      "(attempt %d)" % (cs2, eip2, attempt + 1))
                bp_delete(client, all=True)
                return
            # parked elsewhere (branch); fall through to retry from here
        else:
            if not ensure_parked(client):
                sys.stderr.write(h.output())
                fail("phase2: guest stuck running after continue")
        bp_delete(client, all=True)
        client.call("step", rid())       # advance one instruction and retry

    sys.stderr.write(h.output())
    fail("phase2: never landed on a fall-through instruction to prove the exec bp")


def phase3_int_stop(client, h):
    """An INT breakpoint stops a continuing guest.

    Runs against a *fresh* ``-break-start`` boot (its own harness): phases 1/2
    advance the guest mid-POST, and free-running from there loops back to the
    reset vector without reaching a software INT, so the INT-breakpoint proof
    needs the clean reset-parked starting state."""
    if not wait_for_parked(client):
        sys.stderr.write(h.output())
        fail("phase3: fresh guest never reached parked under -break-start")
    bp_delete(client, all=True)

    bp_add(client, type="int", int="0x10")   # video BIOS — issued during POST
    expect_result(client.call("continue", rid()), "continue")
    if not wait_for_state(client, "parked", timeout=30.0):
        sys.stderr.write(h.output())
        fail("phase3: INT 10h breakpoint never stopped the guest")
    cs, eip = regs(client)
    print("OK: INT 10h bp fired — continue stopped at %04x:%08x" % (cs, eip))
    bp_delete(client, all=True)


def connect(h):
    h.start()
    sock = wait_for_port(h.mcp_port, h.proc)
    if sock is None:
        sys.stderr.write(h.output())
        fail("MCP server never accepted a connection on 127.0.0.1:%d" % h.mcp_port)
    return RpcClient(sock)


def main():
    # Harness #1: list/add/delete mechanics + the exec-bp continue->stop proof.
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice6a_cap_")
    with DosboxHarness(capture_dir=cap, extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.mcp_port = port
        client = connect(h)
        phase1_mechanics(client, h)
        phase2_exec_stop(client, h)
        client.close()

    # Harness #2: the INT-bp continue->stop proof, from a clean reset-parked guest.
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice6b_cap_")
    with DosboxHarness(capture_dir=cap, extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.mcp_port = port
        client = connect(h)
        phase3_int_stop(client, h)
        client.close()

    print("PASS: Slice 6 breakpoints")
    return 0


if __name__ == "__main__":
    sys.exit(main())
