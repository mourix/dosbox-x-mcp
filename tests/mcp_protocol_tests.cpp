/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 *  Slice 2 unit tests (fast, in-binary; no emulator boot). Cover the pure
 *  protocol layer: JSON parse/serialize, the bounded-response helper, request
 *  classification, mode-mismatch fast-reject, and the ping / server_info
 *  dispatch. Run via:  ./src/dosbox-x -tests --gtest_filter=Mcp.*
 */

#include <string>

#include <gtest/gtest.h>

#include "../src/mcp/mcp_json.h"
#include "../src/mcp/mcp_protocol.h"

using namespace mcp;

namespace {

// -- JSON ------------------------------------------------------------------

TEST(Mcp, JsonRoundTripObject)
{
    Json root;
    ASSERT_TRUE(Json::parse(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":{}}", root));
    ASSERT_TRUE(root.isObject());
    ASSERT_NE(root.find("method"), nullptr);
    EXPECT_EQ(root.find("method")->asString(), "ping");
    ASSERT_NE(root.find("id"), nullptr);
    EXPECT_EQ(root.find("id")->asInt(), 7);
}

TEST(Mcp, JsonIntegerSerializesWithoutDecimal)
{
    // ids and counts must round-trip as integers, not 7.0.
    EXPECT_EQ(Json::integer(7).serialize(), "7");
    EXPECT_EQ(Json::integer(-32001).serialize(), "-32001");
}

TEST(Mcp, JsonStringEscaping)
{
    Json s = Json::str("a\"b\\c\nd");
    EXPECT_EQ(s.serialize(), "\"a\\\"b\\\\c\\nd\"");

    Json parsed;
    ASSERT_TRUE(Json::parse(s.serialize(), parsed));
    EXPECT_EQ(parsed.asString(), "a\"b\\c\nd");
}

TEST(Mcp, JsonRejectsTrailingGarbage)
{
    Json j;
    EXPECT_FALSE(Json::parse("{} junk", j));
    EXPECT_FALSE(Json::parse("{", j));
    EXPECT_FALSE(Json::parse("", j));
}

// -- classification & mode matching ---------------------------------------

TEST(Mcp, Classification)
{
    EXPECT_EQ(classify("ping"), CLS_ANY);
    EXPECT_EQ(classify("server_info"), CLS_ANY);
    EXPECT_EQ(classify("read_registers"), CLS_PARKED);
    EXPECT_EQ(classify("read_memory"), CLS_PARKED);
    EXPECT_EQ(classify("break"), CLS_RUN);
    EXPECT_EQ(classify("read_screen"), CLS_RUN);
    EXPECT_EQ(classify("no_such_method"), CLS_UNKNOWN);
}

TEST(Mcp, ModeMatching)
{
    EXPECT_TRUE(mode_matches(CLS_ANY, STATE_RUNNING));
    EXPECT_TRUE(mode_matches(CLS_ANY, STATE_PARKED));
    EXPECT_TRUE(mode_matches(CLS_RUN, STATE_RUNNING));
    EXPECT_FALSE(mode_matches(CLS_RUN, STATE_PARKED));
    EXPECT_TRUE(mode_matches(CLS_PARKED, STATE_PARKED));
    EXPECT_FALSE(mode_matches(CLS_PARKED, STATE_RUNNING));
}

// -- dispatch --------------------------------------------------------------

TEST(Mcp, DispatchPing)
{
    std::string line = dispatch("ping", Json::object(), Json::integer(1), STATE_RUNNING);
    Json r;
    ASSERT_TRUE(Json::parse(line, r));
    ASSERT_NE(r.find("result"), nullptr);
    EXPECT_TRUE(r.find("result")->find("pong")->asBool());
    EXPECT_EQ(r.find("result")->find("state")->asString(), "running");
    EXPECT_EQ(r.find("id")->asInt(), 1);
}

TEST(Mcp, DispatchServerInfoReportsLocalhostBind)
{
    std::string line = dispatch("server_info", Json::object(), Json::integer(2), STATE_RUNNING);
    Json r;
    ASSERT_TRUE(Json::parse(line, r));
    const Json *res = r.find("result");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->find("bind")->asString(), "127.0.0.1");
    EXPECT_TRUE(res->find("single_client")->asBool());
    EXPECT_EQ(res->find("max_payload")->asInt(), (long long)MCP_MAX_PAYLOAD);
}

TEST(Mcp, DispatchUnknownMethod)
{
    std::string line = dispatch("bogus", Json::object(), Json::integer(3), STATE_RUNNING);
    Json r;
    ASSERT_TRUE(Json::parse(line, r));
    ASSERT_NE(r.find("error"), nullptr);
    EXPECT_EQ(r.find("error")->find("code")->asInt(), MCP_ERR_METHOD_NOT_FOUND);
}

TEST(Mcp, DispatchModeMismatchCarriesCurrentState)
{
    // A parked-class request arriving while running must fast-reject and report
    // the *current* state so the client can decide to break first.
    std::string line = dispatch("read_registers", Json::object(), Json::integer(4), STATE_RUNNING);
    Json r;
    ASSERT_TRUE(Json::parse(line, r));
    const Json *err = r.find("error");
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->find("code")->asInt(), MCP_ERR_MODE_MISMATCH);
    EXPECT_EQ(err->find("data")->find("state")->asString(), "running");

    // A parked-class request whose handler is not yet implemented (scan_start
    // arrives in Slice 11) succeeds the mode check while parked, so it reports
    // not-implemented rather than a mismatch. (Must be a method whose handler is
    // still a stub: dispatching an *implemented* state-touching tool here would
    // invoke the real emulator-state bridge — not what this pure test wants.)
    std::string line2 = dispatch("scan_start", Json::object(), Json::integer(5), STATE_PARKED);
    Json r2;
    ASSERT_TRUE(Json::parse(line2, r2));
    ASSERT_NE(r2.find("error"), nullptr);
    EXPECT_EQ(r2.find("error")->find("code")->asInt(), MCP_ERR_NOT_IMPLEMENTED);
}

// -- read_registers formatting (Slice 3) -----------------------------------

namespace {
RegisterSnapshot sample_snapshot()
{
    RegisterSnapshot s;
    s.eax = 0x11223344; s.ebx = 0x55667788;
    s.ecx = 0x99aabbcc; s.edx = 0xddeeff00;
    s.esi = 0x0a0b0c0d; s.edi = 0x10203040;
    s.ebp = 0xcafebabe; s.esp = 0x0000fffe;
    s.eip = 0x0000fff0;
    s.cs = 0xf000; s.ds = 0x0040; s.es = 0x0000;
    s.fs = 0x0000; s.gs = 0x0000; s.ss = 0x0000;
    s.eflags = 0x00000246;  // ZF, PF, IF set (reset-ish flags word)
    s.pmode = false; s.code_big = false; s.vm86 = false; s.cpl = 0;
    return s;
}
} // namespace

