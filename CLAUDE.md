# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DOSBox-X — a cross-platform DOS/PC emulator (fork of DOSBox SVN Daum). It emulates
IBM PC / PC-98 / DOS/V hardware accurately enough to run DOS games and applications,
Windows 1.x–3.x, and Windows 95/98/ME guests. Emphasis is on **emulation accuracy**,
not just game compatibility. Licensed GPLv2.

This checkout is the `mourix/dosbox-x-mcp` fork. The canonical upstream is
`joncampbell123/dosbox-x` (configured as the `upstream` git remote). Default branch
is `master`. Note: the working tree may show nearly every file as modified — this is
line-ending (CRLF) noise from the Windows/WSL checkout, not real changes; don't treat
it as work in progress.

### Goal of this fork

The purpose of `dosbox-x-mcp` is to add an **MCP (Model Context Protocol) server** to
DOSBox-X and its built-in debugger, so that Claude Code (or any MCP client) can drive
the emulator and debugger programmatically — to **reverse-engineer DOS applications**
(set breakpoints, step, inspect registers/memory, disassemble, etc.) under AI control.

The MCP server does not exist in the tree yet — building it is the work. The natural
integration surface is the **debugger** in `src/debug/` (see below): its command
interface and state (registers, memory, disassembly, breakpoints) are what the MCP
tools should expose. Expect new code to bridge that debugger functionality to an MCP
transport rather than re-implementing emulation.

**Read `docs/MCP_BUILD_PLAN.md` before starting MCP work** — it is the incremental
roadmap (Slice 0 → 13, scaffolding to full toolset) and each slice is independently
shippable behind `scripts/mcp-check.sh`. `docs/MCP_MANUAL.md` is the living workflow
guide for the MCP client. The architecture is **decided** (see the plan for detail):
one `dosbox-x` process built `--enable-mcp` (implies `C_DEBUG`), launched **headless**
(`SDL_VIDEODRIVER=dummy`/`SDL_AUDIODRIVER=dummy`), with a TCP JSON-RPC server thread
that only enqueues requests and an emulator-thread dispatcher that drains the queue at
two service points — `GFX_Events()` (input/screen while the game runs) and the debugger
loop (`DEBUG_Loop`/`DEBUG_CheckKeys`, debug ops while the CPU is parked) — so all
emulator state access stays single-threaded. New code lives in `src/mcp/`.

## Guardrails (working agreement)

These govern how MCP/debugger features are built in this fork. They split into rules
that are **checked** (a script or test must pass) and principles that are **judged**
(applied via review, not automatable).

**Checked — a feature is not done until these pass:**

1. **Isolation.** MCP code is additive and isolated — its own directory, compiled
   behind a build flag (e.g. `--enable-mcp`), with the smallest possible edits to core
   emulation/debugger code. This is the primary protection against baseline regressions:
   if core isn't touched, it can't break. Prefer calling existing debugger APIs over
   modifying them.
2. **One-command verification.** All checks run via a single repeatable entry point
   (intended: `scripts/mcp-check.sh` — build + unit tests + integration tests). "Ran the
   tests" means "ran that script and it passed." Never ship a feature without running it.
3. **Bounded responses.** MCP tool outputs must be compact and size-bounded — paginate
   memory/disassembly/log dumps, never return a firehose. Response size is asserted in
   tests against the concrete numbers in `docs/MCP_BUILD_PLAN.md` ("Response bounds"), not
   ad-hoc per-slice constants. This is the runtime side of reducing token usage for the LLM.
4. **Repeatable everything.** Setup, build, test, and demo steps are scripts, not manual
   instructions. If a step can't be re-run from a script, it isn't done.
5. **Safe transport & defined protocol.** The TCP server binds `127.0.0.1` only (never
   `0.0.0.0`) and the bind address is an asserted invariant. Requests that target the wrong
   execution state (`run` vs `parked`) are fast-rejected with the current state and never block;
   every queued request has a bounded timeout. See `docs/MCP_BUILD_PLAN.md` ("Protocol semantics").

**Judged — applied in review:**

6. **Test coverage matches the feature's layer.** Logic (request parsing, response
   formatting) gets fast unit tests in `tests/` (gtest, run via `./src/dosbox-x -tests`).
   Anything touching emulator/debugger *state* (breakpoints, stepping, register/memory
   reads) gets an integration test that boots a small known DOS program and drives the
   debugger. Docs and refactors don't need new tests but must keep existing tests green.
