#include "http/MimeTypes.hpp"

#include <unordered_map>

#include "util/StringUtils.hpp"

namespace http {
namespace {

// Extension of the final path segment, lowercased, without the dot.
// Returns "" if there is no extension (e.g. "/Makefile", "/some.dir/file").
std::string ExtractExtension(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t segment_start = (slash == std::string::npos) ? 0 : slash + 1;

    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < segment_start || dot + 1 == path.size()) {
        return "";
    }
    return util::ToLower(path.substr(dot + 1));
}

const std::unordered_map<std::string, std::string>& MimeTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "application/javascript; charset=utf-8"},
        {"mjs", "application/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"xml", "application/xml; charset=utf-8"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"webp", "image/webp"},
        {"pdf", "application/pdf"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"wasm", "application/wasm"},
    };
    return table;
}

}  // namespace

std::string LookupMimeType(const std::string& path) {
    std::string extension = ExtractExtension(path);
    if (extension.empty()) {
        return "application/octet-stream";
    }
    const auto& table = MimeTable();
    auto it = table.find(extension);
    return it != table.end() ? it->second : "application/octet-stream";
}

}  // namespace http
