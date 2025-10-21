/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "Networking/HTTP/HttpTypes.h"
#include "Networking/HTTP/Proxy.h"
#include <curl/curl.h>
#include <mutex>
#include <memory>
#include <functional>
#include <array>
#include <condition_variable>
#include <thread>

namespace EntropyEngine::Networking::HTTP {

/**
 * @brief Streaming response state for incremental reads
 *
 * Ring buffer with producer/consumer pattern. HTTP parser writes to tail,
 * StreamHandle reads from head. Backpressure via CURL_WRITEFUNC_PAUSE.
 */
struct StreamState {
    std::mutex mutex;                                         ///< Protects ring buffer state
    std::condition_variable cv;                               ///< Signals data available/consumed
    bool headersReady = false;                                ///< true when HTTP headers parsed
    bool done = false;                                        ///< true when response complete
    bool failed = false;                                      ///< true on error
    std::string failureReason;                                ///< Error description
    int statusCode = 0;                                       ///< HTTP status code
    HttpHeaders headers;                                      ///< Response headers (lowercase, last-seen)
    HttpHeaderValuesMap headersMulti;                         ///< All values per header key
    std::vector<uint8_t> buffer;                              ///< Ring buffer storage
    size_t head = 0;                                          ///< Read index
    size_t tail = 0;                                          ///< Write index
    size_t size = 0;                                          ///< Bytes currently stored
    size_t capacity = 0;                                      ///< Total capacity
    size_t totalReceived = 0;                                 ///< Total bytes received
    size_t maxBodyBytes = 0;                                  ///< Hard safety cap
    bool cancelRequested = false;                             ///< Cancellation requested by consumer
};

/**
 * @brief Handle to active streaming HTTP request
 *
 * Provides incremental read access to response body. Use for large downloads
 * where aggregated responses would exceed memory limits.
 *
 * @code
 * StreamOptions opts{.bufferBytes = 4 * 1024 * 1024};
 * auto handle = client.executeStream(req, opts);
 * std::vector<uint8_t> chunk(64 * 1024);
 * while (!handle.isDone()) {
 *     size_t n = handle.read(chunk.data(), chunk.size());
 *     processChunk(chunk.data(), n);
 * }
 * @endcode
 */
class StreamHandle {
public:
    explicit StreamHandle(std::shared_ptr<StreamState> state)
        : _state(std::move(state)) {}

    /**
     * @brief Reads up to size bytes from stream
     * @param buffer Destination buffer
     * @param size Maximum bytes to read
     * @return Number of bytes actually read (0 if none available)
     */
    size_t read(uint8_t* buffer, size_t size);

    /**
     * @brief Checks if stream has completed
     * @return true if all data received and consumed
     */
    bool isDone() const;

    /**
     * @brief Checks if stream failed
     * @return true on error
     */
    bool failed() const;

    /**
     * @brief Gets failure reason if failed
     * @return Error message
     */
    std::string getFailureReason() const;

    /**
     * @brief Gets HTTP status code (available after headers received)
     * @return Status code, or 0 if headers not yet ready
     */
    int getStatusCode() const;

    /**
     * @brief Gets response headers (available after headers received)
     * @return Headers map (lowercase keys)
     */
    HttpHeaders getHeaders() const;

    /**
     * @brief Gets multi-valued response headers (available after headers received)
     * @return Map of header name to all values
     */
    HttpHeaderValuesMap getHeadersMulti() const;

