/*
 *  DOSBox-X MCP — protocol layer (impl). See mcp_protocol.h.
 *  Compiled only under --enable-mcp.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "keyboard.h" /* KBD_KEYS enum (constants only, no emulator state) */

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

/* Like json_to_u32 but signed (for mouse deltas / wheel amounts). */
bool json_to_i32(const Json *v, int &out) {
    if (v == nullptr) return false;
    if (v->isNumber()) { out = (int)v->asInt(); return true; }
    if (v->isString()) {
        const std::string &s = v->asString();
        if (s.empty()) return false;
        char *end = nullptr;
        long val = std::strtol(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0') return false;
        out = (int)val;
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

// -- writes (Slice 7) ------------------------------------------------------

bool parse_reg_write_request(const Json &params, RegWriteRequest &req, std::string &err) {
    const Json *r = params.find("register");
    if (r == nullptr || !r->isString()) { err = "missing/invalid register"; return false; }
    req.reg = r->asString();
    if (req.reg.empty()) { err = "register must be non-empty"; return false; }
    /* Upper-case so the (case-sensitive) ChangeRegister matcher recognises it. */
    for (size_t i = 0; i < req.reg.size(); i++)
        req.reg[i] = (char)std::toupper((unsigned char)req.reg[i]);
    if (!json_to_u32(params.find("value"), req.value)) { err = "missing/invalid value"; return false; }
    return true;
}

Json format_reg_write(const RegWriteRequest &req, bool ok) {
    Json r = Json::object();
    r.set("written", Json::boolean(ok));
    r.set("register", Json::str(req.reg));
    /* The value is masked to the register width by ChangeRegister; echo the
     * requested value (read_registers confirms the stored result). */
    r.set("value", Json::str(hex(req.value, 8)));
    return r;
}

bool parse_mem_write_request(const Json &params, MemWriteRequest &req, std::string &err) {
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
    } else {
        if (!json_to_u32(params.find("phys"), req.off)) { err = "missing/invalid phys"; return false; }
    }

    req.width = 1;
    if (params.has("width")) {
        uint32_t w;
        if (!json_to_u32(params.find("width"), w) || (w != 1 && w != 2 && w != 4)) {
            err = "width must be 1, 2 or 4"; return false;
        }
        req.width = (int)w;
    }

    const Json *vals = params.find("values");
    if (vals == nullptr || !vals->isArray() || vals->size() == 0) {
        err = "missing/empty values array"; return false;
    }
    if (vals->size() * (size_t)req.width > MCP_READMEM_MAX) {
        err = "too many values (bytes exceed cap)"; return false;
    }
    req.values.clear();
    req.values.reserve(vals->size());
    for (size_t i = 0; i < vals->size(); i++) {
        uint32_t v;
        if (!json_to_u32(&vals->at(i), v)) { err = "invalid value in values array"; return false; }
        req.values.push_back(v);
    }
    return true;
}

Json format_mem_write(const MemWriteRequest &req, const MemWriteResult &out) {
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
    r.set("width", Json::integer((long long)req.width));
    r.set("written", Json::integer((long long)out.written));
    r.set("bytes", Json::integer((long long)out.bytes));
    r.set("fault", Json::boolean(out.fault));
    return r;
}

// -- input injection (Slice 8) ---------------------------------------------

namespace {

/* Key-name -> KBD_KEYS table. Lower-cased lookup; a handful of aliases. Only the
 * keys a remote driver realistically needs are listed (the full KBD_KEYS set has
 * JP/KR/AX entries that are not useful here). */
struct KeyName { const char *name; int kbd; };
const KeyName kKeyNames[] = {
    {"1",KBD_1},{"2",KBD_2},{"3",KBD_3},{"4",KBD_4},{"5",KBD_5},
    {"6",KBD_6},{"7",KBD_7},{"8",KBD_8},{"9",KBD_9},{"0",KBD_0},
    {"a",KBD_a},{"b",KBD_b},{"c",KBD_c},{"d",KBD_d},{"e",KBD_e},{"f",KBD_f},
    {"g",KBD_g},{"h",KBD_h},{"i",KBD_i},{"j",KBD_j},{"k",KBD_k},{"l",KBD_l},
    {"m",KBD_m},{"n",KBD_n},{"o",KBD_o},{"p",KBD_p},{"q",KBD_q},{"r",KBD_r},
    {"s",KBD_s},{"t",KBD_t},{"u",KBD_u},{"v",KBD_v},{"w",KBD_w},{"x",KBD_x},
    {"y",KBD_y},{"z",KBD_z},
    {"f1",KBD_f1},{"f2",KBD_f2},{"f3",KBD_f3},{"f4",KBD_f4},{"f5",KBD_f5},
    {"f6",KBD_f6},{"f7",KBD_f7},{"f8",KBD_f8},{"f9",KBD_f9},{"f10",KBD_f10},
    {"f11",KBD_f11},{"f12",KBD_f12},
    {"esc",KBD_esc},{"escape",KBD_esc},{"tab",KBD_tab},
    {"backspace",KBD_backspace},{"bksp",KBD_backspace},
    {"enter",KBD_enter},{"return",KBD_enter},{"space",KBD_space},
    {"capslock",KBD_capslock},{"scrolllock",KBD_scrolllock},{"numlock",KBD_numlock},
    {"leftalt",KBD_leftalt},{"lalt",KBD_leftalt},{"alt",KBD_leftalt},
    {"rightalt",KBD_rightalt},{"ralt",KBD_rightalt},
    {"leftctrl",KBD_leftctrl},{"lctrl",KBD_leftctrl},{"ctrl",KBD_leftctrl},
    {"rightctrl",KBD_rightctrl},{"rctrl",KBD_rightctrl},
    {"leftshift",KBD_leftshift},{"lshift",KBD_leftshift},{"shift",KBD_leftshift},
    {"rightshift",KBD_rightshift},{"rshift",KBD_rightshift},
    {"grave",KBD_grave},{"backtick",KBD_grave},{"minus",KBD_minus},
    {"equals",KBD_equals},{"backslash",KBD_backslash},
    {"leftbracket",KBD_leftbracket},{"rightbracket",KBD_rightbracket},
    {"semicolon",KBD_semicolon},{"quote",KBD_quote},{"period",KBD_period},
    {"comma",KBD_comma},{"slash",KBD_slash},
    {"printscreen",KBD_printscreen},{"pause",KBD_pause},
    {"insert",KBD_insert},{"home",KBD_home},{"pageup",KBD_pageup},
    {"delete",KBD_delete},{"del",KBD_delete},{"end",KBD_end},{"pagedown",KBD_pagedown},
    {"left",KBD_left},{"up",KBD_up},{"down",KBD_down},{"right",KBD_right},
    {"kp0",KBD_kp0},{"kp1",KBD_kp1},{"kp2",KBD_kp2},{"kp3",KBD_kp3},{"kp4",KBD_kp4},
    {"kp5",KBD_kp5},{"kp6",KBD_kp6},{"kp7",KBD_kp7},{"kp8",KBD_kp8},{"kp9",KBD_kp9},
    {"kpdivide",KBD_kpdivide},{"kpmultiply",KBD_kpmultiply},{"kpminus",KBD_kpminus},
    {"kpplus",KBD_kpplus},{"kpenter",KBD_kpenter},{"kpperiod",KBD_kpperiod},
    {"lwindows",KBD_lwindows},{"rwindows",KBD_rwindows},{"menu",KBD_rwinmenu},
};

} // namespace

bool kbd_key_from_name(const std::string &name, int &kbd) {
    std::string s = name;
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)std::tolower((unsigned char)s[i]);
    for (size_t i = 0; i < sizeof(kKeyNames)/sizeof(kKeyNames[0]); i++) {
        if (s == kKeyNames[i].name) { kbd = kKeyNames[i].kbd; return true; }
    }
    return false;
}

bool ascii_to_key(char c, int &kbd, bool &shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') { return kbd_key_from_name(std::string(1, c), kbd); }
    if (c >= 'A' && c <= 'Z') { shift = true; return kbd_key_from_name(std::string(1, (char)(c - 'A' + 'a')), kbd); }
    if (c >= '0' && c <= '9') { return kbd_key_from_name(std::string(1, c), kbd); }
    switch (c) {
        case ' ':  kbd = KBD_space; return true;
        case '\n': case '\r': kbd = KBD_enter; return true;
        case '\t': kbd = KBD_tab; return true;
        case '\b': kbd = KBD_backspace; return true;
        /* unshifted punctuation */
        case '-':  kbd = KBD_minus; return true;
        case '=':  kbd = KBD_equals; return true;
        case '[':  kbd = KBD_leftbracket; return true;
        case ']':  kbd = KBD_rightbracket; return true;
        case ';':  kbd = KBD_semicolon; return true;
        case '\'': kbd = KBD_quote; return true;
        case '`':  kbd = KBD_grave; return true;
        case '\\': kbd = KBD_backslash; return true;
        case ',':  kbd = KBD_comma; return true;
        case '.':  kbd = KBD_period; return true;
        case '/':  kbd = KBD_slash; return true;
        /* shifted symbols (US layout) */
        case '!':  shift = true; kbd = KBD_1; return true;
        case '@':  shift = true; kbd = KBD_2; return true;
        case '#':  shift = true; kbd = KBD_3; return true;
        case '$':  shift = true; kbd = KBD_4; return true;
        case '%':  shift = true; kbd = KBD_5; return true;
        case '^':  shift = true; kbd = KBD_6; return true;
        case '&':  shift = true; kbd = KBD_7; return true;
        case '*':  shift = true; kbd = KBD_8; return true;
        case '(':  shift = true; kbd = KBD_9; return true;
        case ')':  shift = true; kbd = KBD_0; return true;
        case '_':  shift = true; kbd = KBD_minus; return true;
        case '+':  shift = true; kbd = KBD_equals; return true;
        case '{':  shift = true; kbd = KBD_leftbracket; return true;
        case '}':  shift = true; kbd = KBD_rightbracket; return true;
        case ':':  shift = true; kbd = KBD_semicolon; return true;
        case '"':  shift = true; kbd = KBD_quote; return true;
        case '~':  shift = true; kbd = KBD_grave; return true;
        case '|':  shift = true; kbd = KBD_backslash; return true;
        case '<':  shift = true; kbd = KBD_comma; return true;
        case '>':  shift = true; kbd = KBD_period; return true;
        case '?':  shift = true; kbd = KBD_slash; return true;
        default: return false;
    }
}

bool parse_send_keys_request(const Json &params, std::vector<KeyEvent> &out, std::string &err) {
    const Json *keys = params.find("keys");
    if (keys == nullptr || !keys->isArray() || keys->size() == 0) {
        err = "missing/empty keys array"; return false;
    }
    out.clear();
    for (size_t i = 0; i < keys->size(); i++) {
        const Json &el = keys->at(i);
        if (el.isString()) {
            int kbd;
            if (!kbd_key_from_name(el.asString(), kbd)) {
                err = "unknown key: " + el.asString(); return false;
            }
            KeyEvent down = { kbd, true }, up = { kbd, false };
            out.push_back(down);
            out.push_back(up);
        } else if (el.isObject()) {
            const Json *k = el.find("key");
            if (k == nullptr || !k->isString()) { err = "key entry missing string \"key\""; return false; }
            int kbd;
            if (!kbd_key_from_name(k->asString(), kbd)) {
                err = "unknown key: " + k->asString(); return false;
            }
            bool down = true;
            const Json *d = el.find("down");
            if (d != nullptr) {
                if (!d->isBool()) { err = "down must be a boolean"; return false; }
                down = d->asBool();
            }
            KeyEvent ev = { kbd, down };
            out.push_back(ev);
        } else {
            err = "keys entry must be a string or an object"; return false;
        }
        if (out.size() > MCP_KEYS_MAX) { err = "too many key transitions"; return false; }
    }
    return true;
}

Json format_send_keys(size_t transitions) {
    Json r = Json::object();
    r.set("injected", Json::boolean(true));
    r.set("transitions", Json::integer((long long)transitions));
    return r;
}

bool parse_type_text_request(const Json &params, std::vector<KeyEvent> &out,
                             size_t &chars, size_t &skipped, std::string &err) {
    const Json *t = params.find("text");
    if (t == nullptr || !t->isString()) { err = "missing/invalid text"; return false; }
    const std::string &s = t->asString();
    if (s.size() > MCP_TYPE_MAX) { err = "text too long (max 256 chars)"; return false; }
    out.clear();
    chars = 0; skipped = 0;
    for (size_t i = 0; i < s.size(); i++) {
        int kbd; bool shift;
        if (!ascii_to_key(s[i], kbd, shift)) { skipped++; continue; }
        chars++;
        if (shift) { KeyEvent e = { KBD_leftshift, true };  out.push_back(e); }
        { KeyEvent e = { kbd, true };  out.push_back(e); }
        { KeyEvent e = { kbd, false }; out.push_back(e); }
        if (shift) { KeyEvent e = { KBD_leftshift, false }; out.push_back(e); }
    }
    return true;
}

Json format_type_text(size_t chars, size_t skipped) {
    Json r = Json::object();
    r.set("queued", Json::boolean(true));
    r.set("chars", Json::integer((long long)chars));
    r.set("skipped", Json::integer((long long)skipped));
    return r;
}

bool parse_mouse_request(const Json &params, MouseRequest &req, std::string &err) {
    req.dx = req.dy = 0; req.button = 0; req.wheel = 0;
    const Json *a = params.find("action");
    if (a == nullptr || !a->isString()) { err = "missing/invalid action"; return false; }
    const std::string &s = a->asString();
    if (s == "move") {
        req.action = MOUSE_MOVE;
        if (params.has("dx") && !json_to_i32(params.find("dx"), req.dx)) { err = "invalid dx"; return false; }
        if (params.has("dy") && !json_to_i32(params.find("dy"), req.dy)) { err = "invalid dy"; return false; }
    } else if (s == "down" || s == "up" || s == "click") {
        req.action = (s == "down") ? MOUSE_DOWN : (s == "up") ? MOUSE_UP : MOUSE_CLICK;
        int b = 0;
        if (params.has("button") && !json_to_i32(params.find("button"), b)) { err = "invalid button"; return false; }
        if (b < 0 || b > 2) { err = "button must be 0 (left), 1 (right) or 2 (middle)"; return false; }
        req.button = b;
    } else if (s == "wheel") {
        req.action = MOUSE_WHEEL;
        if (!json_to_i32(params.find("amount"), req.wheel)) { err = "missing/invalid amount"; return false; }
    } else {
        err = "unknown action (want move|down|up|click|wheel): " + s; return false;
    }
    return true;
}

Json format_mouse(const MouseRequest &req) {
    Json r = Json::object();
    const char *act = req.action == MOUSE_MOVE  ? "move" :
                      req.action == MOUSE_DOWN  ? "down" :
                      req.action == MOUSE_UP    ? "up" :
                      req.action == MOUSE_CLICK ? "click" : "wheel";
    r.set("action", Json::str(act));
    if (req.action == MOUSE_MOVE) {
        r.set("dx", Json::integer((long long)req.dx));
        r.set("dy", Json::integer((long long)req.dy));
    } else if (req.action == MOUSE_WHEEL) {
        r.set("amount", Json::integer((long long)req.wheel));
    } else {
        r.set("button", Json::integer((long long)req.button));
    }
    r.set("injected", Json::boolean(true));
    return r;
}

/* ---- screen (Slice 9) ---------------------------------------------------- */

uint64_t fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 14695981039346656037ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;           /* FNV prime */
    }
    return h;
}

