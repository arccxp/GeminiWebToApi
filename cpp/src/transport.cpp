#include "gemini_web/transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gemini_web/util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace gwa::http {
namespace {

constexpr size_t kMaxResponseBytes = 8u * 1024u * 1024u;
constexpr int kSessionTimeoutMs = 120000;
constexpr int kStreamFirstByteMs = 30000;
constexpr int kStreamChunkStallMs = 60000;

std::mutex g_mockMutex;
MockTransportFn g_mockTransport;

bool HasPrefixNoCase(const std::string& value, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (value.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(value[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        const unsigned char la =
            (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a - 'A' + 'a') : a;
        const unsigned char lb =
            (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b - 'A' + 'a') : b;
        if (la != lb) return false;
    }
    return true;
}

#ifdef _WIN32

constexpr DWORD kReadChunk = 64u * 1024u;
constexpr DWORD kMaxHeaderBytes = 64u * 1024u;
constexpr const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/120.0.0.0 Safari/537.36";

#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x3
#endif

struct SharedHttp {
    std::mutex mutex;
    HINTERNET session = nullptr;
    std::vector<std::pair<std::wstring, HINTERNET>> connects;
};

SharedHttp& Http() {
    static SharedHttp shared;
    return shared;
}

std::atomic<int> g_inFlight{0};

struct InFlightGuard {
    InFlightGuard() {
        std::lock_guard<std::mutex> lock(Http().mutex);
        g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~InFlightGuard() { g_inFlight.fetch_sub(1, std::memory_order_acq_rel); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
};

HINTERNET SessionLocked(SharedHttp& shared) {
    if (shared.session == nullptr) {
        shared.session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (shared.session != nullptr) {
            ::WinHttpSetTimeouts(shared.session, kSessionTimeoutMs, kSessionTimeoutMs,
                                 kSessionTimeoutMs, kSessionTimeoutMs);
            DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
            ::WinHttpSetOption(shared.session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols,
                               sizeof(protocols));
            DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
            ::WinHttpSetOption(shared.session, WINHTTP_OPTION_DECOMPRESSION, &decompression,
                               sizeof(decompression));
            DWORD maxConns = 16;
            ::WinHttpSetOption(shared.session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxConns,
                               sizeof(maxConns));
            ::WinHttpSetOption(shared.session, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &maxConns,
                               sizeof(maxConns));
        }
    }
    return shared.session;
}

std::wstring ConnectionKey(const std::wstring& host, INTERNET_PORT port) {
    return host + L":" + std::to_wstring(static_cast<unsigned>(port));
}

HINTERNET Connection(const std::wstring& host, INTERNET_PORT port) {
    SharedHttp& shared = Http();
    std::lock_guard<std::mutex> lock(shared.mutex);
    HINTERNET session = SessionLocked(shared);
    if (session == nullptr) return nullptr;

    const std::wstring key = ConnectionKey(host, port);
    for (const auto& entry : shared.connects) {
        if (entry.first == key) return entry.second;
    }

    HINTERNET connect = ::WinHttpConnect(session, host.c_str(), port, 0);
    if (connect != nullptr) {
        shared.connects.emplace_back(key, connect);
    }
    return connect;
}

void ResetConnections() {
    SharedHttp& shared = Http();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (g_inFlight.load(std::memory_order_acquire) > 1) return;
    for (auto& entry : shared.connects) {
        ::WinHttpCloseHandle(entry.second);
    }
    shared.connects.clear();
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HINTERNET handle) : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { Close(); }

    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    HINTERNET Release() {
        HINTERNET handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void Close() {
        if (handle_ != nullptr) {
            ::WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HINTERNET handle_ = nullptr;
};

class Abortable {
public:
    bool Attach(HINTERNET handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        handle_ = handle;
        return true;
    }

    bool Detach() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ == nullptr) return false;
        handle_ = nullptr;
        return true;
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        if (handle_ != nullptr) {
            ::WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    std::mutex mutex_;
    HINTERNET handle_ = nullptr;
    bool closed_ = false;
};

// Whole-call and inter-chunk stall watchdog for blocking WinHTTP transfers.
class StallWatchdog {
public:
    StallWatchdog() = default;
    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;
    ~StallWatchdog() { Stop(); }

    void Start(Abortable* abortable, int initialMs, int chunkMs) {
        Stop();
        abortable_ = abortable;
        stop_.store(false);
        fired_.store(false);
        const int initial = initialMs > 0 ? initialMs : 1;
        chunkMs_ = chunkMs > 0 ? chunkMs : 0;
        deadline_.store(::GetTickCount64() + static_cast<ULONGLONG>(initial),
                        std::memory_order_release);
        thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stop_.load(std::memory_order_acquire)) return;
                const ULONGLONG now = ::GetTickCount64();
                const ULONGLONG deadline = deadline_.load(std::memory_order_acquire);
                if (now >= deadline) break;
                const ULONGLONG wait = deadline - now;
                if (cv_.wait_for(lock, std::chrono::milliseconds(wait),
                                 [this]() { return stop_.load(); })) {
                    return;
                }
            }
            lock.unlock();
            fired_.store(true);
            if (abortable_ != nullptr) {
                abortable_->Abort();
            }
        });
    }

    void Bump() {
        if (chunkMs_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const ULONGLONG next = ::GetTickCount64() + static_cast<ULONGLONG>(chunkMs_);
        ULONGLONG current = deadline_.load(std::memory_order_acquire);
        while (next > current &&
               !deadline_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        }
    }

    void Stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_.store(true);
        }
        cv_.notify_all();
        thread_.join();
    }

    bool Fired() const { return fired_.load(); }

private:
    Abortable* abortable_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> fired_{false};
    std::atomic<ULONGLONG> deadline_{0};
    int chunkMs_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

HttpResponse RequestFailure(DWORD err) {
    HttpResponse response;
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "Request failed (err %lu).",
                  static_cast<unsigned long>(err));
    response.networkError = true;
    response.detail = buffer;
    response.lastError = static_cast<unsigned long>(err);
    return response;
}

HttpResponse StalledResponse(const HttpResponse& partial, const Endpoint& endpoint, int budgetMs) {
    HttpResponse response = partial;
    response.networkError = true;
    response.detail = "Stalled, no progress for " + std::to_string(budgetMs / 1000) + "s (" +
                      endpoint.host + ", " + std::to_string(response.body.size()) +
                      " bytes received)";
    return response;
}

bool IsConnectionFailure(DWORD err) {
    return err == ERROR_WINHTTP_CONNECTION_ERROR || err == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
           err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_TIMEOUT ||
           err == ERROR_WINHTTP_SECURE_FAILURE;
}

bool IsSafeMethod(const std::string& method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

HttpResponse RequestWinHttp(const Endpoint& endpoint, const std::string& method,
                            const std::string& headers, const std::string& body, int timeoutMs,
                            const ChunkSink* sink) {
    HttpResponse response;
    InFlightGuard inFlight;

    if (endpoint.host.empty()) {
        response.networkError = true;
        response.detail = "Connect failed: empty host";
        return response;
    }

    const std::wstring wideHost = util::Utf8ToWide(endpoint.host);
    const std::wstring widePath =
        util::Utf8ToWide(endpoint.path.empty() ? std::string("/") : endpoint.path);
    const std::wstring wideHeaders = util::Utf8ToWide(headers);
    const wchar_t* requestHeaders =
        wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str();
    const DWORD flags = endpoint.https ? WINHTTP_FLAG_SECURE : 0;
    const std::wstring wideMethod = util::Utf8ToWide(method.empty() ? std::string("POST") : method);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const HINTERNET connect =
            Connection(wideHost, static_cast<INTERNET_PORT>(endpoint.port));
        if (connect == nullptr) {
            if (attempt == 0) {
                ResetConnections();
                continue;
            }
            response.networkError = true;
            response.detail = "Connect failed: " + endpoint.host;
            response.lastError = static_cast<unsigned long>(::GetLastError());
            return response;
        }

        UniqueHandle request(::WinHttpOpenRequest(connect, wideMethod.c_str(), widePath.c_str(),
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) {
            if (attempt == 0) {
                ResetConnections();
                continue;
            }
            response.networkError = true;
            response.detail = "OpenRequest failed.";
            response.lastError = static_cast<unsigned long>(::GetLastError());
            return response;
        }

        const int clampedTimeout = (timeoutMs > 0) ? timeoutMs : 1;
        ::WinHttpSetTimeouts(request.get(), clampedTimeout, clampedTimeout, clampedTimeout,
                             clampedTimeout);

        if (sink != nullptr) {
            DWORD features = WINHTTP_DISABLE_KEEP_ALIVE;
            ::WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features,
                               sizeof(features));
        }

        Abortable abortable;
        abortable.Attach(request.get());
        StallWatchdog watchdog;
        if (sink != nullptr) {
            const int firstByte =
                clampedTimeout < kStreamFirstByteMs ? clampedTimeout : kStreamFirstByteMs;
            watchdog.Start(&abortable, firstByte, kStreamChunkStallMs);
        } else {
            watchdog.Start(&abortable, clampedTimeout, 0);
        }

        struct HandleGuard {
            UniqueHandle& handle;
            Abortable& abortable;
            StallWatchdog& watchdog;
            ~HandleGuard() {
                watchdog.Stop();
                if (!abortable.Detach()) {
                    handle.Release();
                }
            }
        } handleGuard{request, abortable, watchdog};

        auto setNetworkError = [&response](DWORD error) {
            const HttpResponse failure = RequestFailure(error);
            response.networkError = failure.networkError;
            response.detail = failure.detail;
            response.lastError = failure.lastError;
        };

        if (!::WinHttpSendRequest(request.get(), requestHeaders, static_cast<DWORD>(-1L),
                                  static_cast<LPVOID>(const_cast<char*>(body.data())),
                                  static_cast<DWORD>(body.size()),
                                  static_cast<DWORD>(body.size()), 0)) {
            if (watchdog.Fired()) {
                return StalledResponse(response, endpoint, clampedTimeout);
            }
            const DWORD error = ::GetLastError();
            if (IsConnectionFailure(error)) {
                ResetConnections();
                if (attempt == 0 && IsSafeMethod(method)) {
                    response = HttpResponse{};
                    continue;
                }
            }
            setNetworkError(error);
            return response;
        }

        if (!::WinHttpReceiveResponse(request.get(), nullptr)) {
            if (watchdog.Fired()) {
                return StalledResponse(response, endpoint, clampedTimeout);
            }
            const DWORD error = ::GetLastError();
            if (IsConnectionFailure(error)) {
                ResetConnections();
            }
            setNetworkError(error);
            return response;
        }

        DWORD statusCode = 0;
        DWORD statusSize = static_cast<DWORD>(sizeof(statusCode));
        if (::WinHttpQueryHeaders(request.get(),
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                  WINHTTP_NO_HEADER_INDEX)) {
            response.status = static_cast<long>(statusCode);
        }

        DWORD headerBytes = 0;
        ::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF,
                              WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                              &headerBytes, WINHTTP_NO_HEADER_INDEX);
        if (headerBytes > 0 && headerBytes <= kMaxHeaderBytes) {
            std::wstring raw(headerBytes / sizeof(wchar_t), L'\0');
            if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                      WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &headerBytes,
                                      WINHTTP_NO_HEADER_INDEX)) {
                response.headers = util::WideToUtf8(raw.c_str());
            }
        }

