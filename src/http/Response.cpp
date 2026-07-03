#include "http/Response.hpp"

#include <sstream>

namespace http {

std::string Response::SerializeHeader() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << ' ' << reason_phrase << "\r\n";
    out << "Content-Length: " << content_length << "\r\n";
    out << "Connection: close\r\n";  // real Keep-Alive handling is Milestone 7
    for (const auto& [name, value] : headers) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    return out.str();
}

}  // namespace http
