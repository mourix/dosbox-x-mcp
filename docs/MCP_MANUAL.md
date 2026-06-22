# DOSBox-X MCP — LLM User Manual

This manual is for an LLM (Claude Code or any MCP client) driving DOSBox-X through the
MCP server to **reverse-engineer DOS applications**. It describes *workflows*. The
authoritative reference for tool names and parameters is the MCP tool definitions
themselves (the single source of truth) — this manual does not duplicate schemas.

> Status: **stub.** The MCP server does not exist yet. Fill in each section as tools
> land. Keep workflows here in sync with the implemented tools; update on every feature.

## Build flag

The MCP server is compiled in only when DOSBox-X is configured with `--enable-mcp`
(Linux only). It **requires** the built-in debugger, so `--enable-debug` (or
`--enable-debug=heavy`) must also be passed — `--enable-mcp` on its own is a configure
error. The flag defines `C_MCP`; with the flag off, none of the MCP code is built
(isolation). The canonical way to build + verify is `scripts/mcp-check.sh`, which builds
both with and without the flag and runs the test suite headless.

## Headless launch & integration harness

MCP runs the emulator **headless** by exporting `SDL_VIDEODRIVER=dummy` +
`SDL_AUDIODRIVER=dummy` (no window, no display/tty). `scripts/mcp_harness.py` is the
stdlib-only Python launcher that does this and owns the process lifecycle with bounded
timeouts; per-slice integration scripts build on it and `scripts/mcp-check.sh` runs them.

Slice 1's integration test (`scripts/mcp_slice1_screenshot.py`) verifies that the
capture path produces a PNG under the dummy video driver. It uses a temporary
self-test hook gated behind the `MCP_SELFTEST_SCREENSHOT` env var (with optional
`MCP_SELFTEST_FRAMES`): when set, the emulator requests one screenshot after the guest
boots. This is scaffolding to de-risk the screen pillar — the real screen tools
(`screen_hash`, `read_screen`, `take_screenshot`) arrive in Slices 9–10.

## Mental model

- DOSBox-X emulates a full PC. The MCP server exposes the **built-in debugger**
  (`src/debug/`): registers, memory, disassembly, breakpoints, stepping.
- You drive a *running emulator*. State is live — reads reflect the current CPU/memory.
- Responses are **bounded** (paginated). For large memory/disasm ranges, request a window
  and page through it rather than asking for everything at once.

## Core workflows (to be filled in as tools are added)

### Attach / inspect state
_TODO: how to connect, confirm the emulator is running, read CPU + segment registers._

### Read memory / disassemble
_TODO: read a memory window (seg:off or linear/physical), disassemble at an address,
pagination conventions._

### Breakpoints and stepping
_TODO: set/clear breakpoints, run-to-breakpoint, single-step, step-over, read state at
the stop._

### Typical reverse-engineering loop
_TODO: load target → break at entry → step → inspect → map behavior → record findings._

## Conventions

- Prefer the smallest query that answers the question; page through large ranges.
- Addresses: state which space you mean (segment:offset, linear, or physical).
