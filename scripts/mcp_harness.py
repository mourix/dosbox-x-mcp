#!/usr/bin/env python3
"""Reusable headless-boot harness for DOSBox-X MCP integration tests.

Stdlib-only (guardrail: repeatable, no extra deps). It launches the freshly
built ``src/dosbox-x`` in **headless** mode (``SDL_VIDEODRIVER=dummy`` +
``SDL_AUDIODRIVER=dummy``) in an isolated temp working directory, and owns the
process lifecycle with bounded timeouts. Later slices add a TCP JSON-RPC client
on top of the same launcher; Slice 1 only needs boot + capture-dir polling.

Used by the per-slice integration scripts and by ``scripts/mcp-check.sh``.
"""

import os
import signal
import subprocess
import sys
import tempfile
import time


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def binary_path():
    return os.path.join(repo_root(), "src", "dosbox-x")


class DosboxHarness:
    """Launch and tear down a headless dosbox-x process.

    Use as a context manager so the process is always reaped:

        with DosboxHarness(capture_dir=d) as h:
            h.start()
            ...
    """

    def __init__(self, capture_dir, extra_args=None, extra_env=None,
                 workdir=None, startup_timeout=60.0):
        self.binary = binary_path()
        self.capture_dir = os.path.abspath(capture_dir)
        self.extra_args = list(extra_args or [])
        self.extra_env = dict(extra_env or {})
        self._own_workdir = workdir is None
        self.workdir = workdir or tempfile.mkdtemp(prefix="mcp_harness_")
        self.startup_timeout = startup_timeout
        self.proc = None

    # -- lifecycle ---------------------------------------------------------
    def start(self):
        if not os.path.isfile(self.binary):
            raise FileNotFoundError(
                "dosbox-x binary not found at %s (build it first)" % self.binary)
        os.makedirs(self.capture_dir, exist_ok=True)

        env = dict(os.environ)
        # Headless: no window, no display/tty, no audio device required.
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_AUDIODRIVER"] = "dummy"
        env.update(self.extra_env)

        # -set keeps dosbox-x from prompting for a working folder/config, and
        # points captures at our isolated dir. Run from an empty temp cwd so no
        # stray dosbox-x.conf is picked up (deterministic defaults).
        args = [
            self.binary,
            "-set", "dosbox captures=%s" % self.capture_dir,
        ] + self.extra_args

        self.proc = subprocess.Popen(
            args, cwd=self.workdir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return self.proc

    def wait_for_file(self, predicate, timeout):
        """Poll until predicate() is true or timeout (seconds) elapses.

        Returns True on success. Fails fast if the process dies early.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            if self.proc is not None and self.proc.poll() is not None:
                # Process exited before satisfying the predicate.
                return predicate()
            time.sleep(0.1)
        return predicate()

    def stop(self, grace=5.0):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=grace)
        self.proc = None

    def output(self):
        """Drain whatever the process has emitted so far (best effort)."""
        if self.proc is None or self.proc.stdout is None:
            return ""
        try:
            data = self.proc.stdout.read()
        except Exception:
            return ""
        return data.decode("utf-8", "replace") if data else ""

    # -- context manager ---------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        if self._own_workdir:
            import shutil
            shutil.rmtree(self.workdir, ignore_errors=True)
        return False


def list_captures(capture_dir, ext=".png"):
    if not os.path.isdir(capture_dir):
        return []
    return sorted(
        os.path.join(capture_dir, f)
        for f in os.listdir(capture_dir)
        if f.lower().endswith(ext))


def fail(msg):
    sys.stderr.write("FAIL: %s\n" % msg)
    sys.exit(1)