TEST(Mcp, FormatRegistersKnownSnapshotIsExact)
{
    Json r = format_registers(sample_snapshot());
    // Exact, deterministic encoding (key order + lowercase fixed-width hex).
    EXPECT_EQ(r.serialize(),
        "{\"eax\":\"0x11223344\",\"ebx\":\"0x55667788\","
        "\"ecx\":\"0x99aabbcc\",\"edx\":\"0xddeeff00\","
        "\"esi\":\"0x0a0b0c0d\",\"edi\":\"0x10203040\","
        "\"ebp\":\"0xcafebabe\",\"esp\":\"0x0000fffe\","
        "\"eip\":\"0x0000fff0\","
        "\"cs\":\"0xf000\",\"ds\":\"0x0040\",\"es\":\"0x0000\","
        "\"fs\":\"0x0000\",\"gs\":\"0x0000\",\"ss\":\"0x0000\","
        "\"eflags\":\"0x00000246\","
        "\"flags\":{\"CF\":0,\"PF\":1,\"AF\":0,\"ZF\":1,\"SF\":0,"
        "\"TF\":0,\"IF\":1,\"DF\":0,\"OF\":0,\"IOPL\":0},"
        "\"mode\":\"real\",\"cpl\":0}");
}

TEST(Mcp, FormatRegistersModeAndIoplDerivation)
{
    RegisterSnapshot s = sample_snapshot();
    s.pmode = true; s.code_big = true; s.vm86 = false;
    s.eflags = 0x00003000;  // IOPL = 3, no other flags
    s.cpl = 3;
    Json r = format_registers(s);
    EXPECT_EQ(r.find("mode")->asString(), "pr32");
    EXPECT_EQ(r.find("flags")->find("IOPL")->asInt(), 3);
    EXPECT_EQ(r.find("cpl")->asInt(), 3);

    s.code_big = false;            // 16-bit protected mode
    EXPECT_EQ(format_registers(s).find("mode")->asString(), "pr16");

    s.vm86 = true;                 // VM86 wins over code size
    EXPECT_EQ(format_registers(s).find("mode")->asString(), "vm86");
}

TEST(Mcp, FormatRegistersWithinCeiling)
{
    std::string body = make_result(Json::integer(1), format_registers(sample_snapshot()));
    EXPECT_LE(body.size(), MCP_MAX_PAYLOAD);
    EXPECT_EQ(enforce_max_payload(Json::integer(1), body), body);
}

// -- response bounding -----------------------------------------------------

TEST(Mcp, EnforceMaxPayloadCeiling)
{
    EXPECT_EQ(MCP_MAX_PAYLOAD, (size_t)(64 * 1024));

    std::string small = "{\"ok\":true}";
    EXPECT_EQ(enforce_max_payload(Json::integer(1), small), small);

    std::string big(MCP_MAX_PAYLOAD + 1, 'x');
    std::string bounded = enforce_max_payload(Json::integer(1), big);
    EXPECT_LE(bounded.size(), MCP_MAX_PAYLOAD);
    Json r;
    ASSERT_TRUE(Json::parse(bounded, r));
    ASSERT_NE(r.find("error"), nullptr);
    EXPECT_EQ(r.find("error")->find("code")->asInt(), MCP_ERR_TOO_LARGE);
}

TEST(Mcp, EveryResponseWithinCeiling)
{
    // ping and server_info are tiny, but assert the invariant explicitly.
    std::string a = dispatch("ping", Json::object(), Json::integer(1), STATE_RUNNING);
    std::string b = dispatch("server_info", Json::object(), Json::integer(1), STATE_PARKED);
    EXPECT_LE(a.size(), MCP_MAX_PAYLOAD);
    EXPECT_LE(b.size(), MCP_MAX_PAYLOAD);
}

// -- read_memory parse + format (Slice 4) ----------------------------------

TEST(Mcp, ParseMemRequestDefaults)
{
    // space defaults to segmented, len to MCP_READMEM_DEFAULT; addresses may be
    // hex strings or JSON integers.
    Json p;
    ASSERT_TRUE(Json::parse("{\"seg\":\"0x1000\",\"off\":256}", p));
    MemReadRequest req;
    std::string err;
    ASSERT_TRUE(parse_mem_request(p, req, err)) << err;
    EXPECT_EQ(req.space, SPACE_SEGMENTED);
    EXPECT_EQ(req.seg, 0x1000u);
    EXPECT_EQ(req.off, 256u);
    EXPECT_EQ(req.len, (uint32_t)MCP_READMEM_DEFAULT);
    EXPECT_EQ(req.requested_len, (uint32_t)MCP_READMEM_DEFAULT);
}

TEST(Mcp, ParseMemRequestClampsLenToMax)
{
    Json p;
    ASSERT_TRUE(Json::parse("{\"seg\":0,\"off\":0,\"len\":100000}", p));
    MemReadRequest req;
    std::string err;
    ASSERT_TRUE(parse_mem_request(p, req, err)) << err;
    EXPECT_EQ(req.len, (uint32_t)MCP_READMEM_MAX);       // capped
    EXPECT_EQ(req.requested_len, 100000u);               // original retained
}

TEST(Mcp, ParseMemRequestSpaceVariants)
{
    MemReadRequest req;
    std::string err;

    Json v;
    ASSERT_TRUE(Json::parse("{\"space\":\"virtual\",\"lin\":\"0xdead\"}", v));
    ASSERT_TRUE(parse_mem_request(v, req, err)) << err;
    EXPECT_EQ(req.space, SPACE_VIRTUAL);
    EXPECT_EQ(req.off, 0xdeadu);

    Json ph;
    ASSERT_TRUE(Json::parse("{\"space\":\"physical\",\"phys\":\"0xb8000\"}", ph));
    ASSERT_TRUE(parse_mem_request(ph, req, err)) << err;
    EXPECT_EQ(req.space, SPACE_PHYSICAL);
    EXPECT_EQ(req.off, 0xb8000u);
}