Json format_screen(const ScreenSnapshot &out) {
    Json r = Json::object();
    r.set("mode", Json::integer((long long)out.mode));
    r.set("is_text", Json::boolean(out.is_text));
    r.set("cols", Json::integer((long long)out.cols));
    r.set("rows", Json::integer((long long)out.rows));
    Json lines = Json::array();
    if (out.is_text && out.cols > 0) {
        for (int row = 0; row < out.rows; row++) {
            std::string line;
            line.reserve((size_t)out.cols);
            for (int col = 0; col < out.cols; col++) {
                size_t idx = (size_t)row * (size_t)out.cols + (size_t)col;
                uint8_t b = idx < out.chars.size() ? out.chars[idx] : 0x20;
                line.push_back((b >= 0x20 && b <= 0x7e) ? (char)b : '.');
            }
            lines.push(Json::str(line));
        }
    }
    r.set("text", lines);
    return r;
}

Json format_screen_hash(const ScreenHash &out) {
    Json r = Json::object();
    r.set("mode", Json::integer((long long)out.mode));
    r.set("is_text", Json::boolean(out.is_text));
    r.set("cols", Json::integer((long long)out.cols));
    r.set("rows", Json::integer((long long)out.rows));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)out.hash);
    r.set("hash", Json::str(buf));
    return r;
}

