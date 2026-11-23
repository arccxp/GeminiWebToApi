#include "gemini_web/cookies.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gemini_web/util.h"

namespace gwa {
namespace {

constexpr size_t kMaxCookieHeaderBytes = 32u * 1024u;
constexpr size_t kMaxCookieValueBytes = 4u * 1024u;

std::string Unwrap(const std::string& text) {
    std::string value = util::Trim(text);

    if (value.compare(0, 3, "-H ") == 0) {
        value = util::Trim(value.substr(3));
    }
    while (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           (value.back() == '\'' || value.back() == '"' || value.back() == '\\')) {
        value.pop_back();
    }

    const std::string lowered = util::ToLower(value);
    if (lowered.compare(0, 7, "cookie:") == 0) {
        value = util::Trim(value.substr(7));
        while (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
            value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == '\'' || value.back() == '"')) {
            value.pop_back();
        }
        value = util::Trim(value);
    }
    return value;
}

bool IsNameChar(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '_' || c == '-' || c == '.' || c == '!' || c == '#' || c == '$' ||
           c == '%' || c == '&' || c == '*' || c == '+' || c == '^' || c == '`' ||
           c == '|' || c == '~';
}

bool IsValidCookieName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!IsNameChar(c)) return false;
    }
    return true;
}

std::string CookieValue(const std::string& raw) {
    std::string value = util::Trim(raw);
    const size_t newline = value.find_first_of("\r\n");
    if (newline != std::string::npos) value.resize(newline);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = util::Trim(value.substr(1, value.size() - 2));
    }
    return value;
}

std::string HeaderCookie(const std::string& header, const std::string& name) {
    const std::string key = name + "=";
    size_t at = header.find(key);
    while (at != std::string::npos) {
        size_t before = at;
        while (before > 0 && (header[before - 1] == ' ' || header[before - 1] == '\t')) --before;
        if (before == 0 || header[before - 1] == ';') {
            const size_t start = at + key.size();
            const size_t end = header.find(';', start);
            return CookieValue(
                header.substr(start, end == std::string::npos ? std::string::npos : end - start));
        }
        at = header.find(key, at + 1);
    }
    return {};
}

bool HasSessionCookie(const std::string& header) {
    if (!HeaderCookie(header, "SID").empty()) {
        return true;
    }
    for (int slot = 0; slot < 4; ++slot) {
        if (!HeaderCookie(header, "__Secure-" + std::to_string(slot + 1) + "PSID").empty()) {
            return true;
        }
    }
    return false;
}

struct CookiePair {
    std::string name;
    std::string value;
};

std::vector<CookiePair> SplitPairs(const std::string& header) {
    std::vector<CookiePair> pairs;
    size_t position = 0;
    while (position < header.size()) {
        const size_t separator = header.find(';', position);
        const size_t end = separator == std::string::npos ? header.size() : separator;
        std::string pair = util::Trim(header.substr(position, end - position));
        position = end + 1;

        const size_t equals = pair.find('=');
        if (equals != std::string::npos && equals != 0) {
            CookiePair entry;
            entry.name = util::Trim(pair.substr(0, equals));
            entry.value = CookieValue(pair.substr(equals + 1));
            if (IsValidCookieName(entry.name) && !entry.value.empty()) {
                pairs.push_back(std::move(entry));
            }
        }
        if (separator == std::string::npos) break;
    }
    return pairs;
}

std::string JoinPairs(const std::vector<CookiePair>& pairs) {
    std::string header;
    for (const CookiePair& pair : pairs) {
        if (!header.empty()) header += "; ";
        header += pair.name;
        header += '=';
        header += pair.value;
    }
    return header;
}

struct Rotation {
    std::string live;
    bool pending = false;
};

struct RotationStore {
    std::mutex mutex;
    std::map<std::pair<int, std::string>, Rotation> byHeader;
};

RotationStore& Store() {
    static RotationStore store;
    return store;
}

std::string SlotPaperCookie(int slot) {
    return "__Secure-" + std::to_string(slot + 1) + "PAPISID";
}

