#pragma once

#include <string>

namespace gwa::session {

// Persistent Gemini Web session metadata and cookie backup.
// On Windows, the serialized record is protected with DPAPI (CryptProtectData).
// On POSIX systems, it is stored with mode 0600 in the user's config directory.
struct Record {
    std::string header;
    long long lastLogin = 0;
    long long lastRefresh = 0;
    int failures = 0;
    int profile = 0;
    int rotationAccount = -1;
};

long long NowUnix();

// Optional override for the directory holding session files (empty = platform default).
void SetStoreDirectory(const std::string& directory);

void Load(int profile, Record& out);
bool Save(const Record& record);
bool RotationDue(const Record& record, int account, long long now);
bool Stale(const Record& record, long long bufferSec);

void MarkRefreshed(Record& record, const std::string& header, int account = 0);
void MarkLogin(Record& record, const std::string& header, int account = 0);
void MarkFailure(Record& record);
bool NeedsManualLogin(const Record& record);

}  // namespace gwa::session