    /**
     * @brief Waits for headers to be received
     * @param timeout Maximum time to wait
     * @return true if headers ready, false on timeout
     */
    bool waitForHeaders(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

    /**
     * @brief Cancels the ongoing HTTP transfer
     *
     * Safe to call from any thread. Causes the transfer to abort promptly.
     */
    void cancel();

private:
    std::shared_ptr<StreamState> _state;

    friend class HttpClient;
};

/**
 * @brief Options for streaming HTTP requests
 */
struct StreamOptions {
    size_t bufferBytes = 4ull * 1024ull * 1024ull;           ///< Ring buffer size (default 4 MiB)
    size_t maxBodyBytes = 0;                                  ///< Max total bytes (0 = unlimited)
    std::chrono::milliseconds connectTimeout{10000};          ///< Connection timeout
    std::chrono::milliseconds totalDeadline{0};               ///< Total timeout (0 = no timeout)
};

/**
 * @brief Production-grade HTTP client using libcurl
 *
 * Provides HTTP/1.1 and HTTP/2 support with automatic protocol negotiation (ALPN).
 * Thread-safe connection pooling and multiplexing handled by libcurl.
 *
 * Features:
 * - HTTP/1.1 and HTTP/2 with automatic fallback
 * - TLS with OpenSSL/SChannel
 * - DNS hostname resolution
 * - Connection pooling and multiplexing
 * - Proxy support (env vars: HTTP_PROXY, HTTPS_PROXY, NO_PROXY)
 * - Timeouts and safety limits
 * - Streaming downloads with backpressure
 *
 * @code
 * HttpClient client;
 * HttpRequest req{.method = HttpMethod::GET, .host = "example.com", .path = "/api/data"};
 * HttpResponse resp = client.execute(req);
 * if (resp.isSuccess()) {
 *     processData(resp.body);
 * }
 * @endcode
 */
class HttpClient {
public:
    /**
     * @brief Constructs HTTP client with libcurl backend
     */
    HttpClient();

    /**
     * @brief Destructor - cleans up libcurl resources
     */
    ~HttpClient();

    // Non-copyable, movable
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = default;
    HttpClient& operator=(HttpClient&&) = default;

    /**
     * @brief Execute HTTP request synchronously (blocks calling thread)
     *
     * @param req HTTP request (method, host, path, headers, body)
     * @param opts Request options (timeouts, max response size, etc.)
     * @return HttpResponse with status code, headers, and body
     */
    HttpResponse execute(const HttpRequest& req, const RequestOptions& opts = {});

    /**
     * @brief Execute streaming HTTP request for large downloads
     *
     * Returns immediately with StreamHandle. curl_easy_perform runs in background
     * thread. Consumer reads incrementally via StreamHandle::read().
     *
     * @param req HTTP request (typically GET)
     * @param opts Streaming options (buffer size, limits, timeouts)
     * @return StreamHandle for incremental reading
     */
    StreamHandle executeStream(const HttpRequest& req, const StreamOptions& opts = {});

    /**
     * @brief Close all idle connections
     *
     * Useful for cleanup or forcing connection refresh.
     */
    void closeIdle();

private:
    CURLSH* _connectionShare;  // Shared connection pool for HTTP/2 multiplexing
    std::mutex _shareMutex;    // Protects connection share access
    std::array<std::mutex, CURL_LOCK_DATA_LAST> _curlShareLocks; // libcurl share locks (per data slot)

    // Proxy auto-detection resolver (env + system)
    std::unique_ptr<ProxyResolver> _proxyResolver;

    static void initGlobalCurl();
    static size_t writeCallback(char* data, size_t size, size_t nmemb, void* userdata);
    static size_t headerCallback(char* data, size_t size, size_t nmemb, void* userdata);

    // Streaming callbacks
    static size_t streamWriteCallback(char* data, size_t size, size_t nmemb, void* userdata);
    static size_t streamHeaderCallback(char* data, size_t size, size_t nmemb, void* userdata);

    struct ResponseData {
        std::vector<uint8_t> body;
        HttpHeaders headers;                // last-seen value per key
        HttpHeaderValuesMap headersMulti;   // all values per key
        std::string curHeaderLine;
        size_t cap = 0;              // max response bytes (0 = unlimited)
        bool abortedByCap = false;   // indicates we aborted due to cap
        char errbuf[CURL_ERROR_SIZE] = {0};
    };

    void configureCurlHandle(CURL* curl, const HttpRequest& req,
                            const RequestOptions& opts, ResponseData& respData,
                            struct curl_slist* headers);

    void configureStreamCurlHandle(CURL* curl, const HttpRequest& req,
                                   const StreamOptions& opts, StreamState& state,
                                   struct curl_slist* headers);
};

} // namespace EntropyEngine::Networking::HTTP
