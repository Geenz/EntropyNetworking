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
 * This file contains WebDAVReadStream, a FileStream wrapper around HttpClient::StreamHandle
 * for incremental reading of large files over HTTP/WebDAV.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include <VirtualFileSystem/FileStream.h>
#include "Networking/HTTP/HttpClient.h"
#include <optional>
#include <cstdint>

namespace EntropyEngine::Networking::WebDAV {

/**
 * @brief Read-only FileStream for WebDAV GET operations via HttpClient
 *
 * Wraps HttpClient::StreamHandle to provide FileStream interface for incremental
 * reading of large files over HTTP/WebDAV.
 *
 * Thread Safety: Can be used from any thread. HttpClient::StreamHandle handles synchronization.
 *
 * @code
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
     * @brief Constructs stream from HttpClient StreamHandle
     *
     * Constructor is non-blocking. HTTP request is already initiated via
     * HttpClient::executeStream(). Stream waits for data on first read() call.
     *
     * @param handle HttpClient StreamHandle for incremental reading
     * @param url Full URL being streamed (for path() method)
     */
    WebDAVReadStream(HTTP::StreamHandle handle, std::string url);

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
        return _handle.getStatusCode();
    }

    /**
     * @brief Returns a copy of response headers (lowercase keys)
     */
    HTTP::HttpHeaders headers() const {
        return _handle.getHeaders();
    }

    // Convenience accessors for common headers
    std::optional<uint64_t> contentLength() const;
    std::optional<std::string> etag() const;
    std::optional<std::string> contentType() const;

private:
    HTTP::StreamHandle _handle;    ///< HttpClient stream handle for incremental reading
    std::string _url;              ///< URL being streamed
    bool _closed = false;          ///< true if close() called
};

} // namespace EntropyEngine::Networking::WebDAV
