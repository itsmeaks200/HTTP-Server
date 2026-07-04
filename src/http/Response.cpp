#include "http/Response.hpp"

#include <sstream>

namespace http {

std::string Response::SerializeHeader() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << ' ' << reason_phrase << "\r\n";
    out << "Content-Length: " << content_length << "\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    // We assert Content-Type ourselves via extension-based lookup; nosniff
    // tells the browser to trust it instead of re-guessing from file bytes,
    // which is the mechanism behind a whole class of MIME-sniffing XSS bugs.
    out << "X-Content-Type-Options: nosniff\r\n";
    for (const auto& [name, value] : headers) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    return out.str();
}

}  // namespace http
