#include "gemini_web/session.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#include "gemini_web/util.h"

namespace gwa::session {
namespace {

constexpr long long kStaleAfterSec = 45ll * 60;
constexpr long long kRotationIntervalSec = 15ll * 60;
constexpr int kMaxFailures = 3;
constexpr size_t kMaxBlob = 64 * 1024;

std::mutex g_mutex;
std::string g_customDir;

#ifdef _WIN32
std::wstring StorePathW(int profile) {
    std::wstring folder;
    if (!g_customDir.empty()) {
        folder = util::Utf8ToWide(g_customDir);
    } else {
        wchar_t appData[MAX_PATH + 1] = {};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            folder.assign(appData, length);
        } else {
            folder = L".";
        }
        folder += L"\\GeminiWebToApi";
    }
    CreateDirectoryW(folder.c_str(), nullptr);
    return profile <= 0 ? folder + L"\\session.bin"
                        : folder + L"\\session-" + std::to_wstring(profile) + L".bin";
}

bool Protect(const std::string& plain, std::string& out) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&in, L"GeminiWebToApi session", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
    LocalFree(encrypted.pbData);
    return true;
}

bool Unprotect(const std::string& encrypted, std::string& out) {
    if (encrypted.empty()) return false;
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()));
    in.cbData = static_cast<DWORD>(encrypted.size());
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    LocalFree(plain.pbData);
    return true;
}
#else
std::string StorePathPosix(int profile) {
    std::string folder = g_customDir;
    if (folder.empty()) {
        const std::string xdg = util::GetEnv("XDG_CONFIG_HOME");
        if (!xdg.empty()) {
            folder = xdg + "/gemini-web-to-api";
        } else {
            const std::string home = util::GetEnv("HOME");
            folder = home.empty() ? ".gemini-web-to-api" : home + "/.config/gemini-web-to-api";
        }
    }
    ::mkdir(folder.c_str(), 0700);
    return profile <= 0 ? folder + "/session.bin"
                        : folder + "/session-" + std::to_string(profile) + ".bin";
}
#endif

std::string Serialize(const Record& record) {
    std::string out;
    char line[128];
    std::snprintf(line, sizeof line, "v2\n%d\n%lld\n%lld\n%d\n%d\n%zu\n", record.profile,
                  record.lastLogin, record.lastRefresh, record.failures,
                  record.rotationAccount, record.header.size());
    out += line;
    out += record.header;
    return out;
}

bool Deserialize(const std::string& blob, Record& out) {
    size_t cursor = 0;
    auto nextLine = [&blob, &cursor](std::string& line) {
        const size_t end = blob.find('\n', cursor);
        if (end == std::string::npos) return false;
        line.assign(blob, cursor, end - cursor);
        cursor = end + 1;
        return true;
    };

    std::string version;
    std::string profileText;
    std::string loginText;
    std::string refreshText;
    std::string failuresText;
    std::string sizeText;
    if (!nextLine(version) || (version != "v1" && version != "v2")) return false;
    if (!nextLine(profileText) || !nextLine(loginText) || !nextLine(refreshText)) return false;
    if (!nextLine(failuresText)) return false;
    if (version == "v2") {
        std::string account;
        if (!nextLine(account) || (account != "-1" &&
            (account.size() != 1 || account[0] < '0' || account[0] > '3'))) {
            return false;
        }
        out.rotationAccount = account == "-1" ? -1 : account[0] - '0';
    }
    if (!nextLine(sizeText)) return false;

    auto parseLongLong = [](const std::string& text, long long& value) -> bool {
        if (text.empty()) return false;
        char* end = nullptr;
        value = std::strtoll(text.c_str(), &end, 10);
        return end != text.c_str() && *end == '\0';
    };
    long long profileVal = 0;
    long long failuresVal = 0;
    long long size = 0;
    if (!parseLongLong(profileText, profileVal) ||
        !parseLongLong(loginText, out.lastLogin) ||
        !parseLongLong(refreshText, out.lastRefresh) ||
        !parseLongLong(failuresText, failuresVal) ||
        !parseLongLong(sizeText, size)) {
        return false;
    }
    out.profile = static_cast<int>(profileVal);
    out.failures = static_cast<int>(failuresVal);
    if (size <= 0 || size > static_cast<long long>(kMaxBlob)) return false;
    if (static_cast<size_t>(size) != blob.size() - cursor) return false;
    out.header.assign(blob, cursor, static_cast<size_t>(size));
    return true;
}

}  // namespace

