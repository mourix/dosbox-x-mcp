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

    // A parked-class request whose handler is not yet implemented (step arrives
    // in Slice 5) succeeds the mode check while parked, so it reports
    // not-implemented rather than a mismatch.
    std::string line2 = dispatch("step", Json::object(), Json::integer(5), STATE_PARKED);
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

} // namespace
