#include "gemini_web/util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace gwa::util {
namespace {

constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kHexUpper[] = "0123456789ABCDEF";
constexpr char kHexLower[] = "0123456789abcdef";

void AppendUtf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool HexDigitValue(char c, uint32_t& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<uint32_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<uint32_t>(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<uint32_t>(c - 'A' + 10);
        return true;
    }
    return false;
}

bool ParseHex4(const std::string& text, size_t pos, uint32_t& out) {
    if (pos + 4 > text.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        uint32_t digit = 0;
        if (!HexDigitValue(text[pos + i], digit)) return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

bool IsUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

#ifndef _WIN32
inline uint32_t Rol32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

std::string PortableSha1Hex(const std::string& input) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    std::vector<uint8_t> msg(input.begin(), input.end());
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8ull;
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u) {
        msg.push_back(0x00u);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu));
    }

    uint32_t w[80];
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
        for (int i = 0; i < 16; ++i) {
            const size_t o = chunk + static_cast<size_t>(i) * 4u;
            w[i] = (static_cast<uint32_t>(msg[o]) << 24) |
                   (static_cast<uint32_t>(msg[o + 1]) << 16) |
                   (static_cast<uint32_t>(msg[o + 2]) << 8) |
                   static_cast<uint32_t>(msg[o + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = Rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol32(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    uint32_t digest[5] = {h0, h1, h2, h3, h4};
    std::string hex;
    hex.reserve(40);
    for (uint32_t word : digest) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(kHexLower[(word >> shift) & 0x0Fu]);
        }
    }
    return hex;
}
#endif

}  // namespace

std::string ToLower(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') {
            c = static_cast<char>(u - 'A' + 'a');
        }
    }
    return s;
}

std::string Trim(std::string s) {
    const auto isTrimmed = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    while (begin < s.size() && isTrimmed(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && isTrimmed(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty() || value.size() > kMaxFileBytes) return {};
    const int length = static_cast<int>(value.size());
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                                            out.data(), needed);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    if (out.find(L'\0') != std::wstring::npos) return {};
    return out;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty() || value.size() > kMaxFileBytes) return {};
    const int length = static_cast<int>(value.size());
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, out.data(),
                                            needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    if (out.find('\0') != std::string::npos) return {};
    return out;
}
#endif

std::string GetEnv(const char* name) {
    if (name == nullptr || *name == '\0') return {};
#ifdef _WIN32
    const std::wstring wideName = Utf8ToWide(name);
    if (wideName.empty()) return {};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
        if (needed == 0) return {};
        std::wstring buffer(static_cast<size_t>(needed), L'\0');
        const DWORD written = GetEnvironmentVariableW(wideName.c_str(), buffer.data(), needed);
        if (written == 0) return {};
        if (written < needed) {
            buffer.resize(static_cast<size_t>(written));
            return WideToUtf8(buffer);
        }
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + value.size() / 8 + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHexUpper[(c >> 4) & 0x0F]);
                    out.push_back(kHexUpper[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string JsonUnescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= value.size()) {
            out.push_back('\\');
            break;
        }
        const char esc = value[++i];
        switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(value, i + 1, cp)) {
                    out += "\\u";
                    break;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (i + 7 <= value.size() && value[i + 1] == '\\' &&
                        value[i + 2] == 'u' && ParseHex4(value, i + 3, low) &&
                        low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                AppendUtf8(cp, out);
                break;
            }
            default:
                out.push_back('\\');
                out.push_back(esc);
                break;
        }
    }
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8) |
                               static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[chunk & 0x3F]);
    }
    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
        out += "==";
    } else if (remaining == 2) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
#ifdef _WIN32
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;
    HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > static_cast<uint64_t>(kMaxFileBytes)) {
        CloseHandle(file);
        return false;
    }
    const size_t total = static_cast<size_t>(size.QuadPart);
    out.resize(total);
    size_t offset = 0;
    constexpr size_t kChunk = 1u << 20;
    while (offset < total) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(total - offset, kChunk));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr) || read == 0) {
            out.clear();
            CloseHandle(file);
            return false;
        }
        offset += static_cast<size_t>(read);
    }
    CloseHandle(file);
    return true;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxFileBytes) return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size)) {
        out.clear();
        return false;
    }
    return true;
#endif
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;
    HANDLE file = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t offset = 0;
    constexpr size_t kChunk = 1u << 20;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, kChunk));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && !FlushFileBuffers(file)) ok = false;
    CloseHandle(file);
    if (!ok) DeleteFileW(widePath.c_str());
    return ok;
#else
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    return file.good();
#endif
}

uint64_t MonotonicMs() {
#ifdef _WIN32
    return static_cast<uint64_t>(::GetTickCount64());
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
#endif
}

std::string Sha1Hex(const std::string& input) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
        return {};
    }
    std::string hex;
    BCRYPT_HASH_HANDLE hash = nullptr;
    do {
        ULONG objectLength = 0;
        ULONG produced = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                              reinterpret_cast<PUCHAR>(&objectLength),
                                              sizeof(objectLength), &produced, 0))) {
            break;
        }
        std::vector<uint8_t> object(objectLength);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr,
                                             0, 0))) {
            break;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hash,
                                           reinterpret_cast<PUCHAR>(
                                               const_cast<char*>(input.data())),
                                           static_cast<ULONG>(input.size()), 0))) {
            break;
        }
        uint8_t digest[20] = {};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            break;
        }
        hex.reserve(40);
        for (uint8_t byte : digest) {
            hex.push_back(kHexLower[byte >> 4]);
            hex.push_back(kHexLower[byte & 0x0F]);
        }
    } while (false);
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
#else
    return PortableSha1Hex(input);
#endif
}

std::string UrlEncode(const std::string& value) {
    std::string out;
    out.reserve(value.size() + value.size() / 3 + 16);
    for (unsigned char c : value) {
        if (IsUnreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHexUpper[(c >> 4) & 0x0F]);
            out.push_back(kHexUpper[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace gwa::util
