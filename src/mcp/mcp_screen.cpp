/*
 *  DOSBox-X MCP — emulator-state bridge for screen reads (Slice 9):
 *  read_screen / screen_hash.
 *
 *  Run-class tools: they execute on the emulator thread at the GFX_Events frame
 *  tick (while the guest free-runs) when the dispatcher drains the request
 *  queue. Like the Slice 3/4/7/8 bridges this reuses existing globals/APIs and
 *  needs NO core edit:
 *
 *   - CurMode / VideoModeBlock (int10.h)  current video mode + its type;
 *   - ReadCharAttr() (int10_char.cpp)      one text cell (char+attr), the same
 *                                          primitive INT 10h AH=08h uses;
 *   - real_readw/real_readb (mem.h)        BIOS data area (cols/rows/page);
 *   - vga.mem.linear (vga.h)               guest video memory for the graphics
 *                                          fingerprint;
 *   - render.src (render.h)                source frame dimensions.
 *
 *  The param-free reads are formatted by the pure protocol layer
 *  (mcp_protocol.cpp), unit-tested without a boot. Compiled only under
 *  --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <vector>

#include "dosbox.h"
#include "mem.h"
#include "vga.h"
#include "render.h"
#include "int10.h" /* VideoModeBlock, CurMode, M_TEXT, BIOSMEM_* defines */

/* Defined in src/ints/int10_char.cpp but not declared in int10.h (only the
 * INT10_ReadCharAttr wrapper is); declared here matching its definition, like
 * the other bridges declare GetAddress/DasmI386. */
extern void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result);

namespace mcp {

/* Read the visible text grid dimensions from the BIOS data area, falling back to
 * the mode block, clamped to the defensive caps. Shared by both tools. */
static void text_grid_dims(int &cols, int &rows) {
    cols = (int)real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
    rows = (int)real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1;
    if (cols <= 0) cols = (int)CurMode->twidth;
    if (rows <= 1) rows = (int)CurMode->theight;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > MCP_SCREEN_MAX_COLS) cols = MCP_SCREEN_MAX_COLS;
    if (rows > MCP_SCREEN_MAX_ROWS) rows = MCP_SCREEN_MAX_ROWS;
}

/* Gather the cols*rows cells into `buf`: low byte = character, and when
 * `with_attr` the following byte = attribute (used for the fingerprint). */
static void gather_cells(int cols, int rows, bool with_attr, std::vector<uint8_t> &buf) {
    uint8_t page = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);
    buf.reserve((size_t)cols * (size_t)rows * (with_attr ? 2u : 1u));
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint16_t cell = 0;
            ReadCharAttr((uint16_t)c, (uint16_t)r, page, &cell);
            buf.push_back((uint8_t)(cell & 0xff));
            if (with_attr) buf.push_back((uint8_t)(cell >> 8));
        }
    }
}

void read_screen(ScreenSnapshot &out) {
    out.chars.clear();
    out.mode    = CurMode ? (int)CurMode->mode : 0;
    out.is_text = (CurMode != NULL && CurMode->type == M_TEXT);
    out.cols    = 0;
    out.rows    = 0;
    if (!out.is_text) return;

    text_grid_dims(out.cols, out.rows);
    gather_cells(out.cols, out.rows, false, out.chars);
}

void screen_hash(ScreenHash &out) {
    out.mode    = CurMode ? (int)CurMode->mode : 0;
    out.is_text = (CurMode != NULL && CurMode->type == M_TEXT);
    out.cols    = 0;
    out.rows    = 0;
    out.hash    = 0;

    if (out.is_text) {
        /* Hash the visible char+attribute cells — exactly the displayed text
         * state, so the fingerprint tracks what read_screen returns. */
        text_grid_dims(out.cols, out.rows);
        std::vector<uint8_t> buf;
        gather_cells(out.cols, out.rows, true, buf);
        out.hash = fnv1a64(buf.data(), buf.size());
        return;
    }

    /* Graphics mode: seed with the source frame dimensions/bpp, then fold in a
     * bounded scan of guest video memory. This changes when the screen changes
     * and is stable otherwise, even under the dummy video driver. */
    out.cols = (int)render.src.width;
    out.rows = (int)render.src.height;
    uint8_t seed[12];
    uint32_t w = (uint32_t)render.src.width;
    uint32_t h = (uint32_t)render.src.height;
    uint32_t b = (uint32_t)render.src.bpp;
    for (int i = 0; i < 4; i++) { seed[i]   = (uint8_t)(w >> (i * 8)); }
    for (int i = 0; i < 4; i++) { seed[4+i] = (uint8_t)(h >> (i * 8)); }
    for (int i = 0; i < 4; i++) { seed[8+i] = (uint8_t)(b >> (i * 8)); }
    uint64_t hash = fnv1a64(seed, sizeof(seed));

    if (vga.mem.linear != NULL && vga.mem.memsize > 0) {
        size_t len = vga.mem.memsize;
        if (len > MCP_SCREENHASH_SCAN_MAX) len = MCP_SCREENHASH_SCAN_MAX;
        /* Continue the FNV-1a chain over the scanned bytes. */
        const uint8_t *p = (const uint8_t *)vga.mem.linear;
        for (size_t i = 0; i < len; i++) { hash ^= (uint64_t)p[i]; hash *= 1099511628211ULL; }
    }
    out.hash = hash;
}

} // namespace mcp

#endif /* C_MCP */
