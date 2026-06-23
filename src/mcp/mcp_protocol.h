/*
 *  DOSBox-X MCP — protocol layer: request classification, response bounding,
 *  and the pure request dispatcher.
 *
 *  Everything here is transport-agnostic and side-effect-free with respect to
 *  the socket, so it is unit-testable without a server (guardrail: tests matched
 *  to layer). Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#ifndef DOSBOX_MCP_PROTOCOL_H
#define DOSBOX_MCP_PROTOCOL_H

#include "config.h"

#if C_MCP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mcp_json.h"

namespace mcp {

/* Response bounds — the single source of the numbers in docs/MCP_BUILD_PLAN.md
 * ("Response bounds"). Tests assert against these, not ad-hoc per-slice values. */
static const size_t MCP_MAX_PAYLOAD       = 64 * 1024; /* hard ceiling, any reply  */
static const size_t MCP_READMEM_DEFAULT   = 256;       /* read_memory default page */
static const size_t MCP_READMEM_MAX       = 4096;      /* read_memory cap per call */
static const size_t MCP_DISASM_DEFAULT    = 16;        /* disassemble default count*/
static const size_t MCP_DISASM_MAX        = 128;       /* disassemble cap per call */
static const size_t MCP_LIST_MAX          = 256;       /* breakpoint/scan page max */
static const size_t MCP_PASSTHROUGH_MAX   = 16 * 1024; /* debugger_command capture */

/* The bind address is an asserted invariant: localhost only, never 0.0.0.0. */
static const char *const MCP_BIND_ADDRESS = "127.0.0.1";

/* Execution state of the emulator, as seen by the dispatcher (see "Protocol
 * semantics"). The two service points are mutually exclusive in time. */
enum ExecState { STATE_RUNNING, STATE_PARKED };

/* Request class: which execution state a tool must be serviced in. */
enum ReqClass { CLS_RUN, CLS_PARKED, CLS_ANY, CLS_UNKNOWN };

/* JSON-RPC error codes (standard range + MCP-specific). */
static const int MCP_ERR_PARSE          = -32700;
static const int MCP_ERR_INVALID_REQ    = -32600;
static const int MCP_ERR_METHOD_NOT_FOUND = -32601;
static const int MCP_ERR_INVALID_PARAMS = -32602;
static const int MCP_ERR_INTERNAL       = -32603;
static const int MCP_ERR_MODE_MISMATCH  = -32001; /* wrong execution state      */
static const int MCP_ERR_BUSY           = -32002; /* single-client: 2nd refused */
static const int MCP_ERR_TIMEOUT        = -32003; /* queued request timed out   */
static const int MCP_ERR_TOO_LARGE      = -32004; /* response exceeded ceiling  */
static const int MCP_ERR_NOT_IMPLEMENTED = -32005;/* known method, no handler yet*/

const char *state_name(ExecState s);              /* "running" / "parked"        */
ReqClass    classify(const std::string &method);
bool        mode_matches(ReqClass cls, ExecState state);

/* Response builders. Each returns a single-line JSON-RPC 2.0 response (no
 * trailing newline; the server frames it). id is echoed verbatim. */
std::string make_result(const Json &id, const Json &result);
std::string make_error(const Json &id, int code, const std::string &message);
std::string make_error(const Json &id, int code, const std::string &message, const Json &data);
std::string make_mismatch_error(const Json &id, ExecState current);
std::string make_timeout_error(const Json &id);
std::string make_busy_error();

/* Enforce the 64 KiB ceiling on an already-built response line. If body fits it
 * is returned unchanged; otherwise an MCP_ERR_TOO_LARGE error (which always
 * fits) is returned instead. Pure — the runtime side of guardrail #3. */
std::string enforce_max_payload(const Json &id, const std::string &body);

/* A plain-data snapshot of the guest CPU at a stop (Slice 3, read_registers).
 * Decoupling the *reading* of emulator globals (MCP_SnapshotRegisters, defined in
 * the emulator-state bridge mcp_registers.cpp) from the *formatting* (format_registers,
 * pure) keeps this layer unit-testable against a known snapshot without an emulator
 * boot. Mirrors the fields the debugger's DrawRegisters shows (debug.cpp:1146). */
struct RegisterSnapshot {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip;
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t eflags;          /* full FLAGS/EFLAGS word */
    bool     pmode;           /* protected mode enabled */
    bool     code_big;        /* current code segment is 32-bit */
    bool     vm86;            /* virtual-8086 mode (FLAG_VM) */
    unsigned cpl;             /* current privilege level 0..3 */
};

