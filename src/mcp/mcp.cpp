/*
 *  DOSBox-X MCP (Model Context Protocol) server.
 *
 *  This translation unit is linked into dosbox-x only when configured with
 *  --enable-mcp (libmcp.a, wired via configure.ac + src/Makefile.am +
 *  src/mcp/Makefile.am). It is the glue between the core emulator and the MCP
 *  module: the per-frame service point that starts the TCP JSON-RPC server and
 *  drains the request queue on the emulator thread. See docs/MCP_BUILD_PLAN.md.
 */

#include "mcp.h"

#if C_MCP

#include <cstdlib>
#include <cstring>

#include "dosbox.h"   /* Bitu, used by hardware.h */
#include "hardware.h" /* CaptureState, CAPTURE_IMAGE */

#include "mcp_server.h"

/* Reported by the debugger (debug.cpp): true while the CPU is parked in
 * DEBUG_Loop. Determines which execution state queued requests are serviced in
 * (see "Protocol semantics"). C_MCP implies C_DEBUG, so this always exists. */
bool IsDebuggerActive(void);

const char *MCP_Version(void) {
    return MCP_VERSION;
}

/* Current execution state as the dispatcher should see it. GFX_Events() (our
 * single run-class call site) runs in both the normal loop and DEBUG_Loop, so a
 * single drain here services requests in either state; the state is what decides
 * mode-mismatch fast-reject, not the call site. */
static mcp::ExecState MCP_CurrentExecState(void) {
    return IsDebuggerActive() ? mcp::STATE_PARKED : mcp::STATE_RUNNING;
}

/* Slice 1 screenshot self-test. Gated behind MCP_SELFTEST_SCREENSHOT so it is
 * inert in any real session: only the integration harness sets it. After the
 * guest has had a fixed number of frames to boot, request exactly one screenshot
 * by setting CAPTURE_IMAGE; the next rendered frame flushes a PNG via
 * RENDER_EndUpdate -> CAPTURE_AddImage and the harness detects the file. */
static void MCP_SelfTestScreenshot(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("MCP_SELFTEST_SCREENSHOT");
        enabled = (e != NULL && *e != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (!enabled)
        return;

    /* Frames to wait before requesting the shot (override via env for slow CI). */
    static unsigned long trigger_at = 0;
    if (trigger_at == 0) {
        const char *f = getenv("MCP_SELFTEST_FRAMES");
        long v = (f != NULL) ? strtol(f, NULL, 10) : 0;
        trigger_at = (v > 0) ? (unsigned long)v : 120UL;
    }

    static unsigned long frame = 0;
    static bool requested = false;
    if (++frame == trigger_at && !requested) {
        requested = true;
        CaptureState |= CAPTURE_IMAGE;
    }
}

/* The emulator-thread "run-class" service point, called once per frame from
 * GFX_Events() (the single core call site; see the core-edit manifest). It is
 * the only place MCP touches the running emulator: it lazily starts the TCP
 * server and drains the request queue against the current execution state.
 * Because GFX_Events() also runs inside DEBUG_Loop while the CPU is parked, this
 * one drain covers both the run-class and parked-class service points. */
void MCP_GFXFrameService(void) {
    MCP_SelfTestScreenshot();

    mcp::Server &srv = mcp::Server::instance();
    srv.ensure_started();
    srv.drain(MCP_CurrentExecState());
}

#endif /* C_MCP */