TEST(Mcp, ParseMemRequestRejectsBadInput)
{
    MemReadRequest req;
    std::string err;

    Json missing;                               // segmented needs seg + off
    ASSERT_TRUE(Json::parse("{\"off\":0}", missing));
    EXPECT_FALSE(parse_mem_request(missing, req, err));

    Json badspace;
    ASSERT_TRUE(Json::parse("{\"space\":\"nope\",\"seg\":0,\"off\":0}", badspace));
    EXPECT_FALSE(parse_mem_request(badspace, req, err));

    Json zerolen;
    ASSERT_TRUE(Json::parse("{\"seg\":0,\"off\":0,\"len\":0}", zerolen));
    EXPECT_FALSE(parse_mem_request(zerolen, req, err));
}

TEST(Mcp, FormatMemoryHexAndUnreadable)
{
    MemReadRequest req;
    req.space = SPACE_SEGMENTED; req.seg = 0x1000; req.off = 0x0100;
    req.len = 4; req.requested_len = 4;

    MemReadResult out;
    out.addr_valid = true; out.addr = 0x00010100;
    out.bytes    = {0x90, 0xcc, 0x00, 0xff};
    out.readable = {true, false, true, true};   // second byte page-faulted

    Json r = format_memory(req, out);
    EXPECT_EQ(r.find("space")->asString(), "segmented");
    EXPECT_EQ(r.find("seg")->asString(), "0x1000");
    EXPECT_EQ(r.find("off")->asString(), "0x00000100");
    EXPECT_EQ(r.find("addr")->asString(), "0x00010100");
    EXPECT_EQ(r.find("hex")->asString(), "90??00ff");   // unreadable -> "??"
    EXPECT_EQ(r.find("unreadable")->asInt(), 1);
    EXPECT_EQ(r.find("len")->asInt(), 4);
    EXPECT_FALSE(r.find("truncated")->asBool());
}

TEST(Mcp, FormatMemoryReportsTruncationAndNext)
{
    MemReadRequest req;
    req.space = SPACE_SEGMENTED; req.seg = 0; req.off = 0x1000;
    req.len = (uint32_t)MCP_READMEM_MAX; req.requested_len = 100000;

    MemReadResult out;
    out.addr_valid = true; out.addr = 0x1000;
    out.bytes.assign(MCP_READMEM_MAX, 0xab);
    out.readable.assign(MCP_READMEM_MAX, true);

    Json r = format_memory(req, out);
    EXPECT_TRUE(r.find("truncated")->asBool());
    EXPECT_EQ(r.find("next_off")->asString(),
              "0x00002000");                            // off + MAX (0x1000+0x1000)
    // Bounded: 4096 bytes -> 8192 hex chars, comfortably under the ceiling.
    std::string body = make_result(Json::integer(1), r);
    EXPECT_LE(body.size(), MCP_MAX_PAYLOAD);
    EXPECT_EQ(enforce_max_payload(Json::integer(1), body), body);
}

TEST(Mcp, FormatMemoryUnresolvedSelector)
{
    MemReadRequest req;
    req.space = SPACE_SEGMENTED; req.seg = 0x0008; req.off = 0;
    req.len = 16; req.requested_len = 16;

    MemReadResult out;
    out.addr_valid = false;                             // GetAddress -> no address

    Json r = format_memory(req, out);
    EXPECT_FALSE(r.find("addr_valid")->asBool());
    EXPECT_EQ(r.find("addr"), nullptr);                 // omitted when unresolved
    EXPECT_EQ(r.find("len")->asInt(), 0);
    EXPECT_EQ(r.find("hex")->asString(), "");
}

// -- disassemble parse + format (Slice 4) ----------------------------------

TEST(Mcp, ParseDisasmRequestDefaultsAndClamp)
{
    Json p;
    ASSERT_TRUE(Json::parse("{\"seg\":\"0xf000\",\"off\":\"0xfff0\"}", p));
    DisasmRequest req;
    std::string err;
    ASSERT_TRUE(parse_disasm_request(p, req, err)) << err;
    EXPECT_EQ(req.seg, 0xf000u);
    EXPECT_EQ(req.off, 0xfff0u);
    EXPECT_EQ(req.count, (uint32_t)MCP_DISASM_DEFAULT);
    EXPECT_FALSE(req.have_big);

    Json p2;
    ASSERT_TRUE(Json::parse("{\"seg\":0,\"off\":0,\"count\":1000,\"big\":true}", p2));
    ASSERT_TRUE(parse_disasm_request(p2, req, err)) << err;
    EXPECT_EQ(req.count, (uint32_t)MCP_DISASM_MAX);      // capped
    EXPECT_EQ(req.requested_count, 1000u);
    EXPECT_TRUE(req.have_big);
    EXPECT_TRUE(req.big);
}

TEST(Mcp, ParseDisasmRequestRejectsMissingAddr)
{
    DisasmRequest req;
    std::string err;
    Json p;
    ASSERT_TRUE(Json::parse("{\"off\":0}", p));         // seg missing
    EXPECT_FALSE(parse_disasm_request(p, req, err));
}

TEST(Mcp, FormatDisasmInstructions)
{
    DisasmRequest req;
    req.seg = 0xf000; req.off = 0xfff0;
    req.count = 16; req.requested_count = 16;

    DisasmResult out;
    out.addr_valid = true; out.big = false;

    DisasmInsn a;
    a.seg = 0xf000; a.off = 0xfff0; a.addr = 0xffff0;
    a.bytes = {0xea, 0x5b, 0xe0}; a.readable = {true, true, true};
    a.text = "jmp f000:e05b";
    out.insns.push_back(a);

    Json r = format_disasm(req, out);
    EXPECT_EQ(r.find("seg")->asString(), "0xf000");
    EXPECT_FALSE(r.find("big")->asBool());
    EXPECT_TRUE(r.find("addr_valid")->asBool());
    EXPECT_EQ(r.find("count")->asInt(), 1);
    EXPECT_FALSE(r.find("truncated")->asBool());

    const Json *insns = r.find("insns");
    ASSERT_NE(insns, nullptr);
    ASSERT_TRUE(insns->isArray());

    // The Json value type exposes object lookup but not array indexing, so assert
    // the (deterministic) serialized instruction object directly.
    EXPECT_NE(r.serialize().find(
        "{\"off\":\"0x0000fff0\",\"addr\":\"0x000ffff0\","
        "\"bytes\":\"ea5be0\",\"text\":\"jmp f000:e05b\"}"), std::string::npos);
}

TEST(Mcp, FormatDisasmTruncationFlag)
{
    DisasmRequest req;
    req.seg = 0; req.off = 0;
    req.count = (uint32_t)MCP_DISASM_MAX; req.requested_count = 1000;
    DisasmResult out;
    out.addr_valid = true; out.big = true;
    Json r = format_disasm(req, out);
    EXPECT_TRUE(r.find("truncated")->asBool());
}

