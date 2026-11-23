#pragma once

#include <string>
#include <vector>

namespace gwa {

// RFC-4122 v4 UUID used as the request/session id inside JSPB payloads.
std::string NewUuid();

// JSON literal helpers.
std::string JsonQuote(const std::string& value);
std::string JsonNumber(long long value);
std::string JsonNull();

// Builds `f.req` (outer [null, "<inner>"] array, URL-encoded) for StreamGenerate.
std::string BuildStreamGenerateForm(const std::string& innerPayload);

// Model entry returned by discovery (rpcid otAQ7b).
struct WebModel {
    std::string id;        // Opaque picker id or bare routing number ("1", "6")
    std::string label;     // Site display label, e.g. "3.6 Flash"
    std::string name;      // Canonical name, e.g. "gemini-3.6-flash"
    bool available = true; // False when out of quota in the live picker
    std::string note;      // Reset message if out of quota
};

// Loose token-based fallback parser for otAQ7b responses.
bool ParseModelList(const std::string& body, std::vector<WebModel>& models);

// "3.6 Flash" -> "gemini-3.6-flash", falling back to "gemini-<number>".
std::string CanonicalModelName(const std::string& label, long long number);

// Canonical name derived from display label or category.
std::string ProductModelName(const std::string& label, const std::string& category,
                             long long number);

// Lowercase alphanumerics with single-dash runs.
std::string ModelSlug(const std::string& value);

// True when `requested` matches a model row in any form (exact id/label/category,
// slug, bare slug without "gemini-" prefix).
bool NameAnswersTo(const std::string& requested, const std::string& id,
                   const std::string& label, const std::string& category);

}  // namespace gwa
