#pragma once

#include <string>
#include <utility>
#include <vector>

namespace gwa {

// Internal prefix used to retain host-scoped accounts.google.com cookies
// inside a single serialized cookie string without colliding with .google.com.
inline constexpr char kAccountsCookiePrefix[] = "GwaAccounts.";

struct CookieJar {
    std::string header;   // "SID=...; HSID=...; ..." ready for the Cookie header
    std::string sapisid;  // value used for SAPISIDHASH
    std::string email;    // best effort; empty when unknown
    int count = 0;

    // 0-based Google account slot (X-Goog-AuthUser).
    // __Secure-1PSID is slot 0, __Secure-2PSID is slot 1, etc.
    int account = 0;
};

// Secret SAPISIDHASH is built from for a given slot:
// SAPISID (or __Secure-1PAPISID / __Secure-3PAPISID) for slot 0,
// __Secure-<slot+1>PAPISID for slots 1..3.
std::string CookieHashKey(const CookieJar& jar, int account);

// Which Google account slots this header carries, 0-based, ascending.
std::vector<int> AccountsInHeader(const CookieJar& jar);

// Per-slot session cookie value (__Secure-<slot+1>PSID) for each slot present.
std::vector<std::pair<int, std::string>> AccountSlotFingerprints(const CookieJar& jar);

// Auth cookie names and detected slots without leaking values.
std::string CookieNamesSummary(const CookieJar& jar);

// Merges an incoming Cookie header into an existing one (incoming wins by name).
std::string MergeCookieHeaders(const std::string& existing, const std::string& incoming);

// Parses a pasted Cookie header, "Cookie: ..." line, or cURL `-H 'cookie: ...'`.
bool ParseCookieHeader(const std::string& text, CookieJar& jar, std::string& error);

// Current live Cookie header with all absorbed Set-Cookie rotations layered on top.
std::string CookieRequestHeader(const CookieJar& jar);

// Current live SAPISID value.
std::string CookieSapisid(const CookieJar& jar);

// Folds Set-Cookie lines from a raw HTTP response header block into the live jar.
std::string AbsorbSetCookieHeaders(const CookieJar& jar,
                                   const std::string& rawResponseHeaders);

void NoteInstalledCookieHeader(int account, const std::string& previous,
                               const std::string& current);
bool RotatingCookieChanged(const std::string& previous, const std::string& current,
                           int account);
std::string RotationUpdates(const std::string& previous, const std::string& current);

// Pops the rotated Cookie header once if it changed since the last call.
bool TakeRotatedCookieHeader(const std::string& expectedBase, std::string& out,
                             int account = 0);

// Clears process-wide rotation cache (useful in tests).
void ClearCookieRotationStore();

}  // namespace gwa