// -- execution control (Slice 5) -------------------------------------------

TEST(Mcp, FormatExecStepStaysParked)
{
    // A plain single step stays parked and reports the new CS:EIP.
    ExecResult out;
    out.resumed = false; out.ran = 0;
    out.cs = 0xf000; out.eip = 0x0000e05b; out.state = STATE_PARKED;

    Json r = format_exec(EXEC_STEP, out);
    EXPECT_EQ(r.find("op")->asString(), "step");
    EXPECT_EQ(r.find("state")->asString(), "parked");
    EXPECT_FALSE(r.find("resumed")->asBool());
    EXPECT_EQ(r.find("ran")->asInt(), 0);
    EXPECT_EQ(r.find("cs")->asString(), "0xf000");
    EXPECT_EQ(r.find("eip")->asString(), "0x0000e05b");
}

TEST(Mcp, FormatExecContinueResumes)
{
    // continue releases the guest to free-run.
    ExecResult out;
    out.resumed = true; out.ran = -1;
    out.cs = 0x1234; out.eip = 0x00000010; out.state = STATE_RUNNING;

    Json r = format_exec(EXEC_CONTINUE, out);
    EXPECT_EQ(r.find("op")->asString(), "continue");
    EXPECT_EQ(r.find("state")->asString(), "running");
    EXPECT_TRUE(r.find("resumed")->asBool());
    EXPECT_EQ(r.find("ran")->asInt(), -1);
}

TEST(Mcp, FormatExecOpNames)
{
    ExecResult out;
    out.resumed = false; out.ran = 0; out.cs = 0; out.eip = 0;
    out.state = STATE_PARKED;
    EXPECT_EQ(format_exec(EXEC_STEP_OVER, out).find("op")->asString(), "step_over");
    out.state = STATE_RUNNING; out.resumed = false;
    EXPECT_EQ(format_exec(EXEC_BREAK, out).find("op")->asString(), "break");
}

TEST(Mcp, FormatExecBounded)
{
    ExecResult out;
    out.resumed = false; out.ran = 0;
    out.cs = 0xffff; out.eip = 0xffffffffu; out.state = STATE_PARKED;
    std::string body = format_exec(EXEC_STEP, out).serialize();
    EXPECT_LE(body.size(), MCP_MAX_PAYLOAD);
}

// step/step_over/continue are parked-class; break is run-class. Mode-mismatch
// fast-reject must therefore key correctly off the current execution state.
TEST(Mcp, ExecControlClassification)
{
    EXPECT_EQ(classify("step"), CLS_PARKED);
    EXPECT_EQ(classify("step_over"), CLS_PARKED);
    EXPECT_EQ(classify("continue"), CLS_PARKED);
    EXPECT_EQ(classify("break"), CLS_RUN);

    EXPECT_FALSE(mode_matches(CLS_PARKED, STATE_RUNNING)); // step while running -> reject
    EXPECT_TRUE(mode_matches(CLS_PARKED, STATE_PARKED));
    EXPECT_FALSE(mode_matches(CLS_RUN, STATE_PARKED));     // break while parked -> reject
    EXPECT_TRUE(mode_matches(CLS_RUN, STATE_RUNNING));
}

// -- breakpoints (Slice 6) -------------------------------------------------

TEST(Mcp, BreakpointTypeNames)
{
    EXPECT_STREQ(bp_type_name(BPT_EXEC), "exec");
    EXPECT_STREQ(bp_type_name(BPT_INT), "int");
    EXPECT_STREQ(bp_type_name(BPT_MEM), "mem");
    EXPECT_STREQ(bp_type_name(BPT_MEM_PROT), "mem_prot");
    EXPECT_STREQ(bp_type_name(BPT_MEM_LINEAR), "mem_linear");
    EXPECT_STREQ(bp_type_name(BPT_MEM_FREEZE), "mem_freeze");
    EXPECT_STREQ(bp_type_name(BPT_UNKNOWN), "unknown");

    BpType t;
    EXPECT_TRUE(parse_bp_type("exec", t));       EXPECT_EQ(t, BPT_EXEC);
    EXPECT_TRUE(parse_bp_type("mem_freeze", t)); EXPECT_EQ(t, BPT_MEM_FREEZE);
    EXPECT_FALSE(parse_bp_type("bogus", t));

    // BpType mirrors debug.cpp's EBreakpoint by value (the integer that crosses
    // the bridge): UNKNOWN=0, EXEC=1, INT=2, MEM=3, ... FREEZE=6.
    EXPECT_EQ((int)BPT_UNKNOWN, 0);
    EXPECT_EQ((int)BPT_EXEC, 1);
    EXPECT_EQ((int)BPT_INT, 2);
    EXPECT_EQ((int)BPT_MEM_FREEZE, 6);
}

TEST(Mcp, ParseBpAddDefaultsExec)
{
    // type defaults to exec; seg+off required; accepts hex strings.
    Json p = Json::object();
    p.set("seg", Json::str("0xf000"));
    p.set("off", Json::str("0xfff0"));
    BpAddRequest req; std::string err;
    ASSERT_TRUE(parse_bp_add_request(p, req, err)) << err;
    EXPECT_EQ(req.type, BPT_EXEC);
    EXPECT_EQ(req.seg, 0xf000);
    EXPECT_EQ(req.off, 0xfff0u);
    EXPECT_FALSE(req.once);
    EXPECT_EQ(req.ah, -1);
    EXPECT_EQ(req.al, -1);
}

TEST(Mcp, ParseBpAddInterrupt)
{
    // int needs the vector; ah/al optional, default -1 (all).
    Json p = Json::object();
    p.set("type", Json::str("int"));
    p.set("int", Json::integer(0x21));
    p.set("ah", Json::str("0x4c"));
    BpAddRequest req; std::string err;
    ASSERT_TRUE(parse_bp_add_request(p, req, err)) << err;
    EXPECT_EQ(req.type, BPT_INT);
    EXPECT_EQ(req.intnr, 0x21);
    EXPECT_EQ(req.ah, 0x4c);
    EXPECT_EQ(req.al, -1);   // unspecified -> all

    Json p2 = Json::object();
    p2.set("type", Json::str("int"));
    BpAddRequest r2; std::string e2;
    EXPECT_FALSE(parse_bp_add_request(p2, r2, e2));  // missing int number
}