/* Reads the live guest CPU state into snap. Touches emulator globals
 * (cpu_regs/Segs/cpu), so it must run on the emulator thread while parked;
 * defined in mcp_registers.cpp. Declared here so the dispatcher can call it. */
void snapshot_registers(RegisterSnapshot &snap);

/* Pure: format a snapshot as compact, bounded JSON-RPC result. Deterministic key
 * order so unit-test assertions are exact. */
Json format_registers(const RegisterSnapshot &snap);

/* ---- read_memory / disassemble (Slice 4) --------------------------------
 *
 * Same reader/formatter split as Slice 3: the param *parsing* and result
 * *formatting* are pure (here / mcp_protocol.cpp, unit-testable with no boot),
 * while the byte/instruction *reading* touches emulator memory and lives in the
 * bridge mcp_memory.cpp. The three address spaces mirror the debugger's data
 * views (DBGBlock::DATV_SEGMENTED/VIRTUAL/PHYSICAL, debug.cpp:1020).
 */

enum AddrSpace { SPACE_SEGMENTED, SPACE_VIRTUAL, SPACE_PHYSICAL };

/* A parsed, already-bounds-clamped read_memory request. For SPACE_SEGMENTED the
 * address is seg:off; for SPACE_VIRTUAL `off` is a linear address; for
 * SPACE_PHYSICAL `off` is a physical address (seg unused). `len` is clamped to
 * [1, MCP_READMEM_MAX]; `requested_len` keeps the pre-clamp value so the
 * formatter can report truncation + the next offset for pagination. */
struct MemReadRequest {
    AddrSpace space;
    uint16_t  seg;
    uint32_t  off;
    uint32_t  len;
    uint32_t  requested_len;
};

/* Filled by the bridge. `bytes`/`readable` each have exactly req.len entries;
 * an unreadable (page-faulting / unmapped) byte has readable[i]==false and is
 * rendered "??". `addr_valid` is false only when a SPACE_SEGMENTED selector does
 * not resolve (GetAddress -> no address); then bytes/readable are empty. */
struct MemReadResult {
    bool                 addr_valid;
    uint64_t             addr;   /* resolved linear (seg/virt) or physical addr */
    std::vector<uint8_t> bytes;
    std::vector<bool>    readable;
};

/* Pure: parse params into req (defaults: space=segmented, len=MCP_READMEM_DEFAULT).
 * Accepts address fields as JSON integers or hex/decimal strings ("0x1000").
 * Returns false and sets err on a missing/invalid field or unknown space. */
bool parse_mem_request(const Json &params, MemReadRequest &req, std::string &err);

/* Pure: format a read result as compact, bounded JSON. */
Json format_memory(const MemReadRequest &req, const MemReadResult &out);

/* Bridge (mcp_memory.cpp): read req.len bytes from the requested space into out.
 * Touches emulator memory, so it must run on the emulator thread while parked. */
void read_memory(const MemReadRequest &req, MemReadResult &out);

/* A parsed, already-bounds-clamped disassemble request. Disassembly is always
 * segmented (seg:off, off = starting EIP). `count` is clamped to
 * [1, MCP_DISASM_MAX]. `big` selects 16/32-bit decoding; when `have_big` is
 * false the bridge defaults it from cpu.code.big. */
struct DisasmRequest {
    uint16_t seg;
    uint32_t off;
    uint32_t count;
    uint32_t requested_count;
    bool     have_big;
    bool     big;
};

/* One decoded instruction. `bytes`/`readable` are the raw instruction bytes;
 * `text` is the disassembly. */
struct DisasmInsn {
    uint16_t             seg;
    uint32_t             off;
    uint64_t             addr;   /* resolved linear/physical address */
    std::vector<uint8_t> bytes;
    std::vector<bool>    readable;
    std::string          text;
};

/* Filled by the bridge. `addr_valid` false only when the starting selector does
 * not resolve. `big` is the code size actually used. */
struct DisasmResult {
    bool                    addr_valid;
    bool                    big;
    std::vector<DisasmInsn> insns;
};

/* Pure: parse params into req (defaults: count=MCP_DISASM_DEFAULT). seg + off are
 * required. Returns false and sets err on a missing/invalid field. */
bool parse_disasm_request(const Json &params, DisasmRequest &req, std::string &err);

