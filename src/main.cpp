#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "http/RequestParser.hpp"
#include "http/Router.hpp"
#include "http/ThreadPool.hpp"
#include "util/Logger.hpp"
#include "util/StringUtils.hpp"

// ── Shutdown flag ─────────────────────────────────────────────────────────────
// Set to true by the SIGINT/SIGTERM handler; the accept loop checks it each
// iteration and breaks cleanly, allowing in-flight requests to finish before
// the process exits.
static std::atomic<bool> g_shutdown{false};

namespace {

// ── Low-level send helpers ───────────────────────────────────────────────────

// Loops send() to handle partial writes, which POSIX explicitly permits
// (send() may transfer fewer bytes than requested even on success).
bool SendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Streams a file to the socket via sendfile() (zero-copy, kernel-space only).
// Loops to handle partial transfers — sendfile() may move fewer bytes than
// requested when the socket send buffer is full.
bool SendFileAll(int socket_fd, int file_fd, size_t len) {
    off_t offset = 0;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = sendfile(socket_fd, file_fd, &offset, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("sendfile");
            return false;
        }
        if (n == 0) {
            break;  // file shrunk between stat() and sendfile() — stop gracefully
        }
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool SendResponse(int client_fd, const http::Response& response) {
    std::string header_bytes = response.SerializeHeader();
    if (!SendAll(client_fd, header_bytes.data(), header_bytes.size())) {
        if (response.body_fd) {
            close(*response.body_fd);
        }
        return false;
    }
    if (response.body_fd) {
        // Zero-copy path (Milestone 12): stream the file directly from kernel
        // space, then close the fd whose ownership was transferred from the router.
        bool ok = SendFileAll(client_fd, *response.body_fd, response.body_size);
        close(*response.body_fd);
        return ok;
    }
    if (!response.body.empty() && !SendAll(client_fd, response.body.data(), response.body.size())) {
        return false;
    }
    return true;
}

void SendBadRequest(int client_fd) {
    const char* bad_request =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SendAll(client_fd, bad_request, std::strlen(bad_request));
}

void SendPayloadTooLarge(int client_fd) {
    const char* response =
        "HTTP/1.1 413 Payload Too Large\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SendAll(client_fd, response, std::strlen(response));
}

void SendNotImplemented(int client_fd) {
    const char* response =
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SendAll(client_fd, response, std::strlen(response));
}

void SendServiceUnavailable(int client_fd) {
    // The thread pool queue is full — there are already kMaxQueueDepth
    // pending connections.  Returning 503 immediately is better than making
    // the client wait for an unknown duration with no feedback.
    const char* response =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SendAll(client_fd, response, std::strlen(response));
}

// ── Request-reading constants ─────────────────────────────────────────────────

// Real-world servers cap this (nginx defaults to 8K) so a client that never
// sends a terminating blank line can't grow this buffer without bound.
constexpr size_t kMaxRequestHeaderBytes = 8192;

// A request body beyond this is rejected with 413 rather than read into
// memory in full — this server has no use for large request bodies since
// the router only ever serves GET/HEAD.
constexpr size_t kMaxRequestBodyBytes = 1 << 20;  // 1 MiB

// How long a connection may sit idle — whether waiting on the first byte of
// a request or, once kept alive, waiting on the next one — before the
// server gives up on it.  Under a bounded thread pool (Milestone 9) a slow
// or idle client can occupy a worker for the full timeout duration, directly
// capping concurrent capacity.  Accepted tradeoff: keeps the I/O model
// simple without a separate epoll/event loop.
constexpr int kIdleTimeoutSeconds = 10;

// Maximum number of requests served on a single persistent connection.
// Without this, a keep-alive client could occupy a worker thread indefinitely
// even if each individual request completes promptly.
constexpr int kMaxRequestsPerConnection = 100;

// ── Header / body reading ─────────────────────────────────────────────────────

enum class HeaderReadStatus {
    kOk,       // a full header block (through the blank line) is in out_headers
    kClosed,   // peer closed or the idle timeout fired before any new request began
    kMalformed,  // a request started but never completed (truncated, too large, or a real I/O error)
};

// Reads from `buf` (bytes already buffered from a previous call) and then
// the socket, until `buf` contains a full "\r\n\r\n"-terminated header
// block.  Whatever comes after that terminator (a pipelined request, or body
// bytes belonging to this request) is left in `buf` for the next call,
// since TCP gives no guarantee that a single recv() lines up with HTTP
// message boundaries.
//
// Known gap (Milestone 11): the SO_RCVTIMEO timeout is per-recv() call, not
// a bound on total time-to-complete-headers.  A slow-loris client that sends
// one byte every (kIdleTimeoutSeconds - 1) seconds will never trigger the
// timeout here.  Closing this gap properly requires tracking wall-clock time
// across recv() calls or switching to non-blocking I/O + epoll.  Accepted
// as a known tradeoff for this milestone.
HeaderReadStatus ReadHeaderBlock(int client_fd, std::string& buf, std::string& out_headers) {
    bool received_any_for_this_request = !buf.empty();

    while (true) {
        size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            out_headers = buf.substr(0, pos + 4);
            buf.erase(0, pos + 4);
            return HeaderReadStatus::kOk;
        }
        if (buf.size() > kMaxRequestHeaderBytes) {
            return HeaderReadStatus::kMalformed;
        }

        char chunk[4096];
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && !received_any_for_this_request) {
                return HeaderReadStatus::kClosed;  // idle timeout between requests
            }
            return HeaderReadStatus::kMalformed;  // timeout or error mid-request
        }
        if (n == 0) {
            return received_any_for_this_request ? HeaderReadStatus::kMalformed
                                                   : HeaderReadStatus::kClosed;
        }