        std::string buffer(kReadChunk, '\0');
        for (;;) {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
                if (watchdog.Fired()) {
                    return StalledResponse(response, endpoint, clampedTimeout);
                }
                setNetworkError(::GetLastError());
                break;
            }
            if (available == 0) break;

            const size_t remaining = kMaxResponseBytes - response.body.size();
            if (remaining == 0) break;

            DWORD toRead = available;
            if (static_cast<size_t>(toRead) > buffer.size()) {
                toRead = static_cast<DWORD>(buffer.size());
            }
            if (static_cast<size_t>(toRead) > remaining) {
                toRead = static_cast<DWORD>(remaining);
            }

            DWORD read = 0;
            if (!::WinHttpReadData(request.get(), buffer.data(), toRead, &read)) {
                if (watchdog.Fired()) {
                    return StalledResponse(response, endpoint, clampedTimeout);
                }
                setNetworkError(::GetLastError());
                break;
            }
            if (read == 0) break;

            watchdog.Bump();
            response.body.append(buffer.data(), read);
            if (sink != nullptr && !(*sink)(buffer.data(), static_cast<size_t>(read))) {
                break;
            }
            if (response.body.size() >= kMaxResponseBytes) break;
        }
        return response;
    }
    return response;
}

#else

struct CurlContext {
    HttpResponse* response = nullptr;
    const ChunkSink* sink = nullptr;
    bool abortedBySink = false;
};

