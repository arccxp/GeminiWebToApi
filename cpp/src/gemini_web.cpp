#include "gemini_web/client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gemini_web/relay.h"
#include "gemini_web/session.h"
#include "gemini_web/transport.h"
#include "gemini_web/util.h"

namespace gwa {
namespace {

constexpr const char* kOrigin = "https://gemini.google.com";
constexpr const char* kBatchPath = "/_/BardChatUi/data/batchexecute";
constexpr const char* kGeneratePath =
    "/_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate";
constexpr const char* kUploadUrl = "https://content-push.googleapis.com/upload";
constexpr const char* kRpcUserStatus = "otAQ7b";
constexpr const char* kRpcQuota = "qpEbW";
constexpr const char* kRpcUsagePage = "jSf9Qc";
constexpr long long kComputeAction = 2;

// Fixed credential used by Google's content-push attachment host.
constexpr const char* kUploadAuthorization = "Basic c2F2ZXM6cyNMdGhlNmxzd2F2b0RsN3J1d1U=";
constexpr const char* kDefaultPushId = "feeds/mcudyrk2a4khkz";
constexpr const char* kUploadFormType = "application/x-www-form-urlencoded;charset=UTF-8";

constexpr const char* kModelHeader = "x-goog-ext-525001261-jspb";
constexpr const char* kSessionHeader = "x-goog-ext-525005358-jspb";

constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

constexpr const char* kErrAuthRejected = "Gemini Web: auth rejected (401).";
constexpr const char* kErrUploadFailed = "Gemini Web: upload failed.";
constexpr const char* kErrTransient = "Gemini Web: the site failed the request - try again.";
constexpr const char* kErrParse = "Could not parse the Gemini response.";

std::mutex g_hookMutex;
LogCallback g_logCb;
StatusCallback g_statusCb;
CookieUpdateCallback g_cookieUpdateCb;

void Log(const std::string& line) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        cb = g_logCb;
    }
    if (cb) cb(line);
}

void ReportStatus(const std::string& status) {
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        cb = g_statusCb;
    }
    if (cb) cb(status);
}

void NotifyCookieUpdate(int account, const std::string& header) {
    CookieUpdateCallback cb;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        cb = g_cookieUpdateCb;
    }
    if (cb) cb(account, header);
}

struct WebSession {
    std::string at;     // SNlM0e (optional on newer shells)
    std::string email;  // oPEP7c
    std::string bl;     // cfb2h
    std::string sid;    // FdrFJe
    std::string hl;     // TuX5cc
    std::string push;   // qKIAYe
    int tier = -1;      // AccountTier inferred from shell markers
    bool valid = false;
};

std::string AccountPrefix(const CookieJar& jar) {
    if (jar.account <= 0) return {};
    return "/u/" + std::to_string(jar.account);
}

std::string ScopedCookieHeader(const CookieJar& jar, bool forAccounts = false) {
    const std::string live = CookieRequestHeader(jar);
    std::string shared;
    std::string accounts;
    size_t cursor = 0;
    const size_t prefixLen = sizeof(kAccountsCookiePrefix) - 1;
    while (cursor < live.size()) {
        const size_t end = live.find(';', cursor);
        const std::string pair = util::Trim(live.substr(cursor, end - cursor));
        cursor = end == std::string::npos ? live.size() : end + 1;
        if (pair.compare(0, prefixLen, kAccountsCookiePrefix) == 0) {
            if (!forAccounts) continue;
            if (!accounts.empty()) accounts += "; ";
            accounts += pair.substr(prefixLen);
        } else {
            if (!shared.empty()) shared += "; ";
            shared += pair;
        }
    }
    return accounts.empty() ? shared : MergeCookieHeaders(shared, accounts);
}

std::string PreserveAccountsCookies(const std::string& base, const std::string& captured) {
    std::string accounts;
    size_t cursor = 0;
    const size_t prefixLen = sizeof(kAccountsCookiePrefix) - 1;
    while (cursor < base.size()) {
        const size_t end = base.find(';', cursor);
        const std::string pair = util::Trim(base.substr(cursor, end - cursor));
        cursor = end == std::string::npos ? base.size() : end + 1;
        if (pair.compare(0, prefixLen, kAccountsCookiePrefix) != 0) continue;
        if (!accounts.empty()) accounts += "; ";
        accounts += pair;
    }
    return accounts.empty() ? captured : MergeCookieHeaders(accounts, captured);
}

CookieJar BrowserRelayJar(const CookieJar& jar) {
    CookieJar scoped = jar;
    scoped.header = ScopedCookieHeader(jar);
    return scoped;
}

std::string AuthHeaderBlock(const CookieJar& jar) {
    const long long stamp = static_cast<long long>(std::time(nullptr));
    const int slot = jar.account < 0 ? 0 : jar.account;
    const std::string sapisid = CookieHashKey(jar, slot);
    const std::string material =
        std::to_string(stamp) + " " + sapisid + " " + std::string(kOrigin);

    std::string headers;
    headers += "Authorization: SAPISIDHASH " + std::to_string(stamp) + "_" +
               util::Sha1Hex(material) + "\r\n";
    headers += "X-Goog-AuthUser: " + std::to_string(slot) + "\r\n";
    headers += "Origin: ";
    headers += kOrigin;
    headers += "\r\nReferer: ";
    headers += kOrigin;
    headers += AccountPrefix(jar) + "/app\r\n";
    headers += std::string("User-Agent: ") + kUserAgent + "\r\n";
    headers += "Accept-Language: en-US,en;q=0.9\r\n";
    headers += "Cookie: " + ScopedCookieHeader(jar) + "\r\n";
    return headers;
}

void AbsorbCookies(const CookieJar& jar, const std::string& responseHeaders) {
    if (responseHeaders.empty()) return;
    const std::string refreshed = AbsorbSetCookieHeaders(jar, responseHeaders);
    if (!refreshed.empty()) {
        NotifyCookieUpdate(jar.account, refreshed);
    }
}

std::string HeaderValue(const std::string& block, const char* name) {
    const size_t nameLength = std::char_traits<char>::length(name);
    size_t position = 0;
    while (position < block.size()) {
        size_t lineEnd = block.find("\r\n", position);
        size_t advance = 2;
        if (lineEnd == std::string::npos) {
            lineEnd = block.find('\n', position);
            advance = 1;
        }
        if (lineEnd == std::string::npos) {
            lineEnd = block.size();
            advance = 0;
        }
        const std::string line = block.substr(position, lineEnd - position);
        if (line.size() > nameLength && line[nameLength] == ':' &&
            util::ToLower(line.substr(0, nameLength)) == util::ToLower(std::string(name))) {
            return util::Trim(line.substr(nameLength + 1));
        }
        if (advance == 0) break;
        position = lineEnd + advance;
    }
    return {};
}

std::string HeaderCookie(const std::string& header, const char* name) {
    const std::string key = std::string(name) + "=";
    size_t at = header.find(key);
    while (at != std::string::npos) {
        if (at == 0 || header[at - 1] == ';' || header[at - 1] == ' ') {
            const size_t start = at + key.size();
            const size_t end = header.find(';', start);
            return header.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        at = header.find(key, at + 1);
    }
    return {};
}

http::HttpResponse AuthedRequest(const CookieJar& jar, const http::Endpoint& endpoint,
                                 const std::string& method, const std::string& headers,
                                 const std::string& body, int timeoutMs,
                                 const http::ChunkSink* onChunk,
                                 const std::function<void()>& resetStream = {},
                                 WebSession* recoveredShell = nullptr);

http::HttpResponse HttpGet(const CookieJar& jar, const std::string& url,
                           const std::string& headers, int timeoutMs,
                           WebSession& recoveredShell) {
    http::Endpoint endpoint;
    http::HttpResponse response;
    if (!http::ParseEndpoint(url, endpoint)) {
        response.networkError = true;
        response.detail = "Connect failed: " + url;
        return response;
    }
    return AuthedRequest(jar, endpoint, "GET", headers, {}, timeoutMs, nullptr, {},
                         &recoveredShell);
}

bool ScrapeToken(const std::string& html, const char* key, std::string& out) {
    const std::string quotedKey = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = html.find(quotedKey, pos)) != std::string::npos) {
        size_t cursor = pos + quotedKey.size();
        while (cursor < html.size() &&
               (html[cursor] == ':' || html[cursor] == ',' || html[cursor] == ']' ||
                html[cursor] == ' ' || html[cursor] == '\t' || html[cursor] == '\n' ||
                html[cursor] == '\r')) {
            ++cursor;
        }
        if (cursor < html.size() && html[cursor] == '"') {
            const size_t start = cursor + 1;
            size_t end = start;
            while (end < html.size()) {
                if (html[end] == '\\') {
                    end += 2;
                    continue;
                }
                if (html[end] == '"') break;
                ++end;
            }
            if (end < html.size() && end > start) {
                out = html.substr(start, end - start);
                return !out.empty();
            }
        }
        pos += quotedKey.size();
    }
    return false;
}

int TierFromShellHtml(const std::string& html) {
    if (html.empty()) return -1;
    const std::string lower = util::ToLower(html);
    auto has = [&lower](const char* needle) { return lower.find(needle) != std::string::npos; };
    if (has("\"gemini_ultra\"") || has("\"gemini-ultra\"") ||
        has("ultra_tier_enrollment") || has("gemini_ultra_tier")) {
        return static_cast<int>(AccountTier::Ultra);
    }
    if (has("\"gemini_advanced\"") || has("\"gemini-advanced\"") ||
        has("advanced_tier_enrollment") || has("gemini_advanced_tier") ||
        has("advancedsubscriber")) {
        return static_cast<int>(AccountTier::Pro);
    }
    if (has("upgrade_to_advanced") || has("upgrade_to_gemini_advanced") ||
        has("subscribe_to_advanced")) {
        return static_cast<int>(AccountTier::Free);
    }
    return -1;
}

bool ParseSessionFromShell(const std::string& html, WebSession& out) {
    WebSession session;
    ScrapeToken(html, "SNlM0e", session.at);
    ScrapeToken(html, "cfb2h", session.bl);
    ScrapeToken(html, "FdrFJe", session.sid);
    ScrapeToken(html, "TuX5cc", session.hl);
    ScrapeToken(html, "qKIAYe", session.push);
    ScrapeToken(html, "oPEP7c", session.email);
    session.tier = TierFromShellHtml(html);
    if (session.hl.empty()) session.hl = "en";
    if (session.bl.empty() || session.sid.empty()) return false;
    session.valid = true;
    out = std::move(session);
    return true;
}

bool HasAuthenticatedShell(const WebSession& shell) {
    return shell.valid && (!shell.at.empty() || !shell.email.empty());
}

void HandleAuthFailure(const CookieJar& jar);

bool FetchSession(const CookieJar& jar, WebSession& out, std::string& error,
                  int timeoutMs = 30000) {
    std::string headers = AuthHeaderBlock(jar);
    headers += "Accept: text/html,application/xhtml+xml\r\n";
    WebSession session;
    const http::HttpResponse response = HttpGet(
        jar, std::string(kOrigin) + AccountPrefix(jar) + "/app", headers, timeoutMs, session);
    if (HasAuthenticatedShell(session)) {
        out = std::move(session);
        return true;
    }
    if (response.networkError) {
        error = response.detail.empty() ? "Request failed." : response.detail;
        return false;
    }
    if (response.status == 401 || response.status == 403) {
        error = kErrAuthRejected;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "API Error: HTTP " + std::to_string(response.status);
        return false;
    }
    if (!ParseSessionFromShell(response.body, session) || !HasAuthenticatedShell(session)) {
        error = "Gemini Web: signed-in shell unavailable (HTTP " +
                std::to_string(response.status) + ").";
        return false;
    }
    if (session.at.empty()) {
        Log("gemini: shell carries no SNlM0e; sending without at");
    }
    out = std::move(session);
    return true;
}

std::string SessionIdentity(const CookieJar& jar) {
    const std::string key = CookieHashKey(jar, jar.account);
    return std::to_string(jar.account) + "|" + (key.empty() ? CookieRequestHeader(jar) : key);
}

constexpr uint64_t kSessionTtlMs = 45ull * 60 * 1000;
constexpr uint64_t kSessionRefreshMs = 30ull * 60 * 1000;

struct SessionEntry {
    std::string key;
    WebSession session;
    uint64_t at = 0;
};

constexpr size_t kSessionSlots = 4;
std::mutex g_sessionMutex;
SessionEntry g_sessions[kSessionSlots];

struct SessionInFlight {
    std::string key;
    int waiters = 0;
    std::condition_variable cv;
};
std::vector<SessionInFlight*> g_sessionInFlight;

SessionInFlight* FindInFlight(const std::string& key) {
    for (SessionInFlight* entry : g_sessionInFlight) {
        if (entry != nullptr && entry->key == key) return entry;
    }
    return nullptr;
}

bool ReadCachedSession(const std::string& key, WebSession& out, uint64_t newerThan = 0) {
    const uint64_t now = util::MonotonicMs();
    for (SessionEntry& entry : g_sessions) {
        if (entry.session.valid && entry.key == key && entry.at >= newerThan &&
            now - entry.at < kSessionTtlMs) {
            out = entry.session;
            return true;
        }
    }
    return false;
}

void StoreSession(const std::string& key, const WebSession& session) {
    SessionEntry* target = &g_sessions[0];
    for (SessionEntry& entry : g_sessions) {
        if (entry.key == key) {
            target = &entry;
            break;
        }
        if (entry.at < target->at) target = &entry;
    }
    *target = {key, session, util::MonotonicMs()};
}

constexpr int kRegistryTimeoutMs = 8000;
constexpr int kShellTimeoutMs = 12000;

