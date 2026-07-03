#include "http/RequestParser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "util/StringUtils.hpp"

namespace http {
namespace {

bool IsValidToken(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (unsigned char c : s) {
        if (std::isspace(c) || std::iscntrl(c)) {
            return false;
        }
    }
    return true;
}

bool HasRawControlChars(const std::string& s) {
    return std::any_of(s.begin(), s.end(),
                        [](unsigned char c) { return std::iscntrl(c) != 0; });
}

// Decodes %XX escapes in a URI component. Returns std::nullopt if a '%' is
// not followed by exactly two hex digits (a malformed escape).
std::optional<std::string> PercentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size() || !std::isxdigit(static_cast<unsigned char>(in[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16)));
        i += 2;
    }
    return out;
}

Method ClassifyMethod(const std::string& raw) {
    if (raw == "GET") return Method::kGet;
    if (raw == "HEAD") return Method::kHead;
    return Method::kOther;
}

// Splits a request's header block on "\r\n", stopping at (and discarding)
// the blank line that terminates the headers.
std::vector<std::string> SplitLines(const std::string& raw) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (true) {
        size_t next = raw.find("\r\n", pos);
        if (next == std::string::npos) {
            break;
        }
        lines.push_back(raw.substr(pos, next - pos));
        pos = next + 2;
        if (lines.back().empty()) {
            lines.pop_back();  // the terminating blank line itself
            break;
        }
    }
    return lines;
}

}  // namespace

std::optional<Request> ParseRequest(const std::string& raw) {
    std::vector<std::string> lines = SplitLines(raw);
    if (lines.empty()) {
        return std::nullopt;
    }

    // --- Request line: "METHOD SP request-target SP HTTP-version" ---
    std::istringstream request_line(lines[0]);
    std::string method_raw, target, version_str, leftover;
    if (!(request_line >> method_raw >> target >> version_str)) {
        return std::nullopt;
    }
    if (request_line >> leftover) {
        return std::nullopt;  // more than three tokens on the request line
    }
    if (!IsValidToken(method_raw)) {
        return std::nullopt;
    }

    // HTTP-version must be literally "HTTP/" (case-sensitive) + digit "." digit.
    if (version_str.rfind("HTTP/", 0) != 0) {
        return std::nullopt;
    }
    std::string version_num = version_str.substr(5);
    size_t dot = version_num.find('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    std::string major_str = version_num.substr(0, dot);
    std::string minor_str = version_num.substr(dot + 1);
    auto is_digits = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(),
                                          [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    if (!is_digits(major_str) || !is_digits(minor_str)) {
        return std::nullopt;
    }

    // We only support origin-form targets ("/path?query"), which covers every
    // real browser/curl GET or HEAD request. Absolute-form, authority-form
    // (CONNECT), and asterisk-form (OPTIONS *) are out of scope for this
    // project and rejected here.
    if (target.empty() || target[0] != '/' || HasRawControlChars(target)) {
        return std::nullopt;
    }

    std::string path_part = target;
    std::string query_part;
    size_t query_pos = target.find('?');
    if (query_pos != std::string::npos) {
        path_part = target.substr(0, query_pos);
        query_part = target.substr(query_pos + 1);
    }

    auto decoded_path = PercentDecode(path_part);
    if (!decoded_path) {
        return std::nullopt;
    }

    Request req;
    req.method_raw = method_raw;
    req.method = ClassifyMethod(method_raw);
    req.path = *decoded_path;
    req.query = query_part;
    req.version_major = std::stoi(major_str);
    req.version_minor = std::stoi(minor_str);

    // --- Headers ---
    for (size_t i = 1; i < lines.size(); ++i) {
        size_t colon = lines[i].find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        std::string name = util::ToLower(util::Trim(lines[i].substr(0, colon)));
        std::string value = util::Trim(lines[i].substr(colon + 1));
        if (name.empty()) {
            return std::nullopt;
        }

        auto it = req.headers.find(name);
        if (it != req.headers.end()) {
            it->second += ", " + value;  // RFC 7230 4.2: combine into a comma-separated list
        } else {
            req.headers.emplace(name, value);
        }
    }

    return req;
}

}  // namespace http
