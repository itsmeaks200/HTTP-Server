#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace util {

// One log record per request.  All fields must be filled in before calling
// Logger::Log().
struct LogEntry {
    std::string client_ip;   // dotted-decimal, from inet_ntop at accept time
    std::string method;      // e.g. "GET"
    std::string path;        // URL path (does not include query string)
    std::string query;       // URL query string, empty if none
    int         status = 0;  // HTTP status code
    long        latency_ms;  // end-of-headers-read → end-of-response-sent
};

// Thread-safe request logger.  All public methods are safe to call from
// multiple threads concurrently — a mutex serialises output so log lines
// from concurrent connections don't interleave.
class Logger {
public:
    // Writes one structured log line to stdout in the format:
    //   [ISO-8601 timestamp] client_ip METHOD /path?query status latency_ms ms
    // Example:
    //   [2024-01-15T12:34:56Z] 127.0.0.1 GET /index.html 200 3ms
    void Log(const LogEntry& e) {
        // ISO-8601 UTC timestamp, formatted once per call.
        char ts[32];
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm utc{};
        gmtime_r(&t, &utc);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

        // Build the optional "?query" suffix only when present.
        std::string query_part;
        if (!e.query.empty()) {
            query_part = "?" + e.query;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::printf("[%s] %s %s %s%s %d %ldms\n",
                    ts,
                    e.client_ip.c_str(),
                    e.method.c_str(),
                    e.path.c_str(),
                    query_part.c_str(),
                    e.status,
                    e.latency_ms);
    }

private:
    std::mutex mutex_;
};

}  // namespace util
