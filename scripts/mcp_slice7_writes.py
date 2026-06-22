#!/usr/bin/env python3
"""Integration Test #7 (Slice 7): writes — write_register / write_memory.

Stdlib-only. Boots dosbox-x headless with ``-break-start`` so the CPU is parked
in the debugger, then drives the two new parked-class write tools and proves each
write actually landed by reading it back with the Slice 3/4 read tools:

  write_register
    * EAX := 0xAAAAAAAA, then read_registers confirms eax == 0xaaaaaaaa;
    * AX := 0x1234 then confirms eax == 0xaaaa1234 — proving the per-register
      width masking inherited from the debugger's ChangeRegister (a 16-bit write
      leaves the high half intact);
    * an unknown register name fast-fails with -32602.

  write_memory (into a safe scratch region of conventional RAM, 3000:0000)
    * byte writes [de ad be ef] read back as "deadbeef" via read_memory;
    * width-2 writes [0x1234,0x5678] read back little-endian as "34127856";
    * a write through the ``virtual`` space (linear addr) is observed identically
      through the ``segmented`` space — the spaces agree on writes too;
    * the reply reports the resolved addr, the written count and byte total, and
      fault=false;
    * bad params (missing values, empty values, bad width) fast-fail with -32602.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 7).
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice3_registers import wait_for_parked

CEILING = 64 * 1024

# A safe scratch region: linear 0x30000 (192 KiB), well inside 640 KiB
# conventional RAM, unused this early in POST, writable, and not the IVT/BDA.
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


def read_eax(client):
    r = expect_result(client.call("read_registers", rid()), "read_registers")
    return int(r["eax"], 16)


def write_reg(client, register, value):
    r = expect_result(client.call("write_register", rid(),
                                  {"register": register, "value": value}),
                      "write_register %s" % register)
    if not r.get("written"):
        fail("write_register %s: not written: %r" % (register, r))
    return r


def write_mem(client, params):
    r = expect_result(client.call("write_memory", rid(), params), "write_memory")
    return r


def read_hex(client, seg, off, length):
    r = expect_result(client.call("read_memory", rid(),
                                  {"seg": seg, "off": off, "len": length}),
                      "read_memory")
    return r


def phase_registers(client):
    write_reg(client, "EAX", "0xAAAAAAAA")
    eax = read_eax(client)
    if eax != 0xAAAAAAAA:
        fail("write_register EAX: read back 0x%08x, expected 0xaaaaaaaa" % eax)
    print("OK: write_register EAX=0xAAAAAAAA read back exactly")

    # 16-bit write must leave the high half of EAX intact (width masking).
    write_reg(client, "ax", "0x1234")
    eax = read_eax(client)
    if eax != 0xAAAA1234:
        fail("write_register AX: read back 0x%08x, expected 0xaaaa1234 "
             "(width masking failed)" % eax)
    print("OK: write_register AX=0x1234 -> eax=0xaaaa1234 (width masking)")

    # lower-case name is accepted (upper-cased before ChangeRegister).
    write_reg(client, "ebx", "0x0BADF00D")
    print("OK: lower-case register name accepted")

    # unknown register -> -32602.
    expect_error(client.call("write_register", rid(),
                             {"register": "ZZ", "value": 1}),
                 "write_register unknown reg")
    print("OK: unknown register -> -32602")


def phase_memory(client):
    # --- byte writes round-trip --------------------------------------------
    w = write_mem(client, {"seg": SCRATCH_SEG, "off": 0x0000,
                           "values": ["0xde", "0xad", "0xbe", "0xef"]})
    if not w.get("addr_valid"):
        fail("write_memory: scratch selector did not resolve: %r" % w)
    if w.get("written") != 4 or w.get("bytes") != 4 or w.get("fault"):
        fail("write_memory bytes: unexpected counts/fault: %r" % w)
    base_lin = int(w["addr"], 16)

    r = read_hex(client, SCRATCH_SEG, 0x0000, 4)
    if r.get("hex") != "deadbeef":
        fail("write_memory bytes: read back %r, expected 'deadbeef'" % r.get("hex"))
    print("OK: write_memory byte values read back as deadbeef")

    # --- width-2 writes are little-endian ----------------------------------
    w = write_mem(client, {"seg": SCRATCH_SEG, "off": 0x0010, "width": 2,
                           "values": ["0x1234", "0x5678"]})
    if w.get("written") != 2 or w.get("bytes") != 4:
        fail("write_memory width2: expected written=2 bytes=4, got %r" % w)
    r = read_hex(client, SCRATCH_SEG, 0x0010, 4)
    if r.get("hex") != "34127856":
        fail("write_memory width2: read back %r, expected '34127856'" % r.get("hex"))
    print("OK: write_memory width=2 values stored little-endian (34127856)")

    # --- a write through the virtual space agrees with the segmented view ---
    lin = base_lin + 0x20      # linear of SCRATCH_SEG:0x0020
    write_mem(client, {"space": "virtual", "lin": lin, "width": 4,
                       "values": ["0xcafebabe"]})
    r = read_hex(client, SCRATCH_SEG, 0x0020, 4)
    if r.get("hex") != "bebafeca":      # 0xcafebabe little-endian
        fail("write_memory virtual: segmented read sees %r, expected 'bebafeca'"
             % r.get("hex"))
    print("OK: write_memory via virtual space observed through segmented space")

    # --- bad params fast-fail ----------------------------------------------
    expect_error(client.call("write_memory", rid(), {"seg": SCRATCH_SEG, "off": 0}),
                 "write_memory missing values")
    expect_error(client.call("write_memory", rid(),
                             {"seg": SCRATCH_SEG, "off": 0, "values": []}),
                 "write_memory empty values")
    expect_error(client.call("write_memory", rid(),
                             {"seg": SCRATCH_SEG, "off": 0, "width": 3, "values": [1]}),
                 "write_memory bad width")
    print("OK: write_memory bad params -> -32602")


def main():
    port = free_port()
    capture_dir = tempfile.mkdtemp(prefix="mcp_slice7_cap_")

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

        phase_registers(client)
        phase_memory(client)
        client.close()

    print("PASS: Slice 7 writes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