/* ---- take_screenshot (Slice 10) ------------------------------------------ */

Json format_screenshot(const ScreenshotResult &out) {
    Json r = Json::object();
    r.set("path", Json::str(out.path));
    r.set("format", Json::str("png"));
    r.set("width", Json::integer((long long)out.width));
    r.set("height", Json::integer((long long)out.height));
    r.set("bytes", Json::integer((long long)out.bytes));
    r.set("mode", Json::integer((long long)out.mode));
    r.set("is_text", Json::boolean(out.is_text));
    return r;
}

/* ---- memory scanner (Slice 11) ------------------------------------------- */

const char *scan_op_name(ScanOp op) {
    switch (op) {
        case SCAN_GT: return ">";
        case SCAN_LT: return "<";
        case SCAN_NE: return "!=";
        case SCAN_GE: return ">=";
        case SCAN_LE: return "<=";
        default:      return "==";
    }
}

bool parse_scan_op(const std::string &s, ScanOp &out) {
    if      (s == "==" || s == "=" || s == "eq") out = SCAN_EQ;
    else if (s == ">"  || s == "gt")             out = SCAN_GT;
    else if (s == "<"  || s == "lt")             out = SCAN_LT;
    else if (s == "!=" || s == "!" || s == "ne") out = SCAN_NE;
    else if (s == ">=" || s == "ge")             out = SCAN_GE;
    else if (s == "<=" || s == "le")             out = SCAN_LE;
    else return false;
    return true;
}