/* Pure: format a disassembly result as compact, bounded JSON. */
Json format_disasm(const DisasmRequest &req, const DisasmResult &out);

/* Bridge (mcp_memory.cpp): decode req.count instructions starting at seg:off.
 * Touches emulator memory, so it must run on the emulator thread while parked. */
void disassemble(const DisasmRequest &req, DisasmResult &out);

/* ---- execution control (Slice 5) ----------------------------------------
 *
 * step / step_over / continue / break. Unlike the read-only Slice 3/4 bridges
 * these *drive* execution, so the bridge (mcp_execution.cpp) mutates debugger
 * state via the single core edit MCP_DebugExec (debug.cpp). The classification
 * and the stop-report formatting stay pure here. step/step_over/continue are
 * parked-class; break is run-class (it engages the debugger on a running guest).
 */

enum ExecOp { EXEC_STEP, EXEC_STEP_OVER, EXEC_CONTINUE, EXEC_BREAK };

/* The outcome of one execution-control op, read back on the emulator thread.
 * `resumed` is true when the guest was released to free-run (continue,
 * step-over-a-call, break) — the caller then polls `ping` until parked again;
 * false when execution stayed parked (a plain single step), in which case
 * cs:eip already point at the new stop. `ran` is the DEBUG_Run status. `state`
 * is the execution state immediately after the op. */
struct ExecResult {
    bool      resumed;
    int32_t   ran;
    uint16_t  cs;
    uint32_t  eip;
    ExecState state;
};

/* Bridge (mcp_execution.cpp): perform the op on the emulator thread (parked for
 * step/step_over/continue; running for break) and read back the result. */
void exec_control(ExecOp op, ExecResult &out);

/* Pure: format an execution-control outcome as a compact, bounded stop report. */
Json format_exec(ExecOp op, const ExecResult &out);

/* ---- breakpoints (Slice 6) ----------------------------------------------
 *
 * breakpoint_list / breakpoint_add / breakpoint_delete. Same reader/formatter
 * split as the other state slices: param parsing + JSON formatting are pure
 * (here / mcp_protocol.cpp, unit-tested with no boot), while the store mutation
 * and listing live in the bridge mcp_breakpoints.cpp, which calls the single
 * core edit (the MCP_Breakpoint* functions in debug.cpp). All three tools are
 * parked-class. BpType mirrors debug.cpp's EBreakpoint by value so the integer
 * crossing the bridge is unambiguous (0=unknown,1=exec,...,6=mem_freeze). */
enum BpType {
    BPT_UNKNOWN = 0, BPT_EXEC, BPT_INT, BPT_MEM,
    BPT_MEM_PROT, BPT_MEM_LINEAR, BPT_MEM_FREEZE
};

/* One breakpoint as listed. For exec/mem* `seg`+`off` locate it (mem_linear uses
 * `off` as a linear address, seg unused). For int, `intnr` is the vector and
 * `ah`/`al` are the AH/AL match values with -1 meaning "match all" (BPINT_ALL).
 * `memvalue` is the watched byte for the mem* types, -1 otherwise. */
struct BreakpointInfo {
    int      index;
    BpType   type;
    uint16_t seg;
    uint32_t off;
    uint8_t  intnr;
    int      ah;        /* -1 == all */
    int      al;        /* -1 == all */
    int      memvalue;  /* -1 == n/a */
    bool     once;
    bool     active;
};

/* A parsed breakpoint_add request. ah/al default to -1 (all) for int types. */
struct BpAddRequest {
    BpType   type;
    uint16_t seg;
    uint32_t off;
    uint8_t  intnr;
    int      ah;
    int      al;
    bool     once;
};

const char *bp_type_name(BpType t);                       /* "exec"/"int"/...  */
bool        parse_bp_type(const std::string &s, BpType &out);

/* Pure: parse params into req (default type=exec, once=false). Returns false and
 * sets err on a missing/invalid field or unknown type. */
bool parse_bp_add_request(const Json &params, BpAddRequest &req, std::string &err);

/* Pure: format one breakpoint / a bounded list (capped at `max`, with count /
 * total / truncated). */
Json format_breakpoint(const BreakpointInfo &bp);
Json format_breakpoint_list(const std::vector<BreakpointInfo> &bps, size_t max);

