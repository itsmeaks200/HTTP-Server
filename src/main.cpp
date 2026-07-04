#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "http/RequestParser.hpp"
#include "http/Router.hpp"

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

void SendBadRequest(int client_fd) {
    const char* bad_request =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    SendAll(client_fd, bad_request, std::strlen(bad_request));
}

// Real-world servers cap this (nginx defaults to 8K) so a client that never
// sends a terminating blank line can't grow this buffer without bound.
constexpr size_t kMaxRequestHeaderBytes = 8192;

// Loops recv() until the buffer contains the "\r\n\r\n" that marks end of
// headers, since a single recv() call is not guaranteed to return a full
// HTTP request (TCP is a byte stream, not a message stream). Returns
// std::nullopt if the peer disconnects early, an unrecoverable recv error
// occurs, or the header block exceeds kMaxRequestHeaderBytes.
std::optional<std::string> ReadRequestHeaders(int client_fd) {
    std::string data;
    char chunk[4096];

    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return std::nullopt;
        }
        if (n == 0) {
            // Peer closed the connection before sending a complete request.
            return std::nullopt;
        }

        data.append(chunk, static_cast<size_t>(n));
        if (data.size() > kMaxRequestHeaderBytes) {
            return std::nullopt;
        }
    }
    return data;
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

        auto raw_request = ReadRequestHeaders(client_fd);
        if (!raw_request) {
            SendBadRequest(client_fd);
            close(client_fd);
            continue;
        }

        auto request = http::ParseRequest(*raw_request);
        if (!request) {
            SendBadRequest(client_fd);
            close(client_fd);
            continue;
        }

        http::Response response = router->Handle(*request);

        std::printf("%s %s%s%s HTTP/%d.%d -> %d %s\n", request->method_raw.c_str(),
                     request->path.c_str(), request->query.empty() ? "" : "?",
                     request->query.c_str(), request->version_major, request->version_minor,
                     response.status_code, response.reason_phrase.c_str());

        std::string header_bytes = response.SerializeHeader();
        if (SendAll(client_fd, header_bytes.data(), header_bytes.size()) && !response.body.empty()) {
            SendAll(client_fd, response.body.data(), response.body.size());
        }

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
