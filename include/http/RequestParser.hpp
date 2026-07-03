#pragma once

#include <optional>
#include <string>

#include "http/Request.hpp"

namespace http {

// Parses a raw HTTP request (request line + headers, terminated by a blank
// line — the format produced by ReadRequestHeaders) into a structured
// Request. Returns std::nullopt if the request is not well-formed HTTP —
// callers should respond 400 Bad Request in that case.
//
// This function validates syntax only. It does not judge whether the method
// is supported or the path exists — those are routing decisions, made by a
// later layer that can distinguish "not valid HTTP" (400) from "valid HTTP
// I don't support" (404/405).
std::optional<Request> ParseRequest(const std::string& raw);

}  // namespace http
