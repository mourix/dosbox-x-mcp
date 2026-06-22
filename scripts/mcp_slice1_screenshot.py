#!/usr/bin/env python3
"""MCP Integration Test #1 (Slice 1): headless screenshot capture.

De-risks the screen pillar before any screen tooling exists: it verifies the
riskiest assumption in the plan -- that the RENDER / CAPTURE_AddImage path
produces a real PNG frame under the **dummy video driver**.

It boots dosbox-x headless to the DOS prompt (the minimal renderable target),
relies on the C_MCP-gated self-test (MCP_SELFTEST_SCREENSHOT) to request exactly
one screenshot once the guest is up, then polls the capture dir for a non-empty
PNG and asserts it exists. No MCP transport is required yet.

Run directly or via scripts/mcp-check.sh. Exit 0 = pass.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import DosboxHarness, list_captures, fail  # noqa: E402

# PNG magic so "non-empty" also means "actually a PNG", not a truncated stub.
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# Generous bound: headless boot + a few rendered frames. The harness fails fast
# if the process dies early, so this only caps the genuine-hang case.
CAPTURE_TIMEOUT = float(os.environ.get("MCP_SLICE1_TIMEOUT", "60"))


def valid_png(path):
    try:
        if os.path.getsize(path) < len(PNG_MAGIC):
            return False
        with open(path, "rb") as fh:
            return fh.read(len(PNG_MAGIC)) == PNG_MAGIC
    except OSError:
        return False


def main():
    workdir = tempfile.mkdtemp(prefix="mcp_slice1_")
    capture_dir = os.path.join(workdir, "captures")

    env = {
        "MCP_SELFTEST_SCREENSHOT": "1",
        # Request the shot fairly early; boot to the Z:\ prompt renders well
        # before this. Overridable for slow CI via MCP_SELFTEST_FRAMES.
        "MCP_SELFTEST_FRAMES": os.environ.get("MCP_SELFTEST_FRAMES", "120"),
    }

    with DosboxHarness(capture_dir=capture_dir, extra_env=env,
                       workdir=workdir) as h:
        print("Slice 1: launching headless dosbox-x (dummy video driver)...")
        h.start()

        def have_png():
            return any(valid_png(p) for p in list_captures(capture_dir))

        ok = h.wait_for_file(have_png, CAPTURE_TIMEOUT)
        pngs = [p for p in list_captures(capture_dir) if valid_png(p)]

        if not ok or not pngs:
            sys.stderr.write("\n--- dosbox-x output ---\n")
            sys.stderr.write(h.output())
            sys.stderr.write("\n-----------------------\n")
            fail("no valid PNG captured under the dummy video driver within "
                 "%.0fs. The dummy driver may not render frames; record a "
                 "fallback (SDL_VIDEODRIVER alternative or offscreen surface) "
                 "before Slice 10 -- see docs/MCP_BUILD_PLAN.md Slice 1 DoD."
                 % CAPTURE_TIMEOUT)

        png = pngs[0]
        size = os.path.getsize(png)
        print("Slice 1 PASS: captured %s (%d bytes) under dummy video driver"
              % (os.path.basename(png), size))

    import shutil
    shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
