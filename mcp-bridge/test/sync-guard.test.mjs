// sync-guard.test.mjs — keep the bridge tool catalog in lockstep with the
// emulator's actual method table. The C++ dispatch (src/mcp/mcp_protocol.cpp) is
// the runtime authority for which methods exist; this asserts the bridge catalog
// (tools.mjs) exposes EXACTLY that set — no tool added/removed in C++ without a
// matching catalog change. Pure file read, no emulator boot.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { TOOL_NAMES } from "../tools.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROTOCOL_CPP = path.resolve(HERE, "..", "..", "src", "mcp", "mcp_protocol.cpp");

test("catalog matches the C++ dispatch method table", () => {
  const src = readFileSync(PROTOCOL_CPP, "utf8");
  const cpp = new Set();
  for (const m of src.matchAll(/method == "([a-z_]+)"/g)) cpp.add(m[1]);

  const catalog = new Set(TOOL_NAMES);

  const missingFromCatalog = [...cpp].filter((n) => !catalog.has(n)).sort();
  const extraInCatalog = [...catalog].filter((n) => !cpp.has(n)).sort();

  assert.deepEqual(missingFromCatalog, [],
    `C++ methods with no catalog entry: ${missingFromCatalog.join(", ")}`);
  assert.deepEqual(extraInCatalog, [],
    `catalog tools with no C++ method: ${extraInCatalog.join(", ")}`);
  assert.equal(catalog.size, cpp.size);
});