bool parse_scan_start_request(const Json &params, ScanStartRequest &req, std::string &err) {
    uint32_t seg;
    if (!json_to_u32(params.find("seg"), seg)) { err = "missing/invalid seg"; return false; }
    req.seg = (uint16_t)seg;
    if (!json_to_u32(params.find("off"), req.off)) { err = "missing/invalid off"; return false; }
    if (!json_to_u32(params.find("range"), req.range)) { err = "missing/invalid range"; return false; }
    if (req.range == 0) { err = "range must be non-zero"; return false; }
    if (req.range > MCP_SCAN_RANGE_MAX) req.range = (uint32_t)MCP_SCAN_RANGE_MAX;

    req.width = 1;
    if (params.has("width")) {
        uint32_t w;
        if (!json_to_u32(params.find("width"), w) || (w != 1 && w != 2 && w != 4)) {
            err = "width must be 1, 2 or 4"; return false;
        }
        req.width = (int)w;
    }
    return true;
}

bool parse_scan_filter_request(const Json &params, ScanFilterRequest &req, std::string &err) {
    const Json *op = params.find("op");
    if (op == nullptr || !op->isString()) { err = "missing/invalid op"; return false; }
    if (!parse_scan_op(op->asString(), req.op)) {
        err = "unknown op (want == != > < >= <=): " + op->asString(); return false;
    }
    req.use_prev = false;
    const Json *up = params.find("use_prev");
    if (up != nullptr) {
        if (!up->isBool()) { err = "use_prev must be a boolean"; return false; }
        req.use_prev = up->asBool();
    }
    req.value = 0;
    if (!req.use_prev) {
        if (!json_to_u32(params.find("value"), req.value)) {
            err = "missing/invalid value (or set use_prev:true)"; return false;
        }
    }
    return true;
}