long long NowUnix() {
    return static_cast<long long>(std::time(nullptr));
}

void SetStoreDirectory(const std::string& directory) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_customDir = directory;
}

void Load(int profile, Record& out) {
    out = Record{};
    out.profile = profile;

    std::string raw;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#ifdef _WIN32
        HANDLE file = CreateFileW(StorePathW(profile).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
            CloseHandle(file);
            return;
        }
        raw.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        const bool ok = ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()),
                                 &read, nullptr) != 0 &&
                        static_cast<size_t>(read) == raw.size();
        CloseHandle(file);
        if (!ok) return;
#else
        std::vector<uint8_t> bytes;
        if (!util::ReadFileBytes(StorePathPosix(profile), bytes) || bytes.empty()) return;
        raw.assign(bytes.begin(), bytes.end());
#endif
    }

    std::string plain;
#ifdef _WIN32
    if (!Unprotect(raw, plain)) return;
#else
    plain = std::move(raw);
#endif
    Record stored;
    if (!Deserialize(plain, stored)) return;
    if (stored.profile != profile) return;
    out = std::move(stored);
}

bool Save(const Record& record) {
    if (record.profile < 0 || record.header.empty() || record.header.size() > kMaxBlob) {
        return false;
    }
    const std::string plain = Serialize(record);
    std::string blob;
#ifdef _WIN32
    if (!Protect(plain, blob)) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    const std::wstring path = StorePathW(record.profile);
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, blob.data(), static_cast<DWORD>(blob.size()),
                              &written, nullptr) != 0 &&
                    static_cast<size_t>(written) == blob.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (MoveFileExW(temp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    DeleteFileW(temp.c_str());
    return false;
#else
    blob = plain;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string path = StorePathPosix(record.profile);
    const std::string temp = path + ".tmp";
    std::vector<uint8_t> bytes(blob.begin(), blob.end());
    if (!util::WriteFileBytes(temp, bytes)) return false;
    ::chmod(temp.c_str(), 0600);
    if (std::rename(temp.c_str(), path.c_str()) == 0) return true;
    ::unlink(temp.c_str());
    return false;
#endif
}

bool Stale(const Record& record, long long bufferSec) {
    if (record.header.empty()) return true;
    if (record.lastRefresh <= 0) return true;
    const long long age = NowUnix() - record.lastRefresh;
    if (age < 0) return true;
    return age >= kStaleAfterSec - bufferSec;
}

bool RotationDue(const Record& record, int account, long long now) {
    return record.rotationAccount != account || record.lastRefresh <= 0 ||
           now < record.lastRefresh ||
           now - record.lastRefresh >= kRotationIntervalSec;
}

void MarkRefreshed(Record& record, const std::string& header, int account) {
    if (!header.empty()) record.header = header;
    record.lastRefresh = NowUnix();
    record.rotationAccount = account;
    record.failures = 0;
}

void MarkLogin(Record& record, const std::string& header, int account) {
    const long long now = NowUnix();
    record.header = header;
    record.lastLogin = now;
    record.lastRefresh = now;
    record.rotationAccount = account;
    record.failures = 0;
}

void MarkFailure(Record& record) {
    ++record.failures;
}

bool NeedsManualLogin(const Record& record) {
    return record.failures >= kMaxFailures;
}

}  // namespace gwa::session
