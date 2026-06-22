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

/* Pure dispatch: given a parsed request and the current execution state, return
 * the full response line. Handles unknown methods, mode-mismatch fast-reject,
 * and the ping / server_info handlers. State-touching handlers (later slices)
 * return MCP_ERR_NOT_IMPLEMENTED for now. */
std::string dispatch(const std::string &method, const Json &params,
                     const Json &id, ExecState state);

} // namespace mcp

#endif /* C_MCP */

#endif /* DOSBOX_MCP_PROTOCOL_H */
