#pragma once

#include <string>
#include <unordered_map>

#include "util/StringUtils.hpp"

namespace http {

enum class Method {
    kGet,
    kHead,
    kOther,
};

struct Request {
    Method method = Method::kOther;
    std::string method_raw;
    std::string path;
    std::string query;
    int version_major = 1;
    int version_minor = 1;
    std::unordered_map<std::string, std::string> headers;  // keys stored lowercase

    const std::string* Header(const std::string& name) const {
        auto it = headers.find(util::ToLower(name));
        return it != headers.end() ? &it->second : nullptr;
    }
};

}  // namespace http
