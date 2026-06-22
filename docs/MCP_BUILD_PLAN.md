# DOSBox-X MCP — Slice-by-Slice Build Plan

Concrete, incremental plan for building the MCP server defined in `CLAUDE.md`
("Goal of this fork" + "Guardrails"). Each slice is independently shippable and is
**not done until `scripts/mcp-check.sh` is green** (build with and without `--enable-mcp`,
unit tests, integration tests, baseline suite). Schemas live in the MCP tool definitions;
workflows live in `docs/MCP_MANUAL.md`.

## Architecture

One `dosbox-x` process, built `--enable-mcp` (implies `C_DEBUG`), launched by a repeatable
launcher script in one of two **launch modes** (a runtime choice, not a build difference —
the same binary):

- **headless** (default, used by `scripts/mcp-check.sh` and CI): `SDL_VIDEODRIVER=dummy` +
  `SDL_AUDIODRIVER=dummy` (not `-silent`, which also exits). No window, no display/tty needed,
  fully scriptable.
- **visible** (interactive RE sessions): the dummy drivers are **not** set, so a real SDL
  window opens and you can watch the guest. Under WSL2 this renders via WSLg. The MCP server,
  tools, and dispatcher are identical to headless — the screen tools (`read_screen`,
  `screen_hash`, `take_screenshot`) and input injection work the same way regardless of mode.

Inside the process:

- a **TCP JSON-RPC server thread** that only enqueues requests and waits for replies;
- an **emulator-thread dispatcher** that drains the queue at **two service points** so all
  state access stays single-threaded on the emulator thread:
  - `GFX_Events()` (every frame, `sdlmain.cpp:5893`) — input + screen, while the game runs;
  - the debugger loop (`DEBUG_Loop`/`DEBUG_CheckKeys`) — debug ops, while the CPU is parked.

All emulator primitives are reused, not reimplemented (see the memory notes / `src/debug/`).

**Platform scope:** MCP rides the in-tree SDL1 + ncurses debugger on **Linux only**. The
Windows VS solution and other build variants do not carry it; `scripts/mcp-check.sh` builds
the Linux `--enable-debug --enable-mcp` config (and a no-flag build to prove isolation).

## Protocol semantics

The two service points are **mutually exclusive in time**: while the CPU free-runs,
`GFX_Events()` ticks but `DEBUG_Loop` is swapped out; while parked, the reverse. A request
therefore targets one of two execution states, and the dispatcher must know which.

- **Request classes.** Each tool is tagged `run` (serviced at `GFX_Events`: input, screen,
  `break`), `parked` (serviced in the debugger loop: registers, memory, disasm, step,
  breakpoints, writes, scan), or `any` (`ping`, `server_info`, `quit`).
- **Mode mismatch = fast reject, never block.** A `parked` request arriving while free-running
  (or a `run` request while parked) is answered immediately with an error carrying the current
  execution state (`{state: "running"|"parked"}`) — it does **not** sit on the queue waiting for
  a mode flip that may never come. The client decides whether to `break`/`continue` first.
- **Timeout.** Every queued request has a bounded wait (default **5 s**); on expiry the TCP
  thread returns a timeout error and the dispatcher drops the dead request. Tested.
- **Transport.** Newline-delimited JSON-RPC, **single client** (second connection refused),
  bound to **`127.0.0.1` only** — never `0.0.0.0`. The bind address is an asserted invariant.
- **Handler discipline.** Handlers run on the emulator thread; they must be non-blocking and
  CPU-bounded (no sleeps, no unbounded loops) so draining the queue never stalls emulation.

## Response bounds

The bounding helper enforces these; tests assert against them, not ad-hoc per-slice constants:

- **Max response payload: 64 KiB** (hard ceiling for any single reply).
- **read_memory:** default page **256 B**, max **4096 B** per call; larger ranges paginate.
- **disassemble:** default **16** instructions, max **128** per call.
- **read_screen:** full text grid (≤ 80×50 = 4000 cells) is within budget; bounded by the 64 KiB ceiling.
- **breakpoint / scan list:** max **256** entries per page.
- **debugger_command passthrough:** captured output truncated to **16 KiB**.

## No ncurses TUI in MCP mode (either launch mode)

This is keyed on `--enable-mcp`, not the launch mode: the debugger's ncurses TUI is skipped in
**both** headless and visible launches (visible only adds the SDL *guest* window; the MCP queue,
not the TUI, is always the debugger input source). In `--enable-mcp` mode, **do not initialize
ncurses** — skip `DEBUG_SetupConsole()`
(`debug.cpp:5790`, the sole `initscr()` path via `DBGUI_StartUp`) so `dbg.win_main` stays `NULL`. Every
draw routine already null-guards on `dbg.win_main` (`DrawData` `debug.cpp:1005`, `DrawRegisters` `:1146`,
`DrawCode` `:1303`, `DrawInput`/`DrawVariables` `debug_gui.cpp:358/427`), so they become no-ops; the debug
loop still polls, breakpoints still fire, all state stays readable. Guard the one curses call that isn't
already null-checked — the `getch()` in `DEBUG_CheckKeys` (`debug.cpp:4430`) — so the MCP queue is the input
source instead. This avoids any `initscr` failure under no-tty CI.

