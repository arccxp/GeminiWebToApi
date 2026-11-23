#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "gemini_web/c_api.h"
#include "gemini_web/client.h"
#include "gemini_web/cookies.h"
#include "gemini_web/jspb.h"
#include "gemini_web/transport.h"
#include "gemini_web/usage.h"
#include "gemini_web/util.h"

namespace {

int g_failed = 0;

#define CHECK(expr)                                                               \
    do {                                                                          \
        if (!(expr)) {                                                            \
            std::cerr << "FAIL at line " << __LINE__ << ": " << #expr << "\n";   \
            ++g_failed;                                                           \
        }                                                                         \
    } while (0)

void TestCookieParsingAndRotation() {
    gwa::ClearCookieRotationStore();
    gwa::CookieJar jar;
    std::string err;

    // Raw cURL wrapper with multi-account cookies + 3P quirk (__Secure-3PSID without 2PSID)
    const std::string raw =
        "-H 'cookie: SID=abc; SAPISID=key0; __Secure-1PSID=p1; __Secure-1PAPISID=kp1; "
        "__Secure-3PSID=p3; __Secure-1PSIDTS=ts_old'";
    CHECK(gwa::ParseCookieHeader(raw, jar, err));
    CHECK(jar.sapisid == "key0");
    CHECK(gwa::CookieHashKey(jar, 0) == "key0");

    const auto slots = gwa::AccountsInHeader(jar);
    CHECK(slots.size() == 1 && slots[0] == 0);

    // Absorb Set-Cookie rotation and deletion
    const std::string respHeaders =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: __Secure-1PSIDTS=ts_new; Path=/; Domain=.google.com; Secure\r\n"
        "Set-Cookie: TEMP=gone; Max-Age=0; Path=/\r\n";
    const std::string updated = gwa::AbsorbSetCookieHeaders(jar, respHeaders);
    CHECK(updated.find("__Secure-1PSIDTS=ts_new") != std::string::npos);
    CHECK(gwa::RotatingCookieChanged(jar.header, updated, 0));

    std::string popped;
    CHECK(gwa::TakeRotatedCookieHeader(jar.header, popped, 0));
    CHECK(popped == updated);
    CHECK(!gwa::TakeRotatedCookieHeader(jar.header, popped, 0));

    // Multi-account slot 1 signing secret
    gwa::CookieJar multi;
    CHECK(gwa::ParseCookieHeader(
        "__Secure-1PSID=a1; __Secure-1PAPISID=k1; __Secure-2PSID=a2; __Secure-2PAPISID=k2",
        multi, err));
    const auto multiSlots = gwa::AccountsInHeader(multi);
    CHECK(multiSlots.size() == 2 && multiSlots[0] == 0 && multiSlots[1] == 1);
    CHECK(gwa::CookieHashKey(multi, 1) == "k2");
    CHECK(gwa::CookieHashKey(multi, 2).empty());
}

void TestModelNamingAndMatching() {
    CHECK(gwa::ModelSlug("3.6 Flash") == "3-6-flash");
    CHECK(gwa::CanonicalModelName("3.6 Flash", 1) == "gemini-3.6-flash");
    CHECK(gwa::CanonicalModelName("Gemini3.6 Flash", 1) == "gemini-3.6-flash");
    CHECK(gwa::CanonicalModelName("", 8) == "gemini-8");

    CHECK(gwa::NameAnswersTo("gemini-3.6-flash", "abc123", "3.6 Flash", "Fast"));
    CHECK(gwa::NameAnswersTo("3.6 flash", "abc123", "3.6 Flash", "Fast"));
    CHECK(gwa::NameAnswersTo("abc123", "abc123", "3.6 Flash", "Fast"));
    CHECK(!gwa::NameAnswersTo("4 argon", "abc123", "3.6 Flash", "Fast"));

    CHECK(gwa::IsUltraOnlyModel("x", "4 Carbon", "gemini-4-carbon"));
    CHECK(!gwa::ModelAllowedOnPlan("x", "4 Carbon", "gemini-4-carbon",
                                   static_cast<int>(gwa::AccountTier::Pro)));
    CHECK(gwa::ModelAllowedOnPlan("x", "4 Carbon", "gemini-4-carbon",
                                  static_cast<int>(gwa::AccountTier::Ultra)));
}

void TestCitationsAndUsage() {
    const std::string raw = "Answer [cite: 1, 2] here [cite_start]```py?code_reference\nprint(1)\n```?code_stdout";
    CHECK(gwa::StripCitations(raw) == "Answer  here ```py\nprint(1)\n```");

    gwa::UsageMeter meter;
    const long long nowMs = 1700000000000ll;
    std::vector<gwa::QuotaRow> rows = {
        {1, 12, nowMs + 2ll * 3600 * 1000 + 9ll * 60 * 1000},
        {2, 40, nowMs + 4ll * 86400 * 1000},
    };
    gwa::ClassifyQuotaRows(rows, nowMs, meter);
    CHECK(meter.valid);
    CHECK(meter.shortWindow.valid && meter.shortWindow.usedPercent == 12);
    CHECK(meter.weekly.valid && meter.weekly.usedPercent == 40);
    CHECK(gwa::UsageSummary(meter, nowMs) == "88% 2h9m");
}

void TestSha1AndFormEncoding() {
    // Known SHA-1 test vector for "abc"
    CHECK(gwa::util::Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const std::string form = gwa::BuildStreamGenerateForm("[\"hello\"]");
    CHECK(form.rfind("f.req=", 0) == 0);
}

void TestEndToEndMockProtocolAndCApi() {
    gwa::ResetSession();
    gwa::ClearCookieRotationStore();

    int shellHits = 0;
    int registryHits = 0;
    int generateHits = 0;
    int usageHits = 0;
    std::string lastGenerateHeaders;

    gwa::http::SetMockTransport([&](const gwa::http::Endpoint& ep, const std::string& method,
                                    const std::string& headers, const std::string& /*body*/,
                                    int /*timeoutMs*/, const gwa::http::ChunkSink* sink) {
        gwa::http::HttpResponse resp;
        resp.status = 200;

        if (method == "GET" && ep.path == "/app") {
            ++shellHits;
            resp.headers = "Set-Cookie: __Secure-1PSIDTS=rotated_ts; Domain=.google.com\r\n";
            resp.body =
                "<html><script>window.WIZ_global_data = {"
                "\"SNlM0e\":\"at_token_123\","
                "\"cfb2h\":\"boq_assistant-bard-web-server_20261120.01_p0\","
                "\"FdrFJe\":\"-123456789\","
                "\"TuX5cc\":\"en\","
                "\"qKIAYe\":\"feeds/testpush\","
                "\"oPEP7c\":\"dev@example.com\","
                "\"tier\":\"gemini_advanced\"};</script></html>";
            return resp;
        }

        if (ep.path.find("rpcids=otAQ7b") != std::string::npos) {
            ++registryHits;
            // Build otAQ7b payload: root[15] has 2 models, root[16]=[8] (Pro), root[17]=[19]
            const std::string inner =
                "[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,"
                "[[\"id_flash\",\"Fast\",null,null,null,null,null,null,null,1,null,\"3.6 Flash\",null,null,null,null,null,1],"
                "[\"id_pro\",\"Pro\",null,null,null,null,null,null,null,3,null,\"3.1 Pro\",null,null,null,null,null,3]],"
                "[8],[19]]";
            resp.body = ")]}'\n\n99\n[[\"wrb.fr\",\"otAQ7b\"," + gwa::JsonQuote(inner) + "]]\n";
            return resp;
        }

        if (ep.path.find("StreamGenerate") != std::string::npos) {
            ++generateHits;
            lastGenerateHeaders = headers;
            // Stream two incremental chunks with thinking + thread IDs + served model metadata
            const std::string frame1Inner =
                "[null,[\"c_conv1\",\"r_resp1\"],null,null,"
                "[[\"rc_cand1\",[\"Hello \"],null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,[\"step 1\"]]]]";
            const std::string frame2Inner =
                "[null,[\"c_conv1\",\"r_resp1\"],null,null,"
                "[[\"rc_cand1\",[\"Hello world [cite: 1]\"],null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,[\"step 1 -> done\"]]]"
                ",null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,"
                "\"3.6 Flash\",null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,1]";
            const std::string chunk1 =
                ")]}\"'\n10\n[[\"wrb.fr\",null," + gwa::JsonQuote(frame1Inner) + "]]\n";
            const std::string chunk2 =
                "10\n[[\"wrb.fr\",null," + gwa::JsonQuote(frame2Inner) + "]]\n";
            resp.body = chunk1 + chunk2;
            if (sink != nullptr) {
                (*sink)(chunk1.data(), chunk1.size());
                (*sink)(chunk2.data(), chunk2.size());
            }
            return resp;
        }

        if (ep.path.find("rpcids=qpEbW") != std::string::npos) {
            ++usageHits;
            const long long resetSec = static_cast<long long>(std::time(nullptr)) + 7200;
            const std::string inner =
                "[[[[null,2],1,25,[" + std::to_string(resetSec) + ",0]],"
                "[[null,2],2,50,[" + std::to_string(resetSec + 86400) + ",0]]]]";
            resp.body = ")]}'\n\n50\n[[\"wrb.fr\",\"qpEbW\"," + gwa::JsonQuote(inner) + "]]\n";
            return resp;
        }

        resp.status = 404;
        return resp;
    });

    gwa::Client client;
    std::string err;
    CHECK(client.Init("__Secure-1PSID=ps1; SAPISID=sap1; __Secure-1PSIDTS=old_ts", 0, err));

    std::vector<gwa::WebModel> models;
    gwa::AccountTier tier = gwa::AccountTier::Unknown;
    std::string email;
    CHECK(client.ListModels(models, err, &tier, &email));
    CHECK(tier == gwa::AccountTier::Pro);
    CHECK(email == "dev@example.com");
    CHECK(models.size() == 2);
    CHECK(models[0].name == "gemini-3.6-flash");
    CHECK(client.LiveCookies().find("__Secure-1PSIDTS=rotated_ts") != std::string::npos);

    std::vector<std::string> partials;
    std::string answer;
    gwa::AskDetails details;
    CHECK(client.Ask("Say hi", answer, err, "gemini-3.6-flash", "high", {},
                     [&](const std::string& snap) { partials.push_back(snap); }, true, &details));
    CHECK(answer == "Hello world ");
    CHECK(details.thinking == "step 1 -> done");
    CHECK(details.servedLabel == "3.6 Flash");
    CHECK(client.Thread().Active());
    CHECK(client.Thread().conversation == "c_conv1");
    CHECK(!partials.empty());
    CHECK(lastGenerateHeaders.find("Authorization: SAPISIDHASH ") != std::string::npos);
    CHECK(lastGenerateHeaders.find("x-goog-ext-525001261-jspb: [1,null,null,null,\"id_flash\"") !=
          std::string::npos);

    gwa::UsageMeter meter;
    CHECK(client.GetUsage(meter, err));
    CHECK(meter.shortWindow.valid && meter.shortWindow.usedPercent == 25);
    CHECK(meter.weekly.valid && meter.weekly.usedPercent == 50);

    // Exercise C ABI over the same mock transport
    char errBuf[256] = {};
    gwa_client_t* cClient = gwa_client_create(
        "__Secure-1PSID=ps1; SAPISID=sap1; __Secure-1PSIDTS=old_ts", 0, errBuf, sizeof(errBuf));
    CHECK(cClient != nullptr);
    char* cAnswer = nullptr;
    char* cThinking = nullptr;
    CHECK(gwa_client_ask(cClient, "Ping", "gemini-3.6-flash", "medium", nullptr, 0, 0, nullptr,
                         nullptr, &cAnswer, &cThinking, errBuf, sizeof(errBuf)) == 1);
    CHECK(cAnswer != nullptr && std::string(cAnswer) == "Hello world ");
    CHECK(cThinking != nullptr && std::string(cThinking) == "step 1 -> done");
    gwa_free(cAnswer);
    gwa_free(cThinking);

    char* modelsJson = nullptr;
    CHECK(gwa_client_list_models_json(cClient, &modelsJson, errBuf, sizeof(errBuf)) == 1);
    CHECK(modelsJson != nullptr && std::string(modelsJson).find("gemini-3.6-flash") != std::string::npos);
    gwa_free(modelsJson);

    gwa_client_destroy(cClient);
    gwa::http::SetMockTransport({});

    CHECK(shellHits >= 1 && registryHits >= 1 && generateHits >= 2 && usageHits >= 1);
}

}  // namespace

int main() {
    TestCookieParsingAndRotation();
    TestModelNamingAndMatching();
    TestCitationsAndUsage();
    TestSha1AndFormEncoding();
    TestEndToEndMockProtocolAndCApi();

    if (g_failed != 0) {
        std::cerr << g_failed << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All C++ tests passed.\n";
    return 0;
}