bool GetSession(const CookieJar& jar, WebSession& out, std::string& error,
                int timeoutMs = 30000) {
    const std::string key = SessionIdentity(jar);
    std::unique_lock<std::mutex> lock(g_sessionMutex);
    if (ReadCachedSession(key, out)) {
        error.clear();
        return true;
    }
    if (SessionInFlight* inflight = FindInFlight(key); inflight != nullptr) {
        inflight->waiters++;
        inflight->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return FindInFlight(key) != inflight;
        });
        inflight->waiters--;
        if (inflight->waiters == 0 && FindInFlight(key) != inflight) {
            delete inflight;
        }
        if (ReadCachedSession(SessionIdentity(jar), out)) {
            error.clear();
            return true;
        }
        error = "the session read is still running";
        return false;
    }
    auto* inflight = new SessionInFlight{key, 0, {}};
    g_sessionInFlight.push_back(inflight);
    lock.unlock();

    WebSession fresh;
    std::string reason;
    const uint64_t started = util::MonotonicMs();
    const bool fetched = FetchSession(jar, fresh, reason, timeoutMs);
    const uint64_t elapsed = util::MonotonicMs() - started;

    lock.lock();
    auto it = std::find(g_sessionInFlight.begin(), g_sessionInFlight.end(), inflight);
    if (it != g_sessionInFlight.end()) g_sessionInFlight.erase(it);
    if (fetched) {
        StoreSession(SessionIdentity(jar), fresh);
        out = std::move(fresh);
    }
    inflight->cv.notify_all();
    if (inflight->waiters == 0) delete inflight;
    lock.unlock();

    if (!fetched) {
        error = reason;
        return false;
    }
    Log("gemini: session scraped in " + std::to_string(elapsed) + " ms");
    error.clear();
    return true;
}

void InvalidateSession() {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    for (SessionEntry& entry : g_sessions) {
        entry.session.valid = false;
        entry.key.clear();
        entry.at = 0;
    }
}

void InvalidateSessionFor(const CookieJar& jar) {
    const std::string key = SessionIdentity(jar);
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    for (SessionEntry& entry : g_sessions) {
        if (entry.session.valid && entry.key == key) {
            entry.session.valid = false;
            entry.key.clear();
            entry.at = 0;
            return;
        }
    }
}

bool SessionOlderThan(const CookieJar& jar, uint64_t maxAgeMs) {
    const std::string key = SessionIdentity(jar);
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    for (const SessionEntry& entry : g_sessions) {
        if (entry.session.valid && entry.key == key) {
            return util::MonotonicMs() - entry.at >= maxAgeMs;
        }
    }
    return true;
}

std::string JoinArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ',';
        out += items[i];
    }
    out += ']';
    return out;
}

std::string Num(long long value) { return std::to_string(value); }

std::string UpperUuid() {
    std::string id = NewUuid();
    for (char& c : id) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return id;
}

const std::string& GenerationId() {
    static const std::string id = UpperUuid();
    return id;
}

long long NextReqid() {
    static std::atomic<long long> seq{
        100000ll + static_cast<long long>((util::MonotonicMs() * 2654435761ull) % 800000ull)};
    return seq.fetch_add(100000ll, std::memory_order_acq_rel);
}

long long ThinkingLevel(const std::string& effort) {
    const std::string key = util::ToLower(util::Trim(effort));
    if (key == "high" || key == "extended") return 2;
    if (key == "medium") return 4;
    return 1;
}

std::string ThinkingLabel(const std::string& effort) {
    const std::string key = util::ToLower(util::Trim(effort));
    if (key == "high" || key == "extended") return "high";
    if (key == "medium") return "medium";
    return "low";
}

struct ModelEntry {
    std::string id;
    std::string category;
    std::string label;
    long long number = 1;
    long long capacity = 1;
    int capacityField = 12;
    bool inPicker = false;
    bool available = true;
    std::string note;
};

// Minimal zero-throw slice-based JSON reader for batchexecute/StreamGenerate frames.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;
};

void SkipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

bool ParseValue(const std::string& s, size_t& i, JsonValue& out, int depth);

bool ParseString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string raw;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            raw.push_back(c);
            raw.push_back(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '"') {
            ++i;
            out = util::JsonUnescape(raw);
            return true;
        }
        raw.push_back(c);
        ++i;
    }
    return false;
}

bool ParseValue(const std::string& s, size_t& i, JsonValue& out, int depth) {
    if (depth > 64) return false;
    SkipSpace(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '"') {
        out.kind = JsonValue::Kind::String;
        return ParseString(s, i, out.text);
    }
    if (c == '[') {
        out.kind = JsonValue::Kind::Array;
        ++i;
        SkipSpace(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        for (;;) {
            JsonValue element;
            if (!ParseValue(s, i, element, depth + 1)) return false;
            out.items.push_back(std::move(element));
            SkipSpace(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            return false;
        }
    }
    if (c == '{') {
        out.kind = JsonValue::Kind::Object;
        ++i;
        SkipSpace(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        for (;;) {
            SkipSpace(s, i);
            std::string key;
            if (!ParseString(s, i, key)) return false;
            SkipSpace(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            JsonValue value;
            if (!ParseValue(s, i, value, depth + 1)) return false;
            out.fields.emplace_back(std::move(key), std::move(value));
            SkipSpace(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            return false;
        }
    }
    if (s.compare(i, 4, "null") == 0) {
        out.kind = JsonValue::Kind::Null;
        i += 4;
        return true;
    }
    if (s.compare(i, 4, "true") == 0) {
        out.kind = JsonValue::Kind::Bool;
        out.text = "true";
        i += 4;
        return true;
    }
    if (s.compare(i, 5, "false") == 0) {
        out.kind = JsonValue::Kind::Bool;
        out.text = "false";
        i += 5;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const size_t start = i;
        ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                                s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        out.kind = JsonValue::Kind::Number;
        out.text = s.substr(start, i - start);
        return true;
    }
    return false;
}

const JsonValue* At(const JsonValue& value, size_t index) {
    if (value.kind != JsonValue::Kind::Array || index >= value.items.size()) return nullptr;
    return &value.items[index];
}

long long AsInt(const JsonValue* value, long long fallback) {
    if (value == nullptr || value->kind != JsonValue::Kind::Number) return fallback;
    return std::strtoll(value->text.c_str(), nullptr, 10);
}

const std::string* AsString(const JsonValue* value) {
    if (value == nullptr || value->kind != JsonValue::Kind::String) return nullptr;
    return &value->text;
}

bool FindRecord(const JsonValue& value, const char* rpcId, std::string& payload, int depth) {
    if (depth > 8 || value.kind != JsonValue::Kind::Array) return false;
    const std::string* tag = AsString(At(value, 0));
    if (tag != nullptr && *tag == "wrb.fr") {
        const std::string* id = AsString(At(value, 1));
        if (id != nullptr && *id == rpcId) {
            const JsonValue* inner = At(value, 2);
            if (inner != nullptr && inner->kind == JsonValue::Kind::String &&
                !inner->text.empty()) {
                payload = inner->text;
                return true;
            }
        }
        return false;
    }
    for (const JsonValue& item : value.items) {
        if (FindRecord(item, rpcId, payload, depth + 1)) return true;
    }
    return false;
}

bool FindRpcPayload(const std::string& body, const char* rpcId, std::string& payload) {
    size_t cursor = 0;
    while (cursor < body.size() &&
           (body[cursor] == ')' || body[cursor] == ']' || body[cursor] == '}' ||
            body[cursor] == '\'' || body[cursor] == '\n' || body[cursor] == '\r' ||
            body[cursor] == ' ' || (body[cursor] >= '0' && body[cursor] <= '9'))) {
        if (body.compare(cursor, 4, ")]}'") == 0) {
            cursor += 4;
            continue;
        }
        ++cursor;
    }
    while (cursor < body.size()) {
        const size_t start = body.find('[', cursor);
        if (start == std::string::npos) return false;
        size_t index = start;
        JsonValue frame;
        if (!ParseValue(body, index, frame, 0)) {
            cursor = start + 1;
            continue;
        }
        cursor = index;
        if (FindRecord(frame, rpcId, payload, 0)) return true;
    }
    return false;
}

bool AnswerFromPayload(const JsonValue& payload, std::string& out) {
    out.clear();
    const JsonValue* branch = At(payload, 4);
    if (branch == nullptr) return false;
    const JsonValue* first = At(*branch, 0);
    if (first == nullptr) return false;
    const JsonValue* slot = At(*first, 1);
    if (slot == nullptr) return false;
    if (slot->kind == JsonValue::Kind::String) {
        if (slot->text.empty()) return false;
        out = slot->text;
        return true;
    }
    if (slot->kind == JsonValue::Kind::Array) {
        std::string joined;
        for (const JsonValue& part : slot->items) {
            if (part.kind == JsonValue::Kind::String) joined += part.text;
        }
        if (joined.empty()) return false;
        out = std::move(joined);
        return true;
    }
    return false;
}

void CollectStrings(const JsonValue& value, std::string& out, int depth) {
    if (depth > 4) return;
    if (value.kind == JsonValue::Kind::String) {
        out += value.text;
        return;
    }
    if (value.kind == JsonValue::Kind::Array) {
        for (const JsonValue& item : value.items) {
            CollectStrings(item, out, depth + 1);
        }
    }
}

bool ThinkingFromPayload(const JsonValue& payload, std::string& out) {
    out.clear();
    const JsonValue* branch = At(payload, 4);
    const JsonValue* first = (branch != nullptr) ? At(*branch, 0) : nullptr;
    const JsonValue* thinking = (first != nullptr) ? At(*first, 37) : nullptr;
    if (thinking == nullptr) return false;
    CollectStrings(*thinking, out, 0);
    return !out.empty();
}

struct ReplyTurn {
    std::string answer;
    std::string servedLabel;
    long long servedNumber = 0;
    std::string thinking;
    ChatThread chat;
};

bool ApplyReplyPayload(const JsonValue& payload, ReplyTurn& out, std::string& thinking) {
    const JsonValue* ids = At(payload, 1);
    if (ids != nullptr && ids->kind == JsonValue::Kind::Array) {
        const std::string* conversation = AsString(At(*ids, 0));
        const std::string* response = AsString(At(*ids, 1));
        if (conversation != nullptr && !conversation->empty()) {
            out.chat.conversation = *conversation;
        }
        if (response != nullptr && !response->empty()) {
            out.chat.response = *response;
        }
    }
    const JsonValue* candidates = At(payload, 4);
    if (candidates != nullptr && candidates->kind == JsonValue::Kind::Array) {
        const JsonValue* firstCandidate = At(*candidates, 0);
        if (firstCandidate != nullptr && firstCandidate->kind == JsonValue::Kind::Array) {
            const std::string* candidateId = AsString(At(*firstCandidate, 0));
            if (candidateId != nullptr && !candidateId->empty()) {
                out.chat.candidate = *candidateId;
            }
        }
    }
    std::string frameThinking;
    if (ThinkingFromPayload(payload, frameThinking) && frameThinking.size() > thinking.size()) {
        thinking = std::move(frameThinking);
    }
    const size_t previous = out.answer.size();
    std::string candidate;
    if (AnswerFromPayload(payload, candidate) && candidate.size() >= previous) {
        out.answer = std::move(candidate);
        const std::string* label = AsString(At(payload, 42));
        if (label != nullptr && !label->empty()) out.servedLabel = *label;
        const long long number = AsInt(At(payload, 58), 0);
        if (number > 0) out.servedNumber = number;
    }
    return out.answer.size() > previous;
}

bool ExtractReply(const std::string& body, ReplyTurn& out) {
    out = ReplyTurn{};
    std::string thinking;
    size_t cursor = 0;
    while (cursor < body.size()) {
        const size_t start = body.find('[', cursor);
        if (start == std::string::npos) break;
        size_t index = start;
        JsonValue frame;
        if (!ParseValue(body, index, frame, 0)) {
            cursor = start + 1;
            continue;
        }
        cursor = index;
        if (frame.kind != JsonValue::Kind::Array) continue;
        for (const JsonValue& record : frame.items) {
            if (record.kind != JsonValue::Kind::Array) continue;
            const std::string* tag = AsString(At(record, 0));
            if (tag == nullptr || *tag != "wrb.fr") continue;
            const JsonValue* inner = At(record, 2);
            if (inner == nullptr || inner->kind != JsonValue::Kind::String) continue;
            size_t innerIndex = 0;
            JsonValue payload;
            if (!ParseValue(inner->text, innerIndex, payload, 0)) continue;
            ApplyReplyPayload(payload, out, thinking);
        }
    }
    out.thinking = std::move(thinking);
    return !out.answer.empty();
}

class ReplyStreamScanner {
public:
    bool Feed(const char* data, size_t size) {
        pending_.append(data, size);
        bool grew = false;
        for (;;) {
            if (cursor_ > kCompactThreshold && cursor_ > pending_.size() / 2) {
                pending_.erase(0, cursor_);
                cursor_ = 0;
            }
            const size_t start = pending_.find('[', cursor_);
            if (start == std::string::npos) {
                cursor_ = pending_.size();
                break;
            }
            cursor_ = start;
            size_t index = cursor_;
            JsonValue frame;
            if (!ParseValue(pending_, index, frame, 0)) break;
            cursor_ = index;
            if (frame.kind != JsonValue::Kind::Array) continue;
            for (const JsonValue& record : frame.items) {
                if (record.kind != JsonValue::Kind::Array) continue;
                const std::string* tag = AsString(At(record, 0));
                if (tag == nullptr || *tag != "wrb.fr") continue;
                const JsonValue* inner = At(record, 2);
                if (inner == nullptr || inner->kind != JsonValue::Kind::String) continue;
                size_t innerIndex = 0;
                JsonValue payload;
                if (!ParseValue(inner->text, innerIndex, payload, 0)) continue;
                sawFrame_ = true;
                grew = ApplyReplyPayload(payload, turn_, thinking_) || grew;
            }
        }
        return grew;
    }

    const std::string& Answer() const { return turn_.answer; }
    bool SawFrame() const { return sawFrame_; }

    ReplyTurn TakeTurn() {
        turn_.thinking = std::move(thinking_);
        return std::move(turn_);
    }

private:
    static constexpr size_t kCompactThreshold = 64 * 1024;
    std::string pending_;
    size_t cursor_ = 0;
    ReplyTurn turn_;
    std::string thinking_;
    bool sawFrame_ = false;
};

bool ParseBardError(const std::string& body, long long& code, std::string& message) {
    code = 0;
    message.clear();
    const size_t marker = body.find("BardErrorInfo");
    if (marker == std::string::npos) return false;
    size_t at = marker;
    while (at < body.size() && !(body[at] >= '0' && body[at] <= '9')) ++at;
    if (at >= body.size()) return false;
    code = std::strtoll(body.c_str() + at, nullptr, 10);
    const size_t quote = body.find('"', at);
    if (quote != std::string::npos) {
        const size_t end = body.find('"', quote + 1);
        if (end != std::string::npos && end > quote + 1) {
            message = body.substr(quote + 1, end - quote - 1);
        }
    }
    return true;
}

bool HasFlag(const JsonValue* list, long long flag) {
    if (list == nullptr || list->kind != JsonValue::Kind::Array) return false;
    for (const JsonValue& item : list->items) {
        if (item.kind == JsonValue::Kind::Number &&
            std::strtoll(item.text.c_str(), nullptr, 10) == flag) {
            return true;
        }
    }
    return false;
}

void ComputeCapacity(const JsonValue& root, long long& capacity, int& field) {
    capacity = 1;
    field = 12;
    const JsonValue* tier = At(root, 16);
    const JsonValue* caps = At(root, 17);
    if (HasFlag(tier, 21)) {
        capacity = 1;
        field = 13;
        return;
    }
    if (HasFlag(tier, 22)) {
        capacity = 2;
        field = 13;
        return;
    }
    if (HasFlag(caps, 115)) {
        capacity = 4;
        return;
    }
    if (HasFlag(tier, 16) || HasFlag(caps, 106)) {
        capacity = 3;
        return;
    }
    if (HasFlag(tier, 8) || HasFlag(caps, 19)) {
        capacity = 2;
    }
}

bool IsUltraOnlyRow(const std::string& id, const std::string& category,
                    const std::string& label) {
    const std::string tag = util::ToLower(id + " " + category + " " + label);
    return tag.find("carbon") != std::string::npos;
}

bool IsUltraOnlyEntry(const ModelEntry& entry) {
    return IsUltraOnlyRow(entry.id, entry.category, entry.label);
}

std::string ModelBaseKey(const std::string& label, const std::string& category,
                         long long number) {
    const std::string text = util::ToLower(label.empty() ? category : label);
    std::string key;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i])) == 0) ++i;
        const size_t start = i;
        while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i])) != 0) ++i;
        if (i == start) break;
        const std::string token = text.substr(start, i - start);
        if (token == "gemini") continue;
        bool numeric = true;
        for (const char c : token) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                numeric = false;
                break;
            }
        }
        if (numeric) continue;
        if (!key.empty()) key += '-';
        key += token;
    }
    if (key.empty() && number > 0) key = "n" + std::to_string(number);
    return key;
}