size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t total = size * nitems;
    auto* ctx = static_cast<CurlContext*>(userdata);
    if (ctx != nullptr && ctx->response != nullptr && ctx->response->headers.size() < 65536) {
        ctx->response->headers.append(buffer, total);
    }
    return total;
}

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* ctx = static_cast<CurlContext*>(userdata);
    if (ctx == nullptr || ctx->response == nullptr) return 0;
    const size_t remaining = kMaxResponseBytes > ctx->response->body.size()
                                 ? kMaxResponseBytes - ctx->response->body.size()
                                 : 0;
    const size_t take = total < remaining ? total : remaining;
    if (take > 0) {
        ctx->response->body.append(ptr, take);
        if (ctx->sink != nullptr && !(*ctx->sink)(ptr, take)) {
            ctx->abortedBySink = true;
            return 0;
        }
    }
    return total;
}

HttpResponse RequestCurl(const Endpoint& endpoint, const std::string& method,
                         const std::string& headers, const std::string& body, int timeoutMs,
                         const ChunkSink* sink) {
    static std::once_flag initFlag;
    std::call_once(initFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.networkError = true;
        response.detail = "curl_easy_init failed";
        return response;
    }

    std::string url = (endpoint.https ? "https://" : "http://") + endpoint.host;
    if ((endpoint.https && endpoint.port != 443) || (!endpoint.https && endpoint.port != 80)) {
        url += ":" + std::to_string(endpoint.port);
    }
    url += endpoint.path.empty() ? "/" : endpoint.path;

    struct curl_slist* headerList = nullptr;
    size_t pos = 0;
    while (pos < headers.size()) {
        const size_t end = headers.find('\n', pos);
        std::string line = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? headers.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            headerList = curl_slist_append(headerList, line.c_str());
        }
    }

    CurlContext ctx{&response, sink, false};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs > 0 ? timeoutMs : kSessionTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && ctx.abortedBySink)) {
        response.networkError = true;
        response.detail = curl_easy_strerror(rc);
        response.lastError = static_cast<unsigned long>(rc);
    }

    if (headerList != nullptr) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response;
}
#endif

