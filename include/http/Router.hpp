#pragma once

#include <optional>
#include <string>

#include "http/Request.hpp"
#include "http/Response.hpp"

namespace http {

// Serves static files out of a root directory. GET/HEAD only; anything else
// gets 405. Every resolved candidate path is canonicalized and checked for
// containment within the root before being touched, which closes both
// literal "../" traversal and symlink-escape vectors in one check.
class StaticFileRouter {
public:
    // Returns std::nullopt if root_directory doesn't exist or can't be
    // resolved to a canonical absolute path — a misconfigured server should
    // fail to start rather than run with a broken or unsafe root.
    static std::optional<StaticFileRouter> Create(const std::string& root_directory);

    Response Handle(const Request& request) const;

private:
    explicit StaticFileRouter(std::string canonical_root);

    std::string canonical_root_;
};

}  // namespace http
