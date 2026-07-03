#include "util/StringUtils.hpp"

#include <algorithm>
#include <cctype>

namespace util {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    // HTTP's OWS (optional whitespace) is defined as SP / HTAB only —
    // deliberately not using isspace(), which would also strip \r or \n.
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

}  // namespace util
