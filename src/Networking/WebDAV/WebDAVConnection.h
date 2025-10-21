/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDAVConnection.h
 * @brief HTTP/1.1 client for WebDAV operations with streaming support
 *
 * This file contains WebDAVConnection, an HTTP client built on NetworkConnection
 * that provides both aggregated and streaming modes for WebDAV requests.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <atomic>

#include "Networking/Transport/NetworkConnection.h"
#include <llhttp.h>

namespace EntropyEngine::Networking::WebDAV {

// Default configuration constants
static constexpr size_t DEFAULT_STREAM_BUFFER_BYTES = 4ull * 1024ull * 1024ull;  // 4 MiB
static constexpr size_t DEFAULT_MAX_BODY_BYTES = 128ull * 1024ull * 1024ull;     // 128 MiB

/**
 * @brief HTTP/1.1 client for WebDAV operations
 *
 * Provides synchronous HTTP methods (GET, HEAD, PROPFIND) with aggregated
 * response bodies, plus streaming GET for large file downloads. Uses llhttp
 * for parsing and supports HTTP/1.1 keep-alive.
 *
 * Thread Safety: Only one request per connection at a time. For concurrent
 * requests, use multiple connections via WebDAVFileSystemBackend::setAggregateConnections().
 *
 * @code
 * WebDAVConnection::Config cfg{.host = "example.com"};
 * auto conn = std::make_shared<WebDAVConnection>(netConn, cfg);
 * auto response = conn->get("/path/to/file");
 * if (response.isSuccess()) {
 *     processBody(response.body);
 * }
 * @endcode
 */
class WebDAVConnection {
public:
    /**
     * @brief Internal state for streaming GET operations
     *
     * Ring buffer state shared between HTTP parser (producer) and
     * WebDAVReadStream (consumer). Protected by mutex with condition variable.
     */
    struct StreamState {
        std::mutex m;                                         ///< Protects ring buffer state
        std::condition_variable cv;                           ///< Signals data available/consumed
        bool headersReady = false;                            ///< true when HTTP headers parsed
        bool done = false;                                    ///< true when response complete
        bool failed = false;                                  ///< true on parse or network error
        std::string failureReason;                            ///< Error description if failed
        int statusCode = 0;                                   ///< HTTP status code (e.g., 200, 404)
        std::unordered_map<std::string, std::string> headers; ///< Response headers (lowercase keys)
        std::vector<uint8_t> buf;                             ///< Ring buffer storage
        size_t head = 0;                                      ///< Read index
        size_t tail = 0;                                      ///< Write index
        size_t size = 0;                                      ///< Bytes currently stored
        size_t capacity = 0;                                  ///< Total capacity (buf.size())
        size_t maxBodyBytes = 0;                              ///< Hard safety cap across whole message
        size_t receivedTotal = 0;                             ///< Byte counter for cap enforcement
        bool parserPaused = false;                            ///< true if llhttp parser is paused due to backpressure
        std::string curHeaderField;                           ///< Temp accumulator for header field
        std::string curHeaderValue;                           ///< Temp accumulator for header value
    };

    /**
     * @brief Configuration for streaming GET operations
     */
    struct StreamConfig {
        size_t bufferBytes = DEFAULT_STREAM_BUFFER_BYTES;                  ///< Ring buffer capacity
        std::vector<std::pair<std::string,std::string>> headers;           ///< Extra request headers (e.g., Range)
    };

    /**
     * @brief Handle to active streaming GET operation
     *
     * Provides access to shared StreamState for reading response data.
     * Returned by openGetStream().
     */
    class StreamHandle {
    public:
        /**
         * @brief Constructs handle from stream state
         * @param st Shared stream state
         */
        explicit StreamHandle(std::shared_ptr<StreamState> st) : _st(std::move(st)) {}

        /**
         * @brief Gets shared stream state
         * @return Pointer to StreamState
         */
        std::shared_ptr<StreamState> state() const { return _st; }

    private:
        std::shared_ptr<StreamState> _st;  ///< Shared stream state
    };

public:
    /**
     * @brief Connection configuration
     */
    struct Config {
        std::string host;                                     ///< Required for Host header (e.g., "example.com")
        std::string userAgent = "EntropyWebDAV/1.0";          ///< User-Agent header value
        std::string authHeader;                               ///< Optional: "Bearer ..." or "Basic ..."
        std::chrono::milliseconds requestTimeout{30000};      ///< Timeout for aggregated requests (30s default)
        size_t maxBodyBytes = DEFAULT_MAX_BODY_BYTES;         ///< Maximum response body size
    };

    /**
     * @brief HTTP response for aggregated operations
     */
    struct Response {
        int statusCode = 0;                                   ///< HTTP status code (0 on timeout/error)
        std::string statusMessage;                            ///< Status message or error description
        std::unordered_map<std::string, std::string> headers; ///< Response headers (lowercase keys)
        std::vector<uint8_t> body;                            ///< Response body bytes

        /**
         * @brief Checks if response indicates success
         * @return true if status code is 2xx
         */
        bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
    };

