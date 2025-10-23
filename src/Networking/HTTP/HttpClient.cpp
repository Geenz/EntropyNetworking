/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "Networking/HTTP/HttpClient.h"
#include <string>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace EntropyEngine::Networking::HTTP {

// Global libcurl initialization (thread-safe, one-time)
void HttpClient::initGlobalCurl() {
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        curl_global_init(CURL_GLOBAL_ALL);
    });
}

namespace {
// libcurl share locking callbacks
static void curlShareLock(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/, void* userptr) {
    auto* locks = static_cast<std::array<std::mutex, CURL_LOCK_DATA_LAST>*>(userptr);
    (*locks)[data].lock();
}
static void curlShareUnlock(CURL* /*handle*/, curl_lock_data data, void* userptr) {
    auto* locks = static_cast<std::array<std::mutex, CURL_LOCK_DATA_LAST>*>(userptr);
    (*locks)[data].unlock();
}
}

HttpClient::HttpClient()
    : _connectionShare(nullptr) {
    initGlobalCurl();

    // Default proxy resolver (env + system)
    _proxyResolver = std::make_unique<DefaultProxyResolver>();

    // Create shared connection cache for HTTP/2 multiplexing
    _connectionShare = curl_share_init();
    if (_connectionShare) {
        curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(_connectionShare, CURLSHOPT_LOCKFUNC, curlShareLock);
        curl_share_setopt(_connectionShare, CURLSHOPT_UNLOCKFUNC, curlShareUnlock);
        curl_share_setopt(_connectionShare, CURLSHOPT_USERDATA, &_curlShareLocks);
    }
}

HttpClient::~HttpClient() {
    closeIdle();
    if (_connectionShare) {
        curl_share_cleanup(_connectionShare);
        _connectionShare = nullptr;
    }
}

void HttpClient::setUseSystemProxy(bool enabled) {
    // Best-effort: only DefaultProxyResolver supports this policy toggle
    if (auto* def = dynamic_cast<DefaultProxyResolver*>(_proxyResolver.get())) {
        def->setUseSystemProxy(enabled);
    }
}

void HttpClient::closeIdle() {
    std::lock_guard<std::mutex> lock(_shareMutex);
    // libcurl doesn't provide explicit API to close idle connections
    // Recreating the share effectively closes all cached connections
    if (_connectionShare) {
        curl_share_cleanup(_connectionShare);
        _connectionShare = curl_share_init();
        if (_connectionShare) {
            curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
            curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(_connectionShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
            curl_share_setopt(_connectionShare, CURLSHOPT_LOCKFUNC, curlShareLock);
            curl_share_setopt(_connectionShare, CURLSHOPT_UNLOCKFUNC, curlShareUnlock);
            curl_share_setopt(_connectionShare, CURLSHOPT_USERDATA, &_curlShareLocks);
        }
    }
}

// Callback for receiving response body
size_t HttpClient::writeCallback(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* respData = static_cast<ResponseData*>(userdata);
    size_t totalSize = size * nmemb;

    if (respData->cap && respData->body.size() + totalSize > respData->cap) {
        respData->abortedByCap = true;
        return 0; // abort transfer
    }

    respData->body.insert(respData->body.end(), reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + totalSize);

    return totalSize;
}

// Callback for receiving response headers
size_t HttpClient::headerCallback(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* respData = static_cast<ResponseData*>(userdata);
    size_t totalSize = size * nmemb;

    std::string line(data, totalSize);

    // Skip status line (HTTP/1.1 200 OK)
    if (line.find("HTTP/") == 0) {
        return totalSize;
    }

    // Parse header: "Name: Value\r\n"
    auto colonPos = line.find(':');
    if (colonPos != std::string::npos) {
        std::string name = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim whitespace from value
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        // Convert name to lowercase for consistent lookups
        std::transform(name.begin(), name.end(), name.begin(),
                      [](unsigned char c){ return std::tolower(c); });

        // Store last-seen value for convenience
        respData->headers[name] = value;
        // Also append to multi-valued header map
        respData->headersMulti[name].push_back(value);
    }

    return totalSize;
}

static void applyProxyIfAny(CURL* curl, ProxyResolver* resolver, const HttpRequest& req, const RequestOptions& opts) {
    if (!resolver) return;
    if (opts.proxyPolicy == ProxyPolicy::DirectOnly) {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        return;
    }
    if (opts.explicitProxy && !opts.explicitProxy->empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, opts.explicitProxy->c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
        return;
    }
    // Parse host and optional port
    std::string host = req.host;
    uint16_t port = (req.scheme == "https") ? 443 : 80;
    auto pos = host.rfind(':');
    if (pos != std::string::npos && host.find(':') == pos) {
        try { port = static_cast<uint16_t>(std::stoi(host.substr(pos+1))); } catch (...) {}
        host = host.substr(0, pos);
    }
    ProxyResult pr = resolver->resolve(req.scheme, host, port);
    if (pr.type == ProxyResult::Type::Direct) return;
    curl_easy_setopt(curl, CURLOPT_PROXY, pr.url.c_str());
    switch (pr.type) {
        case ProxyResult::Type::Http:   curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP); break;
        case ProxyResult::Type::Socks4: curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4); break;
        case ProxyResult::Type::Socks5: curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5); break;
        default: break;
    }
    curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
}

