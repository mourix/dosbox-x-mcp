#!/usr/bin/env python3
"""Integration Test #10 (Slice 10): take_screenshot.

Stdlib-only. take_screenshot is a run-class tool that promotes the Slice 1
capture verification into a real MCP tool: it triggers a PNG of the current frame
(the RENDER / CAPTURE_AddImage path, proven headless under the dummy video driver)
and returns a **path + metadata**, never raw pixels. The capture is asynchronous —
the server defers the request and re-attempts it each frame until the PNG lands,
all under the client's 5 s wait — so to the client it is a single blocking call
that returns a ready-to-read path.

Boot strategy: NO ``-break-start`` (the guest must free-run so the frame loop is
ticking and a frame renders). The guest reaches the ``Z:\\`` prompt on its own; we
screenshot the live frame. take_screenshot is mode-agnostic (the text prompt is
still rasterized to a real PNG); the metadata reports the source ``mode`` /
``is_text``.

Asserts:
  * take_screenshot returns {path, format:"png", width>0, height>0, bytes>0,
    mode, is_text};
  * the returned path exists, starts with the PNG magic, its size == bytes, and
    its IHDR width/height == the reported width/height (correct dims);
  * a second screenshot writes a distinct, also-valid PNG (monotonic naming);
  * mode-mismatch: take_screenshot while parked -> -32001 carrying state "parked".

Every reply is asserted under the 64 KiB ceiling. Reused by
``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 10).
"""

import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, fail
from mcp_slice2_ping import free_port
from mcp_slice5_exec import (connect, ping_state, wait_for_state, ensure_parked,
                             expect_result, expect_error)

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


def ensure_running(client):
    if ping_state(client, rid()) == "parked":
        client.call("continue", rid())
    return wait_for_state(client, "running")


def png_ihdr_dims(path):
    """Read width/height from the PNG IHDR (the first chunk after the magic)."""
    with open(path, "rb") as fh:
        head = fh.read(24)
    if len(head) < 24 or head[:8] != PNG_MAGIC:
        fail("screenshot: %s is not a PNG (bad magic)" % path)
    # IHDR data begins at offset 16: width (u32 BE), height (u32 BE).
    width, height = struct.unpack(">II", head[16:24])
    return width, height


def take_screenshot(client, what):
    """Call take_screenshot (run-class: guest must be running) and validate the
    returned metadata + the PNG file on disk. Returns the result dict."""
    r = expect_result(client.call("take_screenshot", rid(), {}), what)

    if r.get("format") != "png":
        fail("%s: format != png: %r" % (what, r))
    path = r.get("path")
    if not isinstance(path, str) or not path:
        fail("%s: missing path: %r" % (what, r))
    w, h, n = r.get("width"), r.get("height"), r.get("bytes")
    for k, v in (("width", w), ("height", h), ("bytes", n)):
        if not isinstance(v, int) or v <= 0:
            fail("%s: %s not a positive int: %r" % (what, k, r))

    if not os.path.isfile(path):
        fail("%s: returned path does not exist: %s" % (what, path))
    actual = os.path.getsize(path)
    if actual != n:
        fail("%s: bytes %d != on-disk size %d" % (what, n, actual))
    iw, ih = png_ihdr_dims(path)
    if (iw, ih) != (w, h):
        fail("%s: reported %dx%d != IHDR %dx%d" % (what, w, h, iw, ih))
    return r


def phase_capture(client, h):
    if not ensure_running(client):
        sys.stderr.write(h.output())
        fail("guest never reached the running state")
    # Give the guest a moment to render past any transient early-boot frames.
    time.sleep(1.0)

    r1 = take_screenshot(client, "take_screenshot #1")
    print("OK: take_screenshot wrote a valid %dx%d PNG (%d bytes, mode 0x%x)"
          % (r1["width"], r1["height"], r1["bytes"], r1["mode"]))

    r2 = take_screenshot(client, "take_screenshot #2")
    if r2["path"] == r1["path"]:
        fail("second screenshot reused the same path: %s" % r2["path"])
    print("OK: a second take_screenshot wrote a distinct valid PNG (%s)"
          % os.path.basename(r2["path"]))


def phase_mode_mismatch(client):
    # break (run-class) parks the guest; take_screenshot (run-class) is then
    # fast-rejected with the current state.
    if not ensure_running(client):
        fail("guest not running before break")
    expect_result(client.call("break", rid()), "break")
    if not ensure_parked(client):
        fail("guest did not park after break")
    err = expect_error(client.call("take_screenshot", rid(), {}),
                       "take_screenshot while parked", -32001)
    if err.get("data", {}).get("state") != "parked":
        fail("take_screenshot-while-parked: error lacked state=parked: %r" % err)
    client.call("continue", rid())
    wait_for_state(client, "running")
    print("OK: take_screenshot while parked -> fast-reject (-32001, state=parked)")


def main():
    port = free_port()
    cap = tempfile.mkdtemp(prefix="mcp_slice10_cap_")
    # No -break-start: the guest must free-run so frames render and capture.
    with DosboxHarness(capture_dir=cap,
                       extra_env={"MCP_PORT": str(port)}) as h:
        client = connect(h, port)
        phase_capture(client, h)
        phase_mode_mismatch(client)
        client.close()

    print("PASS: Slice 10 take_screenshot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