TEST(Mcp, ParseBpAddLinearAndOnce)
{
    Json p = Json::object();
    p.set("type", Json::str("mem_linear"));
    p.set("lin", Json::str("0xb8000"));
    p.set("once", Json::boolean(true));
    BpAddRequest req; std::string err;
    ASSERT_TRUE(parse_bp_add_request(p, req, err)) << err;
    EXPECT_EQ(req.type, BPT_MEM_LINEAR);
    EXPECT_EQ(req.off, 0xb8000u);
    EXPECT_TRUE(req.once);

    Json p2 = Json::object();    // mem_linear without lin -> reject
    p2.set("type", Json::str("mem_linear"));
    BpAddRequest r2; std::string e2;
    EXPECT_FALSE(parse_bp_add_request(p2, r2, e2));

    Json p3 = Json::object();    // unknown type -> reject
    p3.set("type", Json::str("nope"));
    BpAddRequest r3; std::string e3;
    EXPECT_FALSE(parse_bp_add_request(p3, r3, e3));
}

TEST(Mcp, FormatBreakpointExecAndInt)
{
    BreakpointInfo e;
    e.index = 0; e.type = BPT_EXEC; e.seg = 0x1234; e.off = 0x00000100;
    e.intnr = 0; e.ah = -1; e.al = -1; e.memvalue = -1;
    e.once = true; e.active = true;
    Json je = format_breakpoint(e);
    EXPECT_EQ(je.find("type")->asString(), "exec");
    EXPECT_EQ(je.find("seg")->asString(), "0x1234");
    EXPECT_EQ(je.find("off")->asString(), "0x00000100");
    EXPECT_TRUE(je.find("once")->asBool());
    EXPECT_TRUE(je.find("active")->asBool());
    EXPECT_EQ(je.find("index")->asInt(), 0);
    EXPECT_TRUE(je.find("value") == nullptr);   // exec has no watched value

    BreakpointInfo i;
    i.index = 1; i.type = BPT_INT; i.seg = 0; i.off = 0;
    i.intnr = 0x21; i.ah = 0x4c; i.al = -1; i.memvalue = -1;
    i.once = false; i.active = true;
    Json ji = format_breakpoint(i);
    EXPECT_EQ(ji.find("type")->asString(), "int");
    EXPECT_EQ(ji.find("int")->asString(), "0x21");
    EXPECT_EQ(ji.find("ah")->asString(), "0x4c");
    EXPECT_EQ(ji.find("al")->asString(), "*");   // -1 renders as "match all"
}

TEST(Mcp, FormatBreakpointMemValue)
{
    BreakpointInfo m;
    m.index = 2; m.type = BPT_MEM; m.seg = 0x40; m.off = 0x0000006c;
    m.intnr = 0; m.ah = -1; m.al = -1; m.memvalue = 0xab;
    m.once = false; m.active = true;
    Json jm = format_breakpoint(m);
    EXPECT_EQ(jm.find("type")->asString(), "mem");
    EXPECT_EQ(jm.find("value")->asString(), "0xab");

    BreakpointInfo l;
    l.index = 3; l.type = BPT_MEM_LINEAR; l.seg = 0; l.off = 0xb8000;
    l.intnr = 0; l.ah = -1; l.al = -1; l.memvalue = 0x07;
    l.once = false; l.active = true;
    Json jl = format_breakpoint(l);
    EXPECT_EQ(jl.find("lin")->asString(), "0x000b8000");
    EXPECT_EQ(jl.find("value")->asString(), "0x07");
    EXPECT_TRUE(jl.find("seg") == nullptr);
}

TEST(Mcp, FormatBreakpointListBoundedAndCounts)
{
    std::vector<BreakpointInfo> bps;
    for (int i = 0; i < 5; i++) {
        BreakpointInfo b;
        b.index = i; b.type = BPT_EXEC; b.seg = 0x1000; b.off = (uint32_t)i;
        b.intnr = 0; b.ah = -1; b.al = -1; b.memvalue = -1;
        b.once = false; b.active = true;
        bps.push_back(b);
    }
    // Cap at 3: list reports 3 shown, 5 total, truncated.
    Json r = format_breakpoint_list(bps, 3);
    EXPECT_EQ(r.find("count")->asInt(), 3);
    EXPECT_EQ(r.find("total")->asInt(), 5);
    EXPECT_TRUE(r.find("truncated")->asBool());
    ASSERT_TRUE(r.find("breakpoints")->isArray());

    // Within cap: not truncated.
    Json r2 = format_breakpoint_list(bps, MCP_LIST_MAX);
    EXPECT_EQ(r2.find("count")->asInt(), 5);
    EXPECT_FALSE(r2.find("truncated")->asBool());

    // Empty list is well-formed.
    std::vector<BreakpointInfo> none;
    Json r3 = format_breakpoint_list(none, MCP_LIST_MAX);
    EXPECT_EQ(r3.find("count")->asInt(), 0);
    EXPECT_EQ(r3.find("total")->asInt(), 0);
    EXPECT_FALSE(r3.find("truncated")->asBool());
}

TEST(Mcp, FormatBreakpointListWithinCeiling)
{
    // A full page of breakpoints must stay under the 64 KiB ceiling.
    std::vector<BreakpointInfo> bps;
    for (size_t i = 0; i < MCP_LIST_MAX; i++) {
        BreakpointInfo b;
        b.index = (int)i; b.type = BPT_INT; b.seg = 0; b.off = 0;
        b.intnr = 0x21; b.ah = 0x4c; b.al = 0x00; b.memvalue = -1;
        b.once = false; b.active = true;
        bps.push_back(b);
    }
    std::string body = format_breakpoint_list(bps, MCP_LIST_MAX).serialize();
    EXPECT_LE(body.size(), MCP_MAX_PAYLOAD);
}

TEST(Mcp, BreakpointClassification)
{
    EXPECT_EQ(classify("breakpoint_list"), CLS_PARKED);
    EXPECT_EQ(classify("breakpoint_add"), CLS_PARKED);
    EXPECT_EQ(classify("breakpoint_delete"), CLS_PARKED);
    EXPECT_FALSE(mode_matches(CLS_PARKED, STATE_RUNNING)); // rejected while running
    EXPECT_TRUE(mode_matches(CLS_PARKED, STATE_PARKED));
}

// -- writes (Slice 7) ------------------------------------------------------

static Json parse_params(const char *json)
{
    Json p;
    EXPECT_TRUE(Json::parse(json, p)) << json;
    return p;
}

TEST(Mcp, ParseRegWriteUppercasesAndAcceptsHexOrInt)
{
    RegWriteRequest req;
    std::string err;
    // lower-case name is upper-cased; value as a hex string
    ASSERT_TRUE(parse_reg_write_request(
        parse_params("{\"register\":\"eax\",\"value\":\"0x1234\"}"), req, err)) << err;
    EXPECT_EQ(req.reg, "EAX");
    EXPECT_EQ(req.value, 0x1234u);

    // value as a JSON integer
    ASSERT_TRUE(parse_reg_write_request(
        parse_params("{\"register\":\"BX\",\"value\":513}"), req, err)) << err;
    EXPECT_EQ(req.reg, "BX");
    EXPECT_EQ(req.value, 513u);
}

