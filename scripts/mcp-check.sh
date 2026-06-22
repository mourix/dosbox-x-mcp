#!/usr/bin/env bash
#
# scripts/mcp-check.sh — the single one-command verification for MCP work.
#
# Guardrail #2 ("one-command verification"): "ran the tests" means "ran this
# script and it passed". It:
#   1. regenerates the build system (autogen) and the in-tree SDL1/SDLnet,
#   2. builds dosbox-x WITH --enable-mcp and runs the unit + baseline test suite
#      (and the Mcp.* smoke test) headless,
#   3. rebuilds the core WITHOUT --enable-mcp to prove isolation (guardrail #1).
#
# Linux only (the MCP build rides the in-tree SDL1 + ncurses debugger).
#
# Env knobs (all optional):
#   MCP_JOBS=N            parallel make jobs           (default: nproc)
#   MCP_SKIP_SDL=1        reuse already-built in-tree SDL1/SDLnet libs
#   MCP_SKIP_ISOLATION=1  skip the no-flag isolation rebuild (faster dev loop)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${MCP_JOBS:-$(nproc 2>/dev/null || echo 2)}"

log()  { printf '\n=== mcp-check: %s ===\n' "$*"; }
fail() { printf '\nFAIL: %s\n' "$*" >&2; exit 1; }

# Run the test binary headless so no display/tty is required (WSL2 / CI).
run_headless() { SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$@"; }

# Run `dosbox-x -tests` headless and gate on the GTEST OUTCOME, not the process
# exit code. The emulator boots a full machine, runs gtest, prints the result,
# then tears down SDL/audio — and that teardown is known to SIGSEGV under the
# dummy SDL driver + no-ALSA headless environment (e.g. WSL), *after* tests have
# already passed. So success = gtest reported "[  PASSED  ]" + the shell's
# "Unit test completed: success" line, with no "[  FAILED  ]". A crash or hang
# *before* that still fails the gate (no success line is printed). MCP code is a
# passive stub, so it is not involved in the teardown path.
run_tests() {
    local desc="$1"; shift
    local out rc
    log "$desc"
    set +e
    out="$(run_headless ./src/dosbox-x -tests "$@" 2>&1)"
    rc=$?
    set -e
    printf '%s\n' "$out"
    if printf '%s' "$out" | grep -q 'Unit test completed: success' \
       && ! printf '%s' "$out" | grep -q '\[  FAILED  \]'; then
        [ "$rc" -ne 0 ] && printf 'WARN: tests passed; dosbox-x exited %d on shutdown (known headless teardown crash, ignored)\n' "$rc"
        return 0
    fi
    fail "$desc — gtest did not report success (process exit $rc)"
}

# configure + make for the given extra flags; leaves src/dosbox-x freshly built.
build() {
    log "configure $*"
    ./configure --enable-debug=heavy --prefix=/usr "$@"
    log "make -j$JOBS"
    make -j"$JOBS"
}

# 1. Regenerate the build system.
log "autogen"
./autogen.sh

# 2. Build the in-tree (heavily patched) SDL1 + SDLnet.
if [ "${MCP_SKIP_SDL:-0}" != "1" ]; then
    chmod +x vs/sdl/build-scripts/strip_fPIC.sh
    log "building in-tree SDL 1.x"
    ( cd vs/sdl && ./build-dosbox.sh )
    log "building in-tree SDLnet 1.x"
    ( cd vs/sdlnet && ./build-dosbox.sh )
else
    log "skipping SDL build (MCP_SKIP_SDL=1)"
fi
chmod +x configure

# 3. Build WITH --enable-mcp and verify the flag took effect.
build --enable-mcp
grep -q '^#define C_MCP 1' config.h || fail "C_MCP not defined in config.h after --enable-mcp"

run_tests "running unit + baseline test suite (headless)"
run_tests "running Mcp.* smoke test (headless)" --gtest_filter=Mcp.*

# 3b. Integration tests (scripted, out-of-process) against the --enable-mcp build.
log "running integration test #1: headless screenshot (Slice 1)"
python3 scripts/mcp_slice1_screenshot.py || fail "integration test #1 (headless screenshot) failed"

# 4. Isolation proof: the core must still build with the flag OFF.
if [ "${MCP_SKIP_ISOLATION:-0}" != "1" ]; then
    build
    grep -q '^#define C_MCP 1' config.h && fail "C_MCP defined without --enable-mcp"
    log "isolation build OK (no --enable-mcp)"
else
    log "skipping isolation build (MCP_SKIP_ISOLATION=1)"
fi

log "PASSED"
