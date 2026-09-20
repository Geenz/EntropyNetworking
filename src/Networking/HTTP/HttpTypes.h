/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace EntropyEngine::Networking::HTTP
{

// Lowercase header keys map (single value per key for convenience)
using HttpHeaders = std::unordered_map<std::string, std::string>;
// Multi-valued headers map (stores all values for a given header key)
using HttpHeaderValuesMap = std::unordered_map<std::string, std::vector<std::string>>;

enum class HttpMethod
{
    GET,
    HEAD,
    POST,
    PUT,
    DELETE_,
    OPTIONS,
    PATCH,
    PROPFIND,
    MKCOL,
    MOVE,
    COPY
};

// Optional HTTP version preference for a request
enum class HttpVersionPref
{
    Default,
    PreferH3,
    H3Only
};

// Optional minimum TLS version
enum class TlsMinVersion
{
    Default,
    TLSv1_2,
    TLSv1_3
};

struct HttpRequest
{
    HttpMethod method = HttpMethod::GET;
    std::string scheme = "https";  // "http" or "https" - defaults to HTTPS for security
    std::string host;              // may include ":port"
    std::string path;              // origin-form path + optional query
    HttpHeaders headers;           // lowercase keys; Host/User-Agent auto-filled if missing
    std::vector<uint8_t> body;
};

struct HttpResponse
{
    int statusCode = 0;
    std::string statusMessage;
    HttpHeaders headers;               // lowercase keys (last-seen value)
    HttpHeaderValuesMap headersMulti;  // all values per header key (lowercase keys)
    std::vector<uint8_t> body;         // aggregated body

    bool isSuccess() const {
        return statusCode >= 200 && statusCode < 300;
    }
};

enum class ProxyPolicy
{
    Auto,
    DirectOnly,
    ForceProxy
};

struct RequestOptions
{
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds writeTimeout{10000};
    std::chrono::milliseconds readIdleTimeout{15000};
    std::chrono::milliseconds totalDeadline{30000};
    size_t maxResponseBytes = 128ull * 1024ull * 1024ull;
    bool followRedirects = false;  // honored for aggregated and streaming requests; aggregated defaults to follow for
                                   // safe methods, streaming GET follows by default
    int maxRedirects = 10;         // hop limit for redirects
    // Proxy behaviour (auto = env/system detection; DirectOnly = bypass; ForceProxy = expect proxy)
    ProxyPolicy proxyPolicy = ProxyPolicy::Auto;
    std::optional<std::string> explicitProxy;  // per-request override, e.g., "http://proxy:8080"

    // Retry policy for idempotent methods (GET/HEAD/PROPFIND/OPTIONS)
    bool enableRetries = true;
    int maxRetries = 2;
    int retryBackoffBaseMs = 200;
    int retryBackoffCapMs = 2000;

    // HTTP version preference and TLS options
    HttpVersionPref httpVersionPref = HttpVersionPref::Default;
    TlsMinVersion tlsMinVersion = TlsMinVersion::Default;
    std::optional<std::string> caInfoPath;       // CURLOPT_CAINFO
    std::optional<std::string> caPathDir;        // CURLOPT_CAPATH
    std::optional<std::string> sslCertPath;      // CURLOPT_SSLCERT
    std::optional<std::string> sslKeyPath;       // CURLOPT_SSLKEY
    std::optional<std::string> sslKeyPasswd;     // CURLOPT_KEYPASSWD
    std::optional<std::string> pinnedPublicKey;  // CURLOPT_PINNEDPUBLICKEY (PEM/SPKI)

    // Additional request headers allowing duplicates and ordering
    std::vector<std::pair<std::string, std::string>> extraHeaders;

    // Streaming upload (PUT/POST) support for aggregated execute(): when set, libcurl pulls
    // request body via this callback rather than from req.body buffer.
    // The callback should write up to max bytes into dst and return the number written.
    // Return 0 to signal EOF. It may be called multiple times.
    std::function<size_t(char* dst, size_t max)> uploadRead;  // empty = disabled
    std::optional<uint64_t> contentLength;  // required for HTTP/2 and preferred; when unset, caller may use chunked
                                            // (not yet implemented here)
    bool expect100Continue = true;          // set false to disable Expect: 100-continue
};

}  // namespace EntropyEngine::Networking::HTTP