TEST(Mcp, ParseRegWriteRejectsBadInput)
{
    RegWriteRequest req;
    std::string err;
    EXPECT_FALSE(parse_reg_write_request(parse_params("{\"value\":1}"), req, err));
    EXPECT_FALSE(parse_reg_write_request(parse_params("{\"register\":\"\",\"value\":1}"), req, err));
    EXPECT_FALSE(parse_reg_write_request(parse_params("{\"register\":\"eax\"}"), req, err));
    EXPECT_FALSE(parse_reg_write_request(parse_params("{\"register\":5,\"value\":1}"), req, err));
}

TEST(Mcp, FormatRegWrite)
{
    RegWriteRequest req; req.reg = "EAX"; req.value = 0xdeadbeef;
    Json r = format_reg_write(req, true);
    EXPECT_TRUE(r.find("written")->asBool());
    EXPECT_EQ(r.find("register")->asString(), "EAX");
    EXPECT_EQ(r.find("value")->asString(), "0xdeadbeef");
}

TEST(Mcp, ParseMemWriteDefaultsAndSpaces)
{
    MemWriteRequest req;
    std::string err;
    // segmented, default width 1
    ASSERT_TRUE(parse_mem_write_request(
        parse_params("{\"seg\":\"0x40\",\"off\":\"0x6c\",\"values\":[1,\"0x02\",255]}"),
        req, err)) << err;
    EXPECT_EQ(req.space, SPACE_SEGMENTED);
    EXPECT_EQ(req.seg, 0x40);
    EXPECT_EQ(req.off, 0x6cu);
    EXPECT_EQ(req.width, 1);
    ASSERT_EQ(req.values.size(), 3u);
    EXPECT_EQ(req.values[0], 1u);
    EXPECT_EQ(req.values[1], 2u);
    EXPECT_EQ(req.values[2], 255u);

    // virtual with explicit width
    ASSERT_TRUE(parse_mem_write_request(
        parse_params("{\"space\":\"virtual\",\"lin\":\"0xb8000\",\"width\":2,\"values\":[\"0x0741\"]}"),
        req, err)) << err;
    EXPECT_EQ(req.space, SPACE_VIRTUAL);
    EXPECT_EQ(req.off, 0xb8000u);
    EXPECT_EQ(req.width, 2);

    // physical, width 4
    ASSERT_TRUE(parse_mem_write_request(
        parse_params("{\"space\":\"physical\",\"phys\":\"0x500\",\"width\":4,\"values\":[\"0x12345678\"]}"),
        req, err)) << err;
    EXPECT_EQ(req.space, SPACE_PHYSICAL);
    EXPECT_EQ(req.width, 4);
}

TEST(Mcp, ParseMemWriteRejectsBadInput)
{
    MemWriteRequest req;
    std::string err;
    // missing values
    EXPECT_FALSE(parse_mem_write_request(
        parse_params("{\"seg\":0,\"off\":0}"), req, err));
    // empty values
    EXPECT_FALSE(parse_mem_write_request(
        parse_params("{\"seg\":0,\"off\":0,\"values\":[]}"), req, err));
    // bad width
    EXPECT_FALSE(parse_mem_write_request(
        parse_params("{\"seg\":0,\"off\":0,\"width\":3,\"values\":[1]}"), req, err));
    // unknown space
    EXPECT_FALSE(parse_mem_write_request(
        parse_params("{\"space\":\"flat\",\"seg\":0,\"off\":0,\"values\":[1]}"), req, err));
    // bad element in values array
    EXPECT_FALSE(parse_mem_write_request(
        parse_params("{\"seg\":0,\"off\":0,\"values\":[\"zz\"]}"), req, err));
}

TEST(Mcp, ParseMemWriteEnforcesByteCap)
{
    MemWriteRequest req;
    std::string err;
    // values * width must not exceed MCP_READMEM_MAX (4096). width 4 ->
    // MCP_READMEM_MAX/4 dwords is the most we accept; one more is rejected.
    Json p = Json::object();
    p.set("seg", Json::integer(0));
    p.set("off", Json::integer(0));
    p.set("width", Json::integer(4));
    Json vals = Json::array();
    for (size_t i = 0; i < (MCP_READMEM_MAX / 4) + 1; i++) vals.push(Json::integer(0));
    p.set("values", vals);
    EXPECT_FALSE(parse_mem_write_request(p, req, err));

    // exactly at the cap is accepted
    Json ok = Json::object();
    ok.set("seg", Json::integer(0));
    ok.set("off", Json::integer(0));
    ok.set("width", Json::integer(4));
    Json vals2 = Json::array();
    for (size_t i = 0; i < (MCP_READMEM_MAX / 4); i++) vals2.push(Json::integer(0));
    ok.set("values", vals2);
    EXPECT_TRUE(parse_mem_write_request(ok, req, err)) << err;
}

TEST(Mcp, FormatMemWriteVariants)
{
    MemWriteRequest req;
    req.space = SPACE_SEGMENTED; req.seg = 0x40; req.off = 0x6c; req.width = 1;
    MemWriteResult out;
    out.addr_valid = true; out.addr = 0x46c; out.written = 3; out.bytes = 3; out.fault = false;
    Json r = format_mem_write(req, out);
    EXPECT_EQ(r.find("space")->asString(), "segmented");
    EXPECT_EQ(r.find("seg")->asString(), "0x0040");
    EXPECT_EQ(r.find("off")->asString(), "0x0000006c");
    EXPECT_EQ(r.find("addr")->asString(), "0x0000046c");
    EXPECT_EQ(r.find("width")->asInt(), 1);
    EXPECT_EQ(r.find("written")->asInt(), 3);
    EXPECT_EQ(r.find("bytes")->asInt(), 3);
    EXPECT_FALSE(r.find("fault")->asBool());

    // physical, with a fault partway and no addr field for unresolved? physical
    // always resolves; show the fault + partial counts.
    MemWriteRequest pq;
    pq.space = SPACE_PHYSICAL; pq.seg = 0; pq.off = 0x500; pq.width = 4;
    MemWriteResult po;
    po.addr_valid = true; po.addr = 0x500; po.written = 1; po.bytes = 4; po.fault = false;
    Json pr = format_mem_write(pq, po);
    EXPECT_EQ(pr.find("space")->asString(), "physical");
    EXPECT_EQ(pr.find("phys")->asString(), "0x00000500");
    EXPECT_TRUE(pr.find("seg") == nullptr);

    // unresolved segmented selector: addr_valid false, no addr field
    MemWriteResult bad;
    bad.addr_valid = false; bad.addr = 0; bad.written = 0; bad.bytes = 0; bad.fault = false;
    Json br = format_mem_write(req, bad);
    EXPECT_FALSE(br.find("addr_valid")->asBool());
    EXPECT_TRUE(br.find("addr") == nullptr);
}

