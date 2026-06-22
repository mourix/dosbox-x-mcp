/*
 *  DOSBox-X MCP — emulator-state bridge for read_memory + disassemble (Slice 4)
 *  and write_memory (Slice 7).
 *
 *  The single place these tools touch core emulator memory. The reads (Slice 4)
 *  mirror the debugger's data/code views; the write (Slice 7) mirrors its
 *  SM/SMV/SMP commands. The param parsing and JSON formatting live in the pure
 *  protocol layer (mcp_protocol.cpp) and are unit-tested without a boot. No core
 *  files are edited: this reuses the same primitives those commands use —
 *
 *    GetAddress()        (debug.cpp:445)  selector/offset -> linear address;
 *    mem_readb_checked() (paging.h:486)   linear read, false == success;
 *    mem_write{b,w,d}_checked() (paging.h:514) linear write, true == fault;
 *    physdev_read/writeb() (mem.h:346)    physical-bus access;
 *    DasmI386()          (debug_disasm.cpp:1317) x86 disassembler.
 *
 *  so it adds nothing to the core-edit manifest. Must run on the emulator thread
 *  while parked. Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include "dosbox.h"
#include "mem.h"
#include "paging.h"
#include "cpu.h"

/* Defined in src/debug/ but not declared in any installed header; declared here
 * (matching the definitions) to avoid pulling the ncurses-laden debug headers
 * into the MCP build and to keep this bridge's includes minimal. */
extern uint64_t GetAddress(uint16_t seg, uint32_t offset);
extern Bitu DasmI386(char *buffer, PhysPt pc, uint32_t cur_ip, bool bit32);

namespace mcp {

/* Mirrors debug.cpp's static `mem_no_address` sentinel (GetAddress returns this
 * when a protected-mode selector does not resolve). */
static const uint64_t MCP_NO_ADDRESS = ~(uint64_t)0;

void read_memory(const MemReadRequest &req, MemReadResult &out) {
    out.addr_valid = true;
    out.addr = 0;
    out.bytes.clear();
    out.readable.clear();
    out.bytes.reserve(req.len);
    out.readable.reserve(req.len);

    if (req.space == SPACE_PHYSICAL) {
        /* Physical-bus read: physdev_readb always returns a byte (open bus reads
         * back as 0xFF / device default), so every byte is "readable". */
        out.addr = req.off;
        for (uint32_t i = 0; i < req.len; i++) {
            out.bytes.push_back(physdev_readb((PhysPt64)((uint64_t)req.off + i)));
            out.readable.push_back(true);
        }
        return;
    }

    if (req.space == SPACE_SEGMENTED) {
        uint64_t base = GetAddress(req.seg, req.off);
        if (base == MCP_NO_ADDRESS) { out.addr_valid = false; return; }
        out.addr = base;
        for (uint32_t i = 0; i < req.len; i++) {
            /* Resolve per byte: an expand/limit boundary mid-range yields "na",
             * matching the debugger's data view (debug.cpp:1052). */
            uint64_t a = GetAddress(req.seg, req.off + i);
            if (a == MCP_NO_ADDRESS) { out.bytes.push_back(0); out.readable.push_back(false); continue; }
            uint8_t v = 0;
            bool ok = !mem_readb_checked((LinearPt)a, &v);
            out.bytes.push_back(ok ? v : 0);
            out.readable.push_back(ok);
        }
        return;
    }

    /* SPACE_VIRTUAL: the offset is already a linear address. */
    out.addr = req.off;
    for (uint32_t i = 0; i < req.len; i++) {
        uint8_t v = 0;
        bool ok = !mem_readb_checked((LinearPt)((uint64_t)req.off + i), &v);
        out.bytes.push_back(ok ? v : 0);
        out.readable.push_back(ok);
    }
}

void write_memory(const MemWriteRequest &req, MemWriteResult &out) {
    out.addr_valid = true;
    out.addr = 0;
    out.written = 0;
    out.bytes = 0;
    out.fault = false;

    if (req.space == SPACE_SEGMENTED) {
        uint64_t base = GetAddress(req.seg, req.off);
        if (base == MCP_NO_ADDRESS) { out.addr_valid = false; return; }
        out.addr = base;
    } else {
        out.addr = req.off; /* virtual = linear; physical = physical addr */
    }

    for (size_t i = 0; i < req.values.size(); i++) {
        uint32_t off = req.off + (uint32_t)(i * (size_t)req.width);
        uint32_t v   = req.values[i];

        if (req.space == SPACE_PHYSICAL) {
            /* physdev_write* are void (open-bus writes are dropped silently). */
            if (req.width == 4)      physdev_writed((PhysPt64)((uint64_t)off), v);
            else if (req.width == 2) physdev_writew((PhysPt64)((uint64_t)off), (uint16_t)v);
            else                     physdev_writeb((PhysPt64)((uint64_t)off), (uint8_t)v);
        } else {
            uint64_t a;
            if (req.space == SPACE_SEGMENTED) {
                a = GetAddress(req.seg, off);          /* resolve per slot, like read_memory */
                if (a == MCP_NO_ADDRESS) { out.fault = true; break; }
            } else {
                a = off;                               /* SPACE_VIRTUAL: linear directly */
            }
            /* mem_write*_checked return true on a page fault / write-protect. */
            bool fault = (req.width == 4) ? mem_writed_checked((LinearPt)a, v)
                       : (req.width == 2) ? mem_writew_checked((LinearPt)a, (uint16_t)v)
                                          : mem_writeb_checked((LinearPt)a, (uint8_t)v);
            if (fault) { out.fault = true; break; }
        }

        out.written++;
        out.bytes += (size_t)req.width;
    }
}

void disassemble(const DisasmRequest &req, DisasmResult &out) {
    out.addr_valid = true;
    out.big = req.have_big ? req.big : cpu.code.big;
    out.insns.clear();

    uint32_t eip = req.off;
    for (uint32_t n = 0; n < req.count; n++) {
        uint64_t start = GetAddress(req.seg, eip);
        if (start == MCP_NO_ADDRESS) {
            if (n == 0) out.addr_valid = false;
            break;
        }

        char dline[256] = {0};
        Bitu size = DasmI386(dline, (PhysPt)start, eip, out.big);
        if (size == 0) break; /* never advance zero; avoids an unbounded loop */

        DisasmInsn ins;
        ins.seg  = req.seg;
        ins.off  = eip;
        ins.addr = start;
        ins.text = dline;
        for (Bitu c = 0; c < size; c++) {
            uint8_t v = 0;
            bool ok = !mem_readb_checked((LinearPt)(start + c), &v);
            ins.bytes.push_back(ok ? v : 0);
            ins.readable.push_back(ok);
        }
        out.insns.push_back(ins);
        eip += (uint32_t)size;
    }
}

} // namespace mcp

#endif /* C_MCP */
