/*
 *  DOSBox-X MCP — protocol layer (impl). See mcp_protocol.h.
 *  Compiled only under --enable-mcp.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <cstdio>
#include <cstring>

/* Provided by the rest of the MCP module (mcp.cpp / mcp_server.cpp). Declared
 * here rather than included to keep this layer transport-agnostic. */
const char *MCP_Version(void);
int         MCP_ServerPort(void);

namespace mcp {

const char *state_name(ExecState s) {
    return s == STATE_PARKED ? "parked" : "running";
}

/* Static method->class table. Lists every method name the toolset will expose
 * (across all slices) so mode-mismatch fast-reject is correct from day one,
 * even before a handler exists. Adding a tool means adding its class here. */
ReqClass classify(const std::string &m) {
    /* any: serviceable in either execution state */
    if (m == "ping" || m == "server_info" || m == "reset" || m == "quit")
        return CLS_ANY;
    /* run: serviced at the GFX_Events frame tick (game free-running) */
    if (m == "break" || m == "send_keys" || m == "type_text" || m == "mouse" ||
        m == "read_screen" || m == "screen_hash" || m == "take_screenshot")
        return CLS_RUN;
    /* parked: serviced in the debugger loop (CPU parked) */
    if (m == "read_registers" || m == "read_memory" || m == "disassemble" ||
        m == "step" || m == "step_over" || m == "continue" ||
        m == "write_register" || m == "write_memory" ||
        m == "breakpoint_list" || m == "breakpoint_add" || m == "breakpoint_delete" ||
        m == "scan_start" || m == "scan_filter" || m == "scan_results" ||
        m == "debugger_command")
        return CLS_PARKED;
    return CLS_UNKNOWN;
}

bool mode_matches(ReqClass cls, ExecState state) {
    switch (cls) {
        case CLS_ANY:    return true;
        case CLS_RUN:    return state == STATE_RUNNING;
        case CLS_PARKED: return state == STATE_PARKED;
        default:         return false;
    }
}

// -- response builders -----------------------------------------------------

namespace {
Json envelope(const Json &id) {
    Json r = Json::object();
    r.set("jsonrpc", Json::str("2.0"));
    r.set("id", id);
    return r;
}
} // namespace

std::string make_result(const Json &id, const Json &result) {
    Json r = envelope(id);
    r.set("result", result);
    return r.serialize();
}

std::string make_error(const Json &id, int code, const std::string &message) {
    Json err = Json::object();
    err.set("code", Json::integer(code));
    err.set("message", Json::str(message));
    Json r = envelope(id);
    r.set("error", err);
    return r.serialize();
}

std::string make_error(const Json &id, int code, const std::string &message, const Json &data) {
    Json err = Json::object();
    err.set("code", Json::integer(code));
    err.set("message", Json::str(message));
    err.set("data", data);
    Json r = envelope(id);
    r.set("error", err);
    return r.serialize();
}

std::string make_mismatch_error(const Json &id, ExecState current) {
    /* Carry the current execution state so the client can decide to break /
     * continue first, per "Protocol semantics". */
    Json data = Json::object();
    data.set("state", Json::str(state_name(current)));
    return make_error(id, MCP_ERR_MODE_MISMATCH,
                      "request targets the wrong execution state", data);
}

std::string make_timeout_error(const Json &id) {
    return make_error(id, MCP_ERR_TIMEOUT, "request timed out");
}

std::string make_busy_error() {
    return make_error(Json::null(), MCP_ERR_BUSY,
                      "server busy: single client only");
}

std::string enforce_max_payload(const Json &id, const std::string &body) {
    if (body.size() <= MCP_MAX_PAYLOAD) return body;
    return make_error(id, MCP_ERR_TOO_LARGE, "response exceeded maximum payload size");
}

// -- handlers --------------------------------------------------------------

namespace {

Json handle_ping(ExecState state) {
    Json r = Json::object();
    r.set("pong", Json::boolean(true));
    r.set("state", Json::str(state_name(state)));
    return r;
}

Json handle_server_info(ExecState state) {
    Json r = Json::object();
    r.set("name", Json::str("dosbox-x-mcp"));
    r.set("version", Json::str(MCP_Version()));
    r.set("protocol", Json::str("jsonrpc-2.0-ndjson"));
    r.set("transport", Json::str("tcp"));
    r.set("bind", Json::str(MCP_BIND_ADDRESS));
    r.set("port", Json::integer(MCP_ServerPort()));
    r.set("single_client", Json::boolean(true));
    r.set("state", Json::str(state_name(state)));
    r.set("max_payload", Json::integer((long long)MCP_MAX_PAYLOAD));
    return r;
}

/* Fixed-width lowercase hex with 0x prefix, so register values are unambiguous
 * and the formatted output has a stable size. */
std::string hex(uint32_t v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*x", digits, (unsigned)v);
    return buf;
}

const char *cpu_mode(const RegisterSnapshot &s) {
    if (!s.pmode)  return "real";
    if (s.vm86)    return "vm86";
    return s.code_big ? "pr32" : "pr16";
}

} // namespace

Json format_registers(const RegisterSnapshot &s) {
    Json r = Json::object();
    r.set("eax", Json::str(hex(s.eax, 8)));
    r.set("ebx", Json::str(hex(s.ebx, 8)));
    r.set("ecx", Json::str(hex(s.ecx, 8)));
    r.set("edx", Json::str(hex(s.edx, 8)));
    r.set("esi", Json::str(hex(s.esi, 8)));
    r.set("edi", Json::str(hex(s.edi, 8)));
    r.set("ebp", Json::str(hex(s.ebp, 8)));
    r.set("esp", Json::str(hex(s.esp, 8)));
    r.set("eip", Json::str(hex(s.eip, 8)));

    r.set("cs", Json::str(hex(s.cs, 4)));
    r.set("ds", Json::str(hex(s.ds, 4)));
    r.set("es", Json::str(hex(s.es, 4)));
    r.set("fs", Json::str(hex(s.fs, 4)));
    r.set("gs", Json::str(hex(s.gs, 4)));
    r.set("ss", Json::str(hex(s.ss, 4)));

    r.set("eflags", Json::str(hex(s.eflags, 8)));

    /* Individual flag bits, mirroring the debugger's register view (debug.cpp:1182). */
    Json f = Json::object();
    f.set("CF",   Json::integer((s.eflags & 0x0001) ? 1 : 0));
    f.set("PF",   Json::integer((s.eflags & 0x0004) ? 1 : 0));
    f.set("AF",   Json::integer((s.eflags & 0x0010) ? 1 : 0));
    f.set("ZF",   Json::integer((s.eflags & 0x0040) ? 1 : 0));
    f.set("SF",   Json::integer((s.eflags & 0x0080) ? 1 : 0));
    f.set("TF",   Json::integer((s.eflags & 0x0100) ? 1 : 0));
    f.set("IF",   Json::integer((s.eflags & 0x0200) ? 1 : 0));
    f.set("DF",   Json::integer((s.eflags & 0x0400) ? 1 : 0));
    f.set("OF",   Json::integer((s.eflags & 0x0800) ? 1 : 0));
    f.set("IOPL", Json::integer((s.eflags & 0x3000) >> 12));
    r.set("flags", f);

    r.set("mode", Json::str(cpu_mode(s)));
    r.set("cpl", Json::integer((long long)s.cpl));
    return r;
}

std::string dispatch(const std::string &method, const Json &params,
                     const Json &id, ExecState state) {
    (void)params;
    ReqClass cls = classify(method);
    if (cls == CLS_UNKNOWN)
        return make_error(id, MCP_ERR_METHOD_NOT_FOUND, "method not found: " + method);
    if (!mode_matches(cls, state))
        return make_mismatch_error(id, state);

    if (method == "ping")
        return enforce_max_payload(id, make_result(id, handle_ping(state)));
    if (method == "server_info")
        return enforce_max_payload(id, make_result(id, handle_server_info(state)));
    if (method == "read_registers") {
        /* Parked-class: mode_matches already guaranteed STATE_PARKED above, so
         * the CPU is stopped and the snapshot is coherent. */
        RegisterSnapshot snap;
        snapshot_registers(snap);
        return enforce_max_payload(id, make_result(id, format_registers(snap)));
    }

    /* Known method whose handler arrives in a later slice. */
    return make_error(id, MCP_ERR_NOT_IMPLEMENTED, "method not implemented yet: " + method);
}

} // namespace mcp

#endif /* C_MCP */