void HttpClient::configureCurlHandle(CURL* curl, const HttpRequest& req,
                                        const RequestOptions& opts, ResponseData& respData,
                                        struct curl_slist* headers) {
    // Build full URL from structured fields
    std::string path = req.path.empty() ? "/" : (req.path[0] == '/' ? req.path : std::string("/") + req.path);
    std::string url = req.scheme + "://" + req.host + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Proxy auto-detection and configuration
    applyProxyIfAny(curl, _proxyResolver.get(), req, opts);

    // HTTP method
    switch (req.method) {
        case HttpMethod::GET:
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::HEAD:
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            break;
        case HttpMethod::POST:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            break;
        case HttpMethod::PUT:
            // For non-streaming PUT, use custom request + POSTFIELDS
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        case HttpMethod::DELETE_:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case HttpMethod::OPTIONS:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
            break;
        case HttpMethod::PATCH:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
            break;
        case HttpMethod::PROPFIND:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
            break;
        case HttpMethod::MKCOL:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MKCOL");
            break;
        case HttpMethod::MOVE:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MOVE");
            break;
        case HttpMethod::COPY:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "COPY");
            break;
    }

    // Request body (aggregated)
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }

    // Headers (passed in, already built)
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // Select HTTP version preference
    switch (opts.httpVersionPref) {
        case HttpVersionPref::PreferH3:
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_3);
            break;
        case HttpVersionPref::H3Only:
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_3ONLY);
            break;
        default:
            if (req.scheme == "https") {
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
            } else {
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
            }
            break;
    }

    // TLS options
    if (opts.caInfoPath && !opts.caInfoPath->empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, opts.caInfoPath->c_str());
    }
    if (opts.caPathDir && !opts.caPathDir->empty()) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, opts.caPathDir->c_str());
    }
    if (opts.sslCertPath && !opts.sslCertPath->empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT, opts.sslCertPath->c_str());
    }
    if (opts.sslKeyPath && !opts.sslKeyPath->empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLKEY, opts.sslKeyPath->c_str());
    }
    if (opts.sslKeyPasswd && !opts.sslKeyPasswd->empty()) {
        curl_easy_setopt(curl, CURLOPT_KEYPASSWD, opts.sslKeyPasswd->c_str());
    }
    if (opts.pinnedPublicKey && !opts.pinnedPublicKey->empty()) {
        curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, opts.pinnedPublicKey->c_str());
    }
    switch (opts.tlsMinVersion) {
        case TlsMinVersion::TLSv1_2:
            curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
            break;
        case TlsMinVersion::TLSv1_3:
            curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_3);
            break;
        default:
            break;
    }

    // Callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respData);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respData);

    // Error buffer for richer diagnostics
    respData.errbuf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, respData.errbuf);

    // Timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)opts.connectTimeout.count());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)opts.totalDeadline.count());
    // Approximate idle/write timeouts via low-speed options
    long lowSpeedTimeSec = 0;
    if (opts.readIdleTimeout.count() > 0) {
        lowSpeedTimeSec = std::max<long>(lowSpeedTimeSec, std::max<long>(1, (long)(opts.readIdleTimeout.count()/1000)));
    }
    if (opts.writeTimeout.count() > 0) {
        lowSpeedTimeSec = std::max<long>(lowSpeedTimeSec, std::max<long>(1, (long)(opts.writeTimeout.count()/1000)));
        // Also limit time waiting for 100-continue handshake when Expect is enabled
#ifdef CURLOPT_EXPECT_100_TIMEOUT_MS
        if (opts.expect100Continue) {
            curl_easy_setopt(curl, CURLOPT_EXPECT_100_TIMEOUT_MS, (long)opts.writeTimeout.count());
        }
#endif
    }
    if (lowSpeedTimeSec > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // bytes/sec
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, lowSpeedTimeSec);
    }

    // Avoid signals in multi-threaded apps
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Connection sharing for multiplexing
    {
        std::lock_guard<std::mutex> lock(_shareMutex);
        if (_connectionShare) {
            curl_easy_setopt(curl, CURLOPT_SHARE, _connectionShare);
        }
    }

    // Enforce response cap via our write callback
    respData.cap = opts.maxResponseBytes;

    // Follow redirects: default ON for safe methods (GET/HEAD/PROPFIND/OPTIONS) unless explicitly disabled
    auto isSafe = [](HttpMethod m){ return m==HttpMethod::GET || m==HttpMethod::HEAD || m==HttpMethod::PROPFIND || m==HttpMethod::OPTIONS; };
    bool follow = opts.followRedirects || isSafe(req.method);
    if (follow) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)opts.maxRedirects);
    }

    // Respect system proxy env and allow any proxy auth scheme
    curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);

    // Default User-Agent if not provided via headers
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EntropyHTTP/1.0");

    // Accept compressed encodings; cURL will decompress automatically
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // Enable verbose output for debugging (can be controlled by env var)
    if (std::getenv("ENTROPY_HTTP_DEBUG")) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }
}

