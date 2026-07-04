#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace http {

struct Response {
    int status_code = 200;
    std::string reason_phrase = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;             // actual bytes to send (empty for HEAD)
    std::size_t content_length = 0;  // written into the Content-Length header regardless
    // Set by the connection-handling loop (not the router) once it has
    // negotiated persistence for this request — see main.cpp's WantsKeepAlive.
    bool keep_alive = false;

    void SetHeader(std::string name, std::string value) {
        headers.emplace_back(std::move(name), std::move(value));
    }

    // Status line + all headers + terminating blank line. Does not include
    // the body — callers send that separately to avoid copying a
    // potentially large file body into this string just to concatenate it.
    std::string SerializeHeader() const;
};

}  // namespace http