Json format_scan_state(const ScanState &st) {
    Json r = Json::object();
    r.set("active", Json::boolean(st.active));
    if (st.active) {
        r.set("seg", Json::str(hex(st.seg, 4)));
        r.set("off", Json::str(hex(st.off, 8)));
        r.set("base_linear", Json::str(hex(st.base_linear, 8)));
        r.set("width", Json::integer((long long)st.width));
        r.set("range", Json::integer((long long)st.range));
        r.set("matches", Json::integer((long long)st.matches));
        r.set("iterations", Json::integer((long long)st.iterations));
    }
    return r;
}

Json format_scan_results(const ScanState &st, const std::vector<ScanMatch> &matches,
                         uint32_t start, uint32_t total, size_t max) {
    Json r = Json::object();
    r.set("active", Json::boolean(st.active));
    r.set("width", Json::integer((long long)st.width));
    Json arr = Json::array();
    size_t shown = matches.size() > max ? max : matches.size();
    for (size_t i = 0; i < shown; i++) {
        const ScanMatch &m = matches[i];
        Json o = Json::object();
        o.set("seg", Json::str(hex(m.seg, 4)));
        o.set("off", Json::str(hex(m.off, 8)));
        o.set("lin", Json::str(hex(m.lin, 8)));
        o.set("value", Json::str(hex(m.value, st.width * 2)));
        arr.push(o);
    }
    r.set("matches", arr);
    r.set("start", Json::integer((long long)start));
    r.set("count", Json::integer((long long)shown));
    r.set("total", Json::integer((long long)total));
    r.set("truncated", Json::boolean((uint64_t)start + shown < total));
    return r;
}

