#!/usr/bin/env python3
"""Integration Test #11 (Slice 11): memory scanner — scan_start / scan_filter /
scan_results.

Stdlib-only. Boots dosbox-x headless with ``-break-start`` so the CPU is parked
in the debugger, then drives the three new parked-class scanner tools. The guest
memory is held still (parked), so the scratch region is fully under our control
via the Slice 7 write_memory tool — making the "cheat-engine" narrowing exact and
deterministic:

  phase_no_scan
    * with no scan in progress, scan_filter fast-fails with -32600 and
      scan_results reports active:false / total 0.

  phase_narrow (width 1) — the core build-plan assertion
    * zero-fill a scratch region and scan_start over it: every slot is a
      candidate (matches == range);
    * change ONE byte to a value unique in the region, then scan_filter "=="
      that value — the candidate set narrows to exactly that one address, and
      scan_results returns it (correct seg:off + current value).

  phase_width2
    * a width-2 scan narrows the same way and renders the 16-bit value with the
      element width (4 hex digits).

  phase_pagination
    * a scan whose candidate count exceeds the 256-entry page reports
      count=256/total=N/truncated, and a second page (start=256) returns the rest.

  phase_errors
    * scan_start with a missing range, scan_filter with an unknown op, and a
      value wider than the element size all fast-fail with -32602; a fresh
      scan_start replaces an in-progress scan.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 11).
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice3_registers import wait_for_parked

CEILING = 64 * 1024

# A safe scratch region: 3000:0000 (linear 0x30000, 192 KiB) — well inside the
# 640 KiB conventional RAM, unused this early in POST, writable, not IVT/BDA.
SCRATCH_SEG = 0x3000

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


def assert_bounded(resp, what):
    if len(repr(resp)) > CEILING:
        fail("%s: reply exceeds payload ceiling" % what)


def expect_result(resp, what):
    if not resp or "result" not in resp:
        fail("%s: unexpected reply: %r" % (what, resp))
    assert_bounded(resp, what)
    return resp["result"]


def expect_error(resp, what, code=-32602):
    if not resp or "error" not in resp:
        fail("%s: expected an error, got: %r" % (what, resp))
    if resp["error"].get("code") != code:
        fail("%s: expected error code %d, got %r" % (what, code, resp["error"]))


def write_bytes(client, off, values, width=1):
    r = expect_result(client.call("write_memory", rid(),
                                  {"seg": SCRATCH_SEG, "off": off,
                                   "width": width, "values": values}),
                      "write_memory")
    if not r.get("addr_valid") or r.get("fault"):
        fail("write_memory @%04x: bad result: %r" % (off, r))
    return r


def fill_zero(client, off, nbytes):
    write_bytes(client, off, [0] * nbytes)


def scan_start(client, off, rng, width=1):
    return expect_result(client.call("scan_start", rid(),
                                     {"seg": SCRATCH_SEG, "off": off,
                                      "range": rng, "width": width}),
                         "scan_start")


def scan_filter(client, op, value=None, use_prev=False):
    params = {"op": op}
    if use_prev:
        params["use_prev"] = True
    else:
        params["value"] = value
    return expect_result(client.call("scan_filter", rid(), params), "scan_filter")


def scan_results(client, start=0):
    return expect_result(client.call("scan_results", rid(), {"start": start}),
                         "scan_results")


def phase_no_scan(client):
    # No scan active yet: filter is an invalid request, results report inactive.
    expect_error(client.call("scan_filter", rid(), {"op": "==", "value": 1}),
                 "scan_filter without a scan", code=-32600)
    r = scan_results(client)
    if r.get("active") is not False or r.get("total") != 0 or r.get("matches"):
        fail("scan_results without a scan should be inactive/empty: %r" % r)
    print("OK: no active scan -> filter -32600, results inactive")


def phase_narrow(client):
    OFF, RNG, TARGET = 0x0000, 0x0100, 0x55
    fill_zero(client, OFF, RNG)

    st = scan_start(client, OFF, RNG, width=1)
    if not st.get("active") or st.get("width") != 1 or st.get("range") != RNG:
        fail("scan_start: unexpected state: %r" % st)
    if st.get("matches") != RNG:        # every byte slot is a candidate
        fail("scan_start: expected %d candidates, got %r" % (RNG, st.get("matches")))
    print("OK: scan_start snapshots %d byte candidates" % RNG)

    # Change one known value to something unique in the region, then narrow.
    write_bytes(client, TARGET, ["0x42"])
    st = scan_filter(client, "==", "0x42")
    if st.get("matches") != 1:
        fail("scan_filter ==0x42: expected to narrow to 1, got %r" % st.get("matches"))
    if st.get("iterations") != 1:
        fail("scan_filter: expected iterations=1, got %r" % st.get("iterations"))

    r = scan_results(client)
    if r.get("total") != 1 or r.get("count") != 1 or r.get("truncated"):
        fail("scan_results: expected exactly 1 match: %r" % r)
    m = r["matches"][0]
    if int(m["off"], 16) != TARGET or int(m["value"], 16) != 0x42:
        fail("scan_results: wrong match: %r (wanted off=0x%x value=0x42)" % (m, TARGET))
    print("OK: changed value narrowed scan to exactly 3000:%04x = 0x42" % TARGET)


def phase_width2(client):
    OFF, RNG, TARGET = 0x0200, 0x0080, 0x0210
    fill_zero(client, OFF, RNG)

    st = scan_start(client, OFF, RNG, width=2)
    if st.get("width") != 2 or st.get("matches") != RNG // 2:
        fail("scan_start width2: expected %d candidates, got %r" % (RNG // 2, st))

    write_bytes(client, TARGET, ["0x1234"], width=2)
    st = scan_filter(client, "==", "0x1234")
    if st.get("matches") != 1:
        fail("scan_filter width2: expected to narrow to 1, got %r" % st.get("matches"))

    r = scan_results(client)
    m = r["matches"][0]
    if int(m["off"], 16) != TARGET:
        fail("scan_results width2: wrong offset: %r" % m)
    # value rendered at the element width: 16-bit -> 4 hex digits.
    if m["value"] != "0x1234":
        fail("scan_results width2: value not rendered at element width: %r" % m["value"])
    print("OK: width-2 scan narrowed to 3000:%04x = 0x1234" % TARGET)


def phase_pagination(client):
    OFF, RNG = 0x0400, 0x0120   # 288 byte slots > 256-entry page
    fill_zero(client, OFF, RNG)
    st = scan_start(client, OFF, RNG, width=1)
    if st.get("matches") != RNG:
        fail("scan_start pagination: expected %d candidates, got %r" % (RNG, st))

    p0 = scan_results(client, start=0)
    if p0.get("count") != 256 or p0.get("total") != RNG or not p0.get("truncated"):
        fail("scan_results page0: expected count=256 total=%d truncated: %r" % (RNG, p0))
    if len(p0["matches"]) != 256:
        fail("scan_results page0: array length %d != 256" % len(p0["matches"]))

    p1 = scan_results(client, start=256)
    if p1.get("count") != RNG - 256 or p1.get("truncated"):
        fail("scan_results page1: expected %d, not truncated: %r" % (RNG - 256, p1))
    print("OK: pagination — page0 256/%d truncated, page1 returns the rest" % RNG)


def phase_errors(client):
    expect_error(client.call("scan_start", rid(), {"seg": SCRATCH_SEG, "off": 0}),
                 "scan_start missing range")
    expect_error(client.call("scan_filter", rid(), {"op": "~", "value": 1}),
                 "scan_filter unknown op")
    # value wider than the element size (width 1, value 0x100).
    scan_start(client, 0x0600, 0x10, width=1)
    expect_error(client.call("scan_filter", rid(), {"op": "==", "value": "0x100"}),
                 "scan_filter value too wide")
    # a fresh scan_start replaces the in-progress scan (no "already active" error).
    st = scan_start(client, 0x0700, 0x20, width=1)
    if not st.get("active") or st.get("matches") != 0x20:
        fail("scan_start replace: expected a fresh scan: %r" % st)
    print("OK: bad params -> -32602; fresh scan_start replaces in-progress scan")


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice11_cap_")

    with DosboxHarness(capture_dir=capture_dir,
                       extra_args=["-break-start"],
                       extra_env={"MCP_PORT": str(port)}) as h:
        h.start()
        sock = wait_for_port(port, h.proc)
        if sock is None:
            sys.stderr.write(h.output())
            fail("MCP server never accepted a connection on 127.0.0.1:%d" % port)

        client = RpcClient(sock)
        if not wait_for_parked(client):
            sys.stderr.write(h.output())
            fail("guest never reached the parked (debugger) state under -break-start")
        print("OK: guest parked at startup (state=parked)")

        phase_no_scan(client)
        phase_narrow(client)
        phase_width2(client)
        phase_pagination(client)
        phase_errors(client)
        client.close()

    print("PASS: Slice 11 memory scanner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
