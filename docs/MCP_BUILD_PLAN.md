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
- **scan_start range:** scanned range clamped to **1 MiB** per scan (bounds the snapshot + per-filter work).
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
| Per-frame service call `MCP_GFXFrameService()` | `GFX_Events()`, `sdlmain.cpp:5895` | the **single** core call site for the dispatcher. Introduced in Slice 1 (env-gated screenshot self-test), extended in Slice 2 to start the TCP server and drain the request queue against the current execution state. `GFX_Events()` runs in **both** the normal loop and `DEBUG_Loop` (`debug.cpp:4884`), so this one drain services **both** `run`-class (running) and `parked`-class (parked) requests — no separate `DEBUG_Loop` edit is needed (it was planned but proved redundant, keeping the blast radius smaller). The current state, not the call site, decides mode-mismatch fast-reject. |
| Skip ncurses init in MCP builds | `DEBUG_SetupConsole`, `debug.cpp:5790` | early `return` under `#if C_MCP` leaves `dbg.win_main==NULL`, so the TUI is a no-op (every draw routine null-guards) and `initscr` can't fail under no-tty CI (Slice 2). Keyed on `--enable-mcp`, not launch mode. |
| Guard the parked `getch()` | `DEBUG_CheckKeys`, `debug.cpp:4430` | `if (dbg.win_main==NULL) return 0;` under `#if C_MCP` — the one curses call not already null-guarded; the MCP queue is the input source instead (Slice 2). |
| Skip the no-tty guard in `DEBUG_Enable_Handler` | `debug.cpp:5015` | The Linux/macOS branch returns early (debugger "not available") when stdin/stdout/stderr aren't a terminal, so `-break-start` would never park under the headless harness (pipes, no tty). Compiled out under `#if ... && !C_MCP`: the MCP build has no ncurses TUI and its input source is the request queue, so the debugger must engage headless (Slice 3). |
| Execution-control entry point `MCP_DebugExec()` | `debug.cpp` (after `DEBUG_Run`) | one `#if C_MCP` function the MCP module calls to drive step/step_over/continue/break (Slice 5). It must live in `debug.cpp` because it manipulates debugger statics that are not exported — `debugging`, `exitLoop`, `mustCompleteInstruction`, and the static `StepOver()` — and it reuses the exact primitives the F11/F10/F5 key handlers and `-break-start` use (`DEBUG_Run`, `DEBUG_EnableDebugger`). Compiled away entirely without `--enable-mcp`; no behavior change to the existing handlers. |
| Breakpoint bridge `MCP_Breakpoint{Add,Delete,Count,Get}()` + two `CBreakpoint` friend decls | `debug.cpp` (after `MCP_DebugExec`; friends near `BPoints`) | `#if C_MCP` free functions the MCP module calls for breakpoint_list/add/delete (Slice 6). They must live in `debug.cpp` because the breakpoint store (`CBreakpoint` and its private static `BPoints`) is not exported. Add/Delete reuse the public `CBreakpoint::Add*`/`DeleteByIndex`/`DeleteAll` API the BP/BPINT/BPM/BPDEL commands use; the two list-readers need `BPoints`, so they are `friend`s of `CBreakpoint` (the same pattern as the existing `DEBUG_HeavyIsBreakpoint` friend). Compiled away entirely without `--enable-mcp`; no behavior change to the existing handlers. |
| Memory-scanner bridge `MCP_Scan{Start,Filter,State,Results,Cancel}()` | `debug.cpp` (after `MCP_BreakpointGet`) | `#if C_MCP` free functions the MCP module calls for scan_start/scan_filter/scan_results (Slice 11). They must live in `debug.cpp` because the scanner store — the single global `MEMFINDInstance` and its file-private `MEMFinder` struct — is not exported. They mirror the existing MEMFIND-start / MEMS-filter / MEMFIND-L-list algorithms exactly, exposing structured out-params instead of `DEBUG_ShowMsg` text. Two programmatic-client conveniences vs the interactive commands: Start replaces an in-progress scan (MEMFIND rejects it), and Filter does not auto-delete the instance at 0 matches (MEMS does) so Results stays callable. The existing MEMFIND/MEMS `ParseCommand` handlers are **unchanged**. Compiled away entirely without `--enable-mcp`; no behavior change to the existing handlers. |

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