`DEBUG_Loop` (`debug.cpp:4870`) is already a 1 ms poll loop (`SDL_Delay(1)`) that calls `GFX_Events()` and a
non-blocking `getch()`, so the queue drain slots in without further loop changes. `GFX_Events()` runs in
**both** the normal loop and `DEBUG_Loop` (`debug.cpp:4884`), so a single drain there services `run`-class
requests (notably screen reads) in either state; `parked`-class requests drain in the debug loop.

## Core-edit manifest (the full isolation blast radius — keep this list complete)

Every touch to core emulation/debugger code, in one place, so guardrail 1 is auditable at a
glance. Adding a core edit means adding a line here with its justification.

| Edit | File | Why unavoidable |
| --- | --- | --- |
| `--enable-mcp` flag → `C_DEBUG` | `configure.ac`, `src/Makefile.am` | build wiring (Slice 0) |
| Per-frame service call `MCP_GFXFrameService()` | `GFX_Events()`, `sdlmain.cpp:5893` | single `run`-class core call site; introduced in Slice 1 (env-gated screenshot self-test), extended in Slice 2 to drain the run-class queue. Runs in both normal and debug loops. |
| Dispatcher drain (parked-class) | `DEBUG_Loop`, `debug.cpp:4870` | service `parked`-class requests in the existing poll loop (Slice 2) |
| Skip ncurses init when headless | `DEBUG_SetupConsole`, `debug.cpp:5790` | leave `dbg.win_main==NULL` so the TUI is a no-op and `initscr` can't fail under no-tty CI (Slice 2) |
| Guard the parked `getch()` | `DEBUG_CheckKeys`, `debug.cpp:4430` | the one curses call not already null-guarded; skip input scraping when headless (Slice 2) |

## Test strategy (two layers)

- **Unit (fast, in-binary gtest):** request parsing, JSON formatting, response bounding /
  pagination. New `tests/mcp_*_tests.cpp`, registered in `tests/tests.h` under `#if C_MCP`,
  run via `./src/dosbox-x -tests --gtest_filter=Mcp.*`. No emulator boot.
- **Integration (scripted, out-of-process):** a **Python 3 (stdlib-only)** client script boots
  `dosbox-x` headless with a tiny known target, drives it over TCP (newline-delimited JSON-RPC),
  asserts, and shuts it down. Reused by every state-touching slice. `scripts/mcp-check.sh` runs
  both layers and is the only thing `mcp-check.sh` requires beyond the build toolchain.

## Guardrail gate (applies to every slice)

Isolated (own dir + flag, core edits listed in the manifest) · `scripts/mcp-check.sh` green ·
responses bounded to the numbers above (asserted) · localhost-only bind (asserted) · steps
scripted · tests matched to layer · manual updated.

---

## Slice 0 — Scaffolding & isolation proof
**Goal:** prove the build pipeline and one-command verification end to end, near-zero risk.
**Changes:** `--enable-mcp` in `configure.ac` (`AC_ARG_ENABLE` → `AC_DEFINE(C_MCP)`, require/imply
`C_DEBUG`); new `src/mcp/` with a stub module + `src/mcp/Makefile.am` wiring (and `src/Makefile.am`);
`scripts/mcp-check.sh` (autogen → build `--enable-debug --enable-mcp` → unit + baseline tests → assert
green; also a build **without** the flag to prove isolation); `tests/mcp_smoke_tests.cpp` (trivial
assert) added to `tests.h` under `#if C_MCP`; manual note on the build flag.
**Tests:** unit smoke test passes; builds succeed with and without `--enable-mcp`; baseline suite green.
**DoD:** `scripts/mcp-check.sh` green; core untouched except the flag + Makefile wiring.

## Slice 1 — Headless boot harness + screenshot verification (Integration Test #1)
**Goal:** establish the scripted headless-boot integration harness and, as its **first test**,
verify the riskiest assumption: that the RENDER/`CAPTURE_AddImage` path produces a frame under the
**dummy video driver** (`render.cpp:442–506`). De-risks the screen pillar before any screen tooling.
**Changes:** `scripts/` integration harness that launches `dosbox-x` headless with a minimal target and
a capture dir; trigger a screenshot (resolve the cheapest trigger: mapper autotype file, config, or a
minimal `C_MCP`-guarded hook) and assert a non-empty PNG is written. No MCP transport required yet.
**Tests:** Integration Test #1 — headless screenshot produces a valid non-empty PNG.
**DoD:** harness is reusable by later slices; `mcp-check.sh` runs it and it passes. If the dummy driver
does **not** capture, record the fallback (e.g. `SDL_VIDEODRIVER` alternative or offscreen surface)
before proceeding to Slice 10.

