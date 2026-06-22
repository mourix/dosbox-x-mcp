/*
 *  DOSBox-X MCP — TCP JSON-RPC server + request queue + dispatcher drain.
 *
 *  Threading model (see docs/MCP_BUILD_PLAN.md "Protocol semantics"):
 *    - one server thread accepts a single client on 127.0.0.1, reads
 *      newline-delimited JSON-RPC, and only *enqueues* requests, then waits
 *      (bounded 5 s) for the emulator thread to fill in the reply;
 *    - the emulator thread drains the queue at the GFX_Events() frame tick via
 *      Server::drain(), so all request handling stays single-threaded.
 *
 *  Compiled only under --enable-mcp.
 */

#ifndef DOSBOX_MCP_SERVER_H
#define DOSBOX_MCP_SERVER_H

#include "config.h"

#if C_MCP

#include "mcp_protocol.h"

namespace mcp {

class Server {
public:
    static Server &instance();

    /* Idempotent. On first call reads the MCP_PORT env var; if it names a
     * nonzero port, binds 127.0.0.1:PORT and starts the accept thread. With no
     * MCP_PORT the server stays disabled (normal interactive runs open no
     * socket). Called every frame from MCP_GFXFrameService(). */
    void ensure_started();

    /* Emulator-thread service point: process every queued request against the
     * given current execution state and hand replies back to waiting clients.
     * Non-blocking and bounded — safe to call once per frame. */
    void drain(ExecState state);

    bool running() const { return running_; }
    int  port() const { return port_; }

private:
    Server() {}
    Server(const Server &);
    Server &operator=(const Server &);

    void serverThread();

    bool started_ = false;   /* ensure_started() has run                       */
    bool running_ = false;   /* listening socket is up                         */
    int  listen_fd_ = -1;
    int  port_ = 0;
};

} // namespace mcp

#endif /* C_MCP */

#endif /* DOSBOX_MCP_SERVER_H */
