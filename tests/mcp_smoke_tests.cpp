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
 *  Slice 0 smoke test: proves the --enable-mcp build wiring compiles and that
 *  libmcp.a is linked into dosbox-x. It is registered in tests/tests.h under
 *  #if C_MCP and runs via:  ./src/dosbox-x -tests --gtest_filter=Mcp.*
 */

#include <gtest/gtest.h>

// Defined in src/mcp/mcp.cpp (libmcp.a). Forward-declared so this test needs no
// extra include path; a successful link is itself the isolation/wiring proof.
extern const char *MCP_Version(void);

namespace {

TEST(Mcp, SmokeVersionLinks)
{
	const char *v = MCP_Version();
	ASSERT_NE(v, nullptr);
	EXPECT_STRNE(v, "");
}

} // namespace
