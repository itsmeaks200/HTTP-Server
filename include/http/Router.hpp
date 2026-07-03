#pragma once

#include <string>

#include "http/Request.hpp"
#include "http/Response.hpp"

namespace http {

// Serves static files out of a root directory. GET/HEAD only; anything else
// gets 405. Path resolution is intentionally naive right now (no traversal
// protection) — that hardening is a dedicated later step, not an accident.
class StaticFileRouter {
public:
    explicit StaticFileRouter(std::string root_directory);

    Response Handle(const Request& request) const;

private:
    std::string root_directory_;
};

}  // namespace http
