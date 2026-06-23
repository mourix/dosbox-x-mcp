#!/usr/bin/env python3
"""Integration Test #12 (Slice 12): lifecycle — launcher + reset / quit.

Stdlib-only. Exercises the repeatable launcher ``scripts/mcp-launch.sh`` and the
two in-session lifecycle tools, both CLS_ANY (serviceable running or parked):

  * Launcher env smoke (no boot): ``--mode headless`` exports the dummy SDL
    video+audio drivers; ``--mode visible`` leaves them unset. Both set MCP_PORT.
    (Visible mode is the same code path with the driver env unset, so the CI box
    — which has no display — only needs to assert the env toggle, per the plan.)

  * Launch a known target headless and reach a known state: the launcher boots
    the guest to the ``Z:\\`` shell prompt, reachable over the MCP server.

  * ``quit`` while running: returns a deferred ack, then the process exits on its
    own (no SIGTERM) — the reply is flushed before the kill switch fires.

  * ``reset`` while parked: with ``-break-start`` the guest parks at the reset
    vector; we step EIP away from it, ``reset``, and the machine reboots back to
    the **same** reset vector (proving a real machine reset), re-parking under
    ``-break-start``.

  * ``quit`` while parked: from that re-parked state, ``quit`` exits the process.

Reused by ``scripts/mcp-check.sh``. See docs/MCP_BUILD_PLAN.md (Slice 12).
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_harness import fail
from mcp_slice2_ping import RpcClient, free_port, wait_for_port
from mcp_slice5_exec import expect_result, ping_state

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "scripts", "mcp-launch.sh")

_RID = [0]


def rid():
    _RID[0] += 1
    return _RID[0]


# -- launcher process management -------------------------------------------

class Launcher:
    """Boot dosbox-x through scripts/mcp-launch.sh (the launcher exec's the
    binary, so our pid IS the emulator). Owns the lifecycle with timeouts."""

    def __init__(self, port, mode="headless", break_start=False):
        self.port = port
        self.mode = mode
        self.break_start = break_start
        self.workdir = tempfile.mkdtemp(prefix="mcp_slice12_")
        self.captures = tempfile.mkdtemp(prefix="mcp_slice12_cap_")
        self.proc = None

    def start(self):
        args = ["bash", LAUNCHER, "--port", str(self.port), "--mode", self.mode,
                "--captures", self.captures]
        if self.break_start:
            args.append("--break-start")
        # The launcher sets SDL_*/MCP_PORT itself; start from a clean-ish env.
        env = dict(os.environ)
        env.pop("SDL_VIDEODRIVER", None)
        env.pop("SDL_AUDIODRIVER", None)
        self.proc = subprocess.Popen(
            args, cwd=self.workdir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return self.proc

    def wait_exit(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return True
            time.sleep(0.1)
        return self.proc.poll() is not None

    def output(self):
        if self.proc is None or self.proc.stdout is None:
            return ""
        try:
            os.set_blocking(self.proc.stdout.fileno(), False)
            data = self.proc.stdout.read() or b""
            return data.decode("utf-8", "replace")
        except Exception:
            return ""

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=5)
            except Exception:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        import shutil
        shutil.rmtree(self.workdir, ignore_errors=True)
        shutil.rmtree(self.captures, ignore_errors=True)
        return False


def connect(launcher):
    launcher.start()
    sock = wait_for_port(launcher.port, launcher.proc)
    if sock is None:
        sys.stderr.write(launcher.output())
        fail("MCP server never accepted a connection on 127.0.0.1:%d" % launcher.port)
    return RpcClient(sock)


# -- phase 1: launcher env smoke (no boot) ---------------------------------

def dryrun_env(mode):
    """Run the launcher in dry-run mode; return the resolved env it would use."""
    env = dict(os.environ)
    env["MCP_LAUNCH_DRYRUN"] = "1"
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    out = subprocess.check_output(
        ["bash", LAUNCHER, "--port", "5099", "--mode", mode],
        env=env, stderr=subprocess.STDOUT).decode("utf-8", "replace")
    vals = {}
    for line in out.splitlines():
        if "=" in line and line.split("=", 1)[0] in (
                "SDL_VIDEODRIVER", "SDL_AUDIODRIVER", "MCP_PORT"):
            k, v = line.split("=", 1)
            vals[k] = v
    return vals


def phase_launcher_env():
    head = dryrun_env("headless")
    if head.get("SDL_VIDEODRIVER") != "dummy" or head.get("SDL_AUDIODRIVER") != "dummy":
        fail("headless mode did not set the dummy SDL drivers: %r" % head)
    if head.get("MCP_PORT") != "5099":
        fail("headless mode did not set MCP_PORT: %r" % head)
    print("OK: --mode headless -> SDL_VIDEODRIVER/AUDIODRIVER=dummy, MCP_PORT set")

    vis = dryrun_env("visible")
    if vis.get("SDL_VIDEODRIVER") != "<unset>" or vis.get("SDL_AUDIODRIVER") != "<unset>":
        fail("visible mode left the SDL drivers set: %r" % vis)
    if vis.get("MCP_PORT") != "5099":
        fail("visible mode did not set MCP_PORT: %r" % vis)
    print("OK: --mode visible -> SDL drivers unset, MCP_PORT set (same binary)")


# -- phase 2: launch known target + quit while running ----------------------

def read_text_screen(client):
    r = expect_result(client.call("read_screen", rid(), {}), "read_screen")
    if not r.get("is_text"):
        return None
    return "\n".join(r.get("text", []))


def boot_to_prompt(client, timeout=45.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        if ping_state(client, rid()) != "running":
            time.sleep(0.2)
            continue
        time.sleep(0.4)
        txt = read_text_screen(client)
        if txt is None:
            last = "(graphics splash)"
            continue
        last = txt
        if "Z:\\" in txt:
            return True
    sys.stderr.write("last screen:\n%s\n" % last)
    return False


def phase_launch_and_quit_running(port):
    with Launcher(port, mode="headless") as L:
        client = connect(L)
        if ping_state(client, rid()) != "running":
            fail("fresh launch should be running (no -break-start)")
        if not boot_to_prompt(client):
            sys.stderr.write(L.output())
            fail("launcher did not boot the guest to the Z:\\ prompt")
        print("OK: launcher booted a known target to the Z:\\ shell prompt (headless)")

        r = expect_result(client.call("quit", rid()), "quit")
        if r.get("op") != "quit" or r.get("ok") is not True:
            fail("quit: unexpected ack %r" % r)
        print("OK: quit (running) -> deferred ack")

        if not L.wait_exit(timeout=20.0):
            sys.stderr.write(L.output())
            fail("process did not exit on its own after quit")
        print("OK: emulator process exited on its own after quit (no SIGTERM)")
        client.close()


# -- phase 3: reset (parked) + quit (parked) --------------------------------

def regs(client):
    r = expect_result(client.call("read_registers", rid()), "read_registers")
    return int(r["cs"], 16), int(r["eip"], 16)


def wait_parked_at(client, cs0, eip0, timeout=40.0):
    """Poll until the guest is parked AND back at the reset vector cs0:eip0."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ping_state(client, rid()) == "parked":
            resp = client.call("read_registers", rid())
            if "result" in resp:
                cs = int(resp["result"]["cs"], 16)
                eip = int(resp["result"]["eip"], 16)
                if (cs, eip) == (cs0, eip0):
                    return True
        time.sleep(0.2)
    return False