7. **Simplicity / no overcomplexity.** Favor the simplest design that works; refactor
   when complexity creeps in — including the test harness itself. Reducing development
   token usage is served here and by guardrail 4 (don't re-derive context each session).
8. **Living LLM manual.** Keep `docs/MCP_MANUAL.md` current. The MCP **tool definitions
   are the single source of truth** for parameters; the manual describes *workflows*
   (attach → breakpoint → step → inspect) rather than re-listing schemas, to avoid drift
   and token waste.
9. **Minimal, manifest-tracked core edits.** Every touch to core emulation/debugger code is
   listed with its justification in the `docs/MCP_BUILD_PLAN.md` core-edit manifest, so the
   isolation guardrail (#1) is auditable at a glance instead of reconstructed from the slices.

**Definition of done:** isolated (core edits in the manifest) + `scripts/mcp-check.sh` green +
responses bounded + localhost-only bind + steps scripted + appropriate tests added + manual updated.

## Building (Linux)

The `configure` script is **not committed** — it is regenerated by `autogen.sh`, which
the build wrapper scripts run for you. Always build via the wrapper scripts rather than
calling `./configure` directly:

- `./build` — release build (SDL1), `--enable-debug`
- `./build-debug` — `--enable-debug=heavy` (includes the built-in debugger; use this for development)
- `./build-sdl2` / `./build-debug-sdl2` — SDL2 variants
- `./build 32` — pass `32` as first arg to any wrapper to cross-build 32-bit on x64

Each wrapper runs `autogen.sh`, compiles the **in-tree** SDL 1.x (`vs/sdl/`) and SDLnet
(`vs/sdlnet/`) — the SDL1 sources are heavily patched and incompatible with the stock
library, so they must be built from the tree — then `./configure ... && make -j3`. The
output binary is `src/dosbox-x`.

Other platforms: Windows uses the Visual Studio solution `vs/dosbox-x.sln` (VS 2017/2019/2022)
or `build-mingw*` (MSYS2/MinGW); macOS uses `build-macos*`; DOS/HX-DOS uses `build-mingw-hx-dos`.
See `BUILD.md` for full per-platform prerequisites.

Requires a C++ compiler supporting C++11; **new code must compile as C++14** (per `CONTRIBUTING.md`).

## Tests

Unit tests live in `tests/` and use bundled GoogleTest/GoogleMock (`tests/gtest`, `tests/gmock`).
They are compiled into the emulator binary and run via a command-line flag:

```
./src/dosbox-x -tests                              # run all tests
./src/dosbox-x -tests --gtest_filter=DOSFiles.*    # run a subset (standard gtest flags apply)
```

Add new tests as `*_tests.cpp` files in `tests/` (e.g. `dos_files_tests.cpp`,
`drives_tests.cpp`, `shell_cmds_tests.cpp`).

## Architecture

DOSBox-X emulates the full PC stack. The authoritative source map is
**`README.source-code-description`** — consult it before diving into unfamiliar areas.
The major subsystems under `src/`:

- **`gui/`** — host integration and the main loop. `sdlmain.cpp` is the entry point
  (emulator setup, runtime execution, GFX mode handling). `menu.cpp` maps an abstract
  menu model onto Win32 HMENU / macOS NSMenu / SDL-drawn menus (selected by `DOSBOXMENU_*`
  in `include/menu.h`). `sdl_mapper.cpp` = input/shortcut mapping; `sdl_gui.cpp` = the
  built-in config GUI (gui_tk); `render.cpp` / `render_scalers.cpp` = scaler pipeline;
  `midi.cpp` = MIDI output.
- **`cpu/`** — x86 CPU emulation with multiple interchangeable cores selectable at runtime:
  `core_normal`, `core_simple`, `core_full`, `core_prefetch`, `core_dyn_x86` (dynamic
  recompiler), `core_dynrec`. Plus `paging.cpp` (TLB/paging), `modrm.cpp`, `mmx.cpp`,
  `flags.cpp`/`lazyflags.h` (lazy flag evaluation).
- **`fpu/`** — x87 FPU emulation.
- **`dos/`** — the emulated DOS kernel: `dos.cpp`, `dos_execute.cpp`, `dos_files.cpp`,
  and the drive abstraction (`drive_local`, `drive_fat`, `drive_iso`, `drive_overlay`,
  `drive_physfs`, `drive_virtual`), CD-ROM, and MSCDEX.
- **`ints/`** — BIOS and DOS interrupt services: `bios.cpp`, `int10*` (VGA/VESA video
  BIOS), `ems.cpp`/`xms.cpp` (memory managers), `mouse.cpp`, disk image handlers.
- **`hardware/`** — device emulation: VGA, sound (SB, AdLib/OPL, GUS, MIDI), PIC/PIT/DMA,
  IDE/disk, serial/parallel, NE2000 networking, etc.
- **`shell/`** — the emulated COMMAND.COM: `shell.cpp` (init, CONFIG.SYS/AUTOEXEC.BAT),
  `shell_cmds.cpp` (internal commands: DIR, COPY, SET, ...), `shell_batch.cpp` (.BAT).
- **`builtin/`** — DOS utilities embedded in the binary as `unsigned char[]` arrays
  (DEBUG.EXE, MEM.COM, EDIT.COM, EMM386, DOS4GW, etc.) and registered at runtime.
- **`debug/`** — the built-in debugger (the key surface for this fork's MCP work).
  `debug.cpp` is the ncurses-based interactive debugger and command interpreter;
  `debug_disasm.cpp` / `disasm_tables.h` provide x86 disassembly; `debug_gui.cpp` the
  TUI. Only compiled when configured with `--enable-debug` (or `=heavy`). It exposes
  register/memory views, disassembly, breakpoints, and stepping — see `README.debugger`
  for the command set. On Linux/macOS the emulator must be launched from a terminal for
  the debugger to attach.
- **`output/`** — rendering backends (surface, OpenGL, Direct3D, TTF).
- **`misc/`** — cross-platform glue: `setup.cpp` (config system), `messages.cpp`
  (translations), `cross.cpp`, `support.cpp`, `shiftjis.cpp`.
- **`include/`** — shared headers (note: many inline-heavy headers like `bitop.h`,
  `ptrop.h` have companion `.cpp` files in `src/gui/`).

### Configuration system

Settings flow through `src/misc/setup.cpp` (sections + properties) and surface as the
`dosbox-x.conf` file. `dosbox-x.reference.conf` / `dosbox-x.reference.full.conf` are
generated reference dumps of all options — regenerate them with
`./update-dosbox-x-reference-conf` after adding or changing config options.

## Conventions

- Follow `.editorconfig` (the project relies on it for style; your editor should honor it).
- Prefer short methods, code reuse (DRY), and shallow nesting. Doxygen comments preferred
  but self-explanatory names are acceptable. Much existing code predates these rules.