std::string SlotSessionCookie(int slot) {
    return "__Secure-" + std::to_string(slot + 1) + "PSID";
}

// __Secure-3PSID is written alongside __Secure-1PSID on normal single-account jars.
// It only indicates slot 2 when __Secure-2PSID is also present.
bool SlotIsLogin(const std::string& live, int slot, const std::string& value) {
    if (value.empty()) return false;
    if (slot != 2) return true;
    const bool hasFirst = !HeaderCookie(live, SlotSessionCookie(0)).empty();
    const bool hasSecond = !HeaderCookie(live, SlotSessionCookie(1)).empty();
    return !(hasFirst && !hasSecond);
}

}  // namespace

bool ParseCookieHeader(const std::string& text, CookieJar& jar, std::string& error) {
    jar = CookieJar{};
    error.clear();

    const std::string value = Unwrap(text);
    if (value.empty()) {
        error = "no cookies";
        return false;
    }
    if (value.size() > kMaxCookieHeaderBytes) {
        error = "cookie header too large";
        return false;
    }

    std::string header;
    header.reserve(value.size());

    size_t position = 0;
    while (position <= value.size()) {
        const size_t separator = value.find(';', position);
        const size_t end = separator == std::string::npos ? value.size() : separator;
        std::string pair = util::Trim(value.substr(position, end - position));
        position = end + 1;

        if (pair.empty()) {
            if (separator == std::string::npos) break;
            continue;
        }

        const size_t equals = pair.find('=');
        if (equals == std::string::npos || equals == 0) {
            if (separator == std::string::npos) break;
            continue;
        }

        std::string name = util::Trim(pair.substr(0, equals));
        std::string cookieValue = CookieValue(pair.substr(equals + 1));
        if (!IsValidCookieName(name) || cookieValue.empty()) {
            if (separator == std::string::npos) break;
            continue;
        }

        if (header.size() + name.size() + cookieValue.size() + 2 > kMaxCookieHeaderBytes) {
            jar = CookieJar{};
            error = "cookie header too large";
            return false;
        }

        if (!header.empty()) header += "; ";
        header += name;
        header += '=';
        header += cookieValue;
        ++jar.count;

        if (name == "SAPISID") jar.sapisid = cookieValue;

        if (separator == std::string::npos) break;
    }

    if (jar.count == 0) {
        error = "no cookies";
        return false;
    }
    if (!HasSessionCookie(header)) {
        error = "no session cookie";
        return false;
    }

    jar.header = std::move(header);
    return true;
}

std::string CookieRequestHeader(const CookieJar& jar) {
    RotationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto entry = store.byHeader.find({jar.account, jar.header});
    return entry == store.byHeader.end() ? jar.header : entry->second.live;
}

std::string CookieSapisid(const CookieJar& jar) {
    const std::string value = HeaderCookie(CookieRequestHeader(jar), "SAPISID");
    return value.empty() ? jar.sapisid : value;
}

std::string CookieHashKey(const CookieJar& jar, int account) {
    const int slot = account < 0 ? 0 : account;
    const std::string live = CookieRequestHeader(jar);

    if (slot > 0) {
        return HeaderCookie(live, SlotPaperCookie(slot));
    }

    std::string key = HeaderCookie(live, "SAPISID");
    if (!key.empty()) return key;
    key = HeaderCookie(live, SlotPaperCookie(0));
    if (!key.empty()) return key;
    return HeaderCookie(live, "__Secure-3PAPISID");
}

std::vector<int> AccountsInHeader(const CookieJar& jar) {
    const std::string live = CookieRequestHeader(jar);
    std::vector<int> slots;
    for (int slot = 0; slot < 4; ++slot) {
        if (!SlotIsLogin(live, slot, HeaderCookie(live, SlotSessionCookie(slot)))) {
            continue;
        }
        slots.push_back(slot);
    }
    if (slots.empty() && !CookieSapisid(jar).empty()) {
        slots.push_back(0);
    }
    return slots;
}

