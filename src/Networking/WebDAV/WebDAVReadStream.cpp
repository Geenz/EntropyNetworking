/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVReadStream.h"
#include <cstdlib>

namespace EntropyEngine::Networking::WebDAV {

WebDAVReadStream::WebDAVReadStream(HTTP::StreamHandle handle, std::string url)
    : _handle(std::move(handle)), _url(std::move(url)) {
    // StreamHandle is already streaming in background thread
    // Lazy error detection on first read() keeps constructor non-blocking
}

WebDAVReadStream::~WebDAVReadStream() {
    close();
}

Core::IO::IoResult WebDAVReadStream::read(std::span<std::byte> dst) {
    Core::IO::IoResult r{};

    if (_closed) {
        r.error = Core::IO::FileError::NetworkError;
        return r;
    }

    if (dst.empty()) {
        r.complete = eof();
        return r;
    }

    // Check for HTTP errors (4xx/5xx status codes)
    int status = _handle.getStatusCode();
    if (status >= 400) {
        r.error = Core::IO::FileError::NetworkError;
        return r;
    }

    // Check if stream failed
    if (_handle.failed()) {
        r.error = Core::IO::FileError::NetworkError;
        return r;
    }

    // Read from HttpClient StreamHandle
    size_t bytesRead = _handle.read(reinterpret_cast<uint8_t*>(dst.data()), dst.size());

    r.bytesTransferred = bytesRead;
    r.complete = _handle.isDone() && bytesRead == 0;

    return r;
}

Core::IO::IoResult WebDAVReadStream::write(std::span<const std::byte>) {
    return {0, false, Core::IO::FileError::InvalidPath};
}

bool WebDAVReadStream::seek(int64_t, std::ios_base::seekdir) { return false; }
int64_t WebDAVReadStream::tell() const { return -1; }

bool WebDAVReadStream::good() const {
    return !_closed && !_handle.failed() && _handle.getStatusCode() < 400;
}

bool WebDAVReadStream::eof() const {
    return _handle.isDone();
}

bool WebDAVReadStream::fail() const {
    return _handle.failed() || _handle.getStatusCode() >= 400;
}

void WebDAVReadStream::flush() {}

void WebDAVReadStream::close() {
    if (_closed) return;
    _closed = true;
    // Cancel the underlying HTTP transfer to stop promptly
    _handle.cancel();
}

std::optional<uint64_t> WebDAVReadStream::contentLength() const {
    auto h = _handle.getHeaders();
    auto it = h.find("content-length");
    if (it == h.end()) return std::nullopt;
    const char* s = it->second.c_str();
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s) return std::nullopt;
    return static_cast<uint64_t>(v);
}

std::optional<std::string> WebDAVReadStream::etag() const {
    auto h = _handle.getHeaders();
    auto it = h.find("etag");
    if (it == h.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> WebDAVReadStream::contentType() const {
    auto h = _handle.getHeaders();
    auto it = h.find("content-type");
    if (it == h.end()) return std::nullopt;
    return it->second;
}

} // namespace EntropyEngine::Networking::WebDAV
