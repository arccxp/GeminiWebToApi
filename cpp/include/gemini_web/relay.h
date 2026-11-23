#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gemini_web/cookies.h"

namespace gwa::relay {

// One row of the live Gemini mode picker as rendered in the browser.
struct RowInfo {
    std::string id;        // Picker option id (from data-test-id="bard-mode-option-<id>")
    std::string label;     // e.g. "3.5 Flash-Lite"
    std::string category;  // e.g. "Quick answers"
    bool enabled = true;   // False when quota for the current window is exhausted
    std::string note;      // Site reset line, e.g. "Limit resets 10 Nov, 21:50"
};

// Optional browser-relay interface.
// Some Gemini Web model rows require browser-minted processSession tokens and
// refuse direct StreamGenerate HTTP calls. By default the pure-HTTP build stubs
// these out (Available() returns false). Supply a WebView2/Chromium backend or
// register a custom RelayDriver to enable page-driven fallback.
struct RelayDriver {
    std::function<bool(std::string& error)> available;
    std::function<bool()> started;
    std::function<bool(const CookieJar& jar, const std::string& rowId, const std::string& rowName,
                       const std::string& level, const std::string& prompt,
                       const std::vector<std::string>& imagePaths, bool freshChat,
                       std::string& answer, std::string& error, std::string& thread,
                       const std::function<void(const std::string&)>& onPartial,
                       int timeoutMs)> ask;
    std::function<bool(const CookieJar& jar, std::vector<RowInfo>& rows, std::string& error,
                       int timeoutMs)> listModels;
    std::function<bool(const CookieJar& jar, std::string& header, std::string& error,
                       int timeoutMs)> readCookieHeader;
    std::function<void()> shutdown;
};

void SetDriver(RelayDriver driver);

bool Available(std::string& error);
bool Started();

bool Ask(const CookieJar& jar, const std::string& rowId, const std::string& rowName,
         const std::string& level, const std::string& prompt,
         const std::vector<std::string>& imagePaths, bool freshChat, std::string& answer,
         std::string& error, std::string& thread,
         const std::function<void(const std::string&)>& onPartial, int timeoutMs);

bool ListModels(const CookieJar& jar, std::vector<RowInfo>& rows, std::string& error,
                int timeoutMs);

bool ReadCookieHeader(const CookieJar& jar, std::string& header, std::string& error,
                      int timeoutMs = 8000);

void Shutdown();

// Reference DOM automation scripts used by the browser page relay (mode picker,
// thinking level selection, Quill composer fill, response streaming, KaTeX/code extraction).
const char* DriverScriptTemplate();
const char* PickerScript();

}  // namespace gwa::relay