TEST(Mcp, WriteClassification)
{
    EXPECT_EQ(classify("write_register"), CLS_PARKED);
    EXPECT_EQ(classify("write_memory"), CLS_PARKED);
    EXPECT_FALSE(mode_matches(CLS_PARKED, STATE_RUNNING));
    EXPECT_TRUE(mode_matches(CLS_PARKED, STATE_PARKED));
}

// -- input injection (Slice 8) ---------------------------------------------

TEST(Mcp, KbdKeyFromName)
{
    int k1, k2;
    ASSERT_TRUE(kbd_key_from_name("a", k1));
    // case-insensitive lookup maps to the same key.
    ASSERT_TRUE(kbd_key_from_name("A", k2));
    EXPECT_EQ(k1, k2);
    int kr, ke;
    ASSERT_TRUE(kbd_key_from_name("return", kr));
    ASSERT_TRUE(kbd_key_from_name("enter", ke));
    EXPECT_EQ(kr, ke);                 // alias
    int kc, kl;
    ASSERT_TRUE(kbd_key_from_name("ctrl", kc));
    ASSERT_TRUE(kbd_key_from_name("leftctrl", kl));
    EXPECT_EQ(kc, kl);                 // alias
    int dummy;
    EXPECT_FALSE(kbd_key_from_name("nosuchkey", dummy));
}

TEST(Mcp, AsciiToKey)
{
    int k; bool shift;
    ASSERT_TRUE(ascii_to_key('a', k, shift)); EXPECT_FALSE(shift);
    int ka = k;
    ASSERT_TRUE(ascii_to_key('A', k, shift)); EXPECT_TRUE(shift);
    EXPECT_EQ(k, ka);                 // 'A' is shift + same key as 'a'
    ASSERT_TRUE(ascii_to_key('1', k, shift)); EXPECT_FALSE(shift);
    int k1 = k;
    ASSERT_TRUE(ascii_to_key('!', k, shift)); EXPECT_TRUE(shift);
    EXPECT_EQ(k, k1);                 // '!' is shift + '1'
    ASSERT_TRUE(ascii_to_key(' ', k, shift)); EXPECT_FALSE(shift);
    // a control char with no key fails.
    EXPECT_FALSE(ascii_to_key('\x01', k, shift));
}

TEST(Mcp, ParseSendKeysTapsAndChords)
{
    std::vector<KeyEvent> ev;
    std::string err;
    // string entries expand to press+release taps.
    ASSERT_TRUE(parse_send_keys_request(
        parse_params("{\"keys\":[\"d\",\"i\",\"r\"]}"), ev, err)) << err;
    ASSERT_EQ(ev.size(), 6u);
    EXPECT_TRUE(ev[0].down);
    EXPECT_FALSE(ev[1].down);
    EXPECT_EQ(ev[0].kbd, ev[1].kbd);  // press then release of the same key

    // object entries give explicit transitions (held chord: ctrl down, c, ctrl up).
    ASSERT_TRUE(parse_send_keys_request(
        parse_params("{\"keys\":["
                     "{\"key\":\"leftctrl\",\"down\":true},"
                     "{\"key\":\"c\",\"down\":true},"
                     "{\"key\":\"c\",\"down\":false},"
                     "{\"key\":\"leftctrl\",\"down\":false}]}"),
        ev, err)) << err;
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_TRUE(ev[0].down);
    EXPECT_FALSE(ev[3].down);
}

TEST(Mcp, ParseSendKeysRejectsBadInput)
{
    std::vector<KeyEvent> ev;
    std::string err;
    EXPECT_FALSE(parse_send_keys_request(parse_params("{}"), ev, err));
    EXPECT_FALSE(parse_send_keys_request(parse_params("{\"keys\":[]}"), ev, err));
    EXPECT_FALSE(parse_send_keys_request(parse_params("{\"keys\":[\"zz\"]}"), ev, err));
    EXPECT_FALSE(parse_send_keys_request(parse_params("{\"keys\":[123]}"), ev, err));
    EXPECT_FALSE(parse_send_keys_request(parse_params("{\"keys\":[{\"down\":true}]}"), ev, err));

    // over the transition cap (each tap is 2 transitions) is rejected.
    Json p = Json::object();
    Json keys = Json::array();
    for (size_t i = 0; i < MCP_KEYS_MAX; i++) keys.push(Json::str("a"));
    p.set("keys", keys);
    EXPECT_FALSE(parse_send_keys_request(p, ev, err));
}

TEST(Mcp, ParseTypeTextDecodesAndSkips)
{
    std::vector<KeyEvent> ev;
    size_t chars, skipped;
    std::string err;
    // "Hi" -> H is shifted (4 transitions), i is plain (2) = 6.
    ASSERT_TRUE(parse_type_text_request(
        parse_params("{\"text\":\"Hi\"}"), ev, chars, skipped, err)) << err;
    EXPECT_EQ(chars, 2u);
    EXPECT_EQ(skipped, 0u);
    EXPECT_EQ(ev.size(), 6u);
    EXPECT_TRUE(ev.front().down);     // leftshift down first
    EXPECT_FALSE(ev.back().down);     // ... 'i' release last

    // an undecodable char is skipped and counted; decodable ones still emitted.
    ASSERT_TRUE(parse_type_text_request(
        parse_params("{\"text\":\"a\\u0001b\"}"), ev, chars, skipped, err)) << err;
    EXPECT_EQ(chars, 2u);
    EXPECT_EQ(skipped, 1u);
    EXPECT_EQ(ev.size(), 4u);         // 2 plain chars * 2 transitions
}

TEST(Mcp, ParseTypeTextRejectsBadInput)
{
    std::vector<KeyEvent> ev;
    size_t chars, skipped;
    std::string err;
    EXPECT_FALSE(parse_type_text_request(parse_params("{}"), ev, chars, skipped, err));
    EXPECT_FALSE(parse_type_text_request(parse_params("{\"text\":123}"), ev, chars, skipped, err));
    // over the length cap.
    std::string big = "{\"text\":\"";
    for (size_t i = 0; i < MCP_TYPE_MAX + 1; i++) big += "a";
    big += "\"}";
    EXPECT_FALSE(parse_type_text_request(parse_params(big.c_str()), ev, chars, skipped, err));
}