        received_any_for_this_request = true;
        buf.append(chunk, static_cast<size_t>(n));
    }
}

enum class BodyReadStatus {
    kOk,
    kTooLarge,
    kMalformed,
};

// Reads and discards exactly `content_length` bytes of request body (first
// from whatever's already buffered, then from the socket), so a body a
// client sends alongside GET/HEAD doesn't get misread as the start of the
// next pipelined request on a kept-alive connection.
BodyReadStatus DrainBody(int client_fd, std::string& buf, size_t content_length) {
    if (content_length > kMaxRequestBodyBytes) {
        return BodyReadStatus::kTooLarge;
    }

    while (buf.size() < content_length) {
        char chunk[4096];
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return BodyReadStatus::kMalformed;
        }
        if (n == 0) {
            return BodyReadStatus::kMalformed;
        }
        buf.append(chunk, static_cast<size_t>(n));
    }

    buf.erase(0, content_length);
    return BodyReadStatus::kOk;
}

std::optional<size_t> ParseContentLength(const std::string& value) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(),
                      [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    try {
        size_t pos = 0;
        unsigned long long parsed = std::stoull(value, &pos);
        if (pos != value.size()) {
            return std::nullopt;
        }
        return static_cast<size_t>(parsed);
    } catch (const std::exception&) {
        return std::nullopt;  // out_of_range, etc.
    }
}

// Case-insensitive check for whether a header value contains `token` as a
// substring.  Real Connection/Transfer-Encoding values are short,
// comma-separated lists ("keep-alive", "close", "keep-alive, Upgrade",
// "chunked") where a substring match is equivalent to a proper token match.
bool HeaderValueContains(const std::string& value, const std::string& token) {
    return util::ToLower(value).find(token) != std::string::npos;
}

// RFC 7230 6.3: HTTP/1.1 (and later) connections persist by default unless
// "Connection: close" is present; HTTP/1.0 connections close by default
// unless "Connection: keep-alive" is present.
bool WantsKeepAlive(const http::Request& request) {
    const std::string* connection = request.Header("Connection");
    bool is_1_1_or_later =
        request.version_major > 1 || (request.version_major == 1 && request.version_minor >= 1);

    if (is_1_1_or_later) {
        return !(connection && HeaderValueContains(*connection, "close"));
    }
    return connection && HeaderValueContains(*connection, "keep-alive");
}

// ── Connection handler ────────────────────────────────────────────────────────

