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

    // A parked-class request whose handler is not yet implemented (read_memory
    // arrives in Slice 4) succeeds the mode check while parked, so it reports
    // not-implemented rather than a mismatch.
    std::string line2 = dispatch("read_memory", Json::object(), Json::integer(5), STATE_PARKED);
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

} // namespace
