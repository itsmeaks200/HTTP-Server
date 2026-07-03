# Multi-Threaded HTTP/1.1 Server (C++, Linux, POSIX sockets)

A from-scratch, production-style HTTP/1.1 server built in modern C++ to
demonstrate TCP/IP networking, Linux systems programming, and concurrency.
Built incrementally as a learning project — see the roadmap below.

## Roadmap

- [x] **Milestone 1** — TCP fundamentals: raw `socket`/`bind`/`listen`/`accept` server
- [x] Milestone 2 — Reading raw HTTP requests, understanding the HTTP/1.1 wire format
- [x] Milestone 3 — Robust HTTP request parsing (method, path, version, headers)
- [x] Milestone 4 — Static file serving + response building + status codes
- [ ] Milestone 5 — MIME types, Content-Length, Content-Type
- [ ] Milestone 6 — Security hardening (path traversal, malformed requests, limits)
- [ ] Milestone 7 — Keep-Alive / persistent connections
- [ ] Milestone 8 — Concurrency problem: thread-per-connection and its limits
- [ ] Milestone 9 — Thread pool (task queue, mutex, condition_variable)
- [ ] Milestone 10 — Structured logging (timestamp, client IP, status, latency)
- [ ] Milestone 11 — Error handling, robustness, graceful degradation
- [ ] Milestone 12 — Performance (zero-copy file streaming, benchmarking with ab/wrk)

## Build

Requires Linux (or WSL) with a C++20 compiler and CMake ≥ 3.16.

```bash
sudo apt install -y build-essential cmake gdb valgrind
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/http_server
```

## Test

```bash
curl -v http://localhost:8080/
```
