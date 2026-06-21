# DOSBox-X MCP — LLM User Manual

This manual is for an LLM (Claude Code or any MCP client) driving DOSBox-X through the
MCP server to **reverse-engineer DOS applications**. It describes *workflows*. The
authoritative reference for tool names and parameters is the MCP tool definitions
themselves (the single source of truth) — this manual does not duplicate schemas.

> Status: **stub.** The MCP server does not exist yet. Fill in each section as tools
> land. Keep workflows here in sync with the implemented tools; update on every feature.

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
