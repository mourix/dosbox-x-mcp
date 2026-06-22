/*
 *  DOSBox-X MCP — protocol layer (impl). See mcp_protocol.h.
 *  Compiled only under --enable-mcp.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <cstdio>
#include <cstdlib>
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

/* Lowercase two-char hex per byte; unreadable bytes render as "??" so the dump
 * is self-describing and stays a fixed 2 chars/byte (bounded by construction). */
std::string hex_bytes(const std::vector<uint8_t> &bytes,
                      const std::vector<bool> &readable) {
    std::string s;
    s.reserve(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        if (i < readable.size() && !readable[i]) { s += "??"; continue; }
        char b[3];
        std::snprintf(b, sizeof(b), "%02x", (unsigned)bytes[i]);
        s += b;
    }
    return s;
}

/* Accept a JSON integer or a hex ("0x1f"/"0X1F") / decimal ("31") string. Pure;
 * used by the param parsers so the client may pass either form. */
bool json_to_u32(const Json *v, uint32_t &out) {
    if (v == nullptr) return false;
    if (v->isNumber()) { out = (uint32_t)v->asInt(); return true; }
    if (v->isString()) {
        const std::string &s = v->asString();
        if (s.empty()) return false;
        char *end = nullptr;
        unsigned long val = std::strtoul(s.c_str(), &end, 0); /* 0 -> auto-base */
        if (end == s.c_str() || *end != '\0') return false;
        out = (uint32_t)val;
        return true;
    }
    return false;
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

// -- read_memory (Slice 4) -------------------------------------------------

bool parse_mem_request(const Json &params, MemReadRequest &req, std::string &err) {
    req.space = SPACE_SEGMENTED;
    req.seg = 0; req.off = 0;

    const Json *spc = params.find("space");
    if (spc != nullptr) {
        if (!spc->isString()) { err = "space must be a string"; return false; }
        const std::string &s = spc->asString();
        if      (s == "segmented") req.space = SPACE_SEGMENTED;
        else if (s == "virtual")   req.space = SPACE_VIRTUAL;
        else if (s == "physical")  req.space = SPACE_PHYSICAL;
        else { err = "unknown space (want segmented|virtual|physical): " + s; return false; }
    }

    if (req.space == SPACE_SEGMENTED) {
        uint32_t seg;
        if (!json_to_u32(params.find("seg"), seg)) { err = "missing/invalid seg"; return false; }
        req.seg = (uint16_t)seg;
        if (!json_to_u32(params.find("off"), req.off)) { err = "missing/invalid off"; return false; }
    } else if (req.space == SPACE_VIRTUAL) {
        if (!json_to_u32(params.find("lin"), req.off)) { err = "missing/invalid lin"; return false; }
    } else { /* SPACE_PHYSICAL */
        if (!json_to_u32(params.find("phys"), req.off)) { err = "missing/invalid phys"; return false; }
    }

    uint32_t len = (uint32_t)MCP_READMEM_DEFAULT;
    if (params.has("len") && !json_to_u32(params.find("len"), len)) {
        err = "invalid len"; return false;
    }
    if (len == 0) { err = "len must be >= 1"; return false; }
    req.requested_len = len;
    req.len = len > (uint32_t)MCP_READMEM_MAX ? (uint32_t)MCP_READMEM_MAX : len;
    return true;
}

Json format_memory(const MemReadRequest &req, const MemReadResult &out) {
    Json r = Json::object();
    const char *space = req.space == SPACE_SEGMENTED ? "segmented" :
                        req.space == SPACE_VIRTUAL   ? "virtual" : "physical";
    r.set("space", Json::str(space));
    if (req.space == SPACE_SEGMENTED) {
        r.set("seg", Json::str(hex(req.seg, 4)));
        r.set("off", Json::str(hex(req.off, 8)));
    } else if (req.space == SPACE_VIRTUAL) {
        r.set("lin", Json::str(hex(req.off, 8)));
    } else {
        r.set("phys", Json::str(hex(req.off, 8)));
    }

    r.set("addr_valid", Json::boolean(out.addr_valid));
    if (out.addr_valid)
        r.set("addr", Json::str(hex((uint32_t)out.addr, 8)));

    size_t unreadable = 0;
    for (size_t i = 0; i < out.readable.size(); i++) if (!out.readable[i]) unreadable++;

    r.set("len", Json::integer((long long)out.bytes.size()));
    r.set("hex", Json::str(hex_bytes(out.bytes, out.readable)));
    r.set("unreadable", Json::integer((long long)unreadable));

    /* Pagination: if the caller asked for more than the per-call cap, report the
     * truncation and the next offset so they can continue (Response bounds). */
    bool truncated = req.requested_len > req.len;
    r.set("truncated", Json::boolean(truncated));
    if (truncated) {
        if (req.space == SPACE_SEGMENTED)
            r.set("next_off", Json::str(hex(req.off + req.len, 8)));
        else if (req.space == SPACE_VIRTUAL)
            r.set("next_lin", Json::str(hex(req.off + req.len, 8)));
        else
            r.set("next_phys", Json::str(hex(req.off + req.len, 8)));
    }
    return r;
}

// -- disassemble (Slice 4) -------------------------------------------------

bool parse_disasm_request(const Json &params, DisasmRequest &req, std::string &err) {
    uint32_t seg;
    if (!json_to_u32(params.find("seg"), seg)) { err = "missing/invalid seg"; return false; }
    req.seg = (uint16_t)seg;
    if (!json_to_u32(params.find("off"), req.off)) { err = "missing/invalid off"; return false; }

    uint32_t count = (uint32_t)MCP_DISASM_DEFAULT;
    if (params.has("count") && !json_to_u32(params.find("count"), count)) {
        err = "invalid count"; return false;
    }
    if (count == 0) { err = "count must be >= 1"; return false; }
    req.requested_count = count;
    req.count = count > (uint32_t)MCP_DISASM_MAX ? (uint32_t)MCP_DISASM_MAX : count;

    req.have_big = false;
    req.big = false;
    const Json *big = params.find("big");
    if (big != nullptr) {
        if (!big->isBool()) { err = "big must be a boolean"; return false; }
        req.have_big = true;
        req.big = big->asBool();
    }
    return true;
}

Json format_disasm(const DisasmRequest &req, const DisasmResult &out) {
    Json r = Json::object();
    r.set("seg", Json::str(hex(req.seg, 4)));
    r.set("big", Json::boolean(out.big));
    r.set("addr_valid", Json::boolean(out.addr_valid));

    Json arr = Json::array();
    for (size_t i = 0; i < out.insns.size(); i++) {
        const DisasmInsn &ins = out.insns[i];
        Json o = Json::object();
        o.set("off",  Json::str(hex(ins.off, 8)));
        o.set("addr", Json::str(hex((uint32_t)ins.addr, 8)));
        o.set("bytes", Json::str(hex_bytes(ins.bytes, ins.readable)));
        o.set("text", Json::str(ins.text));
        arr.push(o);
    }
    r.set("count", Json::integer((long long)out.insns.size()));
    r.set("insns", arr);

    bool truncated = req.requested_count > req.count;
    r.set("truncated", Json::boolean(truncated));
    return r;
}

// -- execution control (Slice 5) -------------------------------------------

namespace {
const char *exec_op_name(ExecOp op) {
    switch (op) {
        case EXEC_STEP:      return "step";
        case EXEC_STEP_OVER: return "step_over";
        case EXEC_CONTINUE:  return "continue";
        case EXEC_BREAK:     return "break";
    }
    return "step";
}
} // namespace

Json format_exec(ExecOp op, const ExecResult &out) {
    Json r = Json::object();
    r.set("op", Json::str(exec_op_name(op)));
    r.set("state", Json::str(state_name(out.state)));
    /* resumed=true: the guest was released to free-run; poll ping until parked.
     * resumed=false: still parked; cs:eip below are the new stop. */
    r.set("resumed", Json::boolean(out.resumed));
    r.set("ran", Json::integer((long long)out.ran));
    r.set("cs", Json::str(hex(out.cs, 4)));
    r.set("eip", Json::str(hex(out.eip, 8)));
    return r;
}

// -- breakpoints (Slice 6) -------------------------------------------------

const char *bp_type_name(BpType t) {
    switch (t) {
        case BPT_EXEC:        return "exec";
        case BPT_INT:         return "int";
        case BPT_MEM:         return "mem";
        case BPT_MEM_PROT:    return "mem_prot";
        case BPT_MEM_LINEAR:  return "mem_linear";
        case BPT_MEM_FREEZE:  return "mem_freeze";
        default:              return "unknown";
    }
}

bool parse_bp_type(const std::string &s, BpType &out) {
    if      (s == "exec")       out = BPT_EXEC;
    else if (s == "int")        out = BPT_INT;
    else if (s == "mem")        out = BPT_MEM;
    else if (s == "mem_prot")   out = BPT_MEM_PROT;
    else if (s == "mem_linear") out = BPT_MEM_LINEAR;
    else if (s == "mem_freeze") out = BPT_MEM_FREEZE;
    else return false;
    return true;
}

bool parse_bp_add_request(const Json &params, BpAddRequest &req, std::string &err) {
    req.type = BPT_EXEC;
    req.seg = 0; req.off = 0; req.intnr = 0;
    req.ah = -1; req.al = -1; req.once = false;

    const Json *ty = params.find("type");
    if (ty != nullptr) {
        if (!ty->isString()) { err = "type must be a string"; return false; }
        if (!parse_bp_type(ty->asString(), req.type)) {
            err = "unknown type (want exec|int|mem|mem_prot|mem_linear|mem_freeze): " + ty->asString();
            return false;
        }
    }

    const Json *once = params.find("once");
    if (once != nullptr) {
        if (!once->isBool()) { err = "once must be a boolean"; return false; }
        req.once = once->asBool();
    }

    if (req.type == BPT_INT) {
        uint32_t v;
        if (!json_to_u32(params.find("int"), v)) { err = "missing/invalid int (interrupt number)"; return false; }
        req.intnr = (uint8_t)v;
        if (params.has("ah")) {
            if (!json_to_u32(params.find("ah"), v)) { err = "invalid ah"; return false; }
            req.ah = (int)(uint8_t)v;
        }
        if (params.has("al")) {
            if (!json_to_u32(params.find("al"), v)) { err = "invalid al"; return false; }
            req.al = (int)(uint8_t)v;
        }
    } else if (req.type == BPT_MEM_LINEAR) {
        if (!json_to_u32(params.find("lin"), req.off)) { err = "missing/invalid lin"; return false; }
    } else { /* exec, mem, mem_prot, mem_freeze: seg:off */
        uint32_t seg;
        if (!json_to_u32(params.find("seg"), seg)) { err = "missing/invalid seg"; return false; }
        req.seg = (uint16_t)seg;
        if (!json_to_u32(params.find("off"), req.off)) { err = "missing/invalid off"; return false; }
    }
    return true;
}

Json format_breakpoint(const BreakpointInfo &bp) {
    Json o = Json::object();
    o.set("index", Json::integer((long long)bp.index));
    o.set("type", Json::str(bp_type_name(bp.type)));
    o.set("active", Json::boolean(bp.active));
    o.set("once", Json::boolean(bp.once));
    if (bp.type == BPT_INT) {
        o.set("int", Json::str(hex(bp.intnr, 2)));
        o.set("ah", bp.ah < 0 ? Json::str("*") : Json::str(hex((uint32_t)bp.ah, 2)));
        o.set("al", bp.al < 0 ? Json::str("*") : Json::str(hex((uint32_t)bp.al, 2)));
    } else if (bp.type == BPT_MEM_LINEAR) {
        o.set("lin", Json::str(hex(bp.off, 8)));
        o.set("value", Json::str(hex((uint32_t)bp.memvalue, 2)));
    } else if (bp.type == BPT_MEM || bp.type == BPT_MEM_PROT || bp.type == BPT_MEM_FREEZE) {
        o.set("seg", Json::str(hex(bp.seg, 4)));
        o.set("off", Json::str(hex(bp.off, 8)));
        o.set("value", Json::str(hex((uint32_t)bp.memvalue, 2)));
    } else { /* exec */
        o.set("seg", Json::str(hex(bp.seg, 4)));
        o.set("off", Json::str(hex(bp.off, 8)));
    }
    return o;
}

Json format_breakpoint_list(const std::vector<BreakpointInfo> &bps, size_t max) {
    Json r = Json::object();
    Json arr = Json::array();
    size_t n = bps.size();
    size_t shown = n > max ? max : n;
    for (size_t i = 0; i < shown; i++) arr.push(format_breakpoint(bps[i]));
    r.set("breakpoints", arr);
    r.set("count", Json::integer((long long)shown));
    r.set("total", Json::integer((long long)n));
    r.set("truncated", Json::boolean(n > max));
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
    if (method == "read_memory") {
        MemReadRequest req;
        std::string perr;
        if (!parse_mem_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        MemReadResult out;
        read_memory(req, out);
        return enforce_max_payload(id, make_result(id, format_memory(req, out)));
    }
    if (method == "disassemble") {
        DisasmRequest req;
        std::string perr;
        if (!parse_disasm_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        DisasmResult out;
        disassemble(req, out);
        return enforce_max_payload(id, make_result(id, format_disasm(req, out)));
    }
    if (method == "step" || method == "step_over" || method == "continue") {
        /* Parked-class: mode_matches guaranteed STATE_PARKED above. */
        ExecOp op = method == "step"      ? EXEC_STEP :
                    method == "step_over" ? EXEC_STEP_OVER : EXEC_CONTINUE;
        ExecResult out;
        exec_control(op, out);
        return enforce_max_payload(id, make_result(id, format_exec(op, out)));
    }
    if (method == "break") {
        /* Run-class: mode_matches guaranteed STATE_RUNNING above, so the guest
         * is free-running and engaging the debugger is valid. */
        ExecResult out;
        exec_control(EXEC_BREAK, out);
        return enforce_max_payload(id, make_result(id, format_exec(EXEC_BREAK, out)));
    }

    if (method == "breakpoint_list") {
        /* Parked-class: mode_matches guaranteed STATE_PARKED above. */
        std::vector<BreakpointInfo> bps;
        bp_list(bps);
        return enforce_max_payload(id, make_result(id, format_breakpoint_list(bps, MCP_LIST_MAX)));
    }
    if (method == "breakpoint_add") {
        BpAddRequest req;
        std::string perr;
        if (!parse_bp_add_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        int idx = bp_add(req);
        if (idx < 0)
            return make_error(id, MCP_ERR_INTERNAL, "failed to add breakpoint");
        Json r = Json::object();
        r.set("added", Json::boolean(true));
        r.set("index", Json::integer((long long)idx));
        r.set("type", Json::str(bp_type_name(req.type)));
        return enforce_max_payload(id, make_result(id, r));
    }
    if (method == "breakpoint_delete") {
        bool all = false;
        const Json *a = params.find("all");
        if (a != nullptr) {
            if (!a->isBool()) return make_error(id, MCP_ERR_INVALID_PARAMS, "all must be a boolean");
            all = a->asBool();
        }
        int index = -1;
        if (!all) {
            uint32_t idx;
            if (!json_to_u32(params.find("index"), idx))
                return make_error(id, MCP_ERR_INVALID_PARAMS, "missing/invalid index (or set all:true)");
            index = (int)idx;
        }
        bool ok = bp_delete(all ? -1 : index);
        Json r = Json::object();
        r.set("deleted", Json::boolean(ok));
        if (all) r.set("all", Json::boolean(true));
        else     r.set("index", Json::integer((long long)index));
        return enforce_max_payload(id, make_result(id, r));
    }

    /* Known method whose handler arrives in a later slice. */
    return make_error(id, MCP_ERR_NOT_IMPLEMENTED, "method not implemented yet: " + method);
}

} // namespace mcp

#endif /* C_MCP */