// Serves requests off one accepted connection until the peer disconnects,
// the negotiated Connection semantics call for closing, the connection sits
// idle past kIdleTimeoutSeconds, or kMaxRequestsPerConnection is reached.
//
// client_ip is captured at accept() time (inet_ntop) and threaded through
// here so the structured log line can record it alongside request details.
void HandleConnection(int client_fd, std::string client_ip,
                      const http::StaticFileRouter& router, util::Logger& logger) {
    timeval timeout{};
    timeout.tv_sec = kIdleTimeoutSeconds;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt");
    }

    std::string buf;  // bytes read from the socket but not yet consumed
    int request_count = 0;

    for (;;) {
        if (request_count >= kMaxRequestsPerConnection) {
            break;  // cap total connection lifetime regardless of keep-alive
        }

        std::string header_block;
        HeaderReadStatus read_status = ReadHeaderBlock(client_fd, buf, header_block);
        if (read_status == HeaderReadStatus::kClosed) {
            break;  // nothing pending, nothing to respond to
        }
        if (read_status == HeaderReadStatus::kMalformed) {
            SendBadRequest(client_fd);
            break;
        }

        // Start latency measurement here — after the full header block is
        // received — so we're timing routing + response serialisation + send,
        // not the client's network RTT for delivering the request headers.
        auto req_start = std::chrono::steady_clock::now();

        auto request = http::ParseRequest(header_block);
        if (!request) {
            SendBadRequest(client_fd);
            break;
        }

        // Chunked request bodies aren't supported: without decoding the
        // chunk framing we can't know where the body ends, so we can't
        // safely keep parsing this connection as a byte stream.
        if (const std::string* transfer_encoding = request->Header("Transfer-Encoding");
            transfer_encoding && HeaderValueContains(*transfer_encoding, "chunked")) {
            SendNotImplemented(client_fd);
            break;
        }

        if (const std::string* content_length_header = request->Header("Content-Length")) {
            auto content_length = ParseContentLength(*content_length_header);
            if (!content_length) {
                SendBadRequest(client_fd);
                break;
            }
            BodyReadStatus body_status = DrainBody(client_fd, buf, *content_length);
            if (body_status == BodyReadStatus::kTooLarge) {
                SendPayloadTooLarge(client_fd);
                break;
            }
            if (body_status == BodyReadStatus::kMalformed) {
                break;  // connection died mid-body; nothing meaningful to send back
            }
        }

        bool keep_alive = WantsKeepAlive(*request);

        http::Response response = router.Handle(*request);
        response.keep_alive = keep_alive;

        bool sent = SendResponse(client_fd, response);

        // Latency: end-of-headers-read → end-of-response-sent.
        auto req_end = std::chrono::steady_clock::now();
        long latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              req_end - req_start).count();

        util::LogEntry entry;
        entry.client_ip  = client_ip;
        entry.method     = request->method_raw;
        entry.path       = request->path;
        entry.query      = request->query;
        entry.status     = response.status_code;
        entry.latency_ms = latency_ms;
        logger.Log(entry);

        ++request_count;

        if (!sent || !keep_alive) {
            break;
        }
    }

    close(client_fd);
}

}  // namespace

// ── Signal handling ───────────────────────────────────────────────────────────

#include <csignal>

static void HandleSignal(int /*signum*/) {
    // async-signal-safe: writing to an atomic<bool> is safe from a signal handler.
    g_shutdown.store(true);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const std::string public_root = argc > 2 ? argv[2] : "public";

    // Thread pool size: configurable via argv[3]; defaults to 4.
    // 4 workers is a sensible starting point matching a typical core count;
    // tune upward if your workload is I/O-bound and the OS supports more threads.
    constexpr std::size_t kDefaultWorkerCount = 4;
    const std::size_t worker_count =
        argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3])) : kDefaultWorkerCount;

    constexpr int kBacklog = 16;

    // ── Install signal handlers (Milestone 11) ────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = HandleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    auto router = http::StaticFileRouter::Create(public_root);
    if (!router) {
        std::fprintf(stderr,
                      "Fatal: public root \"%s\" does not exist or cannot be resolved\n",
                      public_root.c_str());
        return 1;
    }

    util::Logger logger;
    http::ThreadPool pool(worker_count);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    // Without this, restarting the server right after a previous run can fail
    // to bind because the port is stuck in TIME_WAIT from the old connection.
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, kBacklog) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    std::printf("Listening on port %d (workers: %zu), serving \"%s\"...\n",
                port, worker_count, public_root.c_str());

    // ── Accept loop ───────────────────────────────────────────────────────────
    for (;;) {
        if (g_shutdown.load()) {
            break;  // SIGINT/SIGTERM received — stop accepting new connections
        }

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // Interrupted by signal — loop back to check g_shutdown.
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                // File descriptor exhaustion: back off briefly so we don't
                // spin-burn CPU while waiting for fds to be released.
                perror("accept (fd exhaustion, backing off)");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // Other errors (e.g. ECONNABORTED) are transient — log and retry.
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::string client_ip_str(client_ip);

        // Enqueue the connection for a worker thread.  If the queue is full,
        // respond with 503 immediately and close — this provides backpressure
        // instead of growing memory without bound during traffic spikes.
        bool enqueued = pool.Enqueue([client_fd, client_ip_str, &router, &logger]() mutable {
            HandleConnection(client_fd, std::move(client_ip_str), *router, logger);
        });

        if (!enqueued) {
            SendServiceUnavailable(client_fd);
            close(client_fd);
        }
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    std::printf("\nShutting down — waiting for in-flight requests to finish...\n");
    pool.Shutdown();   // signals workers to stop, then joins all threads
    close(listen_fd);
    std::printf("Done.\n");
    return 0;
}
