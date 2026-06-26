// tools.mjs — the client-facing tool catalog for the DOSBox-X MCP bridge.
//
// This is the single source of truth for the MCP `tools/list` schemas. Each
// entry mirrors one flat JSON-RPC method exposed by the emulator
// (src/mcp/mcp_protocol.cpp). The `state` field records the execution state the
// emulator services the tool in (run / parked / any) and is also surfaced in the
// description so the model knows when to `break` / `continue` first.
//
// Param shapes mirror what the C++ dispatch actually reads (json_to_u32 accepts
// either a JSON integer or a hex string like "0xb8000", so address-style fields
// are typed ["integer","string"]). Workflow/semantics live in docs/MCP_MANUAL.md;
// keep this catalog in sync with the C++ method table — the sync-guard test
// (test/) asserts the name sets match exactly.

// An address/number field that accepts an int or a hex string ("0x1234").
const addr = (description) => ({ type: ["integer", "string"], description });
const NO_PARAMS = { type: "object", properties: {}, additionalProperties: false };

// Helper to tag a description with its execution-state class.
const S = (state, text) => `[${state}] ${text}`;

export const TOOLS = [
  // ---- any-state -----------------------------------------------------------
  {
    name: "ping",
    state: "any",
    description: S("any", "Liveness probe. Returns {pong:true, state:'running'|'parked'} — the cheapest way to learn the current execution state (poll it after continue/step_over/reset to wait until parked)."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "server_info",
    state: "any",
    description: S("any", "Server metadata: version, transport, bind (always 127.0.0.1), port, single_client, current state, and max_payload."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "reset",
    state: "any",
    description: S("any", "Reboot the emulated machine (hardware reset to BIOS/boot). Returns a deferred ack immediately, then reboots a few frames later; the MCP connection survives. Poll ping until the state settles. Under --break-start it re-parks at the reset vector."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "quit",
    state: "any",
    description: S("any", "Shut the emulator down and exit the process cleanly (kill switch). Returns a deferred ack, then the connection drops as the process exits. Do not send further requests after this."),
    inputSchema: NO_PARAMS,
  },

  // ---- parked-state: inspection -------------------------------------------
  {
    name: "read_registers",
    state: "parked",
    description: S("parked", "Read CPU state: general (eax..eip) and segment (cs..ss) registers as fixed-width hex, the full eflags word + decoded flags object, the CPU mode (real/pr16/pr32/vm86) and cpl. Requires parked — break first."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "read_memory",
    state: "parked",
    description: S("parked", "Read guest memory (parked). Returns the resolved addr, byte count, lowercase hex dump (unreadable bytes render as '??'), and a next_* offset to page when truncated. Choose the address space and pass the matching address."),
    inputSchema: {
      type: "object",
      properties: {
        space: { type: "string", enum: ["segmented", "virtual", "physical"], default: "segmented", description: "Address space (default segmented)." },
        seg: addr("Segment (segmented space)."),
        off: addr("Offset (segmented space)."),
        lin: addr("Linear address (virtual space)."),
        phys: addr("Physical address (physical space)."),
        len: { type: "integer", minimum: 1, maximum: 4096, default: 256, description: "Bytes to read (default 256, max 4096 per call)." },
      },
      additionalProperties: false,
    },
  },
  {
    name: "disassemble",
    state: "parked",
    description: S("parked", "Disassemble from seg:off (parked). Returns an insns array (off, resolved addr, raw bytes, text) plus truncated when capped. Page forward from the last instruction's off + byte length."),
    inputSchema: {
      type: "object",
      properties: {
        seg: addr("Code segment (CS)."),
        off: addr("Starting offset (EIP)."),
        count: { type: "integer", minimum: 1, maximum: 128, default: 16, description: "Instructions to decode (default 16, max 128)." },
        big: { type: "boolean", description: "Force 16/32-bit decode; defaults to the current code-segment size." },
      },
      required: ["seg", "off"],
      additionalProperties: false,
    },
  },

  // ---- parked-state: execution control ------------------------------------
  {
    name: "step",
    state: "parked",
    description: S("parked", "Trace into one instruction. Stays parked; the returned cs:eip are the NEW stop, so you can step-and-inspect in a loop. Call N times to advance N instructions."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "step_over",
    state: "parked",
    description: S("parked", "Step one instruction, but step OVER a call/int/loop/rep (temp breakpoint after it, then run until return). For those it replies resumed:true/state:running — poll ping until parked. For a non-call it behaves like step."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "continue",
    state: "parked",
    description: S("parked", "Release the guest to free-run until the next stop (a breakpoint or break). Replies resumed:true/state:running; the reported cs:eip is the release point, not the eventual stop. Poll ping until parked, then read_registers."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "break",
    state: "run",
    description: S("run", "Stop a free-running guest and re-enter the debugger. Poll ping until parked before issuing parked-class tools."),
    inputSchema: NO_PARAMS,
  },

  // ---- parked-state: breakpoints ------------------------------------------
  {
    name: "breakpoint_list",
    state: "parked",
    description: S("parked", "List the debugger's breakpoints: index, type, active, once, and per-type locator, plus count/total/truncated (bounded to 256). A freshly-added breakpoint reads active:false until the next continue. Indices shift on add/delete — re-list before deleting by index."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "breakpoint_add",
    state: "parked",
    description: S("parked", "Add a breakpoint. type=exec (seg+off), int (int vector + optional ah/al match), mem/mem_prot/mem_freeze (watch a byte at seg+off; fires on next change; freeze re-writes), or mem_linear (watch a byte at lin). once:true = one-shot. Replies {added,index,type}; new breakpoints go to index 0 (others shift down)."),
    inputSchema: {
      type: "object",
      properties: {
        type: { type: "string", enum: ["exec", "int", "mem", "mem_prot", "mem_freeze", "mem_linear"], default: "exec", description: "Breakpoint type (default exec)." },
        seg: addr("Segment (exec / mem*)."),
        off: addr("Offset (exec / mem*)."),
        lin: addr("Linear address (mem_linear)."),
        int: addr("Interrupt vector, e.g. 0x21 (int type)."),
        ah: addr("Optional AH match value (int type); omit to match all."),
        al: addr("Optional AL match value (int type); omit to match all."),
        once: { type: "boolean", description: "One-shot: auto-removed when it fires." },
      },
      additionalProperties: false,
    },
  },
  {
    name: "breakpoint_delete",
    state: "parked",
    description: S("parked", "Delete one breakpoint by index, or all with all:true. Because indices shift on every add/delete, re-run breakpoint_list before deleting by index."),
    inputSchema: {
      type: "object",
      properties: {
        index: { type: "integer", minimum: 0, description: "Index to delete (from breakpoint_list)." },
        all: { type: "boolean", description: "Delete all breakpoints." },
      },
      additionalProperties: false,
    },
  },

  // ---- parked-state: writes -----------------------------------------------
  {
    name: "write_register",
    state: "parked",
    description: S("parked", "Write a register (mirrors the debugger SR command). register is a name in any case (eax, AX, cs, zf, ...); value is a hex string or int. A narrow name only touches its bits (writing AX leaves the high half of EAX intact). Read back with read_registers."),
    inputSchema: {
      type: "object",
      properties: {
        register: { type: "string", description: "Register name (eax, AX, cs, zf, ...; any case)." },
        value: addr("New value (hex string or int)."),
      },
      required: ["register", "value"],
      additionalProperties: false,
    },
  },
  {
    name: "write_memory",
    state: "parked",
    description: S("parked", "Write guest memory (mirrors SM). Address like read_memory (space + seg/off | lin | phys). width 1/2/4 bytes per value (default 1, little-endian); values is a non-empty array of hex strings/ints written at successive width-byte slots. Total capped at 4096 bytes. fault:true means a write hit unmapped/protected memory and stopped early. Read back to confirm."),
    inputSchema: {
      type: "object",
      properties: {
        space: { type: "string", enum: ["segmented", "virtual", "physical"], default: "segmented", description: "Address space (default segmented)." },
        seg: addr("Segment (segmented space)."),
        off: addr("Offset (segmented space)."),
        lin: addr("Linear address (virtual space)."),
        phys: addr("Physical address (physical space)."),
        width: { type: "integer", enum: [1, 2, 4], default: 1, description: "Bytes per value (default 1)." },
        values: { type: "array", minItems: 1, items: addr("A value (hex string or int)."), description: "Values to write at successive width-byte slots." },
      },
      required: ["values"],
      additionalProperties: false,
    },
  },

  // ---- run-state: input injection -----------------------------------------
  {
    name: "send_keys",
    state: "run",
    description: S("run", "Inject keyboard transitions (run-state; continue first). keys is a non-empty array of either a string key name (a tap = press+release) or {key,down} for an explicit press/release (use the object form to hold modifiers / chords). Names are case-insensitive with aliases (enter, ctrl, esc, f1..f12, up/down/left/right, ...). Capped at 64 transitions."),
    inputSchema: {
      type: "object",
      properties: {
        keys: {
          type: "array",
          minItems: 1,
          maxItems: 64,
          description: "Key names (taps) and/or {key,down} press/release objects.",
          items: {
            anyOf: [
              { type: "string", description: "Key name (tap = press then release)." },
              {
                type: "object",
                properties: {
                  key: { type: "string", description: "Key name." },
                  down: { type: "boolean", description: "true = press, false = release." },
                },
                required: ["key", "down"],
                additionalProperties: false,
              },
            ],
          },
        },
      },
      required: ["keys"],
      additionalProperties: false,
    },
  },
  {
    name: "type_text",
    state: "run",
    description: S("run", "Type a literal string (run-state; US layout, shifted chars auto-shifted). Frame-paced: returns {queued,chars,skipped} immediately while typing continues over the next frames — wait briefly / poll the screen before reading back. Unmapped chars are skipped. Capped at 256 chars. Use send_keys for control/function keys and chords."),
    inputSchema: {
      type: "object",
      properties: {
        text: { type: "string", maxLength: 256, description: "Text to type (max 256 chars)." },
      },
      required: ["text"],
      additionalProperties: false,
    },
  },
  {
    name: "mouse",
    state: "run",
    description: S("run", "One mouse action per call (run-state). action=move (dx,dy relative px), down/up/click (button 0=left/1=right/2=middle), or wheel (amount, signed). Mouse events reach the guest only when it has a mouse driver/handler installed."),
    inputSchema: {
      type: "object",
      properties: {
        action: { type: "string", enum: ["move", "down", "up", "click", "wheel"], description: "Mouse action." },
        dx: { type: "integer", description: "Relative X pixels (move)." },
        dy: { type: "integer", description: "Relative Y pixels (move)." },
        button: { type: "integer", enum: [0, 1, 2], description: "0=left, 1=right, 2=middle (down/up/click)." },
        amount: { type: "integer", description: "Signed wheel amount (wheel)." },
      },
      required: ["action"],
      additionalProperties: false,
    },
  },

  // ---- run-state: screen reads --------------------------------------------
  {
    name: "read_screen",
    state: "run",
    description: S("run", "Read the live text grid (run-state). In a text mode returns {mode,is_text:true,cols,rows,text:[...]} (one printable-ASCII string per row; attributes/code-page glyphs dropped). In a graphics mode returns is_text:false with empty text — use take_screenshot for pixels."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "screen_hash",
    state: "run",
    description: S("run", "Cheap change-detection fingerprint (run-state): {mode,is_text,cols,rows,hash} where hash is a 0x-prefixed FNV-1a. Same screen -> same hash; poll it to wait for the screen to settle or detect a change without pulling the whole grid."),
    inputSchema: NO_PARAMS,
  },
  {
    name: "take_screenshot",
    state: "run",
    description: S("run", "Capture the full-fidelity frame as a PNG (run-state; the way to see a graphics mode). Returns {path,format:'png',width,height,bytes,mode,is_text} — an absolute path to a ready-to-read file (never raw pixels). The guest must be free-running; the call blocks briefly until the file is written. Each call writes a new PNG."),
    inputSchema: NO_PARAMS,
  },

  // ---- parked-state: memory scanner ---------------------------------------
  {
    name: "scan_start",
    state: "parked",
    description: S("parked", "Start a cheat-engine-style scan (parked): snapshot a range to search. range is in bytes (clamped to 1 MiB); width is element size 1/2/4 (default 1). A fresh scan_start discards any in progress. Reply is the scan state with matches = every slot (range/width)."),
    inputSchema: {
      type: "object",
      properties: {
        seg: addr("Segment of the range start."),
        off: addr("Offset of the range start."),
        range: { type: "integer", minimum: 1, description: "Range length in bytes (clamped to 1 MiB)." },
        width: { type: "integer", enum: [1, 2, 4], default: 1, description: "Element size in bytes (default 1)." },
      },
      required: ["seg", "off", "range"],
      additionalProperties: false,
    },
  },
  {
    name: "scan_filter",
    state: "parked",
    description: S("parked", "Narrow the scan (parked): keep cells where cell <op> value, op one of == != > < >= <= (aliases =, !). Pass use_prev:true (no value) to compare each cell against its own start snapshot (e.g. op '>' keeps cells that increased). Reply is the updated scan state. Reaching 0 matches does not end the scan."),
    inputSchema: {
      type: "object",
      properties: {
        op: { type: "string", enum: ["==", "!=", ">", "<", ">=", "<=", "=", "!"], description: "Comparison operator." },
        value: addr("Comparison value (omit when use_prev is true)."),
        use_prev: { type: "boolean", description: "Compare each cell against its own start snapshot instead of value." },
      },
      required: ["op"],
      additionalProperties: false,
    },
  },
  {
    name: "scan_results",
    state: "parked",
    description: S("parked", "Return surviving scan addresses, paginated (parked): {active,width,matches:[{seg,off,lin,value}...],start,count,total,truncated}, capped at 256 per page (next page with start:256). value is the cell's current value at the element width."),
    inputSchema: {
      type: "object",
      properties: {
        start: { type: "integer", minimum: 0, default: 0, description: "Page offset into the match list (default 0)." },
      },
      additionalProperties: false,
    },
  },

  // ---- parked-state: raw debugger passthrough -----------------------------
  {
    name: "debugger_command",
    state: "parked",
    description: S("parked", "Escape hatch (parked): run a raw built-in debugger command through ParseCommand and return {command,recognized,truncated,output} (captured text, bounded to 16 KiB). Use for the long tail with no dedicated tool (MEMDUMP, EV, GDT/IDT/LDT, BPLIST, INTHAND, ...). Treat as READ-ONLY: avoid execution-affecting commands (RUN, G, RUNWATCH) — use step/continue/break instead."),
    inputSchema: {
      type: "object",
      properties: {
        command: { type: "string", description: "The raw debugger command line." },
      },
      required: ["command"],
      additionalProperties: false,
    },
  },
];

export const TOOL_NAMES = TOOLS.map((t) => t.name);