HttpResponse HttpClient::execute(const HttpRequest& req, const RequestOptions& opts) {
    auto isIdempotent = [](HttpMethod m){ return m==HttpMethod::GET || m==HttpMethod::HEAD || m==HttpMethod::PROPFIND || m==HttpMethod::OPTIONS; };
    int attempts = (opts.enableRetries && isIdempotent(req.method)) ? (opts.maxRetries + 1) : 1;

    HttpResponse finalResp;
    auto startTime = std::chrono::steady_clock::now();
    bool hasTotalDeadline = opts.totalDeadline.count() > 0;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return HttpResponse{0, "Failed to initialize curl", {}, {}};
        }

        ResponseData respData;
        struct curl_slist* headers = nullptr;

        // Build headers list
        for (const auto& [name, value] : req.headers) {
            std::string header = name + ": " + value;
            headers = curl_slist_append(headers, header.c_str());
        }
        // Additional headers (allow duplicates, order preserved)
        for (const auto& kv : opts.extraHeaders) {
            std::string header = kv.first + ": " + kv.second;
            headers = curl_slist_append(headers, header.c_str());
        }

        // Optionally disable Expect: 100-continue per request
        if (!opts.expect100Continue) {
            headers = curl_slist_append(headers, "Expect:");
        }

        configureCurlHandle(curl, req, opts, respData, headers);

        // Adjust per-attempt timeout to respect overall totalDeadline
        if (hasTotalDeadline) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
            long remainingMs = (long)opts.totalDeadline.count() - (long)elapsedMs;
            if (remainingMs <= 0) {
                // No time left; return last response or timeout immediately
                if (headers) curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                finalResp.statusCode = 0;
                finalResp.statusMessage = "cURL error: timeout";
                break;
            }
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, remainingMs);
            // Clamp connect timeout to remaining time as well
            long connMs = (long)opts.connectTimeout.count();
            if (connMs > remainingMs) connMs = remainingMs;
            if (connMs > 0) curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connMs);
        }

        // Streaming upload support for aggregated execute(): provide read callback when requested
        struct UploadSource { std::function<size_t(char*, size_t)> read; } uploadSrc;
        bool useStreamingUpload = static_cast<bool>(opts.uploadRead);
        if (useStreamingUpload && (req.method == HttpMethod::PUT || req.method == HttpMethod::POST)) {
            uploadSrc.read = opts.uploadRead; // copy function
            // Set read callback
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                auto* src = static_cast<UploadSource*>(userdata);
                size_t max = size * nmemb;
                return src->read ? src->read(ptr, max) : 0; // 0 = EOF
            });
            curl_easy_setopt(curl, CURLOPT_READDATA, &uploadSrc);

            if (req.method == HttpMethod::PUT) {
                curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L); // PUT upload
                // Some servers expect explicit PUT
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            } else if (req.method == HttpMethod::POST) {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
            }

            if (opts.contentLength.has_value()) {
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)opts.contentLength.value());
                // For POST, also set POSTFIELDSIZE_LARGE so libcurl sends Content-Length
                if (req.method == HttpMethod::POST) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)opts.contentLength.value());
                }
            } else {
                // Unknown length not yet supported in our raw-socket test server; prefer known length
                // Force HTTP/1.1 and attempt chunked transfer if user insists (not default)
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
                headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)-1);
            }
        }

        // Perform request (blocking)
        CURLcode res = curl_easy_perform(curl);

        HttpResponse response;

        if (res != CURLE_OK) {
            response.statusCode = 0;
            std::string msg;
            if (respData.abortedByCap) {
                msg = "response exceeds maximum size";
            } else if (respData.errbuf[0] != '\0') {
                msg = respData.errbuf;
            } else {
                msg = curl_easy_strerror(res);
            }
            response.statusMessage = std::string("cURL error: ") + msg;
        } else {
            long statusCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

            response.statusCode = static_cast<int>(statusCode);
            response.statusMessage = "OK";
            response.headers = std::move(respData.headers);
            response.headersMulti = std::move(respData.headersMulti);
            response.body = std::move(respData.body);
        }

        // Optional metrics logging
        if (std::getenv("ENTROPY_HTTP_METRICS")) {
            long httpVer = 0; curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &httpVer);
            curl_off_t szUp=0, szDn=0, totalMs=0; long redirects=0;
            curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &szUp);
            curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &szDn);
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &totalMs);
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirects);
            fprintf(stderr, "[EntropyHTTP] method=%d status=%d up=%lld down=%lld totalMs=%lld redirects=%ld httpVer=%ld\n",
                (int)req.method, response.statusCode, (long long)szUp, (long long)szDn, (long long)totalMs, redirects, httpVer);
        }

        // Cleanup per attempt
        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);

        // Decide on retry
        bool shouldRetry = false;
        if (res != CURLE_OK && !respData.abortedByCap) {
            shouldRetry = true;
        } else {
            int s = response.statusCode;
            if (s == 408 || s == 429 || (s >= 500 && s != 501 && s != 505)) {
                shouldRetry = true;
            }
        }

        if (!shouldRetry || attempt == attempts - 1) {
            finalResp = std::move(response);
            break;
        }

        // Compute backoff delay
        uint64_t delayMs = 0;
        auto itRA = response.headers.find("retry-after");
        if (itRA != response.headers.end()) {
            // try parse seconds
            delayMs = (uint64_t)(std::strtoull(itRA->second.c_str(), nullptr, 10) * 1000ull);
        }
        if (delayMs == 0) {
            uint64_t base = (uint64_t)std::max(0, opts.retryBackoffBaseMs);
            uint64_t cap  = (uint64_t)std::max(0, opts.retryBackoffCapMs);
            uint64_t backoff = base * (1u << attempt);
            if (cap > 0 && backoff > cap) backoff = cap;
            // jitter: 50%-100%
            uint64_t jitter = backoff / 2 + (uint64_t)(rand() % (int)(backoff / 2 + 1));
            delayMs = jitter;
        }
        // Respect remaining deadline for backoff sleep
        if (hasTotalDeadline) {
            auto now2 = std::chrono::steady_clock::now();
            auto elapsed2 = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - startTime).count();
            long remaining2 = (long)opts.totalDeadline.count() - (long)elapsed2;
            if (remaining2 <= 0) {
                finalResp = std::move(response);
                break;
            }
            if ((long)delayMs > remaining2) delayMs = (uint64_t)remaining2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    return finalResp;
}