**De-risk outcome (resolved):** ✅ The in-tree SDL1 `dummy` video driver **does** render frames
under `SDL_VIDEODRIVER=dummy` — `RENDER_EndUpdate` reaches `CAPTURE_AddImage` and a valid PNG is
written headless. **No fallback needed**; Slice 10 can rely on the dummy driver. Verified by
`scripts/mcp_slice1_screenshot.py` (run from `scripts/mcp-check.sh`). Harness: `scripts/mcp_harness.py`
(reusable). Trigger: temporary `MCP_GFXFrameService()` self-test gated by `MCP_SELFTEST_SCREENSHOT`.

## Slice 2 — TCP JSON-RPC server + dispatcher + `ping`
**Goal:** first over-the-wire round trip; the queue/dispatcher skeleton.
**Changes:** TCP server thread (**`127.0.0.1` only**, newline-delimited JSON-RPC, single client);
request queue + condition var carrying the `run`/`parked`/`any` class tag and the **mode-mismatch fast
reject** + **5 s timeout** from "Protocol semantics"; dispatcher draining at `GFX_Events()` and the
debugger loop; JSON encode/decode + the **bounded-response/pagination helper** (pure, unit-tested, enforces
the "Response bounds" numbers); tools `ping` / `server_info`. **Core edits (see manifest):** the two
dispatcher drains; skip `DEBUG_SetupConsole()` so ncurses isn't initialized headless; guard the parked
`getch()` (`debug.cpp:4430`) for `win_main==NULL`. The drain slots into the existing poll loop (see
"Headless: no ncurses TUI in MCP mode").
**Tests:** unit — JSON + bounding helper (assert size ≤ 64 KiB); mode-mismatch returns current state;
queue timeout fires. Integration — connect, `ping`, assert reply; assert the server refuses a non-localhost
bind and a second client.
**DoD:** round trip works headless; bounds + localhost bind + timeout asserted; `mcp-check.sh` green.

## Slice 3 — `read_registers` (first emulator-state tool)
**Goal:** read CPU/segment/flags state at a stop.
**Changes:** `MCP_SnapshotRegisters()` (reads `reg_eax`/`SegValue`/`GETFLAG`/`cpu.*` globals) +
`MCP_FormatRegisters()` (compact, bounded JSON); served on the emulator thread while parked.
**Tests:** unit — known snapshot → exact JSON + size bound. Integration — boot with `-break-start`
(`sdlmain.cpp:10148`), read registers, assert known reset/entry values.
**DoD:** `mcp-check.sh` green.

## Slice 4 — `read_memory` (paginated) + `disassemble` (bounded)
**Goal:** inspect memory and code.
**Changes:** `read_memory(space, seg/lin/phys, off, len)` via `GetAddress` + `mem_readb_checked`,
honoring the three address spaces (`DATV_SEGMENTED/VIRTUAL/PHYSICAL`), **paginated** with a hard byte cap;
`disassemble(addr, count)` looping `DasmI386` with a hard instruction cap.
**Tests:** unit — hex/pagination formatting, cap enforcement. Integration — read known bytes / disasm a
known opcode sequence.
**DoD:** outputs bounded and asserted; `mcp-check.sh` green.

## Slice 5 — Execution control: `step` / `step_over` / `continue` / `break`
**Goal:** drive execution.
**Changes:** `step`=`DEBUG_Run(1,true)`; `step_over`=`StepOver()`+`DEBUG_Run(1,false)`;
`continue`=`debugging=false`+`DEBUG_Run(1,false)`+`DOSBOX_SetNormalLoop()`; `break`=`DEBUG_Enable...`.
Replicate `inhibit_int_breakpoint` guarding. Propagate the `int32_t` return of `DEBUG_Run`
(instructions run / status) into the stop report rather than re-deriving it. `continue` returns when the
next breakpoint re-enters `DEBUG_Loop`; report the stop (seg:off). `break` is a `run`-class tool serviced
at `GFX_Events` (sets `debugging=true`, swaps in `DEBUG_Loop`).
**Tests:** integration — step N, assert EIP advances; continue to a planted breakpoint, assert stop addr.
**DoD:** `mcp-check.sh` green.

