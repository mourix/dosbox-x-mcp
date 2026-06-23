/*
 *  DOSBox-X MCP — emulator-state bridge for input injection (Slice 8):
 *  send_keys / type_text / mouse.
 *
 *  These are run-class tools: they execute on the emulator thread at the
 *  GFX_Events frame tick (while the guest free-runs) when the dispatcher drains
 *  the request queue. Like the Slice 3/4/7 read/write bridges this reuses
 *  existing public APIs and needs NO core edit:
 *
 *   - send_keys      -> KEYBOARD_AddKey (immediate press/release transitions);
 *   - type_text      -> decoded transitions are pushed onto a frame-paced queue
 *                       and fed a few per frame by MCP_InputFrameService, so a
 *                       long string cannot overflow the keyboard controller
 *                       buffer and all injection stays single-threaded (unlike
 *                       MAPPER_AutoType, which would mutate keyboard state from a
 *                       background std::thread — see mcp_protocol.h for the
 *                       rationale of this deviation);
 *   - mouse          -> the direct Mouse_* APIs (Mouse_CursorMoved /
 *                       Mouse_ButtonPressed/Released / Mouse_WheelMoved), which
 *                       are window/SDL-independent and so work headless, unlike
 *                       GFX_EventsMouseProcess.
 *
 *  Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#include "mcp_protocol.h"

#if C_MCP

#include <deque>

#include "dosbox.h"
#include "keyboard.h"
#include "mouse.h"

/* debug.cpp: true while the CPU is parked in DEBUG_Loop. The paced typer only
 * feeds keys while the guest is running (parked: it has no chance to consume
 * them, so the queue is held until execution resumes). */
bool IsDebuggerActive(void);

namespace {
/* Pending type_text transitions, drained on the emulator thread (single owner:
 * the GFX_Events service point, which is the emulator thread). */
std::deque<mcp::KeyEvent> g_type_queue;

/* Transitions fed per frame, and the controller-buffer headroom kept free. A
 * shifted character is 4 transitions; feeding 4/frame paces typing without
 * crowding the 96-slot keyboard buffer. */
const size_t kTypeDrainPerFrame = 4;
const size_t kBufferHeadroom    = 8;
}

namespace mcp {

void send_keys(const std::vector<KeyEvent> &events) {
    for (size_t i = 0; i < events.size(); i++)
        KEYBOARD_AddKey((KBD_KEYS)events[i].kbd, events[i].down);
}

void type_text_enqueue(const std::vector<KeyEvent> &events) {
    for (size_t i = 0; i < events.size(); i++)
        g_type_queue.push_back(events[i]);
}

void mouse_action(const MouseRequest &req) {
    switch (req.action) {
        case MOUSE_MOVE:
            /* Relative motion with emulate=true drives the guest's mouse the
             * same way captured SDL relative movement does. */
            Mouse_CursorMoved((float)req.dx, (float)req.dy, 0.0f, 0.0f, true);
            break;
        case MOUSE_DOWN:
            Mouse_ButtonPressed((uint8_t)req.button);
            break;
        case MOUSE_UP:
            Mouse_ButtonReleased((uint8_t)req.button);
            break;
        case MOUSE_CLICK:
            Mouse_ButtonPressed((uint8_t)req.button);
            Mouse_ButtonReleased((uint8_t)req.button);
            break;
        case MOUSE_WHEEL:
            Mouse_WheelMoved(req.wheel);
            break;
    }
}

} // namespace mcp

/* Called once per frame from MCP_GFXFrameService (the emulator thread). Feeds a
 * bounded batch of queued type_text transitions into the keyboard controller
 * while the guest runs and there is buffer headroom. A no-op when the queue is
 * empty or the CPU is parked. */
void MCP_InputFrameService(void) {
    if (g_type_queue.empty() || IsDebuggerActive())
        return;
    size_t fed = 0;
    while (!g_type_queue.empty() && fed < kTypeDrainPerFrame) {
        if (KEYBOARD_BufferSpaceAvail() < kBufferHeadroom)
            break;
        const mcp::KeyEvent &e = g_type_queue.front();
        KEYBOARD_AddKey((KBD_KEYS)e.kbd, e.down);
        g_type_queue.pop_front();
        fed++;
    }
}

#endif /* C_MCP */
