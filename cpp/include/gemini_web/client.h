#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gemini_web/cookies.h"
#include "gemini_web/jspb.h"
#include "gemini_web/usage.h"

namespace gwa {

inline constexpr const char* kDefaultGeminiModel = "gemini-auto";

enum class AccountTier : int {
    Unknown = -1,
    Free = 0,
    Pro = 1,
    Ultra = 2,
};

inline const char* TierLabel(AccountTier tier) {
    switch (tier) {
        case AccountTier::Free:
            return "Free";
        case AccountTier::Pro:
            return "Pro";
        case AccountTier::Ultra:
            return "Ultra";
        default:
            return "Unknown";
    }
}

// Optional diagnostic and lifecycle hooks.
using LogCallback = std::function<void(const std::string& line)>;
using StatusCallback = std::function<void(const std::string& status)>;
using CookieUpdateCallback = std::function<void(int account, const std::string& updatedHeader)>;

void SetLogCallback(LogCallback cb);
void SetStatusCallback(StatusCallback cb);
void SetCookieUpdateCallback(CookieUpdateCallback cb);

// Conversation state returned by StreamGenerate for multi-turn continuation.
// All three wire IDs (conversation, response, candidate) are required to continue
// a wire conversation; `page` holds the URL pathname for browser-relayed turns.
struct ChatThread {
    std::string conversation;  // c_...
    std::string response;      // r_...
    std::string candidate;     // rc_...
    std::string page;          // /app/... when served by a browser relay

    bool Active() const {
        return !conversation.empty() && !response.empty() && !candidate.empty();
    }
    void Clear() {
        conversation.clear();
        response.clear();
        candidate.clear();
        page.clear();
    }
    void ClearWire() {
        conversation.clear();
        response.clear();
        candidate.clear();
    }
};

// Called with the full growing snapshot of the answer while StreamGenerate is open.
using PartialSink = std::function<void(const std::string& text)>;

struct AskDetails {
    std::string answer;
    std::string thinking;      // Reasoning stream (payload[4][0][37]), empty if none
    std::string servedLabel;   // Model display label reported by the server (slot 42)
    long long servedNumber = 0;// Routing number reported by the server (slot 58)
    unsigned long long elapsedMs = 0;
};

// Discovers the signed-in account's Gemini models (rpcid otAQ7b).
bool FetchModels(const CookieJar& jar, std::vector<WebModel>& models, std::string& error,
                 bool* usedFallback = nullptr, bool* proAccount = nullptr,
                 std::string* email = nullptr, bool* tierKnown = nullptr,
                 int* tier = nullptr, bool allowPageRelay = true);

bool IsUltraOnlyModel(const std::string& id, const std::string& label,
                      const std::string& name);

bool ModelAllowedOnPlan(const std::string& id, const std::string& label,
                        const std::string& name, int tier);

std::vector<WebModel> FallbackModels();
std::string OutOfQuotaSummary();

// Resumable attachment upload to content-push.googleapis.com/upload.
bool UploadImage(const CookieJar& jar, const std::string& pushId, const std::string& filePath,
                 std::string& fileId, std::string& error);

// Full ask: uploads images, builds the 81-slot JSPB payload, streams StreamGenerate.
bool Ask(const CookieJar& jar, const std::string& modelId, const std::string& prompt,
         const std::string& effort, const std::vector<std::string>& imagePaths,
         ChatThread& chat, std::string& answer, std::string& error,
         const PartialSink& onPartial = {}, bool allowUltraOnly = false,
         AskDetails* details = nullptr);

// Reads 5-hour and weekly compute quota (rpcid qpEbW with /usage jSf9Qc fallback).
bool FetchUsage(const CookieJar& jar, UsageMeter& meter, std::string& error);

// Rotates __Secure-<N>PSIDTS via accounts.google.com/RotateCookies.
bool RotateCookies(const CookieJar& jar, std::string& updatedHeader, std::string& error,
                   bool force = false);

// Refreshes the /app shell and validates that the cookie header is signed in.
bool KeepAlive(const CookieJar& jar, std::string& error);
bool IsHeaderSignedIn(const std::string& header, int account = 0);
bool EnsureSessionFresh(CookieJar& jar, bool force, const char* reason = "refresh");

// Removes web-UI artifacts ([cite: N], [cite_start], ?code_reference, ?code_stdout).
std::string StripCitations(const std::string& text);

// Drops cached /app session tokens so the next request scrapes fresh boot tokens.
void ResetSession();

// True for retryable site errors (1013, 1052, 429, 5xx, canned apology, timeout).
bool IsTransientError(const std::string& error);

// Pre-scrapes session boot tokens and the model table ahead of the first request.
void Prewarm(const CookieJar& jar);
void WarmRefresh(const CookieJar& jar);

// High-level ergonomic C++ client wrapper.
class Client {
public:
    Client() = default;

    bool Init(const std::string& cookieHeader, int accountSlot, std::string& error);

    bool ListModels(std::vector<WebModel>& models, std::string& error,
                    AccountTier* tierOut = nullptr, std::string* emailOut = nullptr);

    bool Ask(const std::string& prompt, std::string& answer, std::string& error,
             const std::string& model = kDefaultGeminiModel,
             const std::string& effort = "medium",
             const std::vector<std::string>& imagePaths = {},
             const PartialSink& onPartial = {},
             bool continueChat = false,
             AskDetails* details = nullptr);

    bool GetUsage(UsageMeter& meter, std::string& error);
    bool Rotate(std::string& error, bool force = false);

    void ResetChat() { chat_.Clear(); }
    const ChatThread& Thread() const { return chat_; }
    ChatThread& Thread() { return chat_; }
    const CookieJar& Jar() const { return jar_; }
    std::string LiveCookies() const { return CookieRequestHeader(jar_); }
    void SetAllowUltraOnly(bool allow) { allowUltraOnly_ = allow; }

private:
    CookieJar jar_;
    ChatThread chat_;
    bool initialized_ = false;
    bool allowUltraOnly_ = false;
};

}  // namespace gwa
