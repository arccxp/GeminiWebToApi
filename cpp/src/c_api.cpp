#include "gemini_web/c_api.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "gemini_web/client.h"
#include "gemini_web/util.h"

struct gwa_client {
    gwa::Client impl;
};

namespace {

void WriteErr(char* buf, size_t len, const std::string& msg) {
    if (buf == nullptr || len == 0) return;
    const size_t n = msg.size() < len - 1 ? msg.size() : len - 1;
    if (n > 0) std::memcpy(buf, msg.data(), n);
    buf[n] = '\0';
}

char* DupString(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) return nullptr;
    if (!s.empty()) std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

}  // namespace

extern "C" {

GWA_API gwa_client_t* gwa_client_create(const char* cookie_header, int account_slot,
                                        char* err_buf, size_t err_buf_len) {
    if (cookie_header == nullptr) {
        WriteErr(err_buf, err_buf_len, "cookie_header is null");
        return nullptr;
    }
    auto* handle = new gwa_client();
    std::string err;
    if (!handle->impl.Init(cookie_header, account_slot, err)) {
        WriteErr(err_buf, err_buf_len, err);
        delete handle;
        return nullptr;
    }
    return handle;
}

GWA_API void gwa_client_destroy(gwa_client_t* client) { delete client; }

GWA_API int gwa_client_ask(gwa_client_t* client, const char* prompt, const char* model,
                           const char* effort, const char* const* image_paths,
                           size_t image_count, int continue_chat, gwa_partial_cb on_partial,
                           void* userdata, char** out_answer, char** out_thinking,
                           char* err_buf, size_t err_buf_len) {
    if (out_answer != nullptr) *out_answer = nullptr;
    if (out_thinking != nullptr) *out_thinking = nullptr;
    if (client == nullptr || prompt == nullptr) {
        WriteErr(err_buf, err_buf_len, "invalid arguments");
        return 0;
    }

    std::vector<std::string> images;
    if (image_paths != nullptr && image_count > 0) {
        images.reserve(image_count);
        for (size_t i = 0; i < image_count; ++i) {
            if (image_paths[i] != nullptr) images.emplace_back(image_paths[i]);
        }
    }

    gwa::PartialSink sink;
    if (on_partial != nullptr) {
        sink = [on_partial, userdata](const std::string& text) {
            on_partial(text.c_str(), text.size(), userdata);
        };
    }

    std::string answer;
    std::string err;
    gwa::AskDetails details;
    const bool ok = client->impl.Ask(
        prompt, answer, err,
        (model != nullptr && *model != '\0') ? std::string(model)
                                             : std::string(gwa::kDefaultGeminiModel),
        (effort != nullptr && *effort != '\0') ? std::string(effort) : std::string("medium"),
        images, sink, continue_chat != 0, &details);
    if (!ok) {
        WriteErr(err_buf, err_buf_len, err);
        return 0;
    }
    if (out_answer != nullptr) *out_answer = DupString(answer);
    if (out_thinking != nullptr) *out_thinking = DupString(details.thinking);
    return 1;
}

GWA_API int gwa_client_list_models_json(gwa_client_t* client, char** out_json,
                                        char* err_buf, size_t err_buf_len) {
    if (out_json != nullptr) *out_json = nullptr;
    if (client == nullptr || out_json == nullptr) {
        WriteErr(err_buf, err_buf_len, "invalid arguments");
        return 0;
    }
    std::vector<gwa::WebModel> models;
    std::string err;
    gwa::AccountTier tier = gwa::AccountTier::Unknown;
    std::string email;
    if (!client->impl.ListModels(models, err, &tier, &email)) {
        WriteErr(err_buf, err_buf_len, err);
        return 0;
    }
    std::string json = "{\"tier\":\"" + std::string(gwa::TierLabel(tier)) + "\",\"email\":\"" +
                       gwa::util::JsonEscape(email) + "\",\"models\":[";
    for (size_t i = 0; i < models.size(); ++i) {
        if (i != 0) json += ',';
        json += "{\"id\":\"" + gwa::util::JsonEscape(models[i].id) + "\",\"label\":\"" +
                gwa::util::JsonEscape(models[i].label) + "\",\"name\":\"" +
                gwa::util::JsonEscape(models[i].name) + "\",\"available\":" +
                (models[i].available ? "true" : "false") + ",\"note\":\"" +
                gwa::util::JsonEscape(models[i].note) + "\"}";
    }
    json += "]}";
    *out_json = DupString(json);
    return 1;
}

GWA_API int gwa_client_usage_json(gwa_client_t* client, char** out_json,
                                  char* err_buf, size_t err_buf_len) {
    if (out_json != nullptr) *out_json = nullptr;
    if (client == nullptr || out_json == nullptr) {
        WriteErr(err_buf, err_buf_len, "invalid arguments");
        return 0;
    }
    gwa::UsageMeter meter;
    std::string err;
    if (!client->impl.GetUsage(meter, err)) {
        WriteErr(err_buf, err_buf_len, err);
        return 0;
    }
    const long long nowMs = static_cast<long long>(std::time(nullptr)) * 1000;
    std::string json = "{";
    json += "\"short_window\":{\"valid\":" + std::string(meter.shortWindow.valid ? "true" : "false") +
            ",\"used_percent\":" + std::to_string(meter.shortWindow.usedPercent) +
            ",\"resets_at_ms\":" + std::to_string(meter.shortWindow.resetsAtMs) + "},";
    json += "\"weekly\":{\"valid\":" + std::string(meter.weekly.valid ? "true" : "false") +
            ",\"used_percent\":" + std::to_string(meter.weekly.usedPercent) +
            ",\"resets_at_ms\":" + std::to_string(meter.weekly.resetsAtMs) + "},";
    json += "\"summary\":\"" + gwa::util::JsonEscape(gwa::UsageSummary(meter, nowMs)) + "\"}";
    *out_json = DupString(json);
    return 1;
}

GWA_API int gwa_client_rotate_cookies(gwa_client_t* client, int force,
                                      char* err_buf, size_t err_buf_len) {
    if (client == nullptr) {
        WriteErr(err_buf, err_buf_len, "invalid arguments");
        return 0;
    }
    std::string err;
    if (!client->impl.Rotate(err, force != 0)) {
        WriteErr(err_buf, err_buf_len, err);
        return 0;
    }
    return 1;
}

GWA_API char* gwa_client_live_cookies(gwa_client_t* client) {
    if (client == nullptr) return nullptr;
    return DupString(client->impl.LiveCookies());
}

GWA_API void gwa_client_reset_chat(gwa_client_t* client) {
    if (client != nullptr) client->impl.ResetChat();
}

GWA_API void gwa_free(void* ptr) { std::free(ptr); }

}  // extern "C"
