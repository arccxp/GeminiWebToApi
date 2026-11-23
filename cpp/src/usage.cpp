#include "gemini_web/usage.h"

#include <algorithm>
#include <string>

namespace gwa {
namespace {

constexpr long long kShortWindowMaxMs = 6ll * 60 * 60 * 1000;

}  // namespace

void ClassifyQuotaRows(const std::vector<QuotaRow>& rows, long long nowMs, UsageMeter& meter) {
    meter = UsageMeter{};
    for (const QuotaRow& row : rows) {
        const UsageWindow window{true, std::clamp(row.usedPercent, 0, 100), row.resetsAtMs};
        if (row.unit == 1 && (row.resetsAtMs == 0 || row.resetsAtMs <= nowMs + kShortWindowMaxMs)) {
            meter.shortWindow = window;
        } else if (row.unit == 2) {
            meter.weekly = window;
        }
    }
    meter.valid = meter.shortWindow.valid || meter.weekly.valid;
}

std::string FormatUsageSpan(long long milliseconds) {
    if (milliseconds <= 0) {
        return "now";
    }
    const long long minutes = (milliseconds + 59999) / 60000;
    if (minutes < 60) {
        return std::to_string(minutes) + "m";
    }
    const long long hours = minutes / 60;
    const long long remainder = minutes % 60;
    if (hours < 24) {
        return std::to_string(hours) + "h" + std::to_string(remainder) + "m";
    }
    return std::to_string(hours / 24) + "d" + std::to_string(hours % 24) + "h";
}

std::string UsageSummary(const UsageMeter& meter, long long nowMs) {
    const UsageWindow& window = meter.shortWindow;
    if (!window.valid) return {};
    const int left = 100 - std::clamp(window.usedPercent, 0, 100);
    if (window.resetsAtMs > 0 && nowMs >= window.resetsAtMs) {
        return "100%";
    }
    std::string out = std::to_string(left) + "%";
    if (left == 100) {
        return out;
    }
    if (window.resetsAtMs > 0) {
        const long long span = window.resetsAtMs - nowMs;
        out += ' ';
        out += FormatUsageSpan(span);
    }
    return out;
}

}  // namespace gwa