/* Bridge (mcp_breakpoints.cpp): mutate/read the debugger's breakpoint store on
 * the emulator thread while parked. bp_add returns the new index or -1; bp_delete
 * takes an index (< 0 deletes all) and returns success. */
int  bp_add(const BpAddRequest &req);
bool bp_delete(int index);
void bp_list(std::vector<BreakpointInfo> &out);

/* ---- writes (Slice 7) ----------------------------------------------------
 *
 * write_register / write_memory. Same reader/formatter split as the read slices,
 * mirroring the debugger's own SR / SM commands: the param parsing and JSON
 * formatting are pure (here / mcp_protocol.cpp, unit-tested with no boot), while
 * the actual mutation lives in the bridges. write_register reuses the debugger's
 * ChangeRegister (mcp_registers.cpp), inheriting its longest-name-first register
 * ordering and per-register width masking; write_memory reuses GetAddress +
 * mem_write{b,w,d}_checked / physdev_write{b,w,d} (mcp_memory.cpp), the exact
 * primitives the SM/SMV/SMP commands use. Both tools are parked-class. No core
 * edits: both bridges declare existing globals, like the read bridges. */

/* A parsed write_register request. `reg` is the (upper-cased) register name as
 * ChangeRegister understands it; `value` is masked to the register's width by
 * ChangeRegister itself. */
struct RegWriteRequest {
    std::string reg;
    uint32_t    value;
};

/* Pure: parse params (`register` string + `value` int/hex-string). Returns false
 * and sets err on a missing/invalid field. */
bool parse_reg_write_request(const Json &params, RegWriteRequest &req, std::string &err);

/* Pure: format the outcome of a register write as compact JSON. */
Json format_reg_write(const RegWriteRequest &req, bool ok);

/* Bridge (mcp_registers.cpp): write the register via the debugger's ChangeRegister.
 * Returns false if the register name was not recognised. Runs on the emulator
 * thread while parked. */
bool write_register(const RegWriteRequest &req);

/* A parsed, already-bounds-checked write_memory request. Address fields mirror
 * read_memory (segmented seg:off / virtual lin / physical phys). Each value in
 * `values` is written at successive `width`-byte slots (width ∈ {1,2,4}); the
 * total byte count is capped at MCP_READMEM_MAX so a write does no more work than
 * a max read. */
struct MemWriteRequest {
    AddrSpace             space;
    uint16_t              seg;
    uint32_t              off;     /* off / lin / phys */
    int                   width;   /* 1, 2 or 4 */
    std::vector<uint32_t> values;
};

/* Filled by the bridge. `addr_valid` is false only when a SPACE_SEGMENTED
 * selector does not resolve. `written` is the count of values actually written
 * (it stops early on a faulting/unmapped slot, with `fault` set); `bytes` is the
 * byte total written. */
struct MemWriteResult {
    bool     addr_valid;
    uint64_t addr;
    size_t   written;
    size_t   bytes;
    bool     fault;
};

/* Pure: parse params (space + addr fields like read_memory, `width` default 1,
 * required non-empty `values` array of ints/hex-strings). Returns false and sets
 * err on a missing/invalid field or when values·width exceeds MCP_READMEM_MAX. */
bool parse_mem_write_request(const Json &params, MemWriteRequest &req, std::string &err);

/* Pure: format the outcome of a memory write as compact JSON. */
Json format_mem_write(const MemWriteRequest &req, const MemWriteResult &out);

/* Bridge (mcp_memory.cpp): write the values into the requested space. Runs on the
 * emulator thread while parked. */
void write_memory(const MemWriteRequest &req, MemWriteResult &out);

/* ---- input injection (Slice 8) ------------------------------------------
 *
 * send_keys / type_text / mouse. All run-class (serviced at the GFX_Events frame
 * tick while the guest free-runs). Same reader/formatter split as the other
 * slices: param parsing + JSON formatting are pure (here / mcp_protocol.cpp,
 * unit-tested with no boot); the actual injection lives in the bridge
 * mcp_input.cpp. No core edits: the bridge reuses existing public APIs
 * (KEYBOARD_AddKey, Mouse_*), like the Slice 3/4/7 bridges.
 *
 * Deviations from the build-plan sketch, with justification:
 *  - type_text does NOT use MAPPER_AutoType. That helper spawns a background
 *    std::thread (the mapper's Typer) which mutates keyboard state OFF the
 *    emulator thread — incompatible with this fork's single-threaded discipline
 *    and dependent on the SDL mapper being initialized (fragile headless).
 *    Instead the decoded key transitions are queued and fed a few per frame on
 *    the emulator thread (MCP_InputFrameService), respecting the keyboard
 *    controller buffer (KEYBOARD_BufferSpaceAvail) — single-threaded and paced.
 *  - mouse uses the direct Mouse_* APIs rather than GFX_EventsMouseProcess,
 *    which is window/clip/SDL-event dependent (Win32-centric) and unsuitable
 *    headless.
 */