HttpResponse Dispatch(const Endpoint& endpoint, const std::string& method,
                      const std::string& headers, const std::string& body, int timeoutMs,
                      const ChunkSink* sink) {
    MockTransportFn mock;
    {
        std::lock_guard<std::mutex> lock(g_mockMutex);
        mock = g_mockTransport;
    }
    if (mock) {
        return mock(endpoint, method, headers, body, timeoutMs, sink);
    }
#ifdef _WIN32
    return RequestWinHttp(endpoint, method, headers, body, timeoutMs, sink);
#else
    return RequestCurl(endpoint, method, headers, body, timeoutMs, sink);
#endif
}

}  // namespace

void SetMockTransport(MockTransportFn fn) {
    std::lock_guard<std::mutex> lock(g_mockMutex);
    g_mockTransport = std::move(fn);
}

bool ParseEndpoint(const std::string& url, Endpoint& out) {
    const std::string trimmed = util::Trim(url);
    if (trimmed.empty()) return false;

    std::string rest;
    bool https = true;
    if (HasPrefixNoCase(trimmed, "https://")) {
        https = true;
        rest = trimmed.substr(8);
    } else if (HasPrefixNoCase(trimmed, "http://")) {
        https = false;
        rest = trimmed.substr(7);
    } else {
        return false;
    }

    const size_t pathStart = rest.find('/');
    std::string authority = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    std::string path = (pathStart == std::string::npos) ? std::string("/") : rest.substr(pathStart);
    if (path.empty()) path = "/";

    int port = https ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string portText = authority.substr(colon + 1);
        authority.resize(colon);
        if (!portText.empty()) {
            char* end = nullptr;
            const long parsed = std::strtol(portText.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || parsed <= 0 || parsed > 65535) {
                return false;
            }
            port = static_cast<int>(parsed);
        }
    }

    if (authority.empty()) return false;

    out.host = authority;
    out.path = path;
    out.https = https;
    out.port = port;
    return true;
}

HttpResponse Request(const Endpoint& endpoint, const std::string& method,
                     const std::string& headers, const std::string& body, int timeoutMs) {
    return Dispatch(endpoint, method, headers, body, timeoutMs, nullptr);
}

HttpResponse RequestStream(const Endpoint& endpoint, const std::string& method,
                           const std::string& headers, const std::string& body,
                           int timeoutMs, const ChunkSink& onChunk) {
    if (!onChunk) {
        return Dispatch(endpoint, method, headers, body, timeoutMs, nullptr);
    }
    return Dispatch(endpoint, method, headers, body, timeoutMs, &onChunk);
}

HttpResponse Post(const Endpoint& endpoint, const std::string& headers,
                  const std::string& body, int timeoutMs) {
    return Dispatch(endpoint, "POST", headers, body, timeoutMs, nullptr);
}

}  // namespace gwa::http
