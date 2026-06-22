#!/usr/bin/env python3
"""Integration Test #4 (Slice 4): read_memory (paginated) + disassemble (bounded).

Stdlib-only. Boots dosbox-x headless with ``-break-start`` so the CPU is parked
in the debugger at machine startup, then over JSON-RPC drives the two new
parked-class tools and asserts:

  read_memory
    * a 16-byte segmented read of low RAM (0000:0000, the IVT region) is fully
      readable, returns lowercase hex of the right length, and reports a
      resolved linear address;
    * the same bytes read again through the ``virtual`` space (lin == the linear
      address the segmented read resolved) are identical — proving the address
      spaces agree;
    * the default page is MCP_READMEM_DEFAULT (256) bytes;
    * an over-cap request (len > 4096) is clamped to MCP_READMEM_MAX, flagged
      ``truncated``, and carries a ``next_off`` for pagination;
    * bad params (missing seg, unknown space) fast-fail with -32602.

  disassemble
    * decoding at CS:EIP returns at least one instruction; each carries an
      offset, a resolved address, raw bytes and disassembly text; instruction
      offsets advance by the byte length (sequencing is consistent);
    * an over-cap count (> 128) is clamped to MCP_DISASM_MAX and flagged
      ``truncated``.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 4).
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice3_registers import wait_for_parked

CEILING = 64 * 1024
HEXSTR = re.compile(r"^[0-9a-f]*$")          # readable hex dump (no "??")


def assert_bounded(resp, what):
    size = len(repr(resp))
    if size > CEILING:
        fail("%s: reply exceeds payload ceiling (%d bytes)" % (what, size))


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


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice4_cap_")

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

        # --- read_memory: segmented read of low RAM (always-mapped IVT) -------
        rid = 1
        r = expect_result(client.call("read_memory",
                                      rid, {"seg": 0, "off": 0, "len": 16}),
                          "read_memory segmented")
        if not r.get("addr_valid"):
            fail("read_memory: 0000:0000 did not resolve: %r" % r)
        if r.get("len") != 16:
            fail("read_memory: expected len 16, got %r" % r.get("len"))
        seg_hex = r.get("hex")
        if not isinstance(seg_hex, str) or len(seg_hex) != 32 or not HEXSTR.match(seg_hex):
            fail("read_memory: hex is not 32 lowercase hex chars: %r" % seg_hex)
        if r.get("unreadable") != 0:
            fail("read_memory: low RAM should be fully readable, got %r" % r.get("unreadable"))
        if r.get("truncated"):
            fail("read_memory: 16-byte read must not be truncated")
        linear = int(r["addr"], 16)
        print("OK: read_memory segmented 0000:0000 -> addr=%s hex=%s"
              % (r["addr"], seg_hex))

        # --- read_memory: same bytes via the virtual space --------------------
        rid += 1
        rv = expect_result(client.call("read_memory",
                                       rid, {"space": "virtual", "lin": linear, "len": 16}),
                           "read_memory virtual")
        if rv.get("hex") != seg_hex:
            fail("read_memory: virtual read disagrees with segmented: %r != %r"
                 % (rv.get("hex"), seg_hex))
        print("OK: read_memory virtual lin=%s agrees with segmented" % r["addr"])

        # --- read_memory: default page size -----------------------------------
        rid += 1
        rd = expect_result(client.call("read_memory", rid, {"seg": 0, "off": 0}),
                           "read_memory default len")
        if rd.get("len") != 256:
            fail("read_memory: default page should be 256, got %r" % rd.get("len"))
        if len(rd.get("hex", "")) != 512:
            fail("read_memory: default hex should be 512 chars, got %d"
                 % len(rd.get("hex", "")))
        print("OK: read_memory default page = 256 bytes")

        # --- read_memory: over-cap request is clamped + paginated -------------
        rid += 1
        rc = expect_result(client.call("read_memory",
                                       rid, {"seg": 0, "off": 0, "len": 9000}),
                           "read_memory over-cap")
        if rc.get("len") != 4096:
            fail("read_memory: over-cap len should clamp to 4096, got %r" % rc.get("len"))
        if len(rc.get("hex", "")) != 8192:
            fail("read_memory: clamped hex should be 8192 chars, got %d"
                 % len(rc.get("hex", "")))
        if not rc.get("truncated"):
            fail("read_memory: over-cap request must be flagged truncated")
        if "next_off" not in rc:
            fail("read_memory: truncated reply must carry next_off: %r" % rc)
        if int(rc["next_off"], 16) != 0 + 4096:
            fail("read_memory: next_off should be off+4096, got %r" % rc.get("next_off"))
        print("OK: read_memory len=9000 clamped to 4096, truncated, next_off=%s"
              % rc["next_off"])

        # --- read_memory: bad params fast-fail --------------------------------
        rid += 1
        expect_error(client.call("read_memory", rid, {"off": 0}),
                     "read_memory missing seg")
        rid += 1
        expect_error(client.call("read_memory", rid,
                                 {"space": "nope", "seg": 0, "off": 0}),
                     "read_memory bad space")
        print("OK: read_memory rejects missing seg / unknown space (-32602)")

        # --- disassemble at CS:EIP --------------------------------------------
        reg = expect_result(client.call("read_registers", 100), "read_registers")
        cs = int(reg["cs"], 16)
        eip = int(reg["eip"], 16)

        rid += 1
        da = expect_result(client.call("disassemble",
                                       rid, {"seg": cs, "off": eip, "count": 4}),
                           "disassemble")
        if not da.get("addr_valid"):
            fail("disassemble: CS:EIP did not resolve: %r" % da)
        insns = da.get("insns")
        if not isinstance(insns, list) or len(insns) < 1:
            fail("disassemble: expected >= 1 instruction, got %r" % insns)
        if da.get("count") != len(insns):
            fail("disassemble: count %r != len(insns) %d" % (da.get("count"), len(insns)))
        if int(insns[0]["off"], 16) != eip:
            fail("disassemble: first insn off %r != eip 0x%x" % (insns[0]["off"], eip))
        for k, ins in enumerate(insns):
            for key in ("off", "addr", "bytes", "text"):
                if key not in ins:
                    fail("disassemble: insn %d missing %r: %r" % (k, key, ins))
            if not ins["text"]:
                fail("disassemble: insn %d has empty text" % k)
            # Sequencing: next offset == this offset + byte length.
            nbytes = len(ins["bytes"]) // 2
            if nbytes < 1:
                fail("disassemble: insn %d has no bytes: %r" % (k, ins))
            if k + 1 < len(insns):
                exp = int(ins["off"], 16) + nbytes
                if int(insns[k + 1]["off"], 16) != exp:
                    fail("disassemble: insn %d offset sequencing broken: %r -> %r"
                         % (k, ins["off"], insns[k + 1]["off"]))
        print("OK: disassemble CS:EIP -> %d insns, first: %s  %s"
              % (len(insns), insns[0]["off"], insns[0]["text"]))

        # --- disassemble: over-cap count is clamped ---------------------------
        rid += 1
        dc = expect_result(client.call("disassemble",
                                       rid, {"seg": cs, "off": eip, "count": 500}),
                           "disassemble over-cap")
        if len(dc.get("insns", [])) > 128:
            fail("disassemble: over-cap count must clamp to <= 128, got %d"
                 % len(dc.get("insns", [])))
        if not dc.get("truncated"):
            fail("disassemble: over-cap count must be flagged truncated")
        print("OK: disassemble count=500 clamped to %d, truncated"
              % len(dc.get("insns", [])))

        client.close()

    print("PASS: Slice 4 read_memory + disassemble")
    return 0


if __name__ == "__main__":
    sys.exit(main())