bool PlanIsPaid(int plan) { return plan >= static_cast<int>(AccountTier::Pro); }

bool ParseModelsFromPayload(const std::string& payload, std::vector<ModelEntry>& models,
                            bool* tierKnown = nullptr, int* plan = nullptr) {
    models.clear();
    if (tierKnown != nullptr) *tierKnown = false;
    if (plan != nullptr) *plan = static_cast<int>(AccountTier::Unknown);

    size_t cursor = 0;
    JsonValue root;
    if (!ParseValue(payload, cursor, root, 0) || root.kind != JsonValue::Kind::Array) {
        return false;
    }

    long long capacity = 1;
    int capacityField = 12;
    ComputeCapacity(root, capacity, capacityField);

    {
        const JsonValue* accountList = At(root, 15);
        const JsonValue* tierFlags = At(root, 16);
        const JsonValue* capFlags = At(root, 17);
        const bool ultraFlag = HasFlag(capFlags, 115) || HasFlag(tierFlags, 22);
        const bool proFlag = HasFlag(tierFlags, 16) || HasFlag(tierFlags, 8) ||
                             HasFlag(capFlags, 106) || HasFlag(capFlags, 19);
        const bool freeFlag = HasFlag(tierFlags, 21);
        int tierPlan = static_cast<int>(AccountTier::Unknown);
        if (ultraFlag) {
            tierPlan = static_cast<int>(AccountTier::Ultra);
        } else if (proFlag) {
            tierPlan = static_cast<int>(AccountTier::Pro);
        } else if (freeFlag) {
            tierPlan = static_cast<int>(AccountTier::Free);
        } else if (accountList != nullptr && accountList->kind == JsonValue::Kind::Array &&
                   !accountList->items.empty()) {
            tierPlan = (accountList->items.size() > 1 || capacity > 1)
                           ? static_cast<int>(AccountTier::Pro)
                           : static_cast<int>(AccountTier::Free);
        }
        if (tierPlan >= 0) {
            if (tierKnown != nullptr) *tierKnown = true;
            if (plan != nullptr) *plan = tierPlan;
        }
    }

    const JsonValue* entries = At(root, 15);
    if (entries != nullptr && entries->kind == JsonValue::Kind::Array) {
        for (const JsonValue& entry : entries->items) {
            if (entry.kind != JsonValue::Kind::Array) continue;
            const std::string* id = AsString(At(entry, 0));
            long long number = AsInt(At(entry, 17), AsInt(At(entry, 9), 0));
            if ((id == nullptr || id->empty()) && number <= 0) {
                auto looksLikeLabel = [](const std::string& s) {
                    if (s.size() < 3) return false;
                    bool hasLetter = false;
                    bool hasSpace = false;
                    bool allHex = true;
                    for (char c : s) {
                        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                           (c >= 'A' && c <= 'F');
                        if (!isHex) allHex = false;
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) hasLetter = true;
                        if (c == ' ') hasSpace = true;
                    }
                    if (!hasLetter || s.find("://") != std::string::npos) return false;
                    if (allHex && !hasSpace) return false;
                    return true;
                };
                std::string recovered;
                std::vector<const JsonValue*> stack;
                stack.push_back(&entry);
                while (!stack.empty() && recovered.empty()) {
                    const JsonValue* v = stack.back();
                    stack.pop_back();
                    if (v->kind == JsonValue::Kind::String && looksLikeLabel(v->text)) {
                        recovered = v->text;
                        continue;
                    }
                    if (v->kind == JsonValue::Kind::Array) {
                        for (size_t i = v->items.size(); i-- > 0;) stack.push_back(&v->items[i]);
                    }
                }
                if (!recovered.empty()) {
                    ModelEntry model;
                    model.label = recovered;
                    model.number = 0;
                    model.capacity = capacity;
                    model.capacityField = capacityField;
                    model.inPicker = true;
                    models.push_back(std::move(model));
                }
                continue;
            }
            const std::string* category = AsString(At(entry, 1));
            if (category == nullptr) category = AsString(At(entry, 10));
            const std::string* display = AsString(At(entry, 11));
            if (At(entry, 3) != nullptr && At(entry, 3)->kind == JsonValue::Kind::Array &&
                category != nullptr && !category->empty()) {
                display = category;
                category = AsString(At(entry, 2));
            } else if (display == nullptr) {
                display = AsString(At(entry, 19));
            }
            ModelEntry model;
            model.id = (id != nullptr) ? *id : std::string();
            model.number = number > 0 ? number : 1;
            model.category = (category != nullptr) ? *category : std::string();
            model.label = (display != nullptr && !display->empty()) ? *display : model.category;
            if (model.label.empty()) model.label = model.id;
            model.capacity = capacity;
            model.capacityField = capacityField;
            model.inPicker = true;
            if (!model.label.empty()) models.push_back(std::move(model));
        }
        if (!models.empty()) return true;
    }

    const JsonValue* policy = At(root, 24);
    const JsonValue* catalog = (policy != nullptr) ? At(*policy, 1) : nullptr;
    if (catalog != nullptr && catalog->kind == JsonValue::Kind::Array) {
        for (const JsonValue& group : catalog->items) {
            const JsonValue* row = At(group, 0);
            if (row == nullptr) row = &group;
            if (row->kind != JsonValue::Kind::Array) continue;
            const long long number = AsInt(At(*row, 1), 0);
            if (number <= 0) continue;
            ModelEntry model;
            model.number = number;
            model.capacity = capacity;
            model.capacityField = capacityField;
            const std::string* category = AsString(At(*row, 4));
            const std::string* label = AsString(At(*row, 12));
            model.category = (category != nullptr) ? *category : std::string();
            model.label = (label != nullptr && !label->empty()) ? *label : model.category;
            if (!model.label.empty()) models.push_back(std::move(model));
        }
    }
    return !models.empty();
}

std::vector<ModelEntry> FallbackModelTable() {
    ModelEntry row;
    row.id = "cf41b0e0dd7d53e5";
    row.label = "Auto";
    row.number = 6;
    return {std::move(row)};
}

const ModelEntry* PickModel(const std::vector<ModelEntry>& models, const std::string& requested) {
    if (models.empty()) return nullptr;
    const std::string want = util::ToLower(util::Trim(requested));
    if (want.empty() || want == "gemini-auto" || want == "auto" || want == "default") {
        const ModelEntry* autoRow = nullptr;
        const ModelEntry* fullFlash = nullptr;
        const ModelEntry* nonLite = nullptr;
        for (const ModelEntry& model : models) {
            const std::string tag = util::ToLower(model.category + " " + model.label);
            const bool lite = tag.find("lite") != std::string::npos;
            const bool flash = tag.find("flash") != std::string::npos;
            if (autoRow == nullptr && tag.find("auto") != std::string::npos) autoRow = &model;
            if (flash && !lite && fullFlash == nullptr) fullFlash = &model;
            if (!lite && nonLite == nullptr) nonLite = &model;
        }
        if (autoRow != nullptr) return autoRow;
        if (fullFlash != nullptr) return fullFlash;
        if (nonLite != nullptr) return nonLite;
    } else {
        for (const ModelEntry& model : models) {
            if (NameAnswersTo(requested, model.id, model.label, model.category) ||
                NameAnswersTo(requested, model.id,
                              ProductModelName(model.label, model.category, model.number),
                              model.category)) {
                return &model;
            }
        }
        auto findByNumberText = [&models](const char* text) -> const ModelEntry* {
            const long long number = std::strtoll(text, nullptr, 10);
            for (const ModelEntry& model : models) {
                if (model.number == number) return &model;
            }
            return nullptr;
        };
        if (want.size() <= 2 && want.find_first_not_of("0123456789") == std::string::npos) {
            return findByNumberText(want.c_str());
        }
        if (want.size() > 7 && want.rfind("gemini-", 0) == 0 &&
            want.find_first_not_of("0123456789", 7) == std::string::npos) {
            return findByNumberText(want.c_str() + 7);
        }
        return nullptr;
    }
    return &models.front();
}

std::string BuildModelHeader(const ModelEntry& model, long long level,
                             const std::string& generationId) {
    std::vector<std::string> header(17, "null");
    header[0] = "1";
    header[4] = model.id.empty() ? "null" : JsonQuote(model.id);
    header[7] = "0";
    header[8] = "[4,5,6,8]";
    header[11] = model.capacityField == 13 ? "[null," + Num(model.capacity) + "]"
                                           : Num(model.capacity);
    header[14] = Num(model.number);
    header[15] = level >= 2 ? "2" : "1";
    header[16] = JsonQuote(generationId);
    return JoinArray(header);
}

std::string BuildRequestHeaders(const CookieJar& jar, const std::string& modelHeaderValue) {
    std::string headers = AuthHeaderBlock(jar);
    headers += "Content-Type: application/x-www-form-urlencoded;charset=utf-8\r\n";
    headers += "X-Same-Domain: 1\r\n";
    headers += std::string(kModelHeader) + ": " + modelHeaderValue + "\r\n";
    headers += "X-Goog-Ext-73010989-jspb: [0]\r\n";
    headers += "X-Goog-Ext-73010990-jspb: [0,0,0]\r\n";
    return headers;
}

constexpr int kGenerateTimeoutMs = 300000;

http::HttpResponse PostForm(const CookieJar& jar, const std::string& url,
                            const std::string& headers, const std::string& body,
                            int timeoutMs = 120000) {
    http::Endpoint endpoint;
    if (!http::ParseEndpoint(url, endpoint)) {
        http::HttpResponse failed;
        failed.networkError = true;
        failed.detail = "Connect failed: " + url;
        return failed;
    }
    return AuthedRequest(jar, endpoint, "POST", headers, body, timeoutMs, nullptr);
}

http::HttpResponse RawPost(const std::string& url, const std::string& headers,
                           const std::string& body) {
    http::Endpoint endpoint;
    if (!http::ParseEndpoint(url, endpoint)) {
        http::HttpResponse failed;
        failed.networkError = true;
        failed.detail = "Connect failed: " + url;
        return failed;
    }
    return http::Post(endpoint, headers, body, 120000);
}

