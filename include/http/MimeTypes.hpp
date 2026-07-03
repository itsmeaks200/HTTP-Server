#pragma once

#include <string>

namespace http {

// Determines the Content-Type for a file purely from its extension (no file
// content is read) — the same approach nginx/Apache use. Returns
// "application/octet-stream" for unknown or missing extensions, which tells
// browsers to treat the content as opaque binary rather than guessing.
std::string LookupMimeType(const std::string& path);

}  // namespace http
