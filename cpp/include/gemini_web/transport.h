#pragma once

#include <functional>
#include <string>

namespace gwa::http {

struct Endpoint {
    std::string host;
    std::string path;
    bool https = true;
    int port = 443;
};

// Splits "https://host:port/path" into components. Missing path becomes "/".
bool ParseEndpoint(const std::string& url, Endpoint& out);

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string headers;          // Raw CRLF header block
    bool networkError = false;
    std::string detail;
    unsigned long lastError = 0;
};

// Returning false stops the transfer early while keeping bytes read so far.
using ChunkSink = std::function<bool(const char* data, size_t size)>;

// Optional custom transport handler (used by tests to inject mock responses).
using MockTransportFn = std::function<HttpResponse(
    const Endpoint& endpoint, const std::string& method, const std::string& headers,
    const std::string& body, int timeoutMs, const ChunkSink* sink)>;

void SetMockTransport(MockTransportFn fn);

HttpResponse Request(const Endpoint& endpoint, const std::string& method,
                     const std::string& headers, const std::string& body, int timeoutMs);

HttpResponse RequestStream(const Endpoint& endpoint, const std::string& method,
                           const std::string& headers, const std::string& body,
                           int timeoutMs, const ChunkSink& onChunk);

HttpResponse Post(const Endpoint& endpoint, const std::string& headers,
                  const std::string& body, int timeoutMs);

}  // namespace gwa::http