http::HttpResponse PostFormStream(const CookieJar& jar, const std::string& url,
                                  const std::string& headers, const std::string& body,
                                  const http::ChunkSink& onChunk, int timeoutMs,
                                  const std::function<void()>& resetStream) {
    http::Endpoint endpoint;
    if (!http::ParseEndpoint(url, endpoint)) {
        http::HttpResponse failed;
        failed.networkError = true;
        failed.detail = "Connect failed: " + url;
        return failed;
    }
    return AuthedRequest(jar, endpoint, "POST", headers, body, timeoutMs, &onChunk, resetStream);
}

void AbsorbRotationCookies(const CookieJar& jar, const http::HttpResponse& response) {
    std::string scoped;
    size_t cursor = 0;
    while (cursor < response.headers.size()) {
        const size_t end = response.headers.find('\n', cursor);
        const std::string line = util::Trim(response.headers.substr(cursor, end - cursor));
        cursor = end == std::string::npos ? response.headers.size() : end + 1;
        if (util::ToLower(line.substr(0, 11)) != "set-cookie:") continue;
        const std::string pair = util::Trim(line.substr(11));
        std::string domain;
        size_t attr = pair.find(';');
        while (attr != std::string::npos) {
            const size_t next = pair.find(';', attr + 1);
            const std::string item = util::Trim(pair.substr(attr + 1, next - attr - 1));
            const size_t equals = item.find('=');
            if (equals != std::string::npos &&
                util::ToLower(util::Trim(item.substr(0, equals))) == "domain") {
                domain = util::ToLower(util::Trim(item.substr(equals + 1)));
                break;
            }
            attr = next;
        }
        scoped += "Set-Cookie: ";
        if (domain != "google.com" && domain != ".google.com") {
            scoped += kAccountsCookiePrefix;
        }
        scoped += pair + "\r\n";
    }
    AbsorbCookies(jar, scoped);
}

bool RotateSessionInternal(const CookieJar& jar, bool force) {
    const int slot = jar.account < 0 ? 0 : jar.account;
    const std::string sessionCookie = "__Secure-" + std::to_string(slot + 1) + "PSID";
    std::string identity = HeaderCookie(CookieRequestHeader(jar), sessionCookie.c_str());
    if (identity.empty() && slot == 0) identity = CookieSapisid(jar);
    if (identity.empty()) return false;

    const std::string throttleKey = std::to_string(slot) + ":" + util::Sha1Hex(identity);
    static std::mutex rotateMutex;
    static std::map<std::string, uint64_t> retryAt;
    const uint64_t now = util::MonotonicMs();
    if (!force) {
        std::lock_guard<std::mutex> lock(rotateMutex);
        const auto existing = retryAt.find(throttleKey);
        if (existing != retryAt.end() && now < existing->second) return false;
        if (existing == retryAt.end() && retryAt.size() >= 32) retryAt.erase(retryAt.begin());
        retryAt[throttleKey] = now + 5ull * 60 * 1000;
    }

    std::string headers;
    headers += "X-Goog-AuthUser: " + std::to_string(slot) + "\r\n";
    headers += std::string("User-Agent: ") + kUserAgent + "\r\n";
    headers += "Accept-Language: en-US,en;q=0.9\r\n";
    headers += "Content-Type: application/json\r\n";
    headers += "Origin: https://accounts.google.com\r\n";
    headers += "Referer: https://accounts.google.com/\r\n";
    headers += "Cookie: " + ScopedCookieHeader(jar, true) + "\r\n";

    const char* kBody = "[000,\"-0000000000000000000\"]";
    http::HttpResponse response =
        RawPost("https://accounts.google.com/RotateCookies", headers, kBody);
    if (!response.networkError && (response.status == 400 || response.status == 415)) {
        const char* kLegacyBody =
            "[\"ChromiumBrowser\",null,null,null,0,\"https://accounts.google.com\",null,null,null,0]";
        response = RawPost("https://accounts.google.com/RotateCookies", headers, kLegacyBody);
    }
    Log("gemini: RotateCookies -> HTTP " + std::to_string(response.status));
    {
        std::lock_guard<std::mutex> lock(rotateMutex);
        retryAt[throttleKey] =
            util::MonotonicMs() + (response.status == 429 ? 15ull * 60 * 1000 : 60000ull);
    }
    if (response.networkError || response.status < 200 || response.status >= 300) return false;
    AbsorbRotationCookies(jar, response);
    return true;
}

void HandleAuthFailure(const CookieJar& jar) { InvalidateSessionFor(jar); }

bool ValidateCapturedHeader(const std::string& header, int account, WebSession& out,
                            std::string* refreshedHeader = nullptr) {
    out = WebSession{};
    CookieJar probe;
    std::string parseError;
    if (!ParseCookieHeader(header, probe, parseError)) return false;
    probe.account = account;
    std::string headers = AuthHeaderBlock(probe);
    headers += "Accept: text/html,application/xhtml+xml\r\n";
    http::Endpoint endpoint;
    if (!http::ParseEndpoint(std::string(kOrigin) + AccountPrefix(probe) + "/app", endpoint)) {
        return false;
    }
    const http::HttpResponse response = http::Request(endpoint, "GET", headers, {}, 20000);
    if (response.networkError || response.status < 200 || response.status >= 300) return false;
    WebSession session;
    if (!ParseSessionFromShell(response.body, session) || !HasAuthenticatedShell(session)) {
        return false;
    }
    out = std::move(session);
    AbsorbCookies(probe, response.headers);
    if (refreshedHeader != nullptr) *refreshedHeader = CookieRequestHeader(probe);
    return true;
}

std::string PatchAuthHeaders(const CookieJar& jar, const std::string& headers) {
    std::string out;
    bool hadSapisidHash = false;
    size_t cursor = 0;
    while (cursor < headers.size()) {
        const size_t end = headers.find('\n', cursor);
        const std::string line = headers.substr(
            cursor, end == std::string::npos ? std::string::npos : end - cursor + 1);
        cursor = end == std::string::npos ? headers.size() : end + 1;
        if (line.empty()) continue;
        const std::string lowered = util::ToLower(line);
        if (lowered.compare(0, 7, "cookie:") == 0) continue;
        if (lowered.compare(0, 15, "x-goog-authuser") == 0) continue;
        if (lowered.compare(0, 14, "authorization:") == 0 &&
            lowered.find("sapisidhash") != std::string::npos) {
            hadSapisidHash = true;
            continue;
        }
        out += line;
    }
    const std::string auth = AuthHeaderBlock(jar);
    cursor = 0;
    while (cursor < auth.size()) {
        const size_t end = auth.find('\n', cursor);
        const std::string line = auth.substr(
            cursor, end == std::string::npos ? std::string::npos : end - cursor + 1);
        cursor = end == std::string::npos ? auth.size() : end + 1;
        if (line.empty()) continue;
        const std::string lowered = util::ToLower(line);
        const bool cookie = lowered.compare(0, 7, "cookie:") == 0;
        const bool authUser = lowered.compare(0, 15, "x-goog-authuser") == 0;
        const bool hash = lowered.compare(0, 14, "authorization:") == 0;
        if (cookie || authUser || (hash && hadSapisidHash)) out += line;
    }
    return out;
}

bool RefreshLadder(const CookieJar& jar, std::string& fresh, WebSession& shell,
                   const char* reason, bool rejected = false, uint64_t requestStarted = 0,
                   const std::string& rejectedHeader = {}) {
    static std::timed_mutex refreshMutex;
    const uint64_t entered = util::MonotonicMs();
    std::unique_lock<std::timed_mutex> refreshLock(refreshMutex, std::defer_lock);
    const bool waited = !refreshLock.try_lock();
    if (waited) {
        if (!refreshLock.try_lock_for(std::chrono::milliseconds(60000))) return false;
    }
    const std::string baseHeader = CookieRequestHeader(jar);
    if (waited || baseHeader != jar.header || requestStarted != 0) {
        const std::string key = SessionIdentity(jar);
        const bool headerChanged =
            !rejectedHeader.empty() && rejectedHeader != ScopedCookieHeader(jar);
        const uint64_t since = headerChanged ? 0 : (requestStarted != 0 ? requestStarted : entered);
        bool recovered = false;
        {
            std::lock_guard<std::mutex> lock(g_sessionMutex);
            recovered = ReadCachedSession(key, shell, since) && HasAuthenticatedShell(shell);
        }
        if (recovered) {
            fresh = baseHeader;
            return true;
        }
        shell = {};
    }
    if (requestStarted != 0) InvalidateSessionFor(jar);

    auto accept = [&](const std::string& candidate, const char* source) {
        if (!ValidateCapturedHeader(candidate, jar.account, shell, &fresh)) return false;
        CookieJar verified;
        std::string err;
        if (!ParseCookieHeader(fresh, verified, err)) return false;
        verified.account = jar.account;
        NoteInstalledCookieHeader(jar.account, jar.header, fresh);
        NotifyCookieUpdate(jar.account, fresh);
        {
            std::lock_guard<std::mutex> lock(g_sessionMutex);
            StoreSession(SessionIdentity(verified), shell);
        }
        Log(std::string("session: ") + reason + " - recovered via " + source);
        return true;
    };

    if (relay::Started()) {
        std::string live;
        std::string harvestError;
        if (relay::ReadCookieHeader(BrowserRelayJar(jar), live, harvestError, 15000)) {
            if (accept(PreserveAccountsCookies(baseHeader, live), "browser relay jar")) {
                return true;
            }
        }
    }

    const bool rotated = RotateSessionInternal(jar, rejected);
    const std::string wireHeader = CookieRequestHeader(jar);
    if (accept(wireHeader, rotated ? "RotateCookies" : "shell touch")) {
        return true;
    }
    return false;
}

std::string ReplaceRequestField(const std::string& fields, const std::string& name,
                                const std::string& value) {
    std::string result;
    size_t cursor = 0;
    while (cursor < fields.size()) {
        const size_t end = fields.find('&', cursor);
        const std::string field = fields.substr(cursor, end - cursor);
        if (!field.empty() && field.substr(0, field.find('=')) != name) {
            if (!result.empty()) result += '&';
            result += field;
        }
        cursor = end == std::string::npos ? fields.size() : end + 1;
    }
    if (!value.empty()) {
        if (!result.empty()) result += '&';
        result += name + '=' + util::UrlEncode(value);
    }
    return result;
}

http::HttpResponse AuthedRequest(const CookieJar& jar, const http::Endpoint& endpoint,
                                 const std::string& method, const std::string& headers,
                                 const std::string& body, int timeoutMs,
                                 const http::ChunkSink* onChunk,
                                 const std::function<void()>& resetStream,
                                 WebSession* recoveredShell) {
    const uint64_t requestStarted = util::MonotonicMs();
    http::HttpResponse response =
        onChunk != nullptr
            ? http::RequestStream(endpoint, method, headers, body, timeoutMs, *onChunk)
            : http::Request(endpoint, method, headers, body, timeoutMs);
    if (response.networkError) return response;

    const bool rejected = response.status == 401 || response.status == 403;
    const bool shellGet = method == "GET" && endpoint.path == AccountPrefix(jar) + "/app";
    WebSession shell;
    const bool missingShell =
        shellGet && response.status == 200 &&
        (!ParseSessionFromShell(response.body, shell) || !HasAuthenticatedShell(shell));
    if (!rejected && !missingShell) {
        if (response.status >= 200 && response.status < 300) {
            AbsorbCookies(jar, response.headers);
        }
        return response;
    }

    std::string fresh;
    if (!RefreshLadder(jar, fresh, shell, rejected ? "auth rejection" : "empty shell", rejected,
                       requestStarted, HeaderValue(headers, "Cookie"))) {
        return response;
    }
    if (shellGet && recoveredShell != nullptr) {
        *recoveredShell = std::move(shell);
        return response;
    }

    CookieJar retry;
    std::string parseError;
    if (!ParseCookieHeader(fresh, retry, parseError)) return response;
    retry.account = jar.account;
    retry.email = jar.email;
    const std::string patched = PatchAuthHeaders(retry, headers);
    http::Endpoint retryEndpoint = endpoint;
    std::string retryBody = body;
    if (method == "POST") {
        const size_t queryStart = endpoint.path.find('?');
        if (queryStart == std::string::npos) return response;
        std::string query = endpoint.path.substr(queryStart + 1);
        query = ReplaceRequestField(query, "bl", shell.bl);
        query = ReplaceRequestField(query, "f.sid", shell.sid);
        query = ReplaceRequestField(query, "at", shell.at);
        retryEndpoint.path = endpoint.path.substr(0, queryStart + 1) + query;
        retryBody = ReplaceRequestField(body, "at", shell.at);
    }
    if (resetStream) resetStream();
    http::HttpResponse replay =
        onChunk != nullptr
            ? http::RequestStream(retryEndpoint, method, patched, retryBody, timeoutMs, *onChunk)
            : http::Request(retryEndpoint, method, patched, retryBody, timeoutMs);
    if (!replay.networkError && replay.status >= 200 && replay.status < 300) {
        AbsorbCookies(retry, replay.headers);
    }
    return replay;
}

void AppendQuery(std::string& query, const char* name, const std::string& value) {
    if (value.empty()) return;
    query += '&';
    query += name;
    query += '=';
    query += util::UrlEncode(value);
}