    /**
     * @brief Constructs WebDAV connection
     * @param nc Underlying network connection (must be connected)
     * @param cfg Connection configuration (host, auth, timeouts)
     */
    WebDAVConnection(std::shared_ptr<EntropyEngine::Networking::NetworkConnection> nc, Config cfg);

    /**
     * @brief Destructor ensures clean shutdown
     *
     * Prevents new callbacks, waits for in-flight callbacks to complete,
     * then safely cleans up active requests.
     */
    ~WebDAVConnection();

    /**
     * @brief Performs HTTP GET request
     * @param path Request path (e.g., "/dav/file.txt")
     * @param extraHeaders Additional headers to include
     * @return Response with status, headers, and body
     */
    Response get(const std::string& path,
                 const std::vector<std::pair<std::string,std::string>>& extraHeaders = {});

    /**
     * @brief Performs HTTP HEAD request
     * @param path Request path
     * @param extraHeaders Additional headers to include
     * @return Response with status and headers (no body)
     */
    Response head(const std::string& path,
                  const std::vector<std::pair<std::string,std::string>>& extraHeaders = {});

    /**
     * @brief Performs WebDAV PROPFIND request
     * @param path Request path (e.g., "/dav/folder/")
     * @param depth Depth header value (0 for resource, 1 for immediate children)
     * @param bodyXml XML request body specifying properties to retrieve
     * @return Response with 207 Multistatus XML body
     */
    Response propfind(const std::string& path, int depth, const std::string& bodyXml);

    /**
     * @brief Opens streaming GET request for large downloads
     *
     * Starts HTTP GET in background with ring buffer for incremental reads.
     * Use with WebDAVReadStream to consume data.
     *
     * @param path Request path
     * @param cfg Stream configuration (buffer size, headers)
     * @return StreamHandle providing access to streaming state
     * @throws std::runtime_error if another request is already active
     */
    StreamHandle openGetStream(const std::string& path, const StreamConfig& cfg);

    /**
     * @brief Aborts active request (aggregated or streaming)
     *
     * Marks active request as failed and wakes waiting threads.
     */
    void abortActiveRequest();

    /**
     * @brief Checks if underlying connection is connected
     * @return true if NetworkConnection reports connected state
     */
    bool isConnected() const { return _conn && _conn->isConnected(); }

    /**
     * @brief Resumes a paused streaming parser if there is now buffer capacity
     */
    void resumeIfPaused();

public:
    /**
     * @brief Internal state for aggregated HTTP requests
     */
    struct PendingResponse {
        std::condition_variable cv;                           ///< Signals request completion
        bool done = false;                                    ///< true when response complete
        bool failed = false;                                  ///< true on parse or network error
        std::string failureReason;                            ///< Error description if failed
        Response resp;                                        ///< Accumulated response
        std::string curHeaderField;                           ///< Accumulator for header field (llhttp may chunk)
        std::string curHeaderValue;                           ///< Accumulator for header value (llhttp may chunk)
        std::string statusText;                               ///< Accumulated status text
        size_t maxBodyBytes = DEFAULT_MAX_BODY_BYTES;         ///< Body size cap for this request
    };

private:
    std::shared_ptr<EntropyEngine::Networking::NetworkConnection> _conn;  ///< Underlying network connection
    Config _cfg;                                                           ///< Connection configuration

    std::mutex _reqMutex;                                                  ///< Serializes requests and protects parser
    std::unique_ptr<PendingResponse> _active;                              ///< State for current aggregated request
    std::shared_ptr<StreamState> _activeStream;                            ///< State for current streaming request
    llhttp_t _parser{};                                                    ///< llhttp parser instance
    llhttp_settings_t _settings{};                                         ///< llhttp callbacks for aggregated responses
    llhttp_settings_t _streamSettings{};                                   ///< llhttp callbacks for streaming responses
    std::vector<uint8_t> _leftover;                                        ///< Bytes received when no request active
    std::vector<uint8_t> _pausedRemainder;                                  ///< Bytes held while parser is paused

    std::atomic<bool> _shuttingDown{false};                                ///< Shutdown flag for receive callback
    std::atomic<int>  _inCallback{0};                                      ///< In-flight callback reference count

    /**
     * @brief Callback invoked when data arrives on connection
     * @param bytes Received data bytes
     */
    void onDataReceived(const std::vector<uint8_t>& bytes);

    /**
     * @brief Sends HTTP request and waits for aggregated response
     * @param request Complete HTTP request text
     * @return Response with status, headers, and body
     */
    Response sendAndReceive(std::string request);

    /**
     * @brief Builds HTTP request text
     * @param method HTTP method (GET, HEAD, PROPFIND, etc.)
     * @param path Request path
     * @param headers Additional headers
     * @param body Request body (empty for GET/HEAD)
     * @return Complete HTTP request text
     */
    std::string buildRequest(const char* method,
                             const std::string& path,
                             const std::vector<std::pair<std::string,std::string>>& headers,
                             const std::string& body);
};

} // namespace EntropyEngine::Networking::WebDAV