// ============================================================================
// Streaming Support
// ============================================================================

size_t HttpClient::streamWriteCallback(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<StreamState*>(userdata);
    size_t totalSize = size * nmemb;

    std::lock_guard<std::mutex> lock(state->mutex);

    // Check total bytes cap
    if (state->maxBodyBytes > 0 && state->totalReceived + totalSize > state->maxBodyBytes) {
        state->failed = true;
        state->failureReason = "response exceeds maximum size";
        state->cv.notify_all();
        return 0; // abort transfer
    }

    // Check buffer capacity - pause if full (backpressure)
    if (state->size + totalSize > state->capacity) {
        state->paused = true;
        state->cv.notify_all();
        return CURL_WRITEFUNC_PAUSE; // pause transfer, will resume when consumer frees space
    }

    // Write to ring buffer (may wrap around)
    size_t endSpace = state->capacity - state->tail;
    if (totalSize <= endSpace) {
        std::memcpy(&state->buffer[state->tail], data, totalSize);
    } else {
        std::memcpy(&state->buffer[state->tail], data, endSpace);
        std::memcpy(&state->buffer[0], data + endSpace, totalSize - endSpace);
    }

    state->tail = (state->tail + totalSize) % state->capacity;
    state->size += totalSize;
    state->totalReceived += totalSize;

    state->cv.notify_all();
    return totalSize;
}