bool FetchRegistry(const CookieJar& jar, const WebSession& session,
                   std::vector<ModelEntry>& entries, std::string* rawBody,
                   bool* tierKnown = nullptr, int* plan = nullptr) {
    entries.clear();
    if (tierKnown != nullptr) *tierKnown = false;
    if (plan != nullptr) *plan = static_cast<int>(AccountTier::Unknown);

    std::string query = AccountPrefix(jar) + kBatchPath + "?rpcids=" + kRpcUserStatus +
                        "&source-path=" + util::UrlEncode(AccountPrefix(jar) + "/app") +
                        "&hl=" + util::UrlEncode(session.hl) + "&rt=c";
    AppendQuery(query, "at", session.at);
    AppendQuery(query, "f.sid", session.sid);
    AppendQuery(query, "bl", session.bl);

    const std::string requestJson =
        std::string("[[[\"") + kRpcUserStatus +
        "\",\"[null,null,null,null,null,null,null,null,null,null]\",null,\"generic\"]]]";
    std::string body = "f.req=" + util::UrlEncode(requestJson);
    if (!session.at.empty()) body = "at=" + util::UrlEncode(session.at) + "&" + body;

    std::string headers = BuildRequestHeaders(jar, "[]");
    headers += std::string(kSessionHeader) + ": [\"" + UpperUuid() + "\",1]\r\n";

    const http::HttpResponse response =
        PostForm(jar, std::string(kOrigin) + query, headers, body, kRegistryTimeoutMs);
    if (rawBody != nullptr) *rawBody = response.body;
    if (response.status == 401 || response.status == 403) HandleAuthFailure(jar);
    if (response.networkError || response.status < 200 || response.status >= 300) return false;

    std::string payload;
    if (FindRpcPayload(response.body, kRpcUserStatus, payload)) {
        ParseModelsFromPayload(payload, entries, tierKnown, plan);
    }
    return !entries.empty();
}

constexpr uint64_t kModelTtlMs = 30ull * 60 * 1000;
constexpr uint64_t kModelRefreshMs = 20ull * 60 * 1000;
std::mutex g_modelMutex;
struct CachedModelTable {
    std::vector<ModelEntry> entries;
    uint64_t at = 0;
    bool tierKnown = false;
    int plan = static_cast<int>(AccountTier::Unknown);
};
std::map<std::string, CachedModelTable> g_modelCaches;

struct ModelInFlight {
    std::string key;
    int waiters = 0;
    std::condition_variable cv;
};
std::vector<ModelInFlight*> g_modelInFlight;

ModelInFlight* FindModelInFlight(const std::string& key) {
    for (ModelInFlight* entry : g_modelInFlight) {
        if (entry != nullptr && entry->key == key) return entry;
    }
    return nullptr;
}

bool ModelCacheOlderThan(const CookieJar& jar, const WebSession& session, uint64_t maxAgeMs) {
    const std::string key = SessionIdentity(jar) + "|" + session.sid;
    std::lock_guard<std::mutex> lock(g_modelMutex);
    const auto it = g_modelCaches.find(key);
    if (it == g_modelCaches.end() || it->second.entries.empty()) return true;
    return util::MonotonicMs() - it->second.at >= maxAgeMs;
}

bool LoadModelTable(const CookieJar& jar, const WebSession& session,
                    std::vector<ModelEntry>& table, std::string& error, bool force,
                    bool* tierKnown = nullptr, int* plan = nullptr) {
    if (tierKnown != nullptr) *tierKnown = false;
    if (plan != nullptr) *plan = static_cast<int>(AccountTier::Unknown);

    const std::string key = SessionIdentity(jar) + "|" + session.sid;
    std::unique_lock<std::mutex> lock(g_modelMutex);
    if (!force) {
        const auto cached = g_modelCaches.find(key);
        if (cached != g_modelCaches.end() && !cached->second.entries.empty() &&
            util::MonotonicMs() - cached->second.at < kModelTtlMs) {
            table = cached->second.entries;
            if (tierKnown != nullptr) *tierKnown = cached->second.tierKnown;
            if (plan != nullptr) *plan = cached->second.plan;
            error.clear();
            return true;
        }
    }
    if (ModelInFlight* inflight = FindModelInFlight(key); inflight != nullptr) {
        inflight->waiters++;
        inflight->cv.wait_for(lock, std::chrono::milliseconds(kRegistryTimeoutMs), [&] {
            return FindModelInFlight(key) != inflight;
        });
        inflight->waiters--;
        if (inflight->waiters == 0 && FindModelInFlight(key) != inflight) delete inflight;
        const auto cached = g_modelCaches.find(key);
        if (cached != g_modelCaches.end() && !cached->second.entries.empty()) {
            table = cached->second.entries;
            if (tierKnown != nullptr) *tierKnown = cached->second.tierKnown;
            if (plan != nullptr) *plan = cached->second.plan;
            error.clear();
            return true;
        }
        error = "the registry read is still running";
        return false;
    }

    auto* inflight = new ModelInFlight{key, 0, {}};
    g_modelInFlight.push_back(inflight);
    lock.unlock();

    std::vector<ModelEntry> fresh;
    bool fetchedTierKnown = false;
    int fetchedPlan = static_cast<int>(AccountTier::Unknown);
    const bool fetched = FetchRegistry(jar, session, fresh, nullptr, &fetchedTierKnown,
                                       &fetchedPlan) &&
                         !fresh.empty();

    lock.lock();
    auto it = std::find(g_modelInFlight.begin(), g_modelInFlight.end(), inflight);
    if (it != g_modelInFlight.end()) g_modelInFlight.erase(it);
    if (fetched) {
        int resolvedPlan = fetchedPlan;
        if (resolvedPlan < 0) {
            resolvedPlan = fresh.size() > 1 ? static_cast<int>(AccountTier::Pro)
                                            : static_cast<int>(AccountTier::Free);
        }
        CachedModelTable entry;
        entry.entries = fresh;
        entry.at = util::MonotonicMs();
        entry.tierKnown = true;
        entry.plan = resolvedPlan;
        g_modelCaches[key] = std::move(entry);
        fetchedTierKnown = true;
        fetchedPlan = resolvedPlan;
    }
    inflight->cv.notify_all();
    if (inflight->waiters == 0) delete inflight;
    lock.unlock();

    if (!fetched) {
        if (tierKnown != nullptr) *tierKnown = fetchedTierKnown;
        if (plan != nullptr) *plan = fetchedPlan;
        error = "API Error: no models";
        return false;
    }
    table = std::move(fresh);
    if (tierKnown != nullptr) *tierKnown = fetchedTierKnown;
    if (plan != nullptr) *plan = fetchedPlan;
    error.clear();
    return true;
}

bool IsSiteApology(const std::string& text) {
    const std::string trimmed = util::Trim(text);
    if (trimmed.empty() || trimmed.size() > 320) return false;
    const std::string lower = util::ToLower(trimmed);
    const bool apology =
        lower.rfind("sorry", 0) == 0 || lower.find("sorry,") != std::string::npos;
    if (apology && lower.find("try your request again") != std::string::npos) return true;
    return lower.rfind("i encountered an error doing what you asked", 0) == 0;
}

bool IsSiteModelError(const std::string& text) {
    const std::string trimmed = util::Trim(text);
    if (trimmed.empty() || trimmed.size() > 160) return false;
    return util::ToLower(trimmed).rfind("i seem to be encountering an error", 0) == 0;
}

bool IsCannedFailure(const std::string& text) {
    return IsSiteApology(text) || IsSiteModelError(text);
}

constexpr int kRelayTimeoutMs = 120000;
constexpr int kPickerReadMs = 60000;
std::mutex g_pageRowsMutex;
std::vector<relay::RowInfo> g_pageRows;
std::string g_pageRowsKey;
uint64_t g_pageRowsAt = 0;
constexpr uint64_t kPageRowsTtlMs = 10ull * 60 * 1000;

std::string PageRowsKey(const CookieJar& jar) {
    return std::to_string(jar.account) + "|" + CookieHashKey(jar, jar.account);
}

std::string PickerLabelKey(const std::string& label) {
    std::string key = ModelSlug(label);
    if (key.rfind("gemini-", 0) == 0) key.erase(0, 7);
    const size_t dash = key.find('-');
    if (dash != std::string::npos &&
        key.substr(0, dash).find_first_not_of("0123456789") == std::string::npos) {
        size_t name = dash + 1;
        while (name < key.size() &&
               (key[name] == '-' || (key[name] >= '0' && key[name] <= '9'))) {
            ++name;
        }
        key.erase(0, name);
    }
    return key;
}

bool PickerMatches(const relay::RowInfo& row, const ModelEntry& entry) {
    if (!row.id.empty() && row.id == entry.id) return true;
    if (NameAnswersTo(row.label, {}, entry.label, {})) return true;
    return (row.id.empty() || entry.id.empty()) && !row.label.empty() &&
           PickerLabelKey(row.label) == PickerLabelKey(entry.label);
}

bool IsPaidPickerLabel(const std::string& label) {
    const std::string key = PickerLabelKey(label);
    return key == "pro" || key == "hydrogen" || key == "argon" || key == "advanced";
}

bool PageRows(const CookieJar& jar, std::vector<relay::RowInfo>& rows, bool force) {
    std::string err;
    if (!relay::Available(err)) return false;
    const std::string key = PageRowsKey(jar);
    {
        std::lock_guard<std::mutex> guard(g_pageRowsMutex);
        if (!force && !g_pageRows.empty() && g_pageRowsKey == key &&
            util::MonotonicMs() - g_pageRowsAt < kPageRowsTtlMs) {
            rows = g_pageRows;
            return true;
        }
    }
    std::vector<relay::RowInfo> fresh;
    if (!relay::ListModels(BrowserRelayJar(jar), fresh, err, kPickerReadMs)) return false;
    {
        std::lock_guard<std::mutex> guard(g_pageRowsMutex);
        g_pageRows = fresh;
        g_pageRowsKey = key;
        g_pageRowsAt = util::MonotonicMs();
    }
    rows = std::move(fresh);
    return true;
}

