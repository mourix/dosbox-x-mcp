/*
 *  DOSBox-X MCP (Model Context Protocol) server.
 *
 *  This translation unit is linked into dosbox-x only when configured with
 *  --enable-mcp (libmcp.a, wired via configure.ac + src/Makefile.am +
 *  src/mcp/Makefile.am). It carries no TCP transport yet; that arrives in
 *  Slice 2. See docs/MCP_BUILD_PLAN.md.
 */

#include "mcp.h"

#if C_MCP

#include <cstdlib>
#include <cstring>

#include "dosbox.h"   /* Bitu, used by hardware.h */
#include "hardware.h" /* CaptureState, CAPTURE_IMAGE */

const char *MCP_Version(void) {
    return MCP_VERSION;
}

/* Slice 1 self-test. Gated behind the MCP_SELFTEST_SCREENSHOT env var so it is
 * inert in any real session: only the integration harness sets it. After the
 * guest has had a fixed number of frames to boot to a rendered screen, request
 * exactly one screenshot by setting CAPTURE_IMAGE (the same flag the screenshot
 * mapper action sets). The next rendered frame flushes a PNG via
 * RENDER_EndUpdate -> CAPTURE_AddImage; the harness detects the file and shuts
 * the process down. We request only once: the flag stays set until a frame is
 * actually rendered, so if the dummy video driver never renders, no PNG appears
 * and the harness fails (correctly flagging the de-risk assumption). */
void MCP_GFXFrameService(void) {
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

#endif /* C_MCP */
