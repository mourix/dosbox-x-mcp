# DOSBox-X MCP — LLM User Manual

This manual is for an LLM (Claude Code or any MCP client) driving DOSBox-X through the
MCP server to **reverse-engineer DOS applications**. It describes *workflows*. The
authoritative reference for tool names and parameters is the MCP tool definitions
themselves (the single source of truth) — this manual does not duplicate schemas.

> Status: **early.** The transport is live (Slice 2): a TCP JSON-RPC server with
> `ping` / `server_info`. State-touching tools landed so far: `read_registers` (Slice 3),
> `read_memory` + `disassemble` (Slice 4). Remaining tools land in later slices. Keep
> workflows here in sync with the implemented tools; update on every feature.

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

## Connecting to the server (transport)

The MCP server is a **TCP JSON-RPC** endpoint: newline-delimited JSON-RPC 2.0, one
request per line, one response line per request. It is **opt-in**: the server starts
only when the `MCP_PORT` env var names a nonzero port; with no `MCP_PORT`, no socket is
opened. It binds **`127.0.0.1` only** (never `0.0.0.0`) and accepts a **single client**
at a time — a second concurrent connection is refused with a busy error and closed.

Each request targets one of two execution states (see "Execution states" below). The
dispatcher runs on the emulator thread, so requests for the wrong state are **fast-
rejected** (error `-32001`, with `data.state` carrying the current state) rather than
blocked, and every queued request has a **5 s timeout** (error `-32003`). Responses are
size-bounded (64 KiB ceiling; error `-32004` if exceeded).

Probe the connection with the two always-available tools:

- **`ping`** → `{pong: true, state: "running"|"parked"}` — liveness + current state.
- **`server_info`** → version, transport, `bind` (always `127.0.0.1`), port,
  `single_client`, current `state`, and `max_payload`.

`scripts/mcp_slice2_ping.py` is the Slice 2 integration test: it boots headless with
`MCP_PORT` set and exercises `ping`, `server_info`, a mode-mismatch fast-reject, and the
single-client refusal.

## Execution states

The emulator is either **running** (CPU free-running; the game executes) or **parked**
(stopped in the debugger). The two are mutually exclusive in time. Every tool is tagged:

- **run** — serviced while running (input, screen, `break`).
- **parked** — serviced while parked (registers, memory, disassembly, stepping, writes,
  breakpoints, scan).
- **any** — serviced in either state (`ping`, `server_info`).

If you send a parked-class tool while running (or vice-versa) you get the `-32001`
mismatch error telling you the current state; switch with `break` / `continue` first.
(Implemented so far: the `any`-class `ping` / `server_info`, and the parked-class
`read_registers`, `read_memory`, `disassemble`. The remaining tools are already classified
so mismatches are reported correctly, but their handlers land in later slices.)

## Mental model

- DOSBox-X emulates a full PC. The MCP server exposes the **built-in debugger**
  (`src/debug/`): registers, memory, disassembly, breakpoints, stepping.
- You drive a *running emulator*. State is live — reads reflect the current CPU/memory.
- Responses are **bounded** (paginated). For large memory/disasm ranges, request a window
  and page through it rather than asking for everything at once.

## Core workflows (to be filled in as tools are added)

### Attach / inspect state
Connect over TCP (the `MCP_PORT` the launcher set), then `ping` to confirm liveness and
learn the current state. To inspect CPU state you must be **parked**: launch with
`-break-start` (parks at machine reset) or `break` into a running guest, then call
`read_registers`. It returns the general-purpose registers (`eax`…`eip`) and segment
registers (`cs`…`ss`) as fixed-width hex, the full `eflags` word plus a decoded `flags`
object (`CF`,`PF`,`AF`,`ZF`,`SF`,`TF`,`IF`,`DF`,`OF`,`IOPL`), the CPU `mode`
(`real`/`pr16`/`pr32`/`vm86`) and `cpl`. Calling it while the guest is free-running
returns the `-32001` mismatch error — `break` first.

### Read memory / disassemble
Both are **parked**-class — `break` (or launch `-break-start`) first. Read in the address
space you mean:

- **`read_memory`** takes a `space` (`segmented` (default) | `virtual` | `physical`) and
  the matching address (`seg`+`off`, `lin`, or `phys` — addresses accept either JSON
  integers or hex strings like `"0xb8000"`) plus an optional `len` (default **256**, max
  **4096** bytes per call). It returns the resolved `addr` (when `addr_valid`), the byte
  count `len`, a lowercase `hex` dump (an unreadable/page-faulting byte renders as `??`,
  counted in `unreadable`), and — when the request exceeds the per-call cap — `truncated:
  true` plus a `next_off`/`next_lin`/`next_phys` to continue paging. The three spaces
  agree: a `virtual` read at the `addr` a `segmented` read resolved returns the same bytes.
  A protected-mode selector that does not resolve comes back `addr_valid: false` with an
  empty dump.
- **`disassemble`** takes `seg`+`off` (the starting CS:EIP) and an optional `count`
  (default **16**, max **128** instructions) and an optional `big` (force 16/32-bit decode;
  defaults to the current code-segment size). It returns an `insns` array — each with
  `off`, resolved `addr`, raw `bytes`, and `text` — plus `truncated` when the count was
  capped. Instruction offsets advance by their byte length, so page forward by re-issuing
  from the last `off` + its byte count.

Pagination convention: ask for a window, then continue from the returned `next_*` (memory)
or the last instruction's offset (disasm) rather than requesting a huge range at once.

### Breakpoints and stepping
_TODO: set/clear breakpoints, run-to-breakpoint, single-step, step-over, read state at
the stop._

### Typical reverse-engineering loop
_TODO: load target → break at entry → step → inspect → map behavior → record findings._

## Conventions

- Prefer the smallest query that answers the question; page through large ranges.
- Addresses: state which space you mean (segment:offset, linear, or physical).
