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

        respData->headers[name] = value;
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

    // Select HTTP version based on scheme
    // - For HTTPS: allow HTTP/2 via ALPN with fallback to HTTP/1.1
    // - For HTTP: stick to HTTP/1.1 to avoid h2c upgrade issues with simple servers
    if (req.scheme == "https") {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
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
    // Approximate read-idle timeout via low-speed options if set (>0)
    if (opts.readIdleTimeout.count() > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // bytes/sec
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)std::max<int64_t>(1, opts.readIdleTimeout.count()/1000));
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

    // Follow redirects if requested
    if (opts.followRedirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
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

    configureCurlHandle(curl, req, opts, respData, headers);

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
        response.body = std::move(respData.body);
    }

    // Cleanup
    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);

    return response;
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
        state->headers[name] = value;
    }

    return totalSize;
}

// Progress callback used to support cancellation of streaming transfers
static int xferInfoCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* state = static_cast<StreamState*>(clientp);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->cancelRequested) {
        state->failed = true;
        if (state->failureReason.empty()) state->failureReason = "cancelled";
        // Returning non-zero aborts the transfer
        return 1;
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

    // Connection sharing
    {
        std::lock_guard<std::mutex> lock(_shareMutex);
        if (_connectionShare) {
            curl_easy_setopt(curl, CURLOPT_SHARE, _connectionShare);
        }
    }

    // Proxy support
    curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EntropyHTTP/1.0");

    // Accept encoding
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

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

        configureStreamCurlHandle(curl, req, opts, *state, headersList);

        // Mark headers ready after first header callback
        // (streamHeaderCallback will notify when headers are complete)

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

    _state->cv.notify_all(); // Notify writer that space is available

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