size_t HttpClient::streamHeaderCallback(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<StreamState*>(userdata);
    size_t totalSize = size * nmemb;

    std::string line(data, totalSize);

    // Skip status line
    if (line.find("HTTP/") == 0) {
        return totalSize;
    }

    // Detect end-of-headers (blank line) to mark headers ready and fetch status code early
    if (line == "\r\n") {
        std::lock_guard<std::mutex> lock(state->mutex);
        long sc = 0;
        if (state->easy) {
            curl_easy_getinfo(state->easy, CURLINFO_RESPONSE_CODE, &sc);
        }
        state->statusCode = static_cast<int>(sc);
        // Defer headersReady if this is an intermediate 3xx redirect; final response will trigger another header block
        if (!(state->statusCode >= 300 && state->statusCode < 400)) {
            state->headersReady = true;
            state->cv.notify_all();
        }
        return totalSize;
    }

    // Parse header
    auto colonPos = line.find(':');
    if (colonPos != std::string::npos) {
        std::string name = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        // Lowercase name
        std::transform(name.begin(), name.end(), name.begin(),
                      [](unsigned char c){ return std::tolower(c); });

        std::lock_guard<std::mutex> lock(state->mutex);
        // Store last-seen value for convenience
        state->headers[name] = value;
        // Append to multi-valued map
        state->headersMulti[name].push_back(value);
    }

    return totalSize;
}

// Progress callback used to support cancellation of streaming transfers
static int xferInfoCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* state = static_cast<StreamState*>(clientp);

    // Handle cancellation and (if needed) resume on the worker thread
    CURL* easyLocal = nullptr;
    bool doResume = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelRequested) {
            state->failed = true;
            if (state->failureReason.empty()) state->failureReason = "cancelled";
            return 1; // abort transfer
        }
        if (state->paused && state->resumeRequested && !state->failed && !state->done && state->easy != nullptr) {
            // Only resume if there is room in the buffer now
            if (state->size < state->capacity) {
                doResume = true;
                easyLocal = state->easy;
                state->resumeRequested = false; // consume the request
            }
        }
    }
    if (doResume && easyLocal) {
        // Call libcurl without holding the mutex to avoid deadlocks
        curl_easy_pause(easyLocal, CURLPAUSE_CONT);
    }
    return 0;
}

void HttpClient::configureStreamCurlHandle(CURL* curl, const HttpRequest& req,
                                          const StreamOptions& opts, StreamState& state,
                                          struct curl_slist* headers) {
    // Build URL
    std::string path = req.path.empty() ? "/" : (req.path[0] == '/' ? req.path : std::string("/") + req.path);
    std::string url = req.scheme + "://" + req.host + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Proxy auto-detection for streaming requests as well (defaults to Auto)
    RequestOptions proxyOpts; // default (Auto), no explicit proxy
    applyProxyIfAny(curl, _proxyResolver.get(), req, proxyOpts);

    // HTTP method (streaming typically GET)
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    // Request body (if any)
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }

    // Headers
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // Select HTTP version based on scheme for streaming as well
    if (req.scheme == "https") {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    }

    // Streaming callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);

    // Progress callback for cancellation
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

    // Timeouts
    if (opts.connectTimeout.count() > 0) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)opts.connectTimeout.count());
    }
    if (opts.totalDeadline.count() > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)opts.totalDeadline.count());
    }

    // Thread safety
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Connection sharing intentionally disabled for streaming to avoid cross-thread
    // share lock/unlock interactions that trigger MSVC debug mutex diagnostics.
    // Aggregated requests still use the shared connection cache.
    //{
    //    std::lock_guard<std::mutex> lock(_shareMutex);
    //    if (_connectionShare) {
    //        curl_easy_setopt(curl, CURLOPT_SHARE, _connectionShare);
    //    }
    //}

    // Proxy support
    curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EntropyHTTP/1.0");

    // Accept encoding
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // Follow redirects by default for streaming GET
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
 
    // Debug
    if (std::getenv("ENTROPY_HTTP_DEBUG")) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }
}