/* Input-side caps (bound the work a single input call enqueues). */
static const size_t MCP_KEYS_MAX = 64;   /* send_keys transitions per call    */
static const size_t MCP_TYPE_MAX = 256;  /* type_text characters per call      */

/* One keyboard key transition. `kbd` is the KBD_KEYS integer value (the bridge
 * casts it back to the core enum); `down` true = press, false = release. Using
 * ints keeps the public header free of the core keyboard.h enum. */
struct KeyEvent { int kbd; bool down; };

/* Pure: map a key name ("a"/"enter"/"leftctrl"/"f1"/"space"/...) to its KBD_KEYS
 * integer; accepts a few aliases (return/del/ctrl/...). Returns false on an
 * unknown name. Defined in mcp_protocol.cpp (includes keyboard.h for the enum). */
bool kbd_key_from_name(const std::string &name, int &kbd);

/* Pure: map a printable ASCII char to a US-layout key + whether shift is needed.
 * Returns false for chars with no key. */
bool ascii_to_key(char c, int &kbd, bool &shift);

/* Pure: parse a send_keys request. `keys` is a non-empty array whose entries are
 * either a string key name (expands to a press+release tap) or an object
 * {key, down} for an explicit transition (so chords like ctrl+c are expressible).
 * Total transitions are capped at MCP_KEYS_MAX. Returns false + err on a
 * missing/invalid field or unknown key name. */
bool parse_send_keys_request(const Json &params, std::vector<KeyEvent> &out, std::string &err);

/* Pure: format the outcome of a send_keys call. */
Json format_send_keys(size_t transitions);

/* Pure: parse a type_text request. Required `text` string, capped at
 * MCP_TYPE_MAX chars; decoded to KeyEvents (leftshift bracketing for shifted
 * chars). Undecodable chars are skipped and counted in `skipped`. */
bool parse_type_text_request(const Json &params, std::vector<KeyEvent> &out,
                             size_t &chars, size_t &skipped, std::string &err);

/* Pure: format the outcome of a type_text call. */
Json format_type_text(size_t chars, size_t skipped);

/* Mouse: exactly one action per call. */
enum MouseAction { MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_CLICK, MOUSE_WHEEL };
struct MouseRequest {
    MouseAction action;
    int dx, dy;   /* MOUSE_MOVE: relative motion (pixels)         */
    int button;   /* MOUSE_DOWN/UP/CLICK: 0=left 1=right 2=middle */
    int wheel;    /* MOUSE_WHEEL: signed amount                   */
};

/* Pure: parse a mouse request (`action` ∈ move|down|up|click|wheel + its
 * fields). Returns false + err on a missing/invalid field. */
bool parse_mouse_request(const Json &params, MouseRequest &req, std::string &err);

/* Pure: format the outcome of a mouse call. */
Json format_mouse(const MouseRequest &req);

/* Bridges (mcp_input.cpp): run on the emulator thread at the frame tick.
 * send_keys applies the transitions immediately; type_text_enqueue appends them
 * to the frame-paced queue (drained by MCP_InputFrameService); mouse_action
 * drives the emulated mouse. */
void send_keys(const std::vector<KeyEvent> &events);
void type_text_enqueue(const std::vector<KeyEvent> &events);
void mouse_action(const MouseRequest &req);

/* ---- screen (Slice 9) ----------------------------------------------------
 *
 * read_screen / screen_hash. Both run-class (serviced at the GFX_Events frame
 * tick — see classify): the visible screen is read while the guest runs. Same
 * reader/formatter split as the other slices: the reading of BIOS/video state
 * lives in the bridge mcp_screen.cpp; the JSON formatting and the hash primitive
 * are pure here (unit-tested with no boot). No core edits — the bridge reuses
 * existing globals/APIs (CurMode, ReadCharAttr, real_read*, vga.mem, render),
 * like the Slice 3/4/7/8 bridges.
 *
 * read_screen returns the text grid (int10 ReadCharAttr) when CurMode is a text
 * mode; in graphics modes it reports is_text=false and an empty grid (use
 * take_screenshot, Slice 10). screen_hash is a cheap change-detection
 * fingerprint: the char+attribute cells in text modes, render.src dimensions
 * plus a bounded scan of guest video memory in graphics modes — so it changes
 * when the screen changes and is stable otherwise, even headless. */