bool PageRowForRequest(const CookieJar& jar, const std::string& requested, relay::RowInfo& out) {
    std::vector<relay::RowInfo> rows;
    if (!PageRows(jar, rows, false)) return false;
    for (const relay::RowInfo& row : rows) {
        if (NameAnswersTo(requested, row.id, row.label, row.category) ||
            (!requested.empty() && PickerLabelKey(requested) == PickerLabelKey(row.label))) {
            out = row;
            return true;
        }
    }
    return false;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* MimeForPath(const std::string& path) {
    const std::string lowered = util::ToLower(path);
    if (lowered.size() >= 4 && lowered.compare(lowered.size() - 4, 4, ".png") == 0) {
        return "image/png";
    }
    if (lowered.size() >= 5 && lowered.compare(lowered.size() - 5, 5, ".webp") == 0) {
        return "image/webp";
    }
    if (lowered.size() >= 4 && lowered.compare(lowered.size() - 4, 4, ".pdf") == 0) {
        return "application/pdf";
    }
    return "image/jpeg";
}

std::string UploadHeaders(const CookieJar& jar, const std::string& pushId, size_t size,
                          const char* command, const char* contentType) {
    std::string headers;
    headers += "Accept: */*\r\n";
    headers += std::string("Authorization: ") + kUploadAuthorization + "\r\n";
    headers += std::string("Content-Type: ") + contentType + "\r\n";
    headers += "Origin: https://gemini.google.com\r\n";
    headers += "Referer: https://gemini.google.com/\r\n";
    headers += std::string("Push-ID: ") + (pushId.empty() ? kDefaultPushId : pushId) + "\r\n";
    if (command != nullptr) {
        headers += std::string("X-Goog-Upload-Command: ") + command + "\r\n";
    }
    headers += "X-Goog-Upload-Header-Content-Length: " + std::to_string(size) + "\r\n";
    headers += "X-Goog-Upload-Protocol: resumable\r\n";
    headers += "X-Tenant-Id: bard-storage\r\n";
    headers += std::string("User-Agent: ") + kUserAgent + "\r\n";
    const std::string cookie = ScopedCookieHeader(jar);
    if (!cookie.empty()) headers += "Cookie: " + cookie + "\r\n";
    return headers;
}

constexpr int kGateProbeMs = 40000;
constexpr int kMinRowBudgetMs = 45000;

std::string RowName(const ModelEntry& model) {
    return model.label.empty() ? model.id : model.label;
}

bool IsSuspectRow(const ModelEntry& model) {
    if (model.id == "56fdd199312815e2") return true;
    if (model.number == 8) return true;
    const std::string tag = util::ToLower(model.label + " " + model.category);
    return tag.find("3.8 flash") != std::string::npos || tag.find("argon") != std::string::npos;
}

enum class RowVerdict { Answers, Silent, Gated };
std::mutex g_rowVerdictMutex;
std::map<std::string, RowVerdict> g_rowVerdicts;

std::string RowVerdictKey(const CookieJar& jar, const ModelEntry& model) {
    const std::string row = model.id.empty() ? std::to_string(model.number) : model.id;
    return std::to_string(jar.account) + "|" + row;
}

bool RowHasVerdict(const CookieJar& jar, const ModelEntry& model, RowVerdict verdict) {
    std::lock_guard<std::mutex> guard(g_rowVerdictMutex);
    const auto found = g_rowVerdicts.find(RowVerdictKey(jar, model));
    return found != g_rowVerdicts.end() && found->second == verdict;
}

void RememberRowVerdict(const CookieJar& jar, const ModelEntry& model, RowVerdict verdict) {
    std::lock_guard<std::mutex> guard(g_rowVerdictMutex);
    g_rowVerdicts[RowVerdictKey(jar, model)] = verdict;
}

struct RowStats {
    uint64_t firstTokenMs = 0;
    uint64_t turnMs = 0;
    int samples = 0;
};
std::mutex g_rowStatsMutex;
std::map<std::string, RowStats> g_rowStats;

uint64_t Ewma(uint64_t previous, uint64_t sample) {
    if (sample == 0) return previous;
    if (previous == 0) return sample;
    return (previous * 3 + sample) / 4;
}

RowStats StatsFor(const CookieJar& jar, const ModelEntry& model) {
    std::lock_guard<std::mutex> guard(g_rowStatsMutex);
    const auto found = g_rowStats.find(RowVerdictKey(jar, model));
    return found == g_rowStats.end() ? RowStats{} : found->second;
}

void RecordRowStats(const CookieJar& jar, const ModelEntry& model, uint64_t firstTokenMs,
                    uint64_t turnMs, bool answered) {
    std::lock_guard<std::mutex> guard(g_rowStatsMutex);
    RowStats& row = g_rowStats[RowVerdictKey(jar, model)];
    if (answered) row.firstTokenMs = Ewma(row.firstTokenMs, firstTokenMs);
    row.turnMs = Ewma(row.turnMs, turnMs);
    row.samples += 1;
}

int BudgetForRow(const CookieJar& jar, const ModelEntry& model, bool probe) {
    if (probe) return kGateProbeMs;
    const RowStats stats = StatsFor(jar, model);
    if (stats.samples >= 2 && stats.turnMs > 0) {
        const uint64_t wanted = stats.turnMs * 2 + 15000;
        if (wanted <= static_cast<uint64_t>(kMinRowBudgetMs)) return kMinRowBudgetMs;
        if (wanted >= static_cast<uint64_t>(kGenerateTimeoutMs)) return kGenerateTimeoutMs;
        return static_cast<int>(wanted);
    }
    return kGenerateTimeoutMs;
}

const ModelEntry* FastestMeasuredRow(const CookieJar& jar,
                                     const std::vector<ModelEntry>& models,
                                     bool allowUltraOnly) {
    const ModelEntry* best = nullptr;
    uint64_t bestTurn = 0;
    for (const ModelEntry& row : models) {
        if (!allowUltraOnly && IsUltraOnlyEntry(row)) continue;
        const std::string tag = util::ToLower(row.id + " " + row.label + " " + row.category);
        if (tag.find("deep") != std::string::npos || tag.find("thinking") != std::string::npos ||
            tag.find("reasoning") != std::string::npos) {
            continue;
        }
        const RowStats stats = StatsFor(jar, row);
        if (stats.samples < 2 || stats.turnMs == 0) continue;
        if (best == nullptr || stats.turnMs < bestTurn) {
            best = &row;
            bestTurn = stats.turnMs;
        }
    }
    return best;
}

struct TurnAttempt {
    http::HttpResponse response;
    ReplyTurn turn;
    bool scannerFed = false;
    bool sawFrame = false;
    uint64_t elapsedMs = 0;
};

TurnAttempt AttemptTurn(const CookieJar& jar, const WebSession& session, const ModelEntry& row,
                        const std::string& prompt, const std::vector<std::string>& fileIds,
                        const std::vector<std::string>& fileNames, const ChatThread& chat,
                        long long level, const std::string& uuid, int timeoutMs,
                        const PartialSink& onPartial) {
    TurnAttempt attempt;
    std::vector<std::string> inner(81, "null");
    inner[0] = JoinArray({JsonQuote(prompt)});
    if (!fileIds.empty()) {
        std::string files = "[";
        for (size_t i = 0; i < fileIds.size(); ++i) {
            if (i != 0) files += ',';
            files += "[[" + JsonQuote(fileIds[i]) + "]," + JsonQuote(fileNames[i]) + "]";
        }
        files += ']';
        inner[0] = JoinArray({JsonQuote(prompt), "0", "null", files, "null", "null", "0"});
    }
    inner[1] = JoinArray({JsonQuote(session.hl)});
    const std::string chatConversation =
        chat.Active() ? JsonQuote(chat.conversation) : JsonQuote("");
    const std::string chatResponse = chat.Active() ? JsonQuote(chat.response) : JsonQuote("");
    const std::string chatCandidate = chat.Active() ? JsonQuote(chat.candidate) : JsonQuote("");
    inner[2] = JoinArray({chatConversation, chatResponse, chatCandidate, "null", "null", "null",
                          "null", "null", "null", JsonQuote("")});
    inner[6] = "[1]";
    inner[7] = "1";
    inner[10] = "1";
    inner[11] = "0";
    inner[17] = "[[0]]";
    inner[18] = "0";
    inner[27] = "1";
    inner[30] = "[4]";
    inner[41] = "[1]";
    inner[53] = "0";
    inner[59] = JsonQuote(uuid);
    inner[61] = "[]";
    inner[68] = "1";
    inner[79] = Num(row.number);
    inner[80] = Num(level);

    const std::string innerPayload = JoinArray(inner);
    const std::string generated = BuildStreamGenerateForm(innerPayload);
    std::string query = AccountPrefix(jar) + kGeneratePath + "?hl=" + util::UrlEncode(session.hl) +
                        "&_reqid=" + std::to_string(NextReqid()) + "&rt=c";
    AppendQuery(query, "at", session.at);
    AppendQuery(query, "bl", session.bl);
    AppendQuery(query, "f.sid", session.sid);

    std::string headers = BuildRequestHeaders(jar, BuildModelHeader(row, level, GenerationId()));
    headers += std::string(kSessionHeader) + ": [\"" + uuid + "\",1]\r\n";

    std::string body = generated;
    if (!session.at.empty()) body = "at=" + util::UrlEncode(session.at) + "&" + body;

    ReplyStreamScanner scanner;
    size_t emitted = 0;
    const http::ChunkSink sink = [&scanner, &emitted, &attempt, &onPartial](const char* data,
                                                                             size_t size) {
        attempt.scannerFed = true;
        if (!scanner.Feed(data, size)) return true;
        const std::string& text = scanner.Answer();
        if (!onPartial) return true;
        if (emitted == 0 || text.size() >= emitted + 48) {
            emitted = text.size();
            onPartial(text);
        }
        return true;
    };

    const uint64_t started = util::MonotonicMs();
    attempt.response = PostFormStream(jar, std::string(kOrigin) + query, headers, body, sink,
                                      timeoutMs, [&] {
        scanner = ReplyStreamScanner{};
        emitted = 0;
        attempt.scannerFed = false;
    });
    attempt.elapsedMs = util::MonotonicMs() - started;

    if (attempt.scannerFed) {
        attempt.sawFrame = scanner.SawFrame();
        attempt.turn = scanner.TakeTurn();
        if (attempt.turn.answer.empty() && !attempt.response.body.empty()) {
            ReplyTurn salvaged;
            if (ExtractReply(attempt.response.body, salvaged) && !salvaged.answer.empty()) {
                attempt.turn = std::move(salvaged);
            }
        }
    } else if (!attempt.response.body.empty()) {
        ExtractReply(attempt.response.body, attempt.turn);
    }
    return attempt;
}

long long ResetMillis(const JsonValue* value) {
    if (value == nullptr || value->kind != JsonValue::Kind::Array) return 0;
    const long long seconds = AsInt(At(*value, 0), 0);
    if (seconds <= 0) return 0;
    long long nanos = AsInt(At(*value, 1), 0);
    if (nanos < 0 || nanos > 999999999) nanos = 0;
    return seconds * 1000 + nanos / 1000000;
}

bool ParseUsagePageRow(const JsonValue& row, QuotaRow& out) {
    if (row.kind != JsonValue::Kind::Array || row.items.size() < 4) return false;
    const JsonValue* fractionV = At(row, 1);
    const JsonValue* periodV = At(row, 2);
    const JsonValue* resetWrap = At(row, 3);
    if (fractionV == nullptr || fractionV->kind != JsonValue::Kind::Number) return false;
    if (periodV == nullptr || periodV->kind != JsonValue::Kind::Number) return false;
    const double fraction = std::strtod(fractionV->text.c_str(), nullptr);
    if (fraction < 0.0 || fraction > 1.5) return false;
    const long long period = std::strtoll(periodV->text.c_str(), nullptr, 10);
    if (resetWrap == nullptr || resetWrap->kind != JsonValue::Kind::Array ||
        resetWrap->items.empty() || resetWrap->items[0].kind != JsonValue::Kind::Array) {
        return false;
    }
    const long long resetMs = ResetMillis(&resetWrap->items[0]);
    if (resetMs == 0) return false;
    out.unit = period;
    double pct = fraction * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    out.usedPercent = static_cast<int>(pct + 0.5);
    out.resetsAtMs = resetMs;
    return true;
}

void CollectUsagePageRows(const JsonValue& value, std::vector<QuotaRow>& out, int depth) {
    if (depth > 6 || value.kind != JsonValue::Kind::Array) return;
    QuotaRow row;
    if (ParseUsagePageRow(value, row)) {
        out.push_back(row);
        return;
    }
    for (const JsonValue& item : value.items) CollectUsagePageRows(item, out, depth + 1);
}

bool ParseUsagePagePayload(const std::string& payload, std::vector<QuotaRow>& out) {
    out.clear();
    size_t cursor = 0;
    JsonValue root;
    if (!ParseValue(payload, cursor, root, 0) || root.kind != JsonValue::Kind::Array) return false;
    CollectUsagePageRows(root, out, 0);
    return !out.empty();
}

bool ParseQuotaRow(const JsonValue& row, QuotaRow& out) {
    if (row.kind != JsonValue::Kind::Array) return false;
    const JsonValue* context = At(row, 0);
    if (context == nullptr || context->kind != JsonValue::Kind::Array) return false;
    if (AsInt(At(*context, 1), -1) != kComputeAction) return false;
    const JsonValue* unit = At(row, 1);
    if (unit == nullptr || unit->kind != JsonValue::Kind::Number) return false;
    double percent = 0.0;
    if (const JsonValue* value = At(row, 2);
        value != nullptr && value->kind == JsonValue::Kind::Number) {
        percent = std::strtod(value->text.c_str(), nullptr);
    }
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    out.unit = AsInt(unit, 0);
    out.usedPercent = static_cast<int>(percent + 0.5);
    out.resetsAtMs = ResetMillis(At(row, 3));
    return true;
}

void CollectQuotaRows(const JsonValue& value, std::vector<QuotaRow>& out, int depth) {
    if (depth > 4 || value.kind != JsonValue::Kind::Array) return;
    QuotaRow row;
    if (ParseQuotaRow(value, row)) {
        out.push_back(row);
        return;
    }
    for (const JsonValue& item : value.items) CollectQuotaRows(item, out, depth + 1);
}

bool ParseQuotaRows(const std::string& payload, std::vector<QuotaRow>& out) {
    out.clear();
    size_t cursor = 0;
    JsonValue root;
    if (!ParseValue(payload, cursor, root, 0) || root.kind != JsonValue::Kind::Array) return false;
    CollectQuotaRows(root, out, 0);
    return !out.empty();
}

}  // namespace

void SetLogCallback(LogCallback cb) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_logCb = std::move(cb);
}

void SetStatusCallback(StatusCallback cb) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_statusCb = std::move(cb);
}

void SetCookieUpdateCallback(CookieUpdateCallback cb) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_cookieUpdateCb = std::move(cb);
}

bool IsUltraOnlyModel(const std::string& id, const std::string& label,
                      const std::string& name) {
    return IsUltraOnlyRow(id, label, name);
}

bool ModelAllowedOnPlan(const std::string& id, const std::string& label,
                        const std::string& name, int tier) {
    if (!IsUltraOnlyModel(id, label, name)) return true;
    return tier >= static_cast<int>(AccountTier::Ultra);
}

std::string OutOfQuotaSummary() {
    std::lock_guard<std::mutex> guard(g_pageRowsMutex);
    if (g_pageRows.empty()) return {};
    std::string out;
    bool anyAvailable = false;
    bool anySpent = false;
    for (const relay::RowInfo& row : g_pageRows) {
        if (row.enabled) {
            anyAvailable = true;
            continue;
        }
        anySpent = true;
        if (!out.empty()) out += ", ";
        out += row.label;
    }
    if (!anySpent) return {};
    if (!anyAvailable) return "every model on this login is out of quota";
    return out + " - out of quota";
}

std::vector<WebModel> FallbackModels() {
    WebModel autoRow;
    autoRow.id = "gemini-auto";
    autoRow.label = "Auto";
    autoRow.name = "gemini-auto";
    return {std::move(autoRow)};
}