def phase_reset_and_quit_parked(port):
    with Launcher(port, mode="headless", break_start=True) as L:
        client = connect(L)
        deadline = time.time() + 30.0
        while time.time() < deadline and ping_state(client, rid()) != "parked":
            time.sleep(0.2)
        if ping_state(client, rid()) != "parked":
            sys.stderr.write(L.output())
            fail("guest never parked under -break-start")

        cs0, eip0 = regs(client)
        print("OK: parked at reset vector %04x:%08x" % (cs0, eip0))

        # Step EIP away from the reset vector so the reboot is observable.
        for _ in range(8):
            client.call("step", rid())
        cs1, eip1 = regs(client)
        if (cs1, eip1) == (cs0, eip0):
            fail("stepping did not advance CS:EIP (%04x:%08x)" % (cs1, eip1))
        print("OK: stepped to %04x:%08x (away from the reset vector)" % (cs1, eip1))

        # reset while parked: deferred ack, then the machine reboots.
        r = expect_result(client.call("reset", rid()), "reset")
        if r.get("op") != "reset" or r.get("ok") is not True:
            fail("reset: unexpected ack %r" % r)
        print("OK: reset (parked) -> deferred ack")

        if not wait_parked_at(client, cs0, eip0):
            sys.stderr.write(L.output())
            fail("machine did not reboot back to the reset vector after reset")
        print("OK: machine rebooted to the reset vector (re-parked under -break-start)")

        # quit from the parked state.
        r = expect_result(client.call("quit", rid()), "quit")
        if r.get("op") != "quit" or r.get("ok") is not True:
            fail("quit (parked): unexpected ack %r" % r)
        if not L.wait_exit(timeout=20.0):
            sys.stderr.write(L.output())
            fail("process did not exit after quit while parked")
        print("OK: quit (parked) -> process exited")
        client.close()


def main():
    if not os.access(LAUNCHER, os.X_OK):
        fail("launcher not executable: %s" % LAUNCHER)

    phase_launcher_env()
    phase_launch_and_quit_running(free_port())
    phase_reset_and_quit_parked(free_port())

    print("PASS: Slice 12 lifecycle (launcher + reset/quit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
