#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

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

        char buffer[4096];
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (n < 0) {
            perror("recv");
            close(client_fd);
            continue;
        }
        buffer[n] = '\0';
        std::printf("Received %zd bytes:\n%s\n", n, buffer);

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
