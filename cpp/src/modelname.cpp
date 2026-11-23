#include "gemini_web/jspb.h"

#include <cctype>
#include <string>

#include "gemini_web/util.h"

namespace gwa {
namespace {

std::string Slug(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) != 0) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

}  // namespace

std::string ModelSlug(const std::string& value) { return Slug(value); }

bool NameAnswersTo(const std::string& requested, const std::string& id,
                   const std::string& label, const std::string& category) {
    const std::string want = util::ToLower(util::Trim(requested));
    if (want.empty()) {
        return false;
    }
    if (util::ToLower(id) == want || util::ToLower(label) == want ||
        util::ToLower(category) == want) {
        return true;
    }

    const std::string wantSlug = Slug(requested);
    if (wantSlug.empty()) {
        return false;
    }
    const std::string wantBare =
        wantSlug.rfind("gemini-", 0) == 0 ? wantSlug.substr(7) : wantSlug;

    const std::string labelSlug = Slug(label);
    const std::string categorySlug = Slug(category);
    const std::string idSlug = Slug(id);
    return labelSlug == wantSlug || categorySlug == wantSlug || idSlug == wantSlug ||
           labelSlug == wantBare || categorySlug == wantBare || idSlug == wantBare;
}

std::string CanonicalModelName(const std::string& label, long long number) {
    std::string out;
    bool pending = false;
    for (unsigned char c : label) {
        if (std::isalnum(c) != 0) {
            if (pending && !out.empty() && out.back() != '-' && out.back() != '.') {
                out.push_back('-');
            }
            pending = false;
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == '.') {
            if (!out.empty() && out.back() != '-' && out.back() != '.') {
                out.push_back('.');
            }
        } else {
            pending = true;
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        return number > 0 ? "gemini-" + std::to_string(number) : std::string();
    }
    if (out.rfind("gemini", 0) != 0) {
        return "gemini-" + out;
    }
    if (out.rfind("gemini-", 0) != 0) {
        return "gemini-" + out.substr(6);
    }
    return out;
}

std::string ProductModelName(const std::string& label, const std::string& category,
                             long long number) {
    return CanonicalModelName(label.empty() ? category : label, number);
}

}  // namespace gwa
