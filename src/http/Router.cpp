#include "http/Router.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include "http/MimeTypes.hpp"

namespace http {
namespace {

Response MakeError(int status_code, std::string reason_phrase) {
    Response r;
    r.status_code = status_code;
    r.body = reason_phrase + "\n";
    r.content_length = r.body.size();
    r.reason_phrase = std::move(reason_phrase);
    r.SetHeader("Content-Type", "text/plain; charset=utf-8");
    return r;
}

// Resolves `path` to an absolute, symlink-free canonical form. Returns
// std::nullopt if the path doesn't exist or can't be resolved (broken
// symlink, missing intermediate directory, permission denied, etc.). Uses
// realpath()'s malloc-a-buffer-for-me form (path == nullptr) instead of a
// fixed PATH_MAX stack buffer, so there's no arbitrary length cap involved.
std::optional<std::string> CanonicalPath(const std::string& path) {
    std::unique_ptr<char, decltype(&std::free)> resolved(realpath(path.c_str(), nullptr),
                                                           &std::free);
    if (!resolved) {
        return std::nullopt;
    }
    return std::string(resolved.get());
}

// True if `candidate` is exactly `root`, or nested underneath it. Both
// arguments must already be canonical (absolute, symlinks resolved, no ".."
// segments) — this is then a pure string comparison, so it can't itself be
// fooled by the tricks it exists to catch.
bool IsContainedIn(const std::string& candidate, const std::string& root) {
    if (candidate == root) {
        return true;
    }
    return candidate.size() > root.size() && candidate.compare(0, root.size(), root) == 0 &&
           candidate[root.size()] == '/';
}

}  // namespace

std::optional<StaticFileRouter> StaticFileRouter::Create(const std::string& root_directory) {
    auto canonical_root = CanonicalPath(root_directory);
    if (!canonical_root) {
        return std::nullopt;
    }
    return StaticFileRouter(*canonical_root);
}

StaticFileRouter::StaticFileRouter(std::string canonical_root)
    : canonical_root_(std::move(canonical_root)) {}

Response StaticFileRouter::Handle(const Request& request) const {
    if (request.method != Method::kGet && request.method != Method::kHead) {
        Response r = MakeError(405, "Method Not Allowed");
        r.SetHeader("Allow", "GET, HEAD");
        return r;
    }

    // Reject embedded NUL bytes before they reach any C API that treats
    // strings as NUL-terminated — a raw NUL lets an attacker smuggle a
    // shorter path past validation than the one actually opened (the
    // classic "poison null byte" trick).
    if (request.path.find('\0') != std::string::npos) {
        return MakeError(400, "Bad Request");
    }

    std::string path = request.path == "/" ? "/index.html" : request.path;
    std::string candidate = canonical_root_ + path;

    // This single check closes both traversal vectors flagged as open back
    // in Milestone 4: realpath() collapses "../" segments AND resolves
    // symlinks, so a symlink inside the root pointing outside it is caught
    // by the exact same containment check as a literal "../../etc/passwd".
    auto canonical_candidate = CanonicalPath(candidate);
    if (!canonical_candidate || !IsContainedIn(*canonical_candidate, canonical_root_)) {
        // Identical to "file doesn't exist" on purpose — telling an
        // attacker "that path escapes the root" versus "that path is
        // missing" would itself leak information about the filesystem
        // outside the served root.
        return MakeError(404, "Not Found");
    }
    std::string full_path = *canonical_candidate;

    struct stat st{};
    if (stat(full_path.c_str(), &st) != 0) {
        // realpath() just confirmed this path resolves, so reaching here
        // almost always means the file vanished in the tiny window between
        // resolution and this stat() (TOCTOU) — an operational hiccup, not
        // something the client did wrong.
        return MakeError(500, "Internal Server Error");
    }

    if (!S_ISREG(st.st_mode)) {
        return MakeError(403, "Forbidden");
    }

    Response r;
    r.status_code = 200;
    r.reason_phrase = "OK";
    r.SetHeader("Content-Type", LookupMimeType(full_path));
    r.content_length = static_cast<std::size_t>(st.st_size);

    // HEAD must report the same Content-Length as GET would, but must not
    // actually read the body — so the file read only happens for GET.
    if (request.method == Method::kGet) {
        std::ifstream file(full_path, std::ios::binary);
        if (!file) {
            return MakeError(403, "Forbidden");
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        r.body = contents.str();
    }
    return r;
}

}  // namespace http
