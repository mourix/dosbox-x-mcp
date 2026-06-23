/*
 *  DOSBox-X MCP — emulator-state bridge for take_screenshot (Slice 10):
 *  full-fidelity graphics capture on demand.
 *
 *  Run-class tool serviced at the GFX_Events frame tick. Capture is asynchronous
 *  — CAPTURE_IMAGE only flushes a PNG on the *next* rendered frame (via
 *  RENDER_EndUpdate -> CAPTURE_AddImage, the path Slice 1 verified headless) — so
 *  this bridge runs a tiny per-frame state machine instead of blocking the
 *  emulator thread:
 *
 *    IDLE   : snapshot the capture dir, remember the source mode, arm the capture
 *             (CaptureState |= CAPTURE_IMAGE), arm a deadline, go WAITING.
 *    WAITING: each frame, look for a PNG that appeared since the snapshot. When
 *             found, read its IHDR dims + size and report SHOT_READY; on deadline
 *             report SHOT_ERROR.
 *
 *  While WAITING the dispatcher gets SHOT_PENDING and re-queues the request via
 *  defer_sentinel(), all under the client's 5 s wait. Like the other read bridges
 *  this reuses existing globals/APIs (CaptureState/CAPTURE_IMAGE from hardware.h,
 *  the file-scope `capturedir`, CurMode/M_TEXT, render.src) — NO core edit. The
 *  written PNG never crosses the wire; only its path + metadata do. Compiled only
 *  under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <dirent.h>
#include <climits>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "dosbox.h"
#include "mem.h"       /* PhysPt/RealPt — needed by int10.h's declarations */
#include "hardware.h"  /* CaptureState, CAPTURE_IMAGE */
#include "render.h"    /* render.src dimensions (fallback) */
#include "int10.h"     /* CurMode, M_TEXT */

/* File-scope global in src/hardware/hardware.cpp (external linkage, not in a
 * header); declared here like the other bridges declare the symbols they reuse. */
extern std::string capturedir;

namespace mcp {

namespace {

const char *const kPngExt = ".png";
/* Bound the wait below the 5 s client/queue timeout so a render stall yields a
 * clean error instead of an ambiguous timeout. The PNG normally appears within a
 * frame or two (sub-second). */
const std::chrono::milliseconds kShotDeadline(4000);

bool ends_with_png(const std::string &name) {
    size_t n = name.size(), e = std::strlen(kPngExt);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++)
        if (std::tolower((unsigned char)name[n - e + i]) != kPngExt[i]) return false;
    return true;
}

void list_pngs(const std::string &dir, std::set<std::string> &out) {
    out.clear();
    DIR *d = ::opendir(dir.c_str());
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = ::readdir(d)) != NULL) {
        std::string name = ent->d_name;
        if (ends_with_png(name)) out.insert(name);
    }
    ::closedir(d);
}

/* The newest .png present now that was not in `baseline` (highest filename wins;
 * OpenCaptureFile numbers screenshots monotonically). Returns false if none. */
bool find_new_png(const std::string &dir, const std::set<std::string> &baseline,
                  std::string &found) {
    std::set<std::string> now;
    list_pngs(dir, now);
    bool any = false;
    for (std::set<std::string>::const_iterator it = now.begin(); it != now.end(); ++it) {
        if (baseline.find(*it) == baseline.end()) { found = *it; any = true; }
    }
    return any;
}

/* Read width/height from a PNG IHDR (bytes 16..23, big-endian) and the file
 * size. Returns false if the file is not a readable PNG. */
bool png_dims(const std::string &path, int &w, int &h, long &size) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == NULL) return false;
    unsigned char hdr[24];
    size_t got = std::fread(hdr, 1, sizeof(hdr), fp);
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fclose(fp);
    static const unsigned char magic[8] = { 0x89,'P','N','G','\r','\n',0x1a,'\n' };
    if (got < sizeof(hdr) || std::memcmp(hdr, magic, sizeof(magic)) != 0) return false;
    w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    size = sz;
    return true;
}

long file_size(const std::string &path) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == NULL) return 0;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fclose(fp);
    return sz;
}

std::string abspath(const std::string &p) {
    char buf[PATH_MAX];
    if (::realpath(p.c_str(), buf) != NULL) return std::string(buf);
    return p;
}

enum Phase { IDLE, WAITING };

Phase                                 g_phase = IDLE;
std::set<std::string>                 g_baseline;
std::chrono::steady_clock::time_point g_deadline;
int                                   g_mode = 0;
bool                                  g_is_text = false;

} // namespace

ShotStatus screenshot_service(ScreenshotResult &out) {
    if (g_phase == IDLE) {
        if (capturedir.empty()) {
            out.error = "no capture directory configured (set 'dosbox captures=...')";
            return SHOT_ERROR;
        }
        list_pngs(capturedir, g_baseline);
        g_mode    = (CurMode != NULL) ? (int)CurMode->mode : 0;
        g_is_text = (CurMode != NULL && CurMode->type == M_TEXT);
        CaptureState |= CAPTURE_IMAGE;   /* next rendered frame flushes a PNG */
        g_deadline = std::chrono::steady_clock::now() + kShotDeadline;
        g_phase    = WAITING;
        return SHOT_PENDING;
    }

    /* WAITING: watch the capture dir for the freshly written PNG. */
    std::string name;
    if (find_new_png(capturedir, g_baseline, name)) {
        std::string path = capturedir;
        if (!path.empty() && path[path.size() - 1] != '/') path += '/';
        path += name;
        out.path    = abspath(path);
        out.mode    = g_mode;
        out.is_text = g_is_text;

        int  w = (int)render.src.width;
        int  h = (int)render.src.height;
        long size = 0;
        if (!png_dims(out.path, w, h, size))
            size = file_size(out.path); /* fall back to source dims + raw size */
        out.width  = w;
        out.height = h;
        out.bytes  = size;

        g_phase = IDLE;
        return SHOT_READY;
    }

    if (std::chrono::steady_clock::now() >= g_deadline) {
        g_phase = IDLE;
        out.error = "capture produced no file within the deadline";
        return SHOT_ERROR;
    }
    return SHOT_PENDING;
}

} // namespace mcp

#endif /* C_MCP */