## Slice 6 — Breakpoints: `list` / `add` / `delete`
**Goal:** breakpoint management (exec + interrupt + memory-watch).
**Changes:** wrap `CBreakpoint::AddBreakpoint` / `AddIntBreakpoint` / `AddMemBreakpoint` / `DeleteByIndex`
/ `ShowList`; expose the 6 `EBreakpoint` types; `list` is bounded.
**Tests:** integration — add exec bp, continue, assert stop; add INT bp; delete by index; list reflects state.
**DoD:** `mcp-check.sh` green.

## Slice 7 — Writes: `write_register` / `write_memory`
**Goal:** mutate state.
**Changes:** `write_register` delegates to `ChangeRegister` (inherits longest-name-first ordering);
`write_memory` follows the `SM` model — width-tagged values via `mem_write{b,w,d}_checked(GetAddress(...))`.
**Tests:** integration — write then read back register and memory.
**DoD:** `mcp-check.sh` green.

## Slice 8 — Input injection: `send_keys` / `type_text` / `mouse`
**Goal:** drive the guest while it runs (serviced at the frame tick).
**Changes:** `send_keys` = `KEYBOARD_AddKey(KBD_KEYS, press/release)` pairs; `type_text` =
`MAPPER_AutoType(sequence, wait_ms, pace_ms, choice)`; mouse via `GFX_EventsMouseProcess`.
**Tests:** integration — type a string at the DOS prompt, then read it back from the text screen
(closes the input↔screen loop; depends on Slice 9 read).
**DoD:** `mcp-check.sh` green.

## Slice 9 — Screen: `screen_hash` / `read_screen` (text)
**Goal:** token-cheap screen state with change detection.
**Changes:** `read_screen` returns the text grid via `ReadCharAttr` when `CurMode->type==M_TEXT`;
`screen_hash` returns a cheap fingerprint — hash the text buffer in text modes, hash `render.src`
framebuffer bytes in graphics modes (change-detection even headless / graphics). Both bounded.
**Tests:** integration — assert known prompt text; assert hash changes after a screen-changing action and
is stable otherwise.
**DoD:** `mcp-check.sh` green.

## Slice 10 — `take_screenshot` (graphics, on-demand)
**Goal:** full-fidelity graphics capture when asked.
**Changes:** promote the Slice 1 verification into a tool: trigger capture, return a **PNG path +
metadata** (never raw pixels).
**Tests:** integration — switch to a graphics mode, screenshot, assert valid non-empty PNG + correct dims.
**DoD:** `mcp-check.sh` green.

## Slice 11 — Memory scanner: `scan_start` / `scan_filter` / `scan_results`
**Goal:** "cheat-engine" progressive search for variables.
**Changes:** wrap MEMFIND/MEMS (single global `MEMFINDInstance`): start snapshots a range; filter applies
`= ! > < >= <=`; results are **bounded/paginated**. Treat the instance as session-global state.
**Tests:** integration — start a scan, change a known value, filter `=`, assert the address narrows to it.
**DoD:** `mcp-check.sh` green.

## Slice 12 — Lifecycle: launcher + `reset` / `quit`
**Goal:** repeatable game launch + in-session lifecycle.
**Changes:** finalize the launcher script (mount + config via `-set`/`-c` or `[autoexec]`,
`--enable-debug`, optional `-break-start`, MCP server listening). The launcher takes a **launch
mode** flag (default `headless`, opt-in `visible`): `headless` exports `SDL_VIDEODRIVER=dummy` +
`SDL_AUDIODRIVER=dummy`; `visible` leaves them unset so a real SDL window opens (WSLg under WSL2).
Both modes share the same binary, MCP server, and target/config args — mode only toggles the SDL
driver env. In-session `reset` / `quit` tools.
**Tests:** integration — launch a known target by name in **headless** mode and confirm it reaches
a known state (CI path). Visible mode is the same code path with the driver env unset, so it needs
no separate CI assertion (CI has no display); a scripted smoke check that the launcher accepts the
flag and sets/unsets the env vars is enough.
**DoD:** `mcp-check.sh` green; visible mode documented in `docs/MCP_MANUAL.md`.

## Slice 13 — Manual finalize + optional command passthrough
**Goal:** living manual complete; optional escape hatch.
**Changes:** fill all `docs/MCP_MANUAL.md` workflows (attach → break → inspect → input → screen → scan);
optional **bounded** `debugger_command` passthrough that runs any of the ~110 `ParseCommand` commands and
captures+truncates `DEBUG_ShowMsg` output.
**Tests:** unit — passthrough output truncation. Docs reviewed against implemented tools.
**DoD:** `mcp-check.sh` green; manual matches the tool set.

---

### Dependency notes
- Slices 0→2 are pure infrastructure. 3+ require the harness from Slice 1.
- Slice 8's round-trip test depends on Slice 9's `read_screen`.
- Slice 10 depends on the Slice 1 verification outcome.
- Reorder only within these constraints.
