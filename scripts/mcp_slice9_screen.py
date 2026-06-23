#!/usr/bin/env python3
"""Integration Test #9 (Slice 9): screen reads — read_screen / screen_hash.

Stdlib-only. Both are run-class tools serviced at the frame tick while the guest
free-runs, so unlike the parked read_memory peek Slice 8 used, the screen here is
read **directly** with read_screen while the guest is running.

Boot strategy: NO ``-break-start`` (it can re-park on a BIOS interrupt). The guest
free-runs to the ``Z:\\`` prompt on its own; we read the screen as it runs.

Asserts:
  * read_screen returns a well-shaped 80x25 text grid (is_text, cols, rows, one
    string per row, each `cols` chars) with the ``Z:\\`` prompt visible;
  * mode-mismatch: read_screen while parked -> -32001 carrying state "parked";
  * screen_hash is stable when the screen does not change (two reads agree) and
    changes after a screen-changing action (typing onto the prompt);
  * screen_hash tracks read_screen: the same visible text yields the same hash.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 9).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import free_port
from mcp_slice5_exec import (connect, ping_state, wait_for_state, ensure_parked,
                             expect_result, expect_error)

CEILING = 64 * 1024

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


def ensure_running(client):
    """Make sure the guest is free-running (release it if parked)."""
    if ping_state(client, rid()) == "parked":
        client.call("continue", rid())
    return wait_for_state(client, "running")


def read_screen(client, what="read_screen"):
    """Call read_screen (run-class: guest must be running). Returns the parsed
    result dict without asserting it is a text mode — early boot is graphics."""
    return expect_result(client.call("read_screen", rid(), {}), what)


def assert_text_grid(r):
    """Validate the grid shape of a text-mode read_screen result."""
    if not r.get("is_text"):
        fail("read_screen: expected a text mode, got %r" % r)
    cols, rows = r.get("cols"), r.get("rows")
    text = r.get("text")
    if not isinstance(text, list) or len(text) != rows:
        fail("read_screen: text rows %d != rows %r" % (len(text or []), rows))
    for i, line in enumerate(text):
        if len(line) != cols:
            fail("read_screen: row %d len %d != cols %r" % (i, len(line), cols))


def screen_str(r):
    return "\n".join(r.get("text", []))


def screen_hash(client, what="screen_hash"):
    r = expect_result(client.call("screen_hash", rid(), {}), what)
    h = r.get("hash", "")
    if not (isinstance(h, str) and h.startswith("0x") and len(h) == 18):
        fail("screen_hash: malformed hash %r" % r)
    return r


def boot_to_prompt(client, timeout=40.0):
    """Free-run the guest until the ``Z:\\`` prompt appears, reading the screen
    live (run-class). Leaves the guest running. Returns the read_screen result."""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        if not ensure_running(client):
            continue
        time.sleep(0.5)
        r = read_screen(client, "read_screen(boot)")
        if not r.get("is_text"):
            last = "(graphics mode %r)" % r.get("mode")
            continue   # early boot shows a graphics splash; wait for text
        last = screen_str(r)
        if "Z:\\" in last:
            assert_text_grid(r)
            return r
    sys.stderr.write("last screen seen:\n%s\n" % last)
    fail("guest never reached the DOS prompt (Z:\\) within %.0fs" % timeout)


def phase_read_screen(client):
    r = boot_to_prompt(client)
    if r.get("cols") != 80 or r.get("rows") != 25:
        fail("read_screen: expected 80x25 grid, got %dx%d"
             % (r.get("cols"), r.get("rows")))
    print("OK: read_screen returned a well-shaped 80x25 text grid (Z:\\ visible)")


def phase_mode_mismatch(client):
    # break (run-class) parks the guest; read_screen (run-class) is then rejected.
    if not ensure_running(client):
        fail("guest not running before break")
    expect_result(client.call("break", rid()), "break")
    if not ensure_parked(client):
        fail("guest did not park after break")
    err = expect_error(client.call("read_screen", rid(), {}),
                       "read_screen while parked", -32001)
    if err.get("data", {}).get("state") != "parked":
        fail("read_screen-while-parked: error lacked state=parked: %r" % err)
    expect_error(client.call("screen_hash", rid(), {}),
                 "screen_hash while parked", -32001)
    client.call("continue", rid())
    wait_for_state(client, "running")
    print("OK: read_screen / screen_hash while parked -> fast-reject (-32001)")


def settle_hash(client, tries=10):
    """Read screen_hash until two consecutive reads agree, i.e. the screen has
    stopped changing. Returns the stable hash."""
    prev = screen_hash(client)["hash"]
    for _ in range(tries):
        time.sleep(0.4)
        cur = screen_hash(client)["hash"]
        if cur == prev:
            return cur
        prev = cur
    fail("screen_hash never settled (screen kept changing at idle prompt)")


def phase_hash(client):
    if not ensure_running(client):
        fail("guest not running before hash checks")
    stable = settle_hash(client)
    # Stable: another read with no screen change must still match.
    again = screen_hash(client)["hash"]
    if again != stable:
        fail("screen_hash not stable on an unchanging screen: %s != %s"
             % (again, stable))
    print("OK: screen_hash is stable on an unchanging screen (%s)" % stable)

    # Change detection: type onto the prompt, wait for it to render, hash must
    # differ. Also cross-check that read_screen reflects the same text.
    client.call("type_text", rid(), {"text": "ABCDEF"})
    deadline = time.time() + 12.0
    changed = None
    while time.time() < deadline:
        time.sleep(0.5)
        r = read_screen(client, "read_screen(after type)")
        if "ABCDEF" in screen_str(r):
            changed = screen_hash(client)["hash"]
            break
    if changed is None:
        fail("typed text never rendered onto the screen")
    if changed == stable:
        fail("screen_hash did not change after the screen changed: %s" % changed)
    print("OK: screen_hash changed after a screen-changing action (%s -> %s)"
          % (stable, changed))

    # Determinism: the same visible screen hashes the same on a repeat read.
    repeat = screen_hash(client)["hash"]
    if repeat != changed:
        fail("screen_hash not deterministic for the same screen: %s != %s"
             % (repeat, changed))
    print("OK: screen_hash is deterministic for a given screen")


def main():
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice9_cap_")
    # No -break-start: the guest free-runs to the prompt on its own.
    with DosboxHarness(capture_dir=cap,
                       extra_env={"MCP_PORT": str(port)}) as h:
        client = connect(h, port)
        phase_read_screen(client)
        phase_mode_mismatch(client)
        phase_hash(client)
        client.close()

    print("PASS: Slice 9 screen reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
