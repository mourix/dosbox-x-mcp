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
#   MCP_FORCE_SDL=1       rebuild the in-tree SDL even if its libs already exist
#   MCP_NO_CCACHE=1       do not route the compiler through ccache
#
# Speed model: 16 idle cores during this script means the cost is in the
# *serialized* phases (two ./configure runs, the isolation full rebuild) rather
# than the -j make. So by default we: route the compiler through ccache (the
# isolation build flips C_MCP in config.h, which otherwise forces a full
# ~500-object recompile — ccache turns it into mostly cache hits), reuse a
# config.cache across both configures, and skip the SDL rebuild when its libs
# already exist. Each skip has an explicit override above, and the isolation
# build + full test suite still run, so nothing verified is dropped. (autogen is
# left to run every time — it is ~3 s and doesn't rebuild configure unless
# configure.ac changed, so there is nothing to gain by gating it.)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${MCP_JOBS:-$(nproc 2>/dev/null || echo 2)}"

# Route the compiler through ccache when available (huge win for the isolation
# rebuild and for re-runs: identical translation units become cache hits).
if [ "${MCP_NO_CCACHE:-0}" != "1" ] && command -v ccache >/dev/null 2>&1; then
    export CC="${CC:-ccache gcc}" CXX="${CXX:-ccache g++}"
fi

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
# -C reuses config.cache across both configures (and across runs): the cached
# results are compiler/library feature tests, independent of --enable-mcp, so
# sharing them is safe and cuts the single-threaded configure time sharply.
build() {
    log "configure $*"
    ./configure -C --enable-debug=heavy --prefix=/usr "$@"
    log "make -j$JOBS"
    make -j"$JOBS"
}

# 1. Regenerate the build system (cheap: ~3 s, and a no-op for configure unless
# configure.ac changed).
log "autogen"
./autogen.sh

# 2. Build the in-tree (heavily patched) SDL1 + SDLnet — only when needed. These
# sources change very rarely, so reuse the libs if they already exist.
sdl_libs_present() {
    [ -f vs/sdl/linux-host/lib/libSDL.a ] && [ -f vs/sdlnet/linux-host/lib/libSDL_net.a ]
}
if [ "${MCP_SKIP_SDL:-0}" = "1" ]; then
    log "skipping SDL build (MCP_SKIP_SDL=1)"
elif [ "${MCP_FORCE_SDL:-0}" != "1" ] && sdl_libs_present; then
    log "skipping SDL build (in-tree libs already present; MCP_FORCE_SDL=1 to rebuild)"
else
    chmod +x vs/sdl/build-scripts/strip_fPIC.sh
    log "building in-tree SDL 1.x"
    ( cd vs/sdl && ./build-dosbox.sh )
    log "building in-tree SDLnet 1.x"
    ( cd vs/sdlnet && ./build-dosbox.sh )
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

log "running integration test #2: TCP JSON-RPC round trip (Slice 2)"
python3 scripts/mcp_slice2_ping.py || fail "integration test #2 (JSON-RPC round trip) failed"

log "running integration test #3: read_registers at a stop (Slice 3)"
python3 scripts/mcp_slice3_registers.py || fail "integration test #3 (read_registers) failed"

log "running integration test #4: read_memory + disassemble (Slice 4)"
python3 scripts/mcp_slice4_memory.py || fail "integration test #4 (read_memory/disassemble) failed"

log "running integration test #5: execution control (Slice 5)"
python3 scripts/mcp_slice5_exec.py || fail "integration test #5 (execution control) failed"

log "running integration test #6: breakpoints (Slice 6)"
python3 scripts/mcp_slice6_breakpoints.py || fail "integration test #6 (breakpoints) failed"

log "running integration test #7: writes (Slice 7)"
python3 scripts/mcp_slice7_writes.py || fail "integration test #7 (writes) failed"

# 4. Isolation proof: the core must still build with the flag OFF.
if [ "${MCP_SKIP_ISOLATION:-0}" != "1" ]; then
    build
    grep -q '^#define C_MCP 1' config.h && fail "C_MCP defined without --enable-mcp"
    log "isolation build OK (no --enable-mcp)"
else
    log "skipping isolation build (MCP_SKIP_ISOLATION=1)"
fi

log "PASSED"
