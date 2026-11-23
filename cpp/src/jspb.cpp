#include "gemini_web/jspb.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gemini_web/util.h"

namespace gwa {
namespace {

constexpr char kHexLower[] = "0123456789abcdef";

constexpr size_t kMaxRpcHits = 256;
constexpr size_t kMaxModels = 512;
constexpr size_t kMaxInnerBytes = 8u << 20;
constexpr size_t kMaxModelIdLength = 120;

uint64_t& RngState() {
    static thread_local uint64_t state = 0;
    if (state == 0) {
        const uint64_t ticks = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const uint64_t tid = static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
#ifdef _WIN32
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        state = static_cast<uint64_t>(counter.QuadPart) ^ ticks ^
                (static_cast<uint64_t>(::GetCurrentProcessId()) << 17) ^
                (static_cast<uint64_t>(::GetCurrentThreadId()) << 41) ^ tid;
#else
        state = ticks ^ (static_cast<uint64_t>(::getpid()) << 17) ^ (tid << 7);
#endif
        if (state == 0) {
            state = 0x9E3779B97F4A7C15ull;
        }
    }
    return state;
}

uint64_t NextRandom() {
    uint64_t x = RngState();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    RngState() = x;
    return x * 0x2545F4914F6CDD1Dull;
}

size_t SkipQuoted(const std::string& text, size_t quotePos, std::string* rawOut) {
    size_t i = quotePos + 1;
    const size_t start = i;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '"') {
            if (rawOut != nullptr) *rawOut = text.substr(start, i - start);
            return i + 1;
        }
        ++i;
    }
    if (rawOut != nullptr) *rawOut = text.substr(start);
    return text.size();
}

size_t SkipNested(const std::string& text, size_t pos) {
    const char open = text[pos];
    const char close = (open == '[') ? ']' : '}';
    int depth = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') {
            pos = SkipQuoted(text, pos, nullptr);
            continue;
        }
        if (c == open) {
            ++depth;
            ++pos;
            continue;
        }
        if (c == close) {
            --depth;
            if (depth <= 0) return pos + 1;
            ++pos;
            continue;
        }
        ++pos;
    }
    return text.size();
}

bool ElementStringAt(const std::string& text, size_t pos, size_t target, std::string& raw) {
    size_t index = 0;
    bool captured = false;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == ',') {
            ++index;
            ++pos;
            continue;
        }
        if (c == ']') break;
        if (c == '"') {
            std::string content;
            pos = SkipQuoted(text, pos, &content);
            if (index == target && !captured) {
                raw = content;
                captured = true;
            }
            continue;
        }
        if (c == '[' || c == '{') {
            pos = SkipNested(text, pos);
            continue;
        }
        ++pos;
    }
    return captured;
}

size_t AfterToken(const std::string& text, size_t hit, size_t tokenLength) {
    const size_t end = hit + tokenLength;
    if (end < text.size() && text[end] == '"') return end + 1;
    return end;
}

std::string StripAntiXssi(const std::string& body) {
    std::string text;
    size_t start = 0;
    if (body.compare(0, 4, ")]}'") == 0) start = 4;
    text.reserve(body.size() - start);
    size_t lineStart = start;
    while (lineStart <= body.size()) {
        size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = body.size();
        size_t begin = lineStart;
        size_t end = lineEnd;
        while (begin < end &&
               (body[begin] == ' ' || body[begin] == '\t' || body[begin] == '\r')) {
            ++begin;
        }
        while (end > begin &&
               (body[end - 1] == ' ' || body[end - 1] == '\t' || body[end - 1] == '\r')) {
            --end;
        }
        bool digitOnly = (end > begin);
        for (size_t i = begin; i < end && digitOnly; ++i) {
            if (body[i] < '0' || body[i] > '9') digitOnly = false;
        }
        if (!digitOnly && end > begin) {
            if (!text.empty()) text.push_back('\n');
            text.append(body, begin, end - begin);
        }
        if (lineEnd == body.size()) break;
        lineStart = lineEnd + 1;
    }
    return text;
}