/* Defensive grid caps (a text mode should be ≤ 132×60; enforce_max_payload is
 * the hard ceiling). */
static const int    MCP_SCREEN_MAX_COLS = 255;
static const int    MCP_SCREEN_MAX_ROWS = 100;
/* Bound the graphics-mode video-memory scan that feeds the fingerprint. */
static const size_t MCP_SCREENHASH_SCAN_MAX = 256 * 1024;

/* A plain-data snapshot of the text screen grid (read_screen). `chars` holds
 * cols*rows code-page bytes, row-major. For non-text modes is_text is false and
 * cols/rows/chars are empty. */
struct ScreenSnapshot {
    bool                 is_text;
    int                  mode;   /* CurMode->mode */
    int                  cols;
    int                  rows;
    std::vector<uint8_t> chars;
};

/* Bridge (mcp_screen.cpp): read the text grid. Touches BIOS data area + video
 * memory, so it runs on the emulator thread (the frame tick). */
void read_screen(ScreenSnapshot &out);

/* Pure: format the grid as bounded JSON. Each cell byte is rendered as printable
 * ASCII (0x20..0x7e) or '.', so every line is valid UTF-8 and token-cheap. */
Json format_screen(const ScreenSnapshot &out);

/* A cheap screen fingerprint for change detection (screen_hash). */
struct ScreenHash {
    bool     is_text;
    int      mode;
    int      cols;          /* text: grid cols; graphics: render.src.width  */
    int      rows;          /* text: grid rows; graphics: render.src.height */
    uint64_t hash;
};

/* Pure: FNV-1a 64-bit over a byte buffer — the fingerprint primitive. */
uint64_t fnv1a64(const uint8_t *data, size_t len);

/* Bridge (mcp_screen.cpp): compute the fingerprint on the emulator thread. */
void screen_hash(ScreenHash &out);

/* Pure: format a fingerprint as compact JSON (hash as a 0x-prefixed 16-hex
 * string so no precision is lost crossing JSON's double). */
Json format_screen_hash(const ScreenHash &out);

/* ---- take_screenshot (Slice 10) -----------------------------------------
 *
 * Full-fidelity graphics capture on demand: trigger a PNG of the current frame
 * and return its path + metadata (never raw pixels). Run-class — it rides the
 * RENDER/CAPTURE_AddImage path that emits a frame at the GFX_Events tick (the
 * same path Slice 1 verified under the dummy video driver).
 *
 * The capture is inherently *asynchronous*: setting CAPTURE_IMAGE only flushes a
 * PNG on the next rendered frame, after the handler has returned. Rather than
 * block the emulator thread (forbidden — handlers must be non-blocking), the
 * bridge runs a tiny per-frame state machine: the first service call snapshots
 * the capture dir and arms the capture; later calls watch for the new PNG. While
 * pending, dispatch returns defer_sentinel() and the server re-queues the request
 * to retry next frame, all under the client's 5 s wait. No core edit — the bridge
 * reuses CaptureState/CAPTURE_IMAGE (hardware.h), capturedir, CurMode and
 * render.src, like the Slice 9 screen bridge. */

enum ShotStatus { SHOT_PENDING, SHOT_READY, SHOT_ERROR };

/* A completed capture. width/height/bytes come from the written PNG (IHDR +
 * file size; falls back to render.src dims). mode/is_text are the source video
 * mode at trigger time. On SHOT_ERROR only `error` is meaningful. */
struct ScreenshotResult {
    std::string path;     /* absolute path to the written PNG       */
    int         width;
    int         height;
    long        bytes;    /* PNG file size on disk                   */
    int         mode;     /* CurMode->mode at capture                */
    bool        is_text;  /* source was a text mode                  */
    std::string error;    /* set only when status == SHOT_ERROR      */
};

/* Bridge (mcp_screenshot.cpp): advance the async capture one frame at a time on
 * the emulator thread. Returns SHOT_PENDING until the PNG is written (the
 * dispatcher re-queues via defer_sentinel), then SHOT_READY (out filled) or
 * SHOT_ERROR (out.error set: no capture dir, or no frame within the deadline). */