std::string StripCitations(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 6, "[cite:") == 0) {
            const size_t close = text.find(']', i);
            if (close != std::string::npos) {
                i = close + 1;
                continue;
            }
        }
        if (text.compare(i, 12, "[cite_start]") == 0) {
            i += 12;
            continue;
        }
        if (text.compare(i, 15, "?code_reference") == 0) {
            i += 15;
            continue;
        }
        if (text.compare(i, 12, "?code_stdout") == 0) {
            i += 12;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool FetchModels(const CookieJar& jar, std::vector<WebModel>& models, std::string& error,
                 bool* usedFallback, bool* proAccount, std::string* email, bool* tierKnown,
                 int* tier, bool allowPageRelay) {
    models.clear();
    error.clear();
    if (usedFallback != nullptr) *usedFallback = false;
    if (proAccount != nullptr) *proAccount = false;
    const int seededTier = (tier != nullptr) ? *tier : static_cast<int>(AccountTier::Unknown);
    if (tierKnown != nullptr) *tierKnown = false;
    if (tier != nullptr) *tier = seededTier;
    if (email != nullptr) email->clear();

    if (jar.account > 0 && CookieHashKey(jar, jar.account).empty()) {
        error = "Google slot " + std::to_string(jar.account + 1) +
                " is not in these cookies (no __Secure-" + std::to_string(jar.account + 1) +
                "PAPISID)";
        return false;
    }

    WebSession session;
    const bool haveSession = GetSession(jar, session, error, kShellTimeoutMs);
    if (haveSession) {
        if (email != nullptr) *email = session.email;
    } else {
        error.clear();
    }

    std::vector<ModelEntry> entries;
    bool registryTierKnown = false;
    int registryPlan = static_cast<int>(AccountTier::Unknown);
    if (!haveSession ||
        !LoadModelTable(jar, session, entries, error, true, &registryTierKnown, &registryPlan) ||
        entries.empty()) {
        entries.clear();
        if (usedFallback != nullptr) *usedFallback = true;
        error.clear();
    }

    std::vector<relay::RowInfo> pageRows;
    bool paidPicker = false;
    if (allowPageRelay && PageRows(jar, pageRows, true)) {
        for (const relay::RowInfo& row : pageRows) {
            const auto match = std::find_if(entries.begin(), entries.end(),
                                            [&](const ModelEntry& entry) {
                                                return PickerMatches(row, entry);
                                            });
            if (match != entries.end()) {
                match->available = row.enabled;
                match->note = row.note;
                if (row.enabled && row.label.find_first_of("0123456789") != std::string::npos) {
                    match->label = row.label;
                }
            } else if (!row.label.empty() &&
                       row.label.find_first_of("0123456789") != std::string::npos) {
                ModelEntry entry;
                entry.id = row.id;
                entry.label = row.label;
                entry.category = row.category;
                entry.inPicker = true;
                entry.number = 0;
                entry.available = row.enabled;
                entry.note = row.note;
                entries.push_back(std::move(entry));
            }
            if (row.enabled) paidPicker = paidPicker || IsPaidPickerLabel(row.label);
        }
    }

    int accountPlan = registryTierKnown ? registryPlan : static_cast<int>(AccountTier::Unknown);
    if (paidPicker && accountPlan < static_cast<int>(AccountTier::Pro)) {
        accountPlan = static_cast<int>(AccountTier::Pro);
    }
    if (accountPlan < static_cast<int>(AccountTier::Pro)) {
        for (const ModelEntry& entry : entries) {
            if (entry.available && (entry.capacity > 1 || IsPaidPickerLabel(entry.label))) {
                accountPlan = static_cast<int>(AccountTier::Pro);
                break;
            }
        }
    }
    if (accountPlan < static_cast<int>(AccountTier::Free) && haveSession &&
        session.tier >= static_cast<int>(AccountTier::Free)) {
        accountPlan = session.tier;
    }
    if (accountPlan < static_cast<int>(AccountTier::Free) &&
        seededTier >= static_cast<int>(AccountTier::Free)) {
        accountPlan = seededTier;
    }
    if (tierKnown != nullptr) *tierKnown = accountPlan >= 0;
    if (tier != nullptr) *tier = accountPlan;
    if (proAccount != nullptr) *proAccount = PlanIsPaid(accountPlan);

    const bool allowUltraOnly = accountPlan >= static_cast<int>(AccountTier::Ultra);
    std::vector<std::string> keptKeys;
    for (const ModelEntry& entry : entries) {
        if (!allowUltraOnly && IsUltraOnlyEntry(entry)) continue;
        WebModel model;
        model.id = entry.id;
        if (model.id.empty() && entry.number > 0) model.id = std::to_string(entry.number);
        model.label = !entry.label.empty() ? entry.label : entry.category;
        model.name = ProductModelName(model.label, entry.category, entry.number);
        model.available = entry.available;
        model.note = entry.note;

        const std::string key = ModelBaseKey(model.label, entry.category, entry.number);
        const auto seen = std::find(keptKeys.begin(), keptKeys.end(), key);
        if (seen != keptKeys.end()) {
            WebModel& existing = models[static_cast<size_t>(seen - keptKeys.begin())];
            if (model.label.size() > existing.label.size()) existing.label = model.label;
            if (existing.id.empty()) existing.id = model.id;
            if (existing.note.empty()) existing.note = model.note;
            existing.available = existing.available || model.available;
            continue;
        }
        keptKeys.push_back(key);
        models.push_back(std::move(model));
    }
    return !models.empty();
}

bool UploadImage(const CookieJar& jar, const std::string& pushId, const std::string& filePath,
                 std::string& fileId, std::string& error) {
    fileId.clear();
    error.clear();
    std::vector<uint8_t> bytes;
    if (!util::ReadFileBytes(filePath, bytes) || bytes.empty()) {
        error = kErrUploadFailed;
        return false;
    }
    http::Endpoint endpoint;
    if (!http::ParseEndpoint(kUploadUrl, endpoint)) {
        error = kErrUploadFailed;
        return false;
    }

    const size_t size = bytes.size();
    const std::string fileName = BaseName(filePath);
    const char* const mime = MimeForPath(filePath);

    auto runStep = [&error](const http::Endpoint& target, const char* method,
                            const std::string& headers, const std::string& body,
                            http::HttpResponse& out) -> bool {
        out = http::Request(target, method, headers, body, 120000);
        if (out.networkError) {
            error = out.detail.empty() ? std::string(kErrUploadFailed) : out.detail;
            return false;
        }
        if (out.status < 200 || out.status >= 300) {
            error = kErrUploadFailed;
            return false;
        }
        return true;
    };

    http::HttpResponse response;
    if (!runStep(endpoint, "OPTIONS", UploadHeaders(jar, pushId, size, nullptr, kUploadFormType),
                 {}, response)) {
        return false;
    }

    std::string startHeaders = UploadHeaders(jar, pushId, size, "start", kUploadFormType);
    startHeaders += "Size: " + std::to_string(size) + "\r\n";
    if (!runStep(endpoint, "POST", startHeaders, "File name: " + fileName, response)) {
        return false;
    }

    const std::string uploadUrl = HeaderValue(response.headers, "X-Goog-Upload-Url");
    http::Endpoint fileEndpoint;
    if (uploadUrl.empty() || !http::ParseEndpoint(uploadUrl, fileEndpoint)) {
        error = kErrUploadFailed;
        return false;
    }

    if (!runStep(fileEndpoint, "OPTIONS", startHeaders, {}, response)) return false;

    std::string finalHeaders = UploadHeaders(jar, pushId, size, "upload, finalize", mime);
    finalHeaders += "Size: " + std::to_string(size) + "\r\n";
    finalHeaders += "X-Goog-Upload-Offset: 0\r\n";
    const std::string payload(bytes.begin(), bytes.end());
    if (!runStep(fileEndpoint, "POST", finalHeaders, payload, response)) return false;

    std::string id = util::Trim(response.body);
    if (!id.empty() && (id.front() == '"' || id.front() == '\'')) id.erase(id.begin());
    if (!id.empty() && (id.back() == '"' || id.back() == '\'')) id.pop_back();
    if (id.empty()) {
        error = kErrUploadFailed;
        return false;
    }
    fileId = id;
    return true;
}

bool Ask(const CookieJar& jar, const std::string& modelId, const std::string& prompt,
         const std::string& effort, const std::vector<std::string>& imagePaths, ChatThread& chat,
         std::string& answer, std::string& error, const PartialSink& onPartial,
         bool allowUltraOnly, AskDetails* details) {
    answer.clear();
    error.clear();
    if (details != nullptr) *details = AskDetails{};
    if (!chat.Active()) chat.ClearWire();

    ReportStatus("Gemini: opening session...");
    WebSession session;
    if (!GetSession(jar, session, error)) return false;

    std::vector<ModelEntry> table;
    if (!LoadModelTable(jar, session, table, error, false)) {
        Log("gemini: registry unavailable, falling back to default model table [" + error + "]");
        error.clear();
        table = FallbackModelTable();
    }

    const std::string wantModel = util::ToLower(util::Trim(modelId));
    const bool autoRequest = wantModel.empty() || wantModel == "gemini-auto" ||
                             wantModel == "auto" || wantModel == "default";
    const ModelEntry* model = PickModel(table, modelId);
    if (autoRequest) {
        if (const ModelEntry* measured = FastestMeasuredRow(jar, table, allowUltraOnly);
            measured != nullptr) {
            model = measured;
        }
    }

    relay::RowInfo pageRow;
    const bool pageRowKnown = PageRowForRequest(jar, modelId, pageRow) ||
                              (model != nullptr && PageRowForRequest(jar, model->label, pageRow));
    ModelEntry pageOnlyModel;
    const bool pageOnly = (model == nullptr || (model->id.empty() && model->number <= 0)) &&
                          pageRowKnown && pageRow.enabled;
    if (pageOnly) {
        pageOnlyModel.id = pageRow.id;
        pageOnlyModel.label = pageRow.label;
        pageOnlyModel.category = pageRow.category;
        model = &pageOnlyModel;
    }

    if (model == nullptr || (model->id.empty() && model->number <= 0)) {
        const ModelEntry* fallback = PickModel(table, kDefaultGeminiModel);
        if (const ModelEntry* measured = FastestMeasuredRow(jar, table, allowUltraOnly);
            measured != nullptr) {
            fallback = measured;
        }
        if (fallback != nullptr && (!fallback->id.empty() || fallback->number > 0)) {
            model = fallback;
        } else {
            error = "No model rows for this account. Sign in again.";
            return false;
        }
    }

    if (!ModelAllowedOnPlan(model->id, model->category, model->label,
                            allowUltraOnly ? static_cast<int>(AccountTier::Ultra)
                                           : static_cast<int>(AccountTier::Unknown))) {
        error = "Gemini 4 Carbon requires an Ultra plan";
        return false;
    }

    std::vector<std::string> fileIds(imagePaths.size());
    std::vector<std::string> fileNames(imagePaths.size());
    std::vector<std::string> uploadErrors(imagePaths.size());
    if (!imagePaths.empty()) {
        ReportStatus("Gemini: uploading attachments...");
        std::atomic<size_t> nextImage{0};
        const size_t workerCount = std::min<size_t>(imagePaths.size(), 4);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t w = 0; w < workerCount; ++w) {
            workers.emplace_back([&] {
                for (;;) {
                    const size_t index = nextImage.fetch_add(1);
                    if (index >= imagePaths.size()) return;
                    UploadImage(jar, session.push, imagePaths[index], fileIds[index],
                                uploadErrors[index]);
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        for (size_t index = 0; index < imagePaths.size(); ++index) {
            if (!uploadErrors[index].empty() || fileIds[index].empty()) {
                error = uploadErrors[index].empty() ? std::string(kErrUploadFailed)
                                                    : uploadErrors[index];
                return false;
            }
            fileNames[index] = BaseName(imagePaths[index]);
        }
    }

    const std::string uuid = UpperUuid();
    const long long level = ThinkingLevel(effort);

    const bool rowOutOfQuota = pageRowKnown && !pageRow.enabled;
    std::string relayProbeError;
    if (pageRowKnown && !rowOutOfQuota && relay::Available(relayProbeError)) {
        ModelEntry requestRow;
        requestRow.id = pageRow.id;
        requestRow.label = pageRow.label;
        requestRow.category = pageRow.category;
        requestRow.number = model->number;

        std::string relayError;
        std::string relayAnswer;
        std::string relayThread = chat.page;
        const bool pageContinuing = !relayThread.empty();
        const uint64_t relayStart = util::MonotonicMs();
        uint64_t relayFirst = 0;
        const PartialSink relayPartial = [&](const std::string& text) {
            if (relayFirst == 0 && !text.empty()) relayFirst = util::MonotonicMs();
            if (onPartial) onPartial(text);
        };
        const bool relayed = relay::Ask(BrowserRelayJar(jar), requestRow.id, requestRow.label,
                                        ThinkingLabel(effort), prompt, imagePaths,
                                        !pageContinuing, relayAnswer, relayError, relayThread,
                                        relayPartial, kRelayTimeoutMs);
        const std::string relayText = util::Trim(relayAnswer);
        const uint64_t spent = util::MonotonicMs() - relayStart;
        RecordRowStats(jar, requestRow, relayFirst != 0 ? relayFirst - relayStart : 0,
                       (relayed && !relayText.empty()) ? spent : spent * 2,
                       relayed && !relayText.empty());
        if (relayed && !relayText.empty() && !IsCannedFailure(relayText)) {
            chat.ClearWire();
            chat.page = relayThread;
            answer = StripCitations(relayAnswer);
            if (details != nullptr) {
                details->answer = answer;
                details->servedLabel = requestRow.label;
                details->elapsedMs = spent;
            }
            return true;
        }
    }
    if (pageOnly) {
        error = "The browser relay could not run the selected model.";
        return false;
    }

    std::vector<const ModelEntry*> ladder;
    if (!rowOutOfQuota) ladder.push_back(model);
    if (IsSuspectRow(*model) || rowOutOfQuota) {
        const ModelEntry* pro = nullptr;
        const ModelEntry* lite = nullptr;
        for (const ModelEntry& row : table) {
            if (row.id == model->id || (!allowUltraOnly && IsUltraOnlyEntry(row))) continue;
            if (pro == nullptr && util::ToLower(row.category) == "pro") pro = &row;
            const std::string tag = util::ToLower(row.label + " " + row.category);
            if (lite == nullptr && tag.find("lite") != std::string::npos) lite = &row;
        }
        if (pro != nullptr && pro != model) ladder.push_back(pro);
        if (lite != nullptr && lite != model && lite != pro) ladder.push_back(lite);
    }
    if (ladder.empty()) ladder.push_back(model);

    ReplyTurn turn;
    http::HttpResponse response;
    const ModelEntry* servedRow = model;
    uint64_t servedElapsedMs = 0;
    bool answeredOnLadder = false;
    bool gatedAny = false;

    for (size_t ladderIndex = 0; ladderIndex < ladder.size(); ++ladderIndex) {
        const ModelEntry& row = *ladder[ladderIndex];
        if (RowHasVerdict(jar, row, RowVerdict::Gated)) {
            gatedAny = true;
            continue;
        }
        const bool probeRow = IsSuspectRow(row) &&
                              !RowHasVerdict(jar, row, RowVerdict::Answers) &&
                              !RowHasVerdict(jar, row, RowVerdict::Silent);
        const int budget = BudgetForRow(jar, row, probeRow);
        ReportStatus("Gemini: " + RowName(row) + " is thinking...");

        TurnAttempt attempt;
        bool canned = false;
        uint64_t firstTokenTick = 0;
        const uint64_t sendStart = util::MonotonicMs();
        const PartialSink timedPartial = [&](const std::string& text) {
            if (firstTokenTick == 0 && !text.empty()) firstTokenTick = util::MonotonicMs();
            if (onPartial) onPartial(text);
        };

        for (int sendIndex = 0; sendIndex < 2; ++sendIndex) {
            attempt = AttemptTurn(jar, session, row, prompt, fileIds, fileNames, chat, level, uuid,
                                  budget, timedPartial);
            canned = !attempt.response.networkError && IsCannedFailure(attempt.turn.answer);
            if (!canned) break;
            if (sendIndex == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        const bool statusOk = attempt.response.status == 0 ||
                              (attempt.response.status >= 200 && attempt.response.status < 300);
        const bool realAnswer = !util::Trim(attempt.turn.answer).empty() && !canned;

        if (!attempt.response.networkError) {
            const bool timedOut =
                !realAnswer && attempt.elapsedMs >= static_cast<uint64_t>(budget) * 9 / 10;
            RecordRowStats(jar, row, firstTokenTick != 0 ? firstTokenTick - sendStart : 0,
                           timedOut ? attempt.elapsedMs * 2 : attempt.elapsedMs, realAnswer);
        }

        if (probeRow && statusOk && !attempt.response.networkError) {
            const bool healthy = realAnswer || attempt.sawFrame;
            const bool secondSilence = RowHasVerdict(jar, row, RowVerdict::Silent);
            RowVerdict verdict = RowVerdict::Gated;
            if (healthy) verdict = RowVerdict::Answers;
            else if (attempt.response.status == 0 && !secondSilence) verdict = RowVerdict::Silent;
            RememberRowVerdict(jar, row, verdict);
            if (!healthy) {
                if (verdict == RowVerdict::Gated) gatedAny = true;
                continue;
            }
        }

        if (attempt.response.networkError) {
            error = attempt.response.detail.empty() ? "Network request failed."
                                                    : attempt.response.detail;
            return false;
        }
        if (attempt.response.status == 401 || attempt.response.status == 403) {
            HandleAuthFailure(jar);
            error = kErrAuthRejected;
            return false;
        }
        if (!statusOk) {
            if (attempt.response.status == 400) InvalidateSessionFor(jar);
            error = "API Error: HTTP " + std::to_string(attempt.response.status);
            return false;
        }
        if (canned) {
            if (ladderIndex + 1 < ladder.size()) continue;
            error = kErrTransient;
            return false;
        }

        response = std::move(attempt.response);
        turn = std::move(attempt.turn);
        servedRow = &row;
        servedElapsedMs = attempt.elapsedMs;
        answeredOnLadder = true;
        break;
    }

    if (!answeredOnLadder) {
        error = gatedAny
                    ? "Gemini Web: " + RowName(*model) +
                          " is not served on this connection. Pick another model."
                    : std::string(kErrParse);
        return false;
    }

    const bool parsedAnswer = !util::Trim(turn.answer).empty();
    if (parsedAnswer && IsSiteModelError(turn.answer)) {
        error = "Gemini Web: " + RowName(*servedRow) + " did not answer. Pick another model.";
        return false;
    }
    if (parsedAnswer && IsSiteApology(turn.answer)) {
        error = kErrTransient;
        return false;
    }

    if (turn.chat.conversation.empty() || turn.chat.response.empty() ||
        turn.chat.candidate.empty()) {
        chat.ClearWire();
    } else {
        const std::string page = chat.page;
        chat = turn.chat;
        chat.page = page;
    }

    if (!parsedAnswer) {
        long long code = 0;
        std::string message;
        if (ParseBardError(response.body, code, message)) {
            error = "Gemini Web: " +
                    (message.empty() ? "request refused (code " + std::to_string(code) + ")"
                                     : message);
            if (code == 1097) chat.ClearWire();
            return false;
        }
        error = kErrParse;
        return false;
    }

    answer = StripCitations(turn.answer);
    chat.page.clear();
    if (details != nullptr) {
        details->answer = answer;
        details->thinking = turn.thinking;
        details->servedLabel = turn.servedLabel.empty() ? RowName(*servedRow) : turn.servedLabel;
        details->servedNumber = turn.servedNumber > 0 ? turn.servedNumber : servedRow->number;
        details->elapsedMs = servedElapsedMs;
    }
    return true;
}

bool FetchUsage(const CookieJar& jar, UsageMeter& meter, std::string& error) {
    meter = UsageMeter{};
    error.clear();
    WebSession session;
    if (!GetSession(jar, session, error)) return false;

    std::string query = AccountPrefix(jar) + kBatchPath + "?rpcids=" + kRpcQuota +
                        "&source-path=" + util::UrlEncode(AccountPrefix(jar) + "/app") +
                        "&hl=" + util::UrlEncode(session.hl) + "&rt=c";
    AppendQuery(query, "at", session.at);
    AppendQuery(query, "f.sid", session.sid);
    AppendQuery(query, "bl", session.bl);

    std::string headers = BuildRequestHeaders(jar, "[]");
    headers += std::string(kSessionHeader) + ": [\"" + UpperUuid() + "\",1]\r\n";

    static const char* const kRequestShapes[] = {"[[[1,2]]]", "[]"};
    std::vector<QuotaRow> rows;
    const long long nowMs = static_cast<long long>(std::time(nullptr)) * 1000;

    for (const char* shape : kRequestShapes) {
        const std::string requestJson =
            std::string("[[[\"") + kRpcQuota + "\",\"" + shape + "\",null,\"generic\"]]]";
        std::string body = "f.req=" + util::UrlEncode(requestJson);
        if (!session.at.empty()) body = "at=" + util::UrlEncode(session.at) + "&" + body;

        const http::HttpResponse response = PostForm(jar, std::string(kOrigin) + query, headers, body);
        if (response.networkError) {
            error = response.detail.empty() ? "Request failed." : response.detail;
            return false;
        }
        if (response.status == 401 || response.status == 403) {
            HandleAuthFailure(jar);
            error = kErrAuthRejected;
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            error = "API Error: HTTP " + std::to_string(response.status);
            return false;
        }
        std::string payload;
        if (FindRpcPayload(response.body, kRpcQuota, payload) && ParseQuotaRows(payload, rows)) {
            ClassifyQuotaRows(rows, nowMs, meter);
            if (meter.shortWindow.valid) break;
        }
    }

    if (!meter.shortWindow.valid) {
        const std::string pageReq =
            std::string("[[[\"") + kRpcUsagePage + "\",\"[]\",null,\"generic\"]]]";
        std::string pageBody = "f.req=" + util::UrlEncode(pageReq);
        if (!session.at.empty()) pageBody = "at=" + util::UrlEncode(session.at) + "&" + pageBody;

        std::string pageQuery = AccountPrefix(jar) + kBatchPath + "?rpcids=" + kRpcUsagePage +
                                "&source-path=" + util::UrlEncode(AccountPrefix(jar) + "/usage") +
                                "&hl=" + util::UrlEncode(session.hl) + "&rt=c";
        AppendQuery(pageQuery, "at", session.at);
        AppendQuery(pageQuery, "f.sid", session.sid);
        AppendQuery(pageQuery, "bl", session.bl);

        const http::HttpResponse pageResp =
            PostForm(jar, std::string(kOrigin) + pageQuery, headers, pageBody);
        if (!pageResp.networkError && pageResp.status >= 200 && pageResp.status < 300) {
            std::string pagePayload;
            if (FindRpcPayload(pageResp.body, kRpcUsagePage, pagePayload) &&
                ParseUsagePagePayload(pagePayload, rows)) {
                ClassifyQuotaRows(rows, nowMs, meter);
            }
        }
    }

    if (!meter.shortWindow.valid && !meter.weekly.valid) {
        error = "no quota rows returned";
        return false;
    }
    return true;
}

bool RotateCookies(const CookieJar& jar, std::string& updatedHeader, std::string& error,
                   bool force) {
    updatedHeader.clear();
    error.clear();
    if (!RotateSessionInternal(jar, force)) {
        error = "cookie rotation failed or throttled";
        return false;
    }
    updatedHeader = CookieRequestHeader(jar);
    return true;
}

bool KeepAlive(const CookieJar& jar, std::string& error) {
    error.clear();
    WebSession session;
    return FetchSession(jar, session, error);
}

bool IsHeaderSignedIn(const std::string& header, int account) {
    WebSession probe;
    return ValidateCapturedHeader(header, account, probe);
}

bool EnsureSessionFresh(CookieJar& jar, bool force, const char* reason) {
    std::string fresh;
    WebSession shell;
    if (!RefreshLadder(jar, fresh, shell, reason != nullptr ? reason : "refresh", force)) {
        return false;
    }
    if (fresh.empty() || fresh == jar.header) return true;
    CookieJar parsed;
    std::string parseError;
    if (!ParseCookieHeader(fresh, parsed, parseError)) return false;
    parsed.account = jar.account;
    parsed.email = shell.email.empty() ? jar.email : shell.email;
    jar = std::move(parsed);
    return true;
}

void ResetSession() {
    InvalidateSession();
    std::lock_guard<std::mutex> lock(g_modelMutex);
    g_modelCaches.clear();
}

bool IsTransientError(const std::string& error) {
    if (error.empty()) return false;
    const std::string lower = util::ToLower(error);
    if (lower == util::ToLower(kErrAuthRejected)) return false;
    if (lower.find("gemini web: the site failed") != std::string::npos) return true;
    if (lower.find("something went wrong") != std::string::npos ||
        lower.find("try your request again") != std::string::npos) {
        return true;
    }
    if (lower.find("code 1013") != std::string::npos ||
        lower.find("code 1052") != std::string::npos) {
        return true;
    }
    static const char* const kStatuses[] = {"http 429", "http 500", "http 502", "http 503",
                                            "http 504"};
    for (const char* needle : kStatuses) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    static const char* const kWords[] = {"timed out", "timeout", "overloaded", "temporarily",
                                         "unavailable"};
    for (const char* needle : kWords) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

void Prewarm(const CookieJar& jar) {
    std::string error;
    WebSession session;
    if (!GetSession(jar, session, error)) return;
    std::vector<ModelEntry> table;
    LoadModelTable(jar, session, table, error, false);
}

void WarmRefresh(const CookieJar& jar) {
    if (SessionOlderThan(jar, kSessionRefreshMs)) InvalidateSessionFor(jar);
    std::string error;
    WebSession session;
    if (!GetSession(jar, session, error)) return;
    std::vector<ModelEntry> table;
    LoadModelTable(jar, session, table, error,
                   ModelCacheOlderThan(jar, session, kModelRefreshMs));
}

// --- Ergonomic Client wrapper ---

bool Client::Init(const std::string& cookieHeader, int accountSlot, std::string& error) {
    initialized_ = false;
    chat_.Clear();
    if (!ParseCookieHeader(cookieHeader, jar_, error)) return false;
    jar_.account = accountSlot < 0 ? 0 : accountSlot;
    if (jar_.account > 0 && CookieHashKey(jar_, jar_.account).empty()) {
        error = "Google slot " + std::to_string(jar_.account + 1) +
                " has no __Secure-" + std::to_string(jar_.account + 1) +
                "PAPISID cookie in this header";
        return false;
    }
    initialized_ = true;
    return true;
}

bool Client::ListModels(std::vector<WebModel>& models, std::string& error, AccountTier* tierOut,
                        std::string* emailOut) {
    if (!initialized_) {
        error = "client not initialized";
        return false;
    }
    int tierInt = static_cast<int>(AccountTier::Unknown);
    const bool ok = FetchModels(jar_, models, error, nullptr, nullptr, emailOut, nullptr, &tierInt);
    if (tierOut != nullptr) *tierOut = static_cast<AccountTier>(tierInt);
    if (tierInt >= static_cast<int>(AccountTier::Ultra)) allowUltraOnly_ = true;
    return ok;
}

bool Client::Ask(const std::string& prompt, std::string& answer, std::string& error,
                 const std::string& model, const std::string& effort,
                 const std::vector<std::string>& imagePaths, const PartialSink& onPartial,
                 bool continueChat, AskDetails* details) {
    if (!initialized_) {
        error = "client not initialized";
        return false;
    }
    ChatThread localThread;
    ChatThread& active = continueChat ? chat_ : localThread;
    bool ok = ::gwa::Ask(jar_, model, prompt, effort, imagePaths, active, answer, error, onPartial,
                         allowUltraOnly_, details);
    if (!ok && IsTransientError(error)) {
        for (int attempt = 1; attempt <= 2 && !ok; ++attempt) {
            if (!continueChat) localThread.Clear();
            ResetSession();
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 1 ? 250 : 700));
            ok = ::gwa::Ask(jar_, model, prompt, effort, imagePaths, active, answer, error,
                            onPartial, allowUltraOnly_, details);
        }
    }
    return ok;
}

bool Client::GetUsage(UsageMeter& meter, std::string& error) {
    if (!initialized_) {
        error = "client not initialized";
        return false;
    }
    return FetchUsage(jar_, meter, error);
}

bool Client::Rotate(std::string& error, bool force) {
    if (!initialized_) {
        error = "client not initialized";
        return false;
    }
    std::string updated;
    return RotateCookies(jar_, updated, error, force);
}

}  // namespace gwa
