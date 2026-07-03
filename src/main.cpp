#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

int main() {
    constexpr int kPort = 8080;
    constexpr int kBacklog = 16;

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
    addr.sin_port = htons(kPort);

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

    std::printf("Listening on port %d...\n", kPort);

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

        auto request = ReadRequestHeaders(client_fd);
        if (!request) {
            const char* bad_request =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            send(client_fd, bad_request, std::strlen(bad_request), 0);
            close(client_fd);
            continue;
        }
        std::printf("Received request (%zu bytes):\n%s\n", request->size(), request->c_str());

        const char* response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 13\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello, world!";
        send(client_fd, response, std::strlen(response), 0);

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
