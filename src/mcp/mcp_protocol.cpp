/*
 *  DOSBox-X MCP — protocol layer (impl). See mcp_protocol.h.
 *  Compiled only under --enable-mcp.
 */

#include "mcp_protocol.h"

#if C_MCP

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

} // namespace

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

    /* Known method whose handler arrives in a later slice. */
    return make_error(id, MCP_ERR_NOT_IMPLEMENTED, "method not implemented yet: " + method);
}

} // namespace mcp

#endif /* C_MCP */