ShotStatus screenshot_service(ScreenshotResult &out);

/* Pure: format a completed capture as compact JSON. */
Json format_screenshot(const ScreenshotResult &out);

/* ---- memory scanner (Slice 11) ------------------------------------------
 *
 * scan_start / scan_filter / scan_results — a "cheat-engine" progressive search
 * for variables. Wraps the debugger's MEMFIND/MEMS (the single session-global
 * MEMFINDInstance): start snapshots a seg:off range; filter narrows the
 * candidate set with one of == != > < >= <= (or, with use_prev, against each
 * cell's start snapshot); results are bounded/paginated. Same reader/formatter
 * split as the other parked slices: the param parsing and JSON formatting are
 * pure (here / mcp_protocol.cpp, unit-tested with no boot), while the scan touches
 * emulator memory and lives in the bridge mcp_scan.cpp, which calls the single
 * Slice 11 core edit (the MCP_Scan* functions in debug.cpp). All three tools are
 * parked-class. The scanner is session-global state: a new scan_start replaces
 * any in-progress scan. */

/* Cap the scanned range so the snapshot/scan work per call stays bounded. */
static const size_t MCP_SCAN_RANGE_MAX = 1024 * 1024; /* 1 MiB range per scan */

/* Comparison operator. The integer values match debug.cpp's MEMFinder::opType so
 * the value crossing the bridge is unambiguous. */
enum ScanOp { SCAN_EQ = 0, SCAN_GT, SCAN_LT, SCAN_NE, SCAN_GE, SCAN_LE };

/* A parsed scan_start request. `width` ∈ {1,2,4} (default 1) is the element size;
 * `range` is in bytes, clamped to [1, MCP_SCAN_RANGE_MAX]. */
struct ScanStartRequest {
    uint16_t seg;
    uint32_t off;
    uint32_t range;   /* bytes */
    int      width;   /* 1, 2 or 4 */
};

/* A parsed scan_filter request. When `use_prev` is true the comparison is against
 * each cell's start-snapshot value and `value` is ignored. */
struct ScanFilterRequest {
    ScanOp   op;
    bool     use_prev;
    uint32_t value;
};

/* Read-back scanner state. `active` false means there is no scan; the other
 * fields are then meaningless. `matches` is the current candidate count. */
struct ScanState {
    bool     active;
    uint16_t seg;
    uint32_t off;
    uint32_t base_linear;
    int      width;
    uint32_t range;       /* bytes */
    uint32_t matches;
    uint32_t iterations;
};

/* One scan match: an address still in the candidate set + its current value. */
struct ScanMatch {
    uint16_t seg;
    uint32_t off;
    uint32_t lin;
    uint32_t value;
};

const char *scan_op_name(ScanOp op);                  /* "=="/">"/"<"/...      */
bool        parse_scan_op(const std::string &s, ScanOp &out);

/* Pure: parse params. scan_start needs seg+off+range (width default 1); range is
 * clamped to MCP_SCAN_RANGE_MAX. scan_filter needs `op`; `value` is required
 * unless `use_prev` is true. Return false + err on a missing/invalid field. */
bool parse_scan_start_request(const Json &params, ScanStartRequest &req, std::string &err);
bool parse_scan_filter_request(const Json &params, ScanFilterRequest &req, std::string &err);

/* Pure: format the scanner state (scan_start / scan_filter reply) and a bounded
 * page of matches (scan_results), capped at `max` with start/count/total/truncated. */
Json format_scan_state(const ScanState &st);
Json format_scan_results(const ScanState &st, const std::vector<ScanMatch> &matches,
                         uint32_t start, uint32_t total, size_t max);

/* Bridges (mcp_scan.cpp): run on the emulator thread while parked.
 * scan_start returns 0 on success or a negative error code (-2 empty range,
 * -3 selector did not resolve, -4 range exceeds configured memory, -5 bad width)
 * and fills `st`. scan_filter returns the new match count or <0 (-1 no active
 * scan, -2 value wider than the element size) and refreshes `st`. scan_state
 * reads the current state. scan_results fills up to `max` matches starting at
 * match-index `start` and returns the total match count. */
int      scan_start(const ScanStartRequest &req, ScanState &st);
long     scan_filter(const ScanFilterRequest &req, ScanState &st);
void     scan_state(ScanState &st);
uint32_t scan_results(uint32_t start, size_t max, std::vector<ScanMatch> &out);

