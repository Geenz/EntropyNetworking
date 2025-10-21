/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDAVReadStream.h
 * @brief FileStream implementation for streaming WebDAV GET operations
 *
 * This file contains WebDAVReadStream, a FileStream that consumes data from
 * WebDAVConnection's ring buffer for incremental reading of large files.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

#include <VirtualFileSystem/FileStream.h>
#include "Networking/WebDAV/WebDAVConnection.h"

namespace EntropyEngine::Networking::WebDAV {

/**
 * @brief Read-only FileStream for WebDAV GET operations
 *
 * Consumes data from WebDAVConnection's ring buffer in a producer-consumer pattern.
 * HTTP parser (producer) fills buffer, WebDAVReadStream (consumer) reads from it.
 * Synchronization via mutex and condition variable.
 *
 * Thread Safety: Can be used from any thread. Internal state is protected by mutex.
 *
 * @code
 * WebDAVConnection::StreamConfig cfg{.bufferBytes = 1024 * 1024};  // 1 MiB buffer
 * auto stream = backend->openStream("/remote/large_file.bin");
 *
 * std::vector<std::byte> chunk(8192);
 * while (auto result = stream->read(chunk); result.bytesTransferred > 0) {
 *     processChunk(chunk.data(), result.bytesTransferred);
 *     if (result.complete) break;
 * }
 * stream->close();
 * @endcode
 */
class WebDAVReadStream : public Core::IO::FileStream {
public:
    /**
     * @brief Constructs stream and initiates HTTP GET
     *
     * Constructor is non-blocking. HTTP request is sent immediately but stream
     * waits for data on first read() call.
     *
     * @param conn WebDAVConnection for HTTP operations
     * @param url Full URL to GET
     * @param bufferBytes Ring buffer capacity
     * @param extraHeaders Additional HTTP headers (e.g., Range)
     */
    WebDAVReadStream(std::shared_ptr<WebDAVConnection> conn,
                     std::string url,
                     size_t bufferBytes,
                     const std::vector<std::pair<std::string,std::string>>& extraHeaders = {});

    /**
     * @brief Destructor closes stream and aborts request
     */
    ~WebDAVReadStream() override;

    /**
     * @brief Reads data from ring buffer
     *
     * Blocks until data is available, stream completes, or error occurs.
     * Copies from ring buffer (handles wrapping).
     *
     * @param buffer Destination buffer
     * @return IoResult with bytesTransferred, complete flag, or error
     */
    Core::IO::IoResult read(std::span<std::byte> buffer) override;

    /**
     * @brief Write not supported (read-only stream)
     * @return IoResult with InvalidPath error
     */
    Core::IO::IoResult write(std::span<const std::byte> data) override;

    /**
     * @brief Seek not supported (forward-only stream)
     * @return false
     */
    bool seek(int64_t offset, std::ios_base::seekdir dir) override;

    /**
     * @brief Tell not supported (forward-only stream)
     * @return -1
     */
    int64_t tell() const override;

    /**
     * @brief Checks if stream is in good state
     * @return true if not failed and not closed
     */
    bool good() const override;

    /**
     * @brief Checks if stream reached end-of-file
     * @return true if response complete and buffer empty
     */
    bool eof() const override;

    /**
     * @brief Checks if stream is in failed state
     * @return true if HTTP error or parse error occurred
     */
    bool fail() const override;

    /**
     * @brief Flush is no-op (read-only stream)
     */
    void flush() override;

    /**
     * @brief Closes stream and aborts HTTP request
     *
     * Safe to call multiple times. Wakes blocking read() calls.
     */
    void close() override;

    /**
     * @brief Gets stream path/URL
     * @return URL being streamed
     */
    std::string path() const override { return _url; }

    /**
     * @brief Returns HTTP status code of the streaming response (0 if unavailable)
     */
    int statusCode() const {
        if (!_st) return 0;
        std::scoped_lock<std::mutex> lk(_st->m);
        return _st->statusCode;
    }

    /**
     * @brief Returns a copy of response headers (lowercase keys)
     */
    std::unordered_map<std::string,std::string> headers() const {
        if (!_st) return {};
        std::scoped_lock<std::mutex> lk(_st->m);
        return _st->headers;
    }

private:
    std::shared_ptr<WebDAVConnection> _conn;               ///< Connection for abort on close
    std::shared_ptr<WebDAVConnection::StreamState> _st;    ///< Shared ring buffer state
    std::string _url;                                      ///< URL being streamed
    bool _closed = false;                                  ///< true if close() called
    bool _failed = false;                                  ///< true on error
};

} // namespace EntropyEngine::Networking::WebDAV
