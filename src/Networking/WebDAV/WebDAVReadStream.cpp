/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVReadStream.h"

#include <algorithm>
#include <cstring>

namespace EntropyEngine::Networking::WebDAV {

WebDAVReadStream::WebDAVReadStream(std::shared_ptr<WebDAVConnection> conn,
                                   std::string url,
                                   size_t bufferBytes,
                                   const std::vector<std::pair<std::string,std::string>>& extraHeaders)
    : _conn(std::move(conn)), _url(std::move(url)) {
    // Configure stream buffer capacity and open underlying HTTP GET stream
    WebDAVConnection::StreamConfig sc;
    sc.bufferBytes = bufferBytes > 0 ? bufferBytes : (64u * 1024u);
    sc.headers = extraHeaders;

    auto handle = _conn->openGetStream(_url, sc);
    _st = handle.state();

    // Optionally we could wait until headers are ready here to detect immediate error statuses.
    // Leaving lazy handling to read() keeps constructor non-blocking.
}

WebDAVReadStream::~WebDAVReadStream() {
    close();
}

Core::IO::IoResult WebDAVReadStream::read(std::span<std::byte> dst) {
    Core::IO::IoResult r{};
    if (!_st) { r.error = Core::IO::FileError::NetworkError; return r; }
    if (dst.empty()) { r.complete = eof(); return r; }

    std::unique_lock<std::mutex> lk(_st->m);
    // Wait until some data is available, or stream done/failed, or headers indicate error
    _st->cv.wait(lk, [&]{ return _st->size > 0 || _st->failed || _st->done || _st->headersReady; });

    // If headers indicate error, surface it
    if (_st->headersReady && _st->statusCode >= 400) {
        r.error = Core::IO::FileError::NetworkError;
        _failed = true;
        return r;
    }

    if (_st->failed) {
        r.error = Core::IO::FileError::NetworkError;
        _failed = true;
        return r;
    }

    if (_st->size == 0 && _st->done) {
        r.complete = true;
        return r; // EOF
    }

    // Copy from ring buffer (may wrap)
    size_t toRead = std::min(dst.size(), _st->size);
    size_t first = std::min(toRead, _st->capacity - _st->head);
    std::memcpy(dst.data(), &_st->buf[_st->head], first);
    if (first < toRead) {
        std::memcpy(dst.data() + first, &_st->buf[0], toRead - first);
    }
    _st->head = (_st->head + toRead) % _st->capacity;
    _st->size -= toRead;

    r.bytesTransferred = toRead;
    r.complete = (_st->done && _st->size == 0);

    // Decide if we should resume the HTTP parser (if it was paused due to backpressure)
    bool shouldResume = _st->parserPaused && (_st->size < _st->capacity);

    lk.unlock();
    _st->cv.notify_all(); // wake producer in case it was waiting on space

    if (shouldResume && _conn) {
        _conn->resumeIfPaused();
    }
    return r;
}

Core::IO::IoResult WebDAVReadStream::write(std::span<const std::byte>) {
    return {0, false, Core::IO::FileError::InvalidPath};
}

bool WebDAVReadStream::seek(int64_t, std::ios_base::seekdir) { return false; }
int64_t WebDAVReadStream::tell() const { return -1; }

bool WebDAVReadStream::good() const { return !_failed && !_closed; }

bool WebDAVReadStream::eof() const {
    if (!_st) return true;
    std::scoped_lock<std::mutex> lk(_st->m);
    return _st->done && _st->size == 0;
}

bool WebDAVReadStream::fail() const { return _failed; }

void WebDAVReadStream::flush() {}

void WebDAVReadStream::close() {
    if (_closed) return;
    _closed = true;
    if (_conn) {
        _conn->abortActiveRequest();
    }
}

} // namespace EntropyEngine::Networking::WebDAV
