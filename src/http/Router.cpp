#include "http/Router.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <fstream>
#include <sstream>
#include <utility>

namespace http {
namespace {

Response MakeError(int status_code, std::string reason_phrase) {
    Response r;
    r.status_code = status_code;
    r.body = reason_phrase + "\n";
    r.content_length = r.body.size();
    r.reason_phrase = std::move(reason_phrase);
    r.SetHeader("Content-Type", "text/plain");
    return r;
}

}  // namespace

StaticFileRouter::StaticFileRouter(std::string root_directory)
    : root_directory_(std::move(root_directory)) {}

Response StaticFileRouter::Handle(const Request& request) const {
    if (request.method != Method::kGet && request.method != Method::kHead) {
        Response r = MakeError(405, "Method Not Allowed");
        r.SetHeader("Allow", "GET, HEAD");
        return r;
    }

    // NOTE: naive concatenation — "../" in request.path escapes root_directory_
    // right now. Fixed in Milestone 6, not here.
    std::string path = request.path == "/" ? "/index.html" : request.path;
    std::string full_path = root_directory_ + path;

    struct stat st{};
    if (stat(full_path.c_str(), &st) != 0) {
        if (errno == EACCES) {
            return MakeError(403, "Forbidden");
        }
        return MakeError(404, "Not Found");
    }

    // Directories, sockets, device files, etc. are not servable as static
    // content. stat() follows symlinks, so a symlink to a regular file is
    // accepted here — including one that points outside root_directory_,
    // which is another traversal vector closed in Milestone 6.
    if (!S_ISREG(st.st_mode)) {
        return MakeError(403, "Forbidden");
    }

    Response r;
    r.status_code = 200;
    r.reason_phrase = "OK";
    r.SetHeader("Content-Type", "text/plain");  // real MIME detection is Milestone 5
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
