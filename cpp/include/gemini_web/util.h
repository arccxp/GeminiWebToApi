#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gwa::util {

std::string ToLower(std::string s);
std::string Trim(std::string s);
std::string GetEnv(const char* name);

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
#endif

// JSON string literal helpers (content only, without surrounding quotes).
std::string JsonEscape(const std::string& value);
std::string JsonUnescape(const std::string& value);

std::string Base64Encode(const std::vector<uint8_t>& data);
bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);
bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data);

// Monotonic steady clock in milliseconds.
uint64_t MonotonicMs();

// SHA-1 hex digest (40 lowercase hex chars) used for SAPISIDHASH.
std::string Sha1Hex(const std::string& input);

// RFC 3986 percent-encoding for application/x-www-form-urlencoded values.
std::string UrlEncode(const std::string& value);

}  // namespace gwa::util