**Outcome (resolved):** ✅ Shipped. New `src/mcp/` files: `mcp_json.{h,cpp}` (minimal stdlib JSON),
`mcp_protocol.{h,cpp}` (classification + bounds + pure `dispatch`, with `ping`/`server_info`),
`mcp_server.{h,cpp}` (poll-based single-client TCP server on `127.0.0.1`, request queue, 5 s timeout,
emulator-thread `drain`). Server is **opt-in via the `MCP_PORT` env var** (normal interactive runs open
no socket); the Slice 12 launcher will set it. Core edits landed exactly as in the manifest above (the
planned separate `DEBUG_Loop` drain proved redundant). Unit tests: `tests/mcp_protocol_tests.cpp`
(`Mcp.*`). Integration: `scripts/mcp_slice2_ping.py` (ping, server_info localhost bind, mode-mismatch
fast-reject, second-client refusal), wired into `scripts/mcp-check.sh`. The localhost-bind invariant is
asserted both as the `MCP_BIND_ADDRESS`/`server_info` constant (unit) and by the over-the-wire test. The
queue timeout is the `kRequestTimeout` (5 s) path in `submit_and_wait`.

## Slice 3 — `read_registers` (first emulator-state tool)
**Goal:** read CPU/segment/flags state at a stop.
**Changes:** `MCP_SnapshotRegisters()` (reads `reg_eax`/`SegValue`/`GETFLAG`/`cpu.*` globals) +
`MCP_FormatRegisters()` (compact, bounded JSON); served on the emulator thread while parked.
**Tests:** unit — known snapshot → exact JSON + size bound. Integration — boot with `-break-start`
(`sdlmain.cpp:10148`), read registers, assert known reset/entry values.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. The reader/formatter split keeps the protocol layer pure:
`mcp::snapshot_registers` (new `src/mcp/mcp_registers.cpp`, the emulator-state bridge) only *reads*
`cpu_regs`/`Segs`/`cpu` via `include/regs.h`+`include/cpu.h` — the same globals/macros the debugger's
`DrawRegisters` uses (`debug.cpp:1146`) — into a plain `RegisterSnapshot`; `mcp::format_registers`
(pure, in `mcp_protocol.cpp`) encodes it as compact bounded JSON (fixed-width lowercase hex, individual
flag bits, `mode`∈{real,pr16,pr32,vm86}, `cpl`). The `read_registers` handler in `dispatch` is reached
only when `STATE_PARKED` (parked-class mode check), so the snapshot is coherent. **No core edits** — the
bridge reuses existing headers/globals, so the core-edit manifest is unchanged. Unit tests: exact known-
snapshot JSON + mode/IOPL derivation + payload bound (`tests/mcp_protocol_tests.cpp`, `Mcp.FormatRegisters*`).
Integration: `scripts/mcp_slice3_registers.py` boots headless with `-break-start`, polls `ping` until
`state=parked`, then asserts `read_registers` shape (hex widths, full flag set, real mode + cpl 0 at reset)
and the payload bound; wired into `scripts/mcp-check.sh` (integration test #3).

## Slice 4 — `read_memory` (paginated) + `disassemble` (bounded)
**Goal:** inspect memory and code.
**Changes:** `read_memory(space, seg/lin/phys, off, len)` via `GetAddress` + `mem_readb_checked`,
honoring the three address spaces (`DATV_SEGMENTED/VIRTUAL/PHYSICAL`), **paginated** with a hard byte cap;
`disassemble(addr, count)` looping `DasmI386` with a hard instruction cap.
**Tests:** unit — hex/pagination formatting, cap enforcement. Integration — read known bytes / disasm a
known opcode sequence.
**DoD:** outputs bounded and asserted; `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as Slice 3: the new
emulator-state bridge `src/mcp/mcp_memory.cpp` only *reads* — `read_memory` resolves the three
address spaces (segmented via per-byte `GetAddress`, virtual = linear directly, physical via
`physdev_readb`) and reads through `mem_readb_checked` (false == success); `disassemble` loops
`DasmI386` from `GetAddress(seg,eip)`, capturing raw bytes per instruction and advancing by the
returned length (zero-length guard prevents an unbounded loop). Param parsing (`parse_mem_request`/
`parse_disasm_request`, hex-or-int addresses, default/cap clamping) and JSON formatting
(`format_memory`/`format_disasm`, lowercase hex dump with `??` for unreadable bytes, `truncated`+
`next_*` pagination) are pure in `mcp_protocol.cpp`. The `read_memory`/`disassemble` handlers in
`dispatch` are reached only when `STATE_PARKED`. **No core edits** — the bridge declares the
existing `GetAddress`/`DasmI386` symbols and reuses `mem.h`/`paging.h`/`cpu.h`, so the core-edit
manifest is unchanged. Unit tests: `tests/mcp_protocol_tests.cpp` (`Mcp.ParseMem*`, `Mcp.FormatMemory*`,
`Mcp.ParseDisasm*`, `Mcp.FormatDisasm*`) cover defaults, cap clamping, space variants, bad-input
rejection, the `??`/unreadable rendering, truncation/`next_off`, and the payload bound. Integration:
`scripts/mcp_slice4_memory.py` boots headless with `-break-start`, asserts a segmented low-RAM read
matches the same bytes via the virtual space, default page = 256, over-cap clamp to 4096 + `truncated`
+ `next_off`, bad-param `-32602`, and disassembles CS:EIP (offset sequencing + over-cap clamp to 128);
wired into `scripts/mcp-check.sh` (integration test #4).

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

**Outcome (resolved):** ✅ Shipped. The reader/formatter split is preserved with one wrinkle:
because execution control *mutates* debugger state, the side-effecting primitive lives in
**`debug.cpp` as `MCP_DebugExec`** (the single Slice 5 core edit, in the manifest) — it must,
since it touches statics that are not exported (`debugging`, `exitLoop`, `mustCompleteInstruction`,
the static `StepOver`) and reuses the exact F11/F10/F5/-break-start primitives. The thin bridge
`src/mcp/mcp_execution.cpp` marshals the `ExecOp` into that call and reads back CS:EIP + state;
`mcp::format_exec` (pure, `mcp_protocol.cpp`) renders the compact stop report
(`op`/`state`/`resumed`/`ran`/`cs`/`eip`). `step` traces one instruction and stays parked
(`resumed:false`, CS:EIP = new stop); `step_over` steps over a call/int/loop/rep (else a plain
step); `continue` releases the guest to free-run (`debugging=false`+`DEBUG_Run(1,false)`, with the
`inhibit_int_breakpoint` guard and the `DEBUG_NullCPUCore` exit-status check mirrored from F5);
`break` is run-class and engages the debugger via `DEBUG_EnableDebugger` (same path as
`-break-start`). Resumed ops report `state:running`; the client polls `ping` until parked. The
`step`/`step_over`/`continue` handlers are reached only when `STATE_PARKED`, `break` only when
`STATE_RUNNING` (classify table from Slice 2). Unit tests: `tests/mcp_protocol_tests.cpp`
(`Mcp.FormatExec*`, `Mcp.ExecControlClassification`). Integration: `scripts/mcp_slice5_exec.py`
boots headless with `-break-start`, single-steps (asserts CS:EIP advances + stays parked +
agrees with `read_registers`), step-overs back to parked, `continue`s to running, `break`s back
to parked (asserts CS:EIP advanced, proving the guest executed), and asserts both mode-mismatch
fast-rejects (step-while-running, break-while-parked); wired into `scripts/mcp-check.sh`
(integration test #5).

## Slice 6 — Breakpoints: `list` / `add` / `delete`
**Goal:** breakpoint management (exec + interrupt + memory-watch).
**Changes:** wrap `CBreakpoint::AddBreakpoint` / `AddIntBreakpoint` / `AddMemBreakpoint` / `DeleteByIndex`
/ `ShowList`; expose the 6 `EBreakpoint` types; `list` is bounded.
**Tests:** integration — add exec bp, continue, assert stop; add INT bp; delete by index; list reflects state.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the other state slices, with
the side-effecting primitive in `debug.cpp` (the Slice 6 core edit above) because the breakpoint
store is debugger-private. The thin bridge `src/mcp/mcp_breakpoints.cpp` marshals the parsed
request into `MCP_BreakpointAdd`/`MCP_BreakpointDelete` and reads the list back via
`MCP_BreakpointCount`/`MCP_BreakpointGet` (plain out-params, no shared struct — keeping
`debug.cpp` free of MCP headers, like Slice 5). The pure layer (`mcp_protocol.cpp`) owns param
parsing (`parse_bp_add_request`, `parse_bp_type`) and JSON formatting (`format_breakpoint`,
`format_breakpoint_list`); the `BpType` enum mirrors `EBreakpoint` by value so the integer crossing
the bridge is unambiguous. All six breakpoint types are exposed — `exec` (BKPNT_PHYSICAL), `int`
(interrupt, with `ah`/`al` match or `*`=all), and the four memory watches `mem`/`mem_prot`/
`mem_linear`/`mem_freeze` (the watched byte is seeded with the current value on add). `breakpoint_list`
is bounded to `MCP_LIST_MAX` (256) with `count`/`total`/`truncated`. All three tools are parked-class.
Unit tests: `tests/mcp_protocol_tests.cpp` (`Mcp.BreakpointTypeNames`, `Mcp.ParseBpAdd*`,
`Mcp.FormatBreakpoint*`, `Mcp.BreakpointClassification`) cover type mapping, param parsing/rejection,
formatting of each type family, list bounding/counts, the payload bound, and classification.
Integration: `scripts/mcp_slice6_breakpoints.py` boots headless with `-break-start` and asserts
list/add/delete mechanics (add exec + INT, delete-by-index keeps the other, delete-all empties,
bad-param `-32602`), then proves an exec breakpoint stops a continuing guest (control-flow-robust
fall-through search) and an `INT 10h` breakpoint stops it during POST; wired into
`scripts/mcp-check.sh` (integration test #6).

## Slice 7 — Writes: `write_register` / `write_memory`
**Goal:** mutate state.
**Changes:** `write_register` delegates to `ChangeRegister` (inherits longest-name-first ordering);
`write_memory` follows the `SM` model — width-tagged values via `mem_write{b,w,d}_checked(GetAddress(...))`.
**Tests:** integration — write then read back register and memory.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the read slices, mirroring the
debugger's own `SR`/`SM` commands. **No core edits** — both bridges reuse existing globals exactly like
the read bridges (Slice 3/4), so the core-edit manifest is unchanged. `write_register`
(`src/mcp/mcp_registers.cpp`) hands the upper-cased `"<REG> <value>"` to the debugger's **`ChangeRegister`**
(external-linkage global, declared in the bridge), inheriting its longest-name-first matching and
per-register width masking (a 16-bit write leaves the high half of the dword intact); an unrecognised name
returns `-32602`. `write_memory` (`src/mcp/mcp_memory.cpp`) resolves the three address spaces like
`read_memory` — segmented via per-slot `GetAddress`, virtual = linear, physical via `physdev_write*` — and
writes width-1/2/4 values through `mem_write{b,w,d}_checked` (true == fault: it stops early and reports
`fault:true` + the partial `written`/`bytes` counts). Param parsing (`parse_reg_write_request`/
`parse_mem_write_request`, value as JSON int or hex string, `values` array, `width`∈{1,2,4}) and JSON
formatting (`format_reg_write`/`format_mem_write`) are pure in `mcp_protocol.cpp`; both tools are
parked-class. The write byte total is capped at `MCP_READMEM_MAX` (4096) — a write does no more work than a
max read — and an over-cap request is **rejected** (`-32602`), not silently clamped, so no partial write
surprises the caller. A small read accessor (`Json::size`/`Json::at`) was added to the MCP-private JSON value
to iterate the `values` array (isolated, not a core edit). Unit tests: `tests/mcp_protocol_tests.cpp`
(`Mcp.ParseRegWrite*`, `Mcp.FormatRegWrite`, `Mcp.ParseMemWrite*`, `Mcp.FormatMemWriteVariants`,
`Mcp.WriteClassification`) cover name upper-casing, int/hex values, the byte-cap boundary, space variants,
bad-input rejection, formatting of each space + the unresolved-selector case, and classification.
Integration: `scripts/mcp_slice7_writes.py` boots headless with `-break-start`, writes EAX then a 16-bit AX
(asserting the high half survives), rejects an unknown register, byte-writes `de ad be ef` to a scratch
RAM region and reads it back, proves width-2 little-endian storage and that a `virtual`-space write is seen
through the `segmented` space, and asserts the bad-param rejections; wired into `scripts/mcp-check.sh`
(integration test #7).

## Slice 8 — Input injection: `send_keys` / `type_text` / `mouse`
**Goal:** drive the guest while it runs (serviced at the frame tick).
**Changes:** `send_keys` = `KEYBOARD_AddKey(KBD_KEYS, press/release)` pairs; `type_text` =
`MAPPER_AutoType(sequence, wait_ms, pace_ms, choice)`; mouse via `GFX_EventsMouseProcess`.
**Tests:** integration — type a string at the DOS prompt, then read it back from the text screen
(closes the input↔screen loop; depends on Slice 9 read).
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the other slices; all three
tools are run-class (serviced at the `GFX_Events` frame tick while the guest free-runs). The thin
bridge `src/mcp/mcp_input.cpp` reuses existing public APIs, so **no core edits** — the core-edit
manifest is unchanged (isolation guardrail intact). Two deliberate deviations from the sketch above,
each to honor the single-threaded / headless discipline (documented in `mcp_protocol.h`):
- **`type_text` does not use `MAPPER_AutoType`.** That helper spawns a background `std::thread` (the
  mapper's `Typer`) that mutates keyboard state off the emulator thread and depends on the SDL mapper
  being initialized — both at odds with this fork's single-threaded model and fragile headless.
  Instead the pure layer decodes the text to `KeyEvent` transitions (US-layout ASCII→key+shift), the
  bridge queues them, and `MCP_InputFrameService` (called from the existing `MCP_GFXFrameService`, not
  a new core call site) feeds a few per frame via `KEYBOARD_AddKey` while the guest runs, respecting
  `KEYBOARD_BufferSpaceAvail` — single-threaded and paced.
- **`mouse` uses the direct `Mouse_*` APIs** (`Mouse_CursorMoved`/`Mouse_ButtonPressed`/`Released`/
  `Mouse_WheelMoved`) rather than `GFX_EventsMouseProcess`, which is window/clip/SDL-event dependent
  (Win32-centric) and unusable headless.

`send_keys` is `KEYBOARD_AddKey` press/release pairs (string entries = taps; `{key,down}` objects =
explicit transitions for chords), capped at `MCP_KEYS_MAX` (64) transitions. `type_text` is capped at
`MCP_TYPE_MAX` (256) chars; undecodable chars are skipped+counted. The pure name/ASCII tables, param
parsing and JSON formatting live in `mcp_protocol.cpp` (unit-tested, no boot). Unit tests:
`tests/mcp_protocol_tests.cpp` (`Mcp.KbdKeyFromName`, `Mcp.AsciiToKey`, `Mcp.ParseSendKeys*`,
`Mcp.ParseTypeText*`, `Mcp.ParseMouse*`, `Mcp.FormatInputResults`, `Mcp.InputClassification`).
Integration: `scripts/mcp_slice8_input.py` boots headless **without** `-break-start` (the guest
free-runs to the `Z:\` prompt; `-break-start` can re-park on a BIOS interrupt), then asserts the
parked mode-mismatch fast-reject, that `send_keys ['d','i','r']` echoes "dir" and `type_text "HELLO"`
echoes "HELLO" onto the text screen (read back via `read_memory` at `B800:0`, since Slice 9's
`read_screen` is not built yet), that the five `mouse` actions are accepted/well-formed (no mouse
driver loaded, so only the bridge wiring is asserted), and the `-32602` bad-param rejections; wired
into `scripts/mcp-check.sh` (integration test #8).

## Slice 9 — Screen: `screen_hash` / `read_screen` (text)
**Goal:** token-cheap screen state with change detection.
**Changes:** `read_screen` returns the text grid via `ReadCharAttr` when `CurMode->type==M_TEXT`;
`screen_hash` returns a cheap fingerprint — hash the text buffer in text modes, hash `render.src`
framebuffer bytes in graphics modes (change-detection even headless / graphics). Both bounded.
**Tests:** integration — assert known prompt text; assert hash changes after a screen-changing action and
is stable otherwise.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the other state slices; both
tools are run-class (serviced at the `GFX_Events` frame tick while the guest free-runs — the screen
is read live, not via a parked memory peek). The new bridge `src/mcp/mcp_screen.cpp` reuses existing
globals/APIs, so **no core edits** — the core-edit manifest is unchanged. `read_screen` reads the text
grid via the int10 `ReadCharAttr` primitive (the same one INT 10h AH=08h uses) for each cell, with the
grid dimensions taken from the BIOS data area (`BIOSMEM_NB_COLS`/`NB_ROWS`, mode-block fallback,
clamped to `MCP_SCREEN_MAX_COLS`×`ROWS` = 255×100); in graphics modes it returns `is_text:false` and an
empty grid (use `take_screenshot`, Slice 10). `screen_hash` is an FNV-1a 64-bit fingerprint — over the
visible char+attribute cells in text modes (so it tracks exactly what `read_screen` shows), and over
`render.src` dimensions + a bounded (`MCP_SCREENHASH_SCAN_MAX` = 256 KiB) scan of `vga.mem.linear` in
graphics modes — rendered as a `0x`-prefixed 16-hex string so no precision is lost across JSON's double.
The pure layer (`mcp_protocol.cpp`) owns the `fnv1a64` primitive and the formatters: `format_screen`
sanitizes each cell to printable ASCII (`0x20..0x7e`) or `.` so every line is valid UTF-8 and
token-cheap, and a full 132×60 grid is asserted under the 64 KiB ceiling. The one isolated build touch
is `-I$(top_srcdir)/src/ints` added to `src/mcp/Makefile.am` so the bridge can include `int10.h`
(`CurMode`/`VideoModeBlock`/`M_TEXT`/`BIOSMEM_*`) — MCP-local, not a core edit. Unit tests:
`tests/mcp_protocol_tests.cpp` (`Mcp.Fnv1a64KnownVectors`, `Mcp.FormatScreen*`,
`Mcp.FormatScreenHashRendersHexString`, `Mcp.ScreenClassification`) cover the hash against canonical
FNV-1a vectors, grid sanitization/shaping, the non-text empty grid, the full-grid payload bound, hex
rendering, and classification. Integration: `scripts/mcp_slice9_screen.py` boots headless **without**
`-break-start` (early boot shows a graphics splash — the test waits through it), asserts a well-shaped
80×25 text grid with the `Z:\` prompt, the parked mode-mismatch fast-reject for both tools, and that
`screen_hash` is stable on an unchanging screen, changes after typing onto the prompt, and is
deterministic for a given screen; wired into `scripts/mcp-check.sh` (integration test #9).

## Slice 10 — `take_screenshot` (graphics, on-demand)
**Goal:** full-fidelity graphics capture when asked.
**Changes:** promote the Slice 1 verification into a tool: trigger capture, return a **PNG path +
metadata** (never raw pixels).
**Tests:** integration — switch to a graphics mode, screenshot, assert valid non-empty PNG + correct dims.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the other run-class slices, and
**no core edit** — the new bridge `src/mcp/mcp_screenshot.cpp` reuses the exact path Slice 1 proved
headless (`CaptureState |= CAPTURE_IMAGE` → the next rendered frame flushes a PNG via
`RENDER_EndUpdate` → `CAPTURE_AddImage`) plus the file-scope `capturedir`, `CurMode`, and `render.src`,
so the core-edit manifest is unchanged. The capture is inherently **asynchronous** (the PNG lands a frame
*after* the handler returns), and handlers must stay non-blocking, so the bridge runs a tiny per-frame
state machine: the first service call snapshots the capture dir, records the source mode, arms the capture
and a ~4 s deadline (under the 5 s client timeout); later calls watch for the PNG that appeared since the
snapshot, then read its **IHDR dims + file size** (falling back to `render.src` dims) and report it. While
pending the dispatcher returns the new **`defer_sentinel()`** and the server's `drain` **re-queues** the
request to retry next frame (a small isolated change to `mcp_server.cpp`, holding deferred requests aside
so they don't spin within a drain) — so to the client it is a single blocking call that returns a
ready-to-read absolute `path` + `{format:"png", width, height, bytes, mode, is_text}`. The tool is
mode-agnostic (a text screen is still rasterized to a real PNG); it is run-class (already in the Slice 2
classify table) and reached only when `STATE_RUNNING`. Unit tests: `tests/mcp_protocol_tests.cpp`
(`Mcp.FormatScreenshot`, `Mcp.FormatScreenshotWithinCeiling`, `Mcp.DeferSentinelIsNotValidJson`,
`Mcp.ScreenshotClassification`) cover the JSON shape, the payload bound, the non-JSON defer marker, and
classification. Integration: `scripts/mcp_slice10_screenshot.py` boots headless **without** `-break-start`
(the guest free-runs so frames render), asserts `take_screenshot` returns a PNG whose on-disk magic, size,
and IHDR dims match the reported metadata, that a second call writes a distinct valid PNG (monotonic
naming), and the parked mode-mismatch fast-reject; wired into `scripts/mcp-check.sh` (integration test #10).

## Slice 11 — Memory scanner: `scan_start` / `scan_filter` / `scan_results`
**Goal:** "cheat-engine" progressive search for variables.
**Changes:** wrap MEMFIND/MEMS (single global `MEMFINDInstance`): start snapshots a range; filter applies
`= ! > < >= <=`; results are **bounded/paginated**. Treat the instance as session-global state.
**Tests:** integration — start a scan, change a known value, filter `=`, assert the address narrows to it.
**DoD:** `mcp-check.sh` green.

**Outcome (resolved):** ✅ Shipped. Same reader/formatter split as the other parked slices: the param
parsing + JSON formatting are pure in `mcp_protocol.cpp` (unit-tested with no boot); the scan touches
emulator memory and lives in the bridge `src/mcp/mcp_scan.cpp`, which calls the **single core edit** —
`MCP_Scan{Start,Filter,State,Results,Cancel}()` in `debug.cpp` (in the manifest above). Those functions
wrap the debugger's existing `MEMFIND`/`MEMS` logic over the session-global `MEMFINDInstance`, mirroring the
start-snapshot / filter / list algorithms exactly but returning structured out-params; the interactive
`MEMFIND`/`MEMS` `ParseCommand` handlers are untouched. `scan_start` snapshots a `seg:off` range (range
clamped to `MCP_SCAN_RANGE_MAX` = 1 MiB, `width` ∈ {1,2,4}); `scan_filter` narrows the candidate set with
`== != > < >= <=` (op integers match `MEMFinder::opType`), or against each cell's start snapshot via
`use_prev`; `scan_results` returns a `MCP_LIST_MAX` (256)-bounded page with `start`/`count`/`total`/
`truncated`. Two deliberate client conveniences vs the interactive commands: a fresh `scan_start` **replaces**
an in-progress scan, and `scan_filter` does **not** auto-delete the instance at 0 matches (so `scan_results`
stays callable). All three tools are parked-class. Unit tests: `tests/mcp_protocol_tests.cpp`
(`Mcp.ScanOpNamesAndParsing`, `Mcp.ParseScanStart*`, `Mcp.ParseScanFilter*`, `Mcp.FormatScanState*`,
`Mcp.FormatScanResults*`, `Mcp.ScanClassification`) cover op parsing, param defaults/clamp/rejection, the
JSON shape, value-width rendering, pagination, the payload bound, and classification. Integration:
`scripts/mcp_slice11_scan.py` boots headless parked (`-break-start`), and — holding guest memory still while
parked — uses the Slice 7 `write_memory` tool to control a scratch region: it proves the build-plan
assertion (zero-fill → `scan_start` → change one byte → `scan_filter ==` narrows to exactly that address with
the right value), plus a width-2 narrowing, pagination past the 256-entry page, the no-active-scan rejection,
the bad-param fast-rejects, and that a fresh `scan_start` replaces an in-progress scan; wired into
`scripts/mcp-check.sh` (integration test #11).

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
