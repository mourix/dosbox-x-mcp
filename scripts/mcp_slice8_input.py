#!/usr/bin/env python3
"""Integration Test #8 (Slice 8): input injection — send_keys / type_text / mouse.

Stdlib-only. These are run-class tools serviced at the frame tick while the guest
free-runs. Slice 9's ``read_screen`` does not exist yet, so this closes the
input->screen loop the cheap way the earlier slices already give us: it reads the
**text-mode video memory** (``B800:0``, 80x25) back via the parked-class
``read_memory`` tool and looks for the echoed characters.

Boot strategy: NO ``-break-start`` (that can re-park the guest on a BIOS
interrupt, fighting our progress to the prompt). The guest free-runs to the DOS
prompt on its own; we ``break`` only momentarily to read the screen, then
``continue``. ``break`` on a free-running guest is the Slice 5 phase-B path.

Asserts:
  * the guest reaches the ``Z:\\`` prompt (free-running);
  * mode-mismatch: ``send_keys`` while parked -> -32001 carrying state "parked";
  * ``send_keys`` ["d","i","r"] at the prompt echoes "dir" onto the text screen;
  * ``type_text`` "HELLO" (shifted chars, frame-paced) echoes "HELLO";
  * ``mouse`` move/down/up/click/wheel are accepted and well-formed (no mouse
    driver loaded, so only the bridge wiring is asserted, not a guest effect);
  * bad params (unknown key, bad mouse action, missing text) -> -32602.

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 8).
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice5_exec import (connect, ping_state, wait_for_state, ensure_parked,
                             expect_result, expect_error)

CEILING = 64 * 1024
TEXT_SEG = 0xB800           # text-mode video memory segment (80x25 color)
TEXT_LEN = 80 * 25 * 2      # one screen page, char+attr per cell = 4000 bytes

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


def screen_text(client):
    """Read the 80x25 text page (assumes the guest is parked) and return its
    visible characters as one string (char cells only, attributes dropped)."""
    r = expect_result(
        client.call("read_memory", rid(),
                    {"seg": TEXT_SEG, "off": 0, "len": TEXT_LEN}),
        "read_memory(screen)")
    raw = bytes.fromhex(r.get("hex", "").replace("?", "0"))
    chars = raw[0::2]   # even bytes are the character cells
    return "".join(chr(c) if 32 <= c < 127 else " " for c in chars)


def ensure_running(client):
    """Make sure the guest is free-running (release it if parked)."""
    if ping_state(client, rid()) == "parked":
        client.call("continue", rid())
    return wait_for_state(client, "running")


def boot_to_prompt(client, timeout=40.0):
    """Free-run the guest until the DOS prompt (``Z:\\``) appears on the text
    screen. Returns once it is found, leaving the guest PARKED. The guest boots
    with no -break-start, so it advances on its own; we only break to peek."""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        ensure_running(client)
        time.sleep(0.5)
        if not ensure_parked(client):
            continue
        last = screen_text(client)
        if "Z:\\" in last:
            return last
    sys.stderr.write("last screen seen:\n%r\n" % last)
    fail("guest never reached the DOS prompt (Z:\\) within %.0fs" % timeout)


def inject_and_find(client, send_fn, needle, settle=12.0):
    """Run ``send_fn`` (a run-class input call) while the guest is free-running,
    then poll the text screen until ``needle`` is echoed. Returns the screen text
    (guest left parked) or fails."""
    if not ensure_running(client):
        fail("guest never reported running before input")
    send_fn()
    deadline = time.time() + settle
    while time.time() < deadline:
        time.sleep(0.5)
        if not ensure_parked(client):
            continue
        text = screen_text(client)
        if needle in text:
            return text
        # not echoed yet: give the guest more run time and retry.
        client.call("continue", rid())
    fail("input never echoed %r onto the screen" % needle)


def phase_prompt(client):
    boot_to_prompt(client)
    print("OK: guest free-ran to the DOS prompt (Z:\\ visible)")

    # mode-mismatch: a run-class input tool while parked is fast-rejected.
    err = expect_error(client.call("send_keys", rid(), {"keys": ["a"]}),
                       "send_keys while parked", -32001)
    if err.get("data", {}).get("state") != "parked":
        fail("send_keys-while-parked: error lacked state=parked: %r" % err)
    print("OK: send_keys while parked -> fast-reject (-32001, state=parked)")


def phase_send_keys(client):
    def send():
        r = expect_result(
            client.call("send_keys", rid(), {"keys": ["d", "i", "r"]}),
            "send_keys dir")
        if not r.get("injected") or r.get("transitions") != 6:
            fail("send_keys: unexpected reply %r" % r)
    text = inject_and_find(client, send, "dir")
    print("OK: send_keys ['d','i','r'] echoed 'dir' onto the screen")
    return text


def phase_type_text(client):
    def send():
        r = expect_result(
            client.call("type_text", rid(), {"text": "HELLO"}),
            "type_text HELLO")
        if not r.get("queued") or r.get("chars") != 5 or r.get("skipped") != 0:
            fail("type_text: unexpected reply %r" % r)
    inject_and_find(client, send, "HELLO")
    print("OK: type_text 'HELLO' (shifted, frame-paced) echoed 'HELLO'")


def phase_mouse(client):
    if not ensure_running(client):
        fail("guest never reported running before mouse")
    cases = [
        ({"action": "move", "dx": 5, "dy": -3}, "move"),
        ({"action": "down", "button": 0}, "down"),
        ({"action": "up", "button": 0}, "up"),
        ({"action": "click", "button": 1}, "click"),
        ({"action": "wheel", "amount": 2}, "wheel"),
    ]
    for params, name in cases:
        r = expect_result(client.call("mouse", rid(), params), "mouse %s" % name)
        if r.get("action") != name or not r.get("injected"):
            fail("mouse %s: unexpected reply %r" % (name, r))
    print("OK: mouse move/down/up/click/wheel accepted and well-formed")


def phase_bad_params(client):
    if not ensure_running(client):
        fail("guest never reported running before bad-param checks")
    expect_error(client.call("send_keys", rid(), {"keys": ["nosuchkey"]}),
                 "send_keys unknown key", -32602)
    expect_error(client.call("send_keys", rid(), {"keys": []}),
                 "send_keys empty", -32602)
    expect_error(client.call("mouse", rid(), {"action": "spin"}),
                 "mouse bad action", -32602)
    expect_error(client.call("type_text", rid(), {}),
                 "type_text missing text", -32602)
    print("OK: bad params (unknown key, empty keys, bad action, no text) -> -32602")


def main():
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice8_cap_")
    # No -break-start: the guest free-runs to the prompt on its own.
    with DosboxHarness(capture_dir=cap,
                       extra_env={"MCP_PORT": str(port)}) as h:
        client = connect(h, port)
        phase_prompt(client)
        phase_send_keys(client)
        phase_type_text(client)
        phase_mouse(client)
        phase_bad_params(client)
        client.close()

    print("PASS: Slice 8 input injection")
    return 0


if __name__ == "__main__":
    sys.exit(main())