std::vector<std::pair<int, std::string>> AccountSlotFingerprints(const CookieJar& jar) {
    const std::string live = CookieRequestHeader(jar);
    std::vector<std::pair<int, std::string>> fingerprints;
    for (int slot = 0; slot < 4; ++slot) {
        std::string value = HeaderCookie(live, SlotSessionCookie(slot));
        if (!SlotIsLogin(live, slot, value)) {
            continue;
        }
        fingerprints.emplace_back(slot, std::move(value));
    }
    return fingerprints;
}

std::string CookieNamesSummary(const CookieJar& jar) {
    const std::string live = CookieRequestHeader(jar);
    const std::vector<CookiePair> pairs = SplitPairs(live);
    static const char* const kAuthNames[] = {
        "SID",              "HSID",             "SSID",             "APISID",
        "SAPISID",          "__Secure-1PSID",   "__Secure-2PSID",   "__Secure-3PSID",
        "__Secure-4PSID",   "__Secure-1PSIDTS", "__Secure-2PSIDTS", "__Secure-3PSIDTS",
        "__Secure-4PSIDTS", "__Secure-1PAPISID","__Secure-2PAPISID","__Secure-3PAPISID",
        "__Secure-4PAPISID","__Secure-1PSIDCC", "__Secure-2PSIDCC", "__Secure-3PSIDCC",
        "__Secure-4PSIDCC",
    };
    std::string names;
    for (const char* name : kAuthNames) {
        for (const CookiePair& pair : pairs) {
            if (pair.name == name) {
                if (!names.empty()) names += ' ';
                names += name;
                break;
            }
        }
    }
    std::string slots;
    for (int slot : AccountsInHeader(jar)) {
        if (!slots.empty()) slots += ',';
        slots += std::to_string(slot);
    }
    std::string out = "[";
    out += names.empty() ? "no auth cookies" : names;
    out += "; slot ";
    out += slots.empty() ? "none" : slots;
    out += "]";
    return out;
}

std::string MergeCookieHeaders(const std::string& existing, const std::string& incoming) {
    std::vector<CookiePair> merged = SplitPairs(existing);
    const std::vector<CookiePair> fresh = SplitPairs(incoming);
    for (const CookiePair& pair : fresh) {
        bool replaced = false;
        for (CookiePair& current : merged) {
            if (current.name == pair.name) {
                current.value = pair.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.push_back(pair);
        }
    }
    return JoinPairs(merged);
}

std::string AbsorbSetCookieHeaders(const CookieJar& jar,
                                   const std::string& rawResponseHeaders) {
    if (rawResponseHeaders.empty()) return {};

    RotationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto key = std::make_pair(jar.account, jar.header);
    const auto entry = store.byHeader.find(key);
    std::vector<CookiePair> pairs = SplitPairs(
        entry == store.byHeader.end() ? jar.header : entry->second.live);
    bool changed = false;

    size_t position = 0;
    while (position < rawResponseHeaders.size()) {
        const size_t lineEnd = rawResponseHeaders.find('\n', position);
        const size_t end = lineEnd == std::string::npos ? rawResponseHeaders.size() : lineEnd;
        std::string line = rawResponseHeaders.substr(position, end - position);
        position = lineEnd == std::string::npos ? rawResponseHeaders.size() : lineEnd + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 12) continue;
        if (util::ToLower(line.substr(0, 11)) != "set-cookie:") continue;

        std::string nv = util::Trim(line.substr(11));
        const size_t semicolon = nv.find(';');
        const std::string attributes =
            semicolon == std::string::npos ? std::string() : nv.substr(semicolon);
        if (semicolon != std::string::npos) nv.resize(semicolon);

        const size_t equals = nv.find('=');
        if (equals == std::string::npos || equals == 0) continue;

        const std::string name = util::Trim(nv.substr(0, equals));
        const std::string value = CookieValue(nv.substr(equals + 1));
        if (!IsValidCookieName(name) || value.size() > kMaxCookieValueBytes) continue;

        const std::string loweredAttributes = util::ToLower(attributes);
        bool maxAgeExpired = false;
        size_t attrPos = 0;
        while (attrPos < loweredAttributes.size()) {
            const size_t semi = loweredAttributes.find(';', attrPos);
            const std::string attr = util::Trim(loweredAttributes.substr(
                attrPos, semi == std::string::npos ? std::string::npos : semi - attrPos));
            const size_t eq = attr.find('=');
            if (eq != std::string::npos && util::Trim(attr.substr(0, eq)) == "max-age") {
                const std::string age = util::Trim(attr.substr(eq + 1));
                if (age == "0" || (!age.empty() && age[0] == '-')) {
                    maxAgeExpired = true;
                    break;
                }
            }
            if (semi == std::string::npos) break;
            attrPos = semi + 1;
        }
        const bool deleted = value.empty() || maxAgeExpired;

        bool found = false;
        for (CookiePair& pair : pairs) {
            if (pair.name != name) continue;
            found = true;
            if (deleted) {
                pair.value.clear();
                changed = true;
            } else if (pair.value != value) {
                pair.value = value;
                changed = true;
            }
            break;
        }
        if (!found && !deleted) {
            pairs.push_back({name, value});
            changed = true;
        }
    }

    if (!changed) return {};

    std::vector<CookiePair> kept;
    kept.reserve(pairs.size());
    for (CookiePair& pair : pairs) {
        if (!pair.value.empty()) kept.push_back(std::move(pair));
    }

    std::string header = JoinPairs(kept);
    if (header.size() > kMaxCookieHeaderBytes || !HasSessionCookie(header)) return {};
    if (entry == store.byHeader.end() && store.byHeader.size() >= 32) {
        store.byHeader.erase(store.byHeader.begin());
    }
    store.byHeader[key] = {header, true};
    return header;
}