/* ---- lifecycle (Slice 12) -----------------------------------------------
 *
 * reset / quit — in-session lifecycle control. Both are CLS_ANY (serviceable in
 * either execution state). Same reader/formatter split as the other slices: the
 * JSON formatting is pure here (unit-tested with no boot), while the actual
 * action is recorded by the bridge mcp_lifecycle.cpp and *performed on the
 * emulator thread after the reply has been flushed* — the reset/quit throw
 * (int(3) reboot / int(0) kill switch, the same signals the menu Reset and the
 * quit path use) must propagate up to DOSBOX_RunMachine, not interrupt the
 * dispatcher mid-reply. So the handler returns a normal success reply and the
 * action fires a few frames later via MCP_LifecycleService (called from
 * MCP_GFXFrameService, the existing core call site). No new core edit: the throw
 * originates in the MCP module, and reset-while-parked first disengages the
 * debugger by reusing the Slice 5 core edit MCP_DebugExec (continue). */
enum LifecycleOp { LIFE_RESET, LIFE_QUIT };

/* Bridge (mcp_lifecycle.cpp): record a pending reset/quit. Runs on the emulator
 * thread (in the dispatcher). The action is deferred to MCP_LifecycleService so
 * the success reply reaches the client before the machine reboots / exits. */
void lifecycle_request(LifecycleOp op);

/* Pure: format the lifecycle acknowledgement reply. */
Json format_lifecycle(LifecycleOp op);

/* ---- debugger_command passthrough (Slice 13) ----------------------------
 *
 * The escape hatch: run any of the debugger's ~110 ParseCommand commands and
 * return its captured DEBUG_ShowMsg output, truncated to MCP_PASSTHROUGH_MAX
 * (16 KiB). Parked-class. Same reader/formatter split as the other slices: the
 * param parsing, JSON formatting, and the capped-append helper are pure here
 * (unit-tested with no boot); the actual ParseCommand call + output capture live
 * in the bridge mcp_passthrough.cpp, which relies on the single Slice 13 core
 * edit (the capture hook in DEBUG_ShowMsg, debug_gui.cpp). The reply carries
 * whatever the command printed; execution-affecting commands (RUN/G/…) are
 * better driven by the dedicated step/continue tools — see docs/MCP_MANUAL.md. */

/* Pure: parse a debugger_command request (required, non-empty `command` string).
 * Returns false + err on a missing/invalid/empty field. */
bool parse_debugger_command_request(const Json &params, std::string &cmd, std::string &err);

/* Pure: append `line` then '\n' to `acc`, capping the total length at
 * MCP_PASSTHROUGH_MAX. Once the cap is reached further appends are dropped and
 * `truncated` is set. The bridge's per-line capture hook uses this, so the
 * truncation is unit-testable without a debugger boot. */
void passthrough_append(std::string &acc, const std::string &line, bool &truncated);

/* Pure: format the passthrough result as compact, bounded JSON
 * ({command, recognized, truncated, output}). */
Json format_debugger_command(const std::string &cmd, const std::string &output,
                             bool truncated, bool recognized);

/* Bridge (mcp_passthrough.cpp): run `cmd` through the debugger's ParseCommand on
 * the emulator thread while parked, capturing the emitted DEBUG_ShowMsg lines
 * (capped via passthrough_append). Returns ParseCommand's recognized flag; `out`
 * and `truncated` are the captured text and whether it was clipped. */
bool run_debugger_command(const std::string &cmd, std::string &out, bool &truncated);

/* Internal sentinel reply meaning "not ready — re-queue and retry next frame".
 * Returned by dispatch for an in-progress take_screenshot; the server re-queues
 * the request instead of replying. Never sent to a client (it is not valid
 * JSON, so it can never collide with a real response line). */
const std::string &defer_sentinel();

/* Pure dispatch: given a parsed request and the current execution state, return
 * the full response line. Handles unknown methods, mode-mismatch fast-reject,
 * and the ping / server_info handlers. State-touching handlers (later slices)
 * return MCP_ERR_NOT_IMPLEMENTED for now. An in-progress take_screenshot returns
 * defer_sentinel() (the server re-queues it; see above). */
std::string dispatch(const std::string &method, const Json &params,
                     const Json &id, ExecState state);

} // namespace mcp

#endif /* C_MCP */

#endif /* DOSBOX_MCP_PROTOCOL_H */
