// Payload codec: SHA-256, base64 (RFC 4648 standard alphabet) and raw DEFLATE (RFC 1951).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ncx {

// Lower-case hex SHA-256 of the bytes.
std::string sha256_hex(std::string_view data);

std::string base64_encode(std::string_view data);
// Returns nullopt on any character outside the alphabet or bad padding.
std::optional<std::string> base64_decode(std::string_view text);

// Raw DEFLATE, no zlib/gzip wrapper (so .NET's DeflateStream can also read it).
std::string deflate_raw(std::string_view data);
std::optional<std::string> inflate_raw(std::string_view data);

}  // namespace ncx