StreamHandle HttpClient::executeStream(const HttpRequest& req, const StreamOptions& opts) {
    auto state = std::make_shared<StreamState>();
    state->capacity = std::max<size_t>(opts.bufferBytes, 64 * 1024); // min 64 KB
    state->buffer.resize(state->capacity);
    state->maxBodyBytes = opts.maxBodyBytes;

    // Build headers
    struct curl_slist* headersList = nullptr;
    for (const auto& [name, value] : req.headers) {
        std::string header = name + ": " + value;
        headersList = curl_slist_append(headersList, header.c_str());
    }

    // Launch background thread to run curl_easy_perform
    std::thread([this, req, opts, state, headersList]() mutable {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->failureReason = "Failed to initialize curl";
            state->cv.notify_all();
            if (headersList) curl_slist_free_all(headersList);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(state->mutex);
            state->easy = curl;
        }

        configureStreamCurlHandle(curl, req, opts, *state, headersList);

        // Perform request (blocks until complete or error)
        CURLcode res = curl_easy_perform(curl);

        {
            std::lock_guard<std::mutex> lock(state->mutex);

            if (res != CURLE_OK) {
                state->failed = true;
                if (state->cancelRequested) {
                    state->failureReason = "cancelled";
                } else {
                    state->failureReason = std::string("cURL error: ") + curl_easy_strerror(res);
                }
            } else {
                long statusCode = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
                state->statusCode = static_cast<int>(statusCode);
                state->headersReady = true;
            }

            state->done = true;
            state->cv.notify_all();
        }

        // Cleanup
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            // Invalidate easy handle for other threads before cleanup to avoid races
            state->easy = nullptr;
        }
        if (headersList) {
            curl_slist_free_all(headersList);
        }
        curl_easy_cleanup(curl);
    }).detach();

    return StreamHandle(state);
}

// ============================================================================
// StreamHandle Implementation
// ============================================================================

size_t StreamHandle::read(uint8_t* buffer, size_t size) {
    std::unique_lock<std::mutex> lock(_state->mutex);

    // Wait for data or completion
    _state->cv.wait(lock, [this]() {
        return _state->size > 0 || _state->done || _state->failed;
    });

    if (_state->failed) {
        return 0;
    }

    // Read up to size bytes from ring buffer
    size_t toRead = std::min(size, _state->size);
    if (toRead == 0) {
        return 0; // No data available and stream done
    }

    // Copy from ring buffer (may wrap)
    size_t endSpace = _state->capacity - _state->head;
    if (toRead <= endSpace) {
        std::memcpy(buffer, &_state->buffer[_state->head], toRead);
    } else {
        std::memcpy(buffer, &_state->buffer[_state->head], endSpace);
        std::memcpy(buffer + endSpace, &_state->buffer[0], toRead - endSpace);
    }

    _state->head = (_state->head + toRead) % _state->capacity;
    _state->size -= toRead;

    // Decide if we should resume the paused transfer
    bool needResume = _state->paused && _state->easy != nullptr && !_state->failed && !_state->done;
    if (needResume) {
        // Request resume to be handled by the worker (libcurl) thread via progress callback
        _state->resumeRequested = true;
    }

    _state->cv.notify_all(); // Notify writer that space is available

    lock.unlock();

    return toRead;
}

bool StreamHandle::isDone() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->done && _state->size == 0;
}

bool StreamHandle::failed() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->failed;
}

std::string StreamHandle::getFailureReason() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->failureReason;
}

int StreamHandle::getStatusCode() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->statusCode;
}

HttpHeaders StreamHandle::getHeaders() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->headers;
}

HttpHeaderValuesMap StreamHandle::getHeadersMulti() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->headersMulti;
}

bool StreamHandle::waitForHeaders(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_state->mutex);
    return _state->cv.wait_for(lock, timeout, [this]() {
        return _state->headersReady || _state->failed;
    });
}

void StreamHandle::cancel() {
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->done || _state->failed) return;
    _state->cancelRequested = true;
    _state->cv.notify_all();
}

} // namespace EntropyEngine::Networking::HTTP