Json format_lifecycle(LifecycleOp op) {
    Json r = Json::object();
    r.set("op", Json::str(op == LIFE_RESET ? "reset" : "quit"));
    r.set("ok", Json::boolean(true));
    /* The action is deferred so this reply reaches the client first; the machine
     * reboots (reset) or the process exits (quit) a few frames later. */
    r.set("deferred", Json::boolean(true));
    return r;
}

const std::string &defer_sentinel() {
    /* Leading control byte → never a valid JSON response line, so the server can
     * distinguish "re-queue me" from a real reply with a simple equality check. */
    static const std::string s("\x01" "__MCP_DEFER__");
    return s;
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
    if (method == "reset" || method == "quit") {
        /* CLS_ANY: serviceable in either state. Record the action; it fires a few
         * frames later (MCP_LifecycleService) so this reply is flushed before the
         * machine reboots / the process exits. */
        LifecycleOp op = method == "reset" ? LIFE_RESET : LIFE_QUIT;
        lifecycle_request(op);
        return enforce_max_payload(id, make_result(id, format_lifecycle(op)));
    }
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

    if (method == "write_register") {
        /* Parked-class: mode_matches guaranteed STATE_PARKED above. */
        RegWriteRequest req;
        std::string perr;
        if (!parse_reg_write_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        bool ok = write_register(req);
        if (!ok)
            return make_error(id, MCP_ERR_INVALID_PARAMS, "unknown register: " + req.reg);
        return enforce_max_payload(id, make_result(id, format_reg_write(req, ok)));
    }
    if (method == "write_memory") {
        MemWriteRequest req;
        std::string perr;
        if (!parse_mem_write_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        MemWriteResult out;
        write_memory(req, out);
        if (!out.addr_valid)
            return make_error(id, MCP_ERR_INVALID_PARAMS, "segmented selector did not resolve");
        return enforce_max_payload(id, make_result(id, format_mem_write(req, out)));
    }

    if (method == "send_keys") {
        /* Run-class: mode_matches guaranteed STATE_RUNNING above, so the guest is
         * free-running and will consume injected keystrokes. */
        std::vector<KeyEvent> ev;
        std::string perr;
        if (!parse_send_keys_request(params, ev, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        send_keys(ev);
        return enforce_max_payload(id, make_result(id, format_send_keys(ev.size())));
    }
    if (method == "type_text") {
        std::vector<KeyEvent> ev;
        size_t chars = 0, skipped = 0;
        std::string perr;
        if (!parse_type_text_request(params, ev, chars, skipped, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        type_text_enqueue(ev);
        return enforce_max_payload(id, make_result(id, format_type_text(chars, skipped)));
    }
    if (method == "mouse") {
        MouseRequest req;
        std::string perr;
        if (!parse_mouse_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        mouse_action(req);
        return enforce_max_payload(id, make_result(id, format_mouse(req)));
    }

    if (method == "read_screen") {
        /* Run-class: mode_matches guaranteed STATE_RUNNING above. The screen is
         * read at the frame tick, reflecting what the guest is displaying. */
        ScreenSnapshot out;
        read_screen(out);
        return enforce_max_payload(id, make_result(id, format_screen(out)));
    }
    if (method == "screen_hash") {
        ScreenHash out;
        screen_hash(out);
        return enforce_max_payload(id, make_result(id, format_screen_hash(out)));
    }
    if (method == "take_screenshot") {
        /* Async capture (see screenshot_service): PENDING re-queues this request
         * via the defer sentinel; READY/ERROR complete it. Run-class — mode_matches
         * guaranteed STATE_RUNNING above, so the frame loop is ticking and the
         * RENDER/CAPTURE_AddImage path will emit a PNG. */
        ScreenshotResult out;
        ShotStatus st = screenshot_service(out);
        if (st == SHOT_PENDING)
            return defer_sentinel();
        if (st == SHOT_ERROR)
            return make_error(id, MCP_ERR_INTERNAL,
                              out.error.empty() ? "screenshot failed" : out.error);
        return enforce_max_payload(id, make_result(id, format_screenshot(out)));
    }

    if (method == "scan_start") {
        /* Parked-class: mode_matches guaranteed STATE_PARKED above. */
        ScanStartRequest req;
        std::string perr;
        if (!parse_scan_start_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        ScanState st;
        int rc = scan_start(req, st);
        if (rc < 0) {
            const char *why = rc == -3 ? "segmented selector did not resolve" :
                              rc == -4 ? "range exceeds configured memory" :
                              rc == -5 ? "width must be 1, 2 or 4" :
                                         "invalid scan range";
            return make_error(id, MCP_ERR_INVALID_PARAMS, why);
        }
        return enforce_max_payload(id, make_result(id, format_scan_state(st)));
    }
    if (method == "scan_filter") {
        ScanFilterRequest req;
        std::string perr;
        if (!parse_scan_filter_request(params, req, perr))
            return make_error(id, MCP_ERR_INVALID_PARAMS, perr);
        ScanState st;
        long rc = scan_filter(req, st);
        if (rc == -1)
            return make_error(id, MCP_ERR_INVALID_REQ, "no active scan (call scan_start first)");
        if (rc < 0)
            return make_error(id, MCP_ERR_INVALID_PARAMS, "value wider than the element size");
        return enforce_max_payload(id, make_result(id, format_scan_state(st)));
    }
    if (method == "scan_results") {
        uint32_t start = 0;
        if (params.has("start") && !json_to_u32(params.find("start"), start))
            return make_error(id, MCP_ERR_INVALID_PARAMS, "invalid start");
        ScanState st;
        scan_state(st);
        std::vector<ScanMatch> matches;
        uint32_t total = scan_results(start, MCP_LIST_MAX, matches);
        return enforce_max_payload(id, make_result(id, format_scan_results(st, matches, start, total, MCP_LIST_MAX)));
    }

    /* Known method whose handler arrives in a later slice. */
    return make_error(id, MCP_ERR_NOT_IMPLEMENTED, "method not implemented yet: " + method);
}

} // namespace mcp

#endif /* C_MCP */
