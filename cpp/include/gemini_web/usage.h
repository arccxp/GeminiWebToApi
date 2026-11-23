#pragma once

#include <string>
#include <vector>

namespace gwa {

// One metered window from CheckGeminiQuota (rpcid qpEbW) or /usage (jSf9Qc).
struct UsageWindow {
    bool valid = false;
    int usedPercent = 0;       // 0..100, clamped
    long long resetsAtMs = 0;  // unix epoch milliseconds, 0 when unknown
};

struct UsageMeter {
    bool valid = false;
    UsageWindow shortWindow;  // rolling 5-hour window
    UsageWindow weekly;       // weekly window
};

struct QuotaRow {
    long long unit = 0;
    int usedPercent = 0;
    long long resetsAtMs = 0;
};

void ClassifyQuotaRows(const std::vector<QuotaRow>& rows, long long nowMs, UsageMeter& meter);

// Formats a remaining span as "42m", "2h5m", or "3d4h".
std::string FormatUsageSpan(long long milliseconds);

// Short-window summary such as "88% 2h9m". Empty when shortWindow is invalid.
std::string UsageSummary(const UsageMeter& meter, long long nowMs);

}  // namespace gwa