bool IsTokenChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '/' || c == ':';
}

std::string TrimTrailingDots(std::string token) {
    while (token.size() > 1 && token.back() == '.') token.pop_back();
    return token;
}

bool LooksLikeModelId(const std::string& token) {
    if (token.size() < 3 || token.size() > kMaxModelIdLength) return false;
    const std::string lowered = util::ToLower(token);
    if (lowered.compare(0, 7, "models/") == 0) return true;
    return lowered.find("gemini") != std::string::npos;
}

void ScanModelTokens(const std::string& text, std::vector<WebModel>& models,
                     std::set<std::string>& seen) {
    if (text.empty()) return;
    const std::string lowered = util::ToLower(text);
    for (size_t i = 0; i < lowered.size() && models.size() < kMaxModels; ++i) {
        const bool geminiHit = lowered.compare(i, 6, "gemini") == 0;
        const bool modelsHit = lowered.compare(i, 7, "models/") == 0;
        if (!geminiHit && !modelsHit) continue;

        size_t begin = i;
        while (begin > 0 && IsTokenChar(static_cast<unsigned char>(text[begin - 1]))) --begin;
        size_t end = i + (geminiHit ? 6 : 7);
        while (end < text.size() && IsTokenChar(static_cast<unsigned char>(text[end]))) ++end;

        const std::string token = TrimTrailingDots(text.substr(begin, end - begin));
        i = (end > i) ? end - 1 : i;
        if (!LooksLikeModelId(token)) continue;
        if (!seen.insert(token).second) continue;

        WebModel model;
        model.id = token;
        model.label = token;
        model.name = CanonicalModelName(token, 0);
        models.push_back(std::move(model));
    }
}

template <typename Consumer>
void ForEachRpcPayload(const std::string& text, const char* rpcName, Consumer consume) {
    const size_t nameLength = std::char_traits<char>::length(rpcName);
    size_t from = 0;
    size_t hits = 0;
    while (hits < kMaxRpcHits) {
        const size_t hit = text.find(rpcName, from);
        if (hit == std::string::npos) break;
        from = hit + nameLength;
        ++hits;
        const size_t pos = AfterToken(text, hit, nameLength);
        std::string raw;
        if (!ElementStringAt(text, pos, 2, raw) || raw.empty()) continue;
        if (raw.size() > kMaxInnerBytes * 2) continue;
        consume(util::JsonUnescape(raw));
    }
}

}  // namespace

std::string NewUuid() {
    uint64_t high = NextRandom();
    uint64_t low = NextRandom();
    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>((high >> (56 - 8 * i)) & 0xFF);
        bytes[i + 8] = static_cast<unsigned char>((low >> (56 - 8 * i)) & 0xFF);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHexLower[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHexLower[bytes[i] & 0x0F]);
    }
    return out;
}

std::string JsonQuote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    out += util::JsonEscape(value);
    out.push_back('"');
    return out;
}

std::string JsonNumber(long long value) { return std::to_string(value); }

std::string JsonNull() { return "null"; }

std::string BuildStreamGenerateForm(const std::string& innerPayload) {
    std::string wrapper = "[null,";
    wrapper += JsonQuote(innerPayload);
    wrapper += ']';
    return "f.req=" + util::UrlEncode(wrapper);
}

bool ParseModelList(const std::string& body, std::vector<WebModel>& models) {
    models.clear();
    const std::string text = StripAntiXssi(body);
    if (text.empty()) return false;

    std::set<std::string> seen;
    ForEachRpcPayload(text, "otAQ7b", [&](const std::string& inner) {
        if (models.size() >= kMaxModels) return;
        ScanModelTokens(inner, models, seen);
    });

    if (models.empty()) {
        ScanModelTokens(text, models, seen);
    }
    return !models.empty();
}

}  // namespace gwa