TEST(Mcp, ParseMouseVariants)
{
    MouseRequest req;
    std::string err;
    ASSERT_TRUE(parse_mouse_request(parse_params("{\"action\":\"move\",\"dx\":-5,\"dy\":3}"), req, err)) << err;
    EXPECT_EQ(req.action, MOUSE_MOVE);
    EXPECT_EQ(req.dx, -5);
    EXPECT_EQ(req.dy, 3);

    ASSERT_TRUE(parse_mouse_request(parse_params("{\"action\":\"click\",\"button\":1}"), req, err)) << err;
    EXPECT_EQ(req.action, MOUSE_CLICK);
    EXPECT_EQ(req.button, 1);

    ASSERT_TRUE(parse_mouse_request(parse_params("{\"action\":\"down\"}"), req, err)) << err;
    EXPECT_EQ(req.action, MOUSE_DOWN);
    EXPECT_EQ(req.button, 0);          // default left

    ASSERT_TRUE(parse_mouse_request(parse_params("{\"action\":\"wheel\",\"amount\":-2}"), req, err)) << err;
    EXPECT_EQ(req.action, MOUSE_WHEEL);
    EXPECT_EQ(req.wheel, -2);
}

TEST(Mcp, ParseMouseRejectsBadInput)
{
    MouseRequest req;
    std::string err;
    EXPECT_FALSE(parse_mouse_request(parse_params("{}"), req, err));
    EXPECT_FALSE(parse_mouse_request(parse_params("{\"action\":\"spin\"}"), req, err));
    EXPECT_FALSE(parse_mouse_request(parse_params("{\"action\":\"click\",\"button\":5}"), req, err));
    EXPECT_FALSE(parse_mouse_request(parse_params("{\"action\":\"wheel\"}"), req, err));
}

TEST(Mcp, FormatInputResults)
{
    EXPECT_EQ(format_send_keys(6).serialize(),
              "{\"injected\":true,\"transitions\":6}");
    EXPECT_EQ(format_type_text(2, 1).serialize(),
              "{\"queued\":true,\"chars\":2,\"skipped\":1}");

    MouseRequest m;
    m.action = MOUSE_MOVE; m.dx = -5; m.dy = 3; m.button = 0; m.wheel = 0;
    EXPECT_EQ(format_mouse(m).serialize(),
              "{\"action\":\"move\",\"dx\":-5,\"dy\":3,\"injected\":true}");
    m.action = MOUSE_CLICK; m.button = 1;
    EXPECT_EQ(format_mouse(m).serialize(),
              "{\"action\":\"click\",\"button\":1,\"injected\":true}");
    m.action = MOUSE_WHEEL; m.wheel = -2;
    EXPECT_EQ(format_mouse(m).serialize(),
              "{\"action\":\"wheel\",\"amount\":-2,\"injected\":true}");
}

TEST(Mcp, InputClassification)
{
    EXPECT_EQ(classify("send_keys"), CLS_RUN);
    EXPECT_EQ(classify("type_text"), CLS_RUN);
    EXPECT_EQ(classify("mouse"), CLS_RUN);
    EXPECT_TRUE(mode_matches(CLS_RUN, STATE_RUNNING));
    EXPECT_FALSE(mode_matches(CLS_RUN, STATE_PARKED));
}

// -- screen (Slice 9) ------------------------------------------------------

TEST(Mcp, Fnv1a64KnownVectors)
{
    // Empty input hashes to the FNV-1a 64-bit offset basis.
    EXPECT_EQ(fnv1a64(nullptr, 0), 14695981039346656037ULL);
    // Canonical "a" / "foobar" FNV-1a 64-bit reference values.
    const uint8_t a[]      = { 'a' };
    const uint8_t foobar[] = { 'f','o','o','b','a','r' };
    EXPECT_EQ(fnv1a64(a, sizeof(a)),           0xaf63dc4c8601ec8cULL);
    EXPECT_EQ(fnv1a64(foobar, sizeof(foobar)), 0x85944171f73967e8ULL);
}

TEST(Mcp, FormatScreenTextGridSanitizesAndShapesRows)
{
    ScreenSnapshot s;
    s.is_text = true;
    s.mode = 3;
    s.cols = 4;
    s.rows = 2;
    // Row 0: "Z:\>" ; row 1 mixes a NUL, a high byte, and a tab -> all '.'.
    const uint8_t cells[] = {
        'Z', ':', '\\', '>',
        0x00, 0xB0, '\t', 'X'
    };
    s.chars.assign(cells, cells + sizeof(cells));
    EXPECT_EQ(format_screen(s).serialize(),
              "{\"mode\":3,\"is_text\":true,\"cols\":4,\"rows\":2,"
              "\"text\":[\"Z:\\\\>\",\"...X\"]}");
}

TEST(Mcp, FormatScreenNonTextHasEmptyGrid)
{
    ScreenSnapshot s;
    s.is_text = false;
    s.mode = 19;
    s.cols = 0;
    s.rows = 0;
    EXPECT_EQ(format_screen(s).serialize(),
              "{\"mode\":19,\"is_text\":false,\"cols\":0,\"rows\":0,\"text\":[]}");
}

TEST(Mcp, FormatScreenFullGridWithinCeiling)
{
    // A 132x60 grid is the largest a text mode reaches; assert it stays under
    // the 64 KiB ceiling so dispatch never has to swap in a TOO_LARGE error.
    ScreenSnapshot s;
    s.is_text = true;
    s.mode = 3;
    s.cols = 132;
    s.rows = 60;
    s.chars.assign((size_t)s.cols * s.rows, (uint8_t)'#');
    EXPECT_LT(format_screen(s).serialize().size(), MCP_MAX_PAYLOAD);
}

TEST(Mcp, FormatScreenHashRendersHexString)
{
    ScreenHash h;
    h.is_text = true;
    h.mode = 3;
    h.cols = 80;
    h.rows = 25;
    h.hash = 0x0123456789abcdefULL;
    EXPECT_EQ(format_screen_hash(h).serialize(),
              "{\"mode\":3,\"is_text\":true,\"cols\":80,\"rows\":25,"
              "\"hash\":\"0x0123456789abcdef\"}");
}

TEST(Mcp, ScreenClassification)
{
    EXPECT_EQ(classify("read_screen"), CLS_RUN);
    EXPECT_EQ(classify("screen_hash"), CLS_RUN);
}

} // namespace