void NoteInstalledCookieHeader(int account, const std::string& previous,
                               const std::string& current) {
    if (previous == current) return;
    RotationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.byHeader.erase({account, current});
    for (auto& entry : store.byHeader) {
        if (entry.first.first == account && entry.second.live == previous) {
            entry.second = {current, false};
        }
    }
    const auto key = std::make_pair(account, previous);
    if (store.byHeader.size() >= 32 && store.byHeader.find(key) == store.byHeader.end()) {
        store.byHeader.erase(store.byHeader.begin());
    }
    store.byHeader[key] = {current, false};
}

bool TakeRotatedCookieHeader(const std::string& expectedBase, std::string& out, int account) {
    RotationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto entry = store.byHeader.find({account, expectedBase});
    if (entry == store.byHeader.end() || !entry->second.pending) return false;
    out = entry->second.live;
    entry->second.pending = false;
    return true;
}

bool RotatingCookieChanged(const std::string& previous, const std::string& current,
                           int account) {
    const auto changed = [&](const std::string& name) {
        const std::string next = HeaderCookie(current, name);
        return !next.empty() && next != HeaderCookie(previous, name);
    };
    const std::string name = "__Secure-" + std::to_string(account + 1) + "PSIDTS";
    if (changed(name) || changed(std::string(kAccountsCookiePrefix) + name)) return true;
    return account == 0 &&
           (changed("__Secure-3PSIDTS") ||
            changed(std::string(kAccountsCookiePrefix) + "__Secure-3PSIDTS"));
}

std::string RotationUpdates(const std::string& previous, const std::string& current) {
    std::vector<CookiePair> updates;
    for (const CookiePair& pair : SplitPairs(current)) {
        const std::string prefix = kAccountsCookiePrefix;
        const std::string name = pair.name.rfind(prefix, 0) == 0
                                     ? pair.name.substr(prefix.size())
                                     : pair.name;
        const bool shortLived =
            name == "SIDCC" || name == "NID" ||
            (name.rfind("__Secure-", 0) == 0 && name.size() > 6 &&
             (name.compare(name.size() - 6, 6, "PSIDTS") == 0 ||
              name.compare(name.size() - 6, 6, "PSIDCC") == 0));
        if (shortLived && HeaderCookie(previous, pair.name) != pair.value) {
            updates.push_back(pair);
        }
    }
    return JoinPairs(updates);
}

void ClearCookieRotationStore() {
    RotationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.byHeader.clear();
}

}  // namespace gwa
