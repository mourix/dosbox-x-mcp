/*
 *  DOSBox-X MCP — TCP JSON-RPC server + request queue + dispatcher drain (impl).
 *  See mcp_server.h. Linux POSIX sockets only (the MCP build is Linux-only; see
 *  docs/MCP_BUILD_PLAN.md "Platform scope"). Compiled only under --enable-mcp.
 */

#include "mcp_server.h"

#if C_MCP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "logging.h"

namespace mcp {

namespace {

/* One in-flight request, handed from the server thread to the emulator thread
 * and back. shared_ptr-owned so a timed-out request stays alive until the
 * dispatcher is done with it (never a use-after-free across the threads). */
struct PendingRequest {
    std::string method;
    Json        params;
    Json        id;

    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    std::string             reply;   /* set by the emulator thread */
};

using RequestPtr = std::shared_ptr<PendingRequest>;

std::mutex               g_queue_mutex;
std::deque<RequestPtr>   g_queue;

const std::chrono::seconds kRequestTimeout(5);

/* Bounded so a hostile/garbled client can't make us buffer unboundedly while
 * waiting for a newline. One line carries one JSON-RPC request. */
const size_t kMaxLineBytes = MCP_MAX_PAYLOAD;

bool write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

bool write_line(int fd, const std::string &s) {
    std::string framed = s;
    framed.push_back('\n');
    return write_all(fd, framed.data(), framed.size());
}

/* Enqueue a parsed request and block (bounded) for the emulator thread to fill
 * in the reply. Returns the reply line to send back to the client. */
std::string submit_and_wait(const std::string &method, const Json &params, const Json &id) {
    RequestPtr req = std::make_shared<PendingRequest>();
    req->method = method;
    req->params = params;
    req->id     = id;

    {
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        g_queue.push_back(req);
    }

    std::unique_lock<std::mutex> lk(req->m);
    bool ok = req->cv.wait_for(lk, kRequestTimeout, [&] { return req->done; });
    if (ok) return req->reply;
    /* Timed out. The request may still be sitting in the queue; the dispatcher
     * will harmlessly complete it later (no waiter), and the shared_ptr keeps it
     * alive until then. */
    return make_timeout_error(req->id);
}

/* Handle one complete JSON-RPC line from the client. */
std::string handle_line(const std::string &line) {
    Json root;
    if (!Json::parse(line, root) || !root.isObject())
        return make_error(Json::null(), MCP_ERR_PARSE, "parse error");

    const Json *jmethod = root.find("method");
    if (jmethod == nullptr || !jmethod->isString())
        return make_error(Json::null(), MCP_ERR_INVALID_REQ, "missing method");

    const Json *jid = root.find("id");
    Json id = (jid != nullptr) ? *jid : Json::null();

    const Json *jparams = root.find("params");
    Json params = (jparams != nullptr) ? *jparams : Json::object();

    return submit_and_wait(jmethod->asString(), params, id);
}

/* Drain whatever complete lines are in buf, dispatching each. Leaves any
 * trailing partial line in buf. Returns false if the client should be dropped. */
bool process_buffer(int fd, std::string &buf) {
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!write_line(fd, handle_line(line))) return false;
    }
    if (buf.size() > kMaxLineBytes) return false; /* runaway line, drop client */
    return true;
}

int accept_one(int listen_fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    return ::accept(listen_fd, (sockaddr *)&addr, &len);
}

} // namespace

// -- Server ----------------------------------------------------------------

Server &Server::instance() {
    static Server inst;
    return inst;
}

void Server::ensure_started() {
    if (started_) return;
    started_ = true;

    const char *ps = ::getenv("MCP_PORT");
    long port = (ps != nullptr) ? ::strtol(ps, nullptr, 10) : 0;
    if (port <= 0 || port > 65535) {
        LOG(LOG_MISC, LOG_DEBUG)("MCP: server disabled (MCP_PORT unset/invalid)");
        return;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_MSG("MCP: socket() failed: %s", strerror(errno));
        return;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    /* Localhost only — never INADDR_ANY/0.0.0.0. Asserted invariant (guardrail
     * #5). MCP_BIND_ADDRESS documents the same constant for tooling. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_MSG("MCP: bind 127.0.0.1:%ld failed: %s", port, strerror(errno));
        ::close(fd);
        return;
    }
    /* Backlog 1: a single client at a time; extras are accepted then refused. */
    if (::listen(fd, 1) < 0) {
        LOG_MSG("MCP: listen failed: %s", strerror(errno));
        ::close(fd);
        return;
    }

    listen_fd_ = fd;
    port_ = (int)port;
    running_ = true;
    LOG_MSG("MCP: JSON-RPC server listening on 127.0.0.1:%d", port_);

    std::thread(&Server::serverThread, this).detach();
}

void Server::serverThread() {
    int client_fd = -1;
    std::string buf;

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds].fd = listen_fd_; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        if (client_fd >= 0) {
            fds[nfds].fd = client_fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        }

        int r = ::poll(fds, nfds, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        /* New connection. */
        if (fds[0].revents & POLLIN) {
            int c = accept_one(listen_fd_);
            if (c >= 0) {
                if (client_fd >= 0) {
                    /* Single-client: refuse the newcomer with a clear error. */
                    write_line(c, make_busy_error());
                    ::shutdown(c, SHUT_RDWR);
                    ::close(c);
                } else {
                    client_fd = c;
                    buf.clear();
                }
            }
        }

        /* Client data / disconnect. */
        if (client_fd >= 0 && nfds == 2) {
            short ev = fds[1].revents;
            if (ev & POLLIN) {
                char tmp[4096];
                ssize_t n = ::recv(client_fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    buf.append(tmp, (size_t)n);
                    if (!process_buffer(client_fd, buf)) {
                        ::close(client_fd); client_fd = -1; buf.clear();
                    }
                } else {
                    /* EOF or error: drop the client, allow a fresh connection. */
                    ::close(client_fd); client_fd = -1; buf.clear();
                }
            } else if (ev & (POLLHUP | POLLERR | POLLNVAL)) {
                ::close(client_fd); client_fd = -1; buf.clear();
            }
        }
    }
}

void Server::drain(ExecState state) {
    if (!running_) return;

    for (;;) {
        RequestPtr req;
        {
            std::lock_guard<std::mutex> lk(g_queue_mutex);
            if (g_queue.empty()) break;
            req = g_queue.front();
            g_queue.pop_front();
        }

        std::string reply = dispatch(req->method, req->params, req->id, state);

        {
            std::lock_guard<std::mutex> lk(req->m);
            req->reply = reply;
            req->done = true;
        }
        req->cv.notify_one();
    }
}

} // namespace mcp

/* Exposed to the protocol layer (server_info) without pulling in the server
 * header there; keeps mcp_protocol.cpp transport-agnostic. */
int MCP_ServerPort(void) {
    return mcp::Server::instance().port();
}

#endif /* C_MCP */
