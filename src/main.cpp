#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "http/RequestParser.hpp"
#include "http/Router.hpp"
#include "util/StringUtils.hpp"

namespace {

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

bool SendResponse(int client_fd, const http::Response& response) {
    std::string header_bytes = response.SerializeHeader();
    if (!SendAll(client_fd, header_bytes.data(), header_bytes.size())) {
        return false;
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

// Real-world servers cap this (nginx defaults to 8K) so a client that never
// sends a terminating blank line can't grow this buffer without bound.
constexpr size_t kMaxRequestHeaderBytes = 8192;

// A request body beyond this is rejected with 413 rather than read into
// memory in full — this server has no use for large request bodies since
// the router only ever serves GET/HEAD.
constexpr size_t kMaxRequestBodyBytes = 1 << 20;  // 1 MiB

// How long a connection may sit idle — whether waiting on the first byte of
// a request or, once kept alive, waiting on the next one — before the
// server gives up on it. Without this, a client that opens a connection and
// never sends anything (or never sends another request) would tie up this
// thread forever.
constexpr int kIdleTimeoutSeconds = 10;

enum class HeaderReadStatus {
    kOk,       // a full header block (through the blank line) is in out_headers
    kClosed,   // peer closed or the idle timeout fired before any new request began
    kMalformed,  // a request started but never completed (truncated, too large, or a real I/O error)
};

// Reads from `buf` (bytes already buffered from a previous call) and then
// the socket, until `buf` contains a full "\r\n\r\n"-terminated header
// block. Whatever comes after that terminator (a pipelined request, or body
// bytes belonging to this request) is left in `buf` for the next call,
// since TCP gives no guarantee that a single recv() lines up with HTTP
// message boundaries.
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
// substring. Real Connection/Transfer-Encoding values are short,
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

// Serves requests off one accepted connection until the peer disconnects,
// the negotiated Connection semantics call for closing, or the connection
// sits idle past kIdleTimeoutSeconds.
void HandleConnection(int client_fd, const http::StaticFileRouter& router) {
    timeval timeout{};
    timeout.tv_sec = kIdleTimeoutSeconds;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt");
    }

    std::string buf;  // bytes read from the socket but not yet consumed

    for (;;) {
        std::string header_block;
        HeaderReadStatus read_status = ReadHeaderBlock(client_fd, buf, header_block);
        if (read_status == HeaderReadStatus::kClosed) {
            break;  // nothing pending, nothing to respond to
        }
        if (read_status == HeaderReadStatus::kMalformed) {
            SendBadRequest(client_fd);
            break;
        }

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

        std::printf("%s %s%s%s HTTP/%d.%d -> %d %s%s\n", request->method_raw.c_str(),
                     request->path.c_str(), request->query.empty() ? "" : "?",
                     request->query.c_str(), request->version_major, request->version_minor,
                     response.status_code, response.reason_phrase.c_str(),
                     keep_alive ? "" : " (closing)");

        if (!SendResponse(client_fd, response)) {
            break;
        }
        if (!keep_alive) {
            break;
        }
    }

    close(client_fd);
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const std::string public_root = argc > 2 ? argv[2] : "public";
    constexpr int kBacklog = 16;

    auto router = http::StaticFileRouter::Create(public_root);
    if (!router) {
        std::fprintf(stderr,
                      "Fatal: public root \"%s\" does not exist or cannot be resolved\n",
                      public_root.c_str());
        return 1;
    }

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

    std::printf("Listening on port %d, serving files from \"%s\"...\n", port, public_root.c_str());

    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        HandleConnection(client_fd, *router);
    }

    close(listen_fd);
    return 0;
}
