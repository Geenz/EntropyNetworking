/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVConnection.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <thread>

#include "EntropyCore.h"

namespace EntropyEngine::Networking::WebDAV
{

static inline void toLowerInPlace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

// llhttp C callbacks (aggregated response mode)
static int on_status_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    pr->statusText.append(at, len);
    return 0;
}

static int on_header_field_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    if (!pr->curHeaderField.empty() && !pr->curHeaderValue.empty()) {
        toLowerInPlace(pr->curHeaderField);
        pr->resp.headers[pr->curHeaderField] = pr->curHeaderValue;
        pr->curHeaderField.clear();
        pr->curHeaderValue.clear();
    }
    pr->curHeaderField.append(at, len);
    return 0;
}

static int on_header_value_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    pr->curHeaderValue.append(at, len);
    return 0;
}

static int on_headers_complete_cb(llhttp_t* p) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    if (!pr->curHeaderField.empty()) {
        toLowerInPlace(pr->curHeaderField);
        pr->resp.headers[pr->curHeaderField] = pr->curHeaderValue;
        pr->curHeaderField.clear();
        pr->curHeaderValue.clear();
    }
    pr->resp.statusCode = p->status_code;
    pr->resp.statusMessage = pr->statusText;
    return 0;
}

static int on_body_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    const size_t CAP = pr->maxBodyBytes;
    if (pr->resp.body.size() + len > CAP) {
        pr->failed = true;
        pr->failureReason = "Response body exceeds maximum size";
        return (int)HPE_USER;
    }
    pr->resp.body.insert(pr->resp.body.end(), reinterpret_cast<const uint8_t*>(at),
                         reinterpret_cast<const uint8_t*>(at) + len);
    return 0;
}

static int on_message_complete_cb(llhttp_t* p) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    pr->done = true;
    pr->cv.notify_one();
    return 0;
}

// llhttp C callbacks (streaming mode)
static int on_stream_header_field_cb(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<WebDAVConnection::StreamState*>(p->data);
    if (!st->curHeaderField.empty() && !st->curHeaderValue.empty()) {
        std::string name = st->curHeaderField;
        toLowerInPlace(name);
        st->headers[name] = st->curHeaderValue;
        st->curHeaderField.clear();
        st->curHeaderValue.clear();
    }
    st->curHeaderField.append(at, len);
    return 0;
}

static int on_stream_header_value_cb(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<WebDAVConnection::StreamState*>(p->data);
    st->curHeaderValue.append(at, len);
    return 0;
}

static int on_stream_headers_complete_cb(llhttp_t* p) {
    auto* st = static_cast<WebDAVConnection::StreamState*>(p->data);
    if (!st->curHeaderField.empty()) {
        std::string name = st->curHeaderField;
        toLowerInPlace(name);
        st->headers[name] = st->curHeaderValue;
        st->curHeaderField.clear();
        st->curHeaderValue.clear();
    }
    st->statusCode = p->status_code;
    st->headersReady = true;
    st->cv.notify_all();
    return 0;
}

static int on_stream_body_cb(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<WebDAVConnection::StreamState*>(p->data);
    // Enforce total cap
    if (st->maxBodyBytes > 0 && st->receivedTotal + len > st->maxBodyBytes) {
        st->failed = true;
        st->failureReason = "Stream exceeds maximum size";
        st->cv.notify_all();
        return (int)HPE_USER;
    }
    // Enforce ring capacity with backpressure: pause parser if full
    if (st->size + len > st->capacity) {
        return (int)HPE_PAUSED;
    }
    // Copy into ring buffer (may wrap)
    size_t endSpace = st->capacity - st->tail;
    if (len <= endSpace) {
        std::memcpy(&st->buf[st->tail], at, len);
    } else {
        std::memcpy(&st->buf[st->tail], at, endSpace);
        std::memcpy(&st->buf[0], at + endSpace, len - endSpace);
    }
    st->tail = (st->tail + len) % st->capacity;
    st->size += len;
    st->receivedTotal += len;
    st->cv.notify_all();
    return 0;
}

static int on_stream_message_complete_cb(llhttp_t* p) {
    auto* st = static_cast<WebDAVConnection::StreamState*>(p->data);
    st->done = true;
    st->cv.notify_all();
    return 0;
}

WebDAVConnection::WebDAVConnection(std::shared_ptr<EntropyEngine::Networking::NetworkConnection> nc, Config cfg)
    : _conn(std::move(nc)), _cfg(std::move(cfg)) {
    if (!_conn) throw std::invalid_argument("NetworkConnection cannot be null");
    if (_cfg.host.empty()) throw std::invalid_argument("Host must be specified for HTTP requests");
    if (_cfg.requestTimeout.count() <= 0) throw std::invalid_argument("Request timeout must be positive");

    llhttp_settings_init(&_settings);

    _settings.on_status = on_status_cb;

    _settings.on_header_field = on_header_field_cb;

    _settings.on_header_value = on_header_value_cb;

    _settings.on_headers_complete = on_headers_complete_cb;

    _settings.on_body = on_body_cb;

    _settings.on_message_complete = on_message_complete_cb;

    // Initialize streaming settings
    llhttp_settings_init(&_streamSettings);
    _streamSettings.on_header_field = on_stream_header_field_cb;
    _streamSettings.on_header_value = on_stream_header_value_cb;
    _streamSettings.on_headers_complete = on_stream_headers_complete_cb;
    _streamSettings.on_body = on_stream_body_cb;
    _streamSettings.on_message_complete = on_stream_message_complete_cb;

    // Install permanent callback that feeds data into the parser of the active request
    _conn->setMessageCallback([this](const std::vector<uint8_t>& bytes) { onDataReceived(bytes); });
}

WebDAVConnection::~WebDAVConnection() {
    // Prevent new callbacks and wait for in-flight ones to complete
    _shuttingDown.store(true, std::memory_order_release);
    if (_conn) {
        _conn->setMessageCallback(nullptr);
    }
    // Spin-wait briefly for any active onDataReceived to finish
    while (_inCallback.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
    // Clear any active request safely
    std::lock_guard<std::mutex> lk(_reqMutex);
    _active.reset();
    _leftover.clear();
}

std::string WebDAVConnection::buildRequest(const char* method, const std::string& path,
                                           const std::vector<std::pair<std::string, std::string>>& headers,
                                           const std::string& body) {
    std::ostringstream o;
    o << method << ' ' << path << " HTTP/1.1\r\n";
    o << "Host: " << _cfg.host << "\r\n";
    o << "User-Agent: " << _cfg.userAgent << "\r\n";
    o << "Accept: application/xml, text/xml; q=0.9, */*; q=0.1\r\n";
    o << "Connection: keep-alive\r\n";
    if (!_cfg.authHeader.empty()) o << "Authorization: " << _cfg.authHeader << "\r\n";
    for (auto& kv : headers) {
        o << kv.first << ": " << kv.second << "\r\n";
    }
    if (!body.empty()) {
        o << "Content-Length: " << body.size() << "\r\n";
    }
    o << "\r\n";
    if (!body.empty()) o << body;
    return o.str();
}

WebDAVConnection::Response WebDAVConnection::sendAndReceive(std::string request) {
    std::unique_lock<std::mutex> lk(_reqMutex);

    _active = std::make_unique<PendingResponse>();
    _active->maxBodyBytes = _cfg.maxBodyBytes;
    llhttp_init(&_parser, HTTP_RESPONSE, &_settings);
    _parser.data = _active.get();

    // Feed any leftover bytes first (rare but possible if a response raced in)
    if (!_leftover.empty()) {
        auto err = llhttp_execute(&_parser, reinterpret_cast<const char*>(_leftover.data()), _leftover.size());
        _leftover.clear();
        if (err != HPE_OK && err != HPE_PAUSED) {
            _active->failed = true;
            _active->failureReason = std::string("llhttp error on leftover: ") + llhttp_errno_name(err);
            _active->cv.notify_one();
        }
    }

    // Send without holding the request mutex to avoid deadlock with the receive callback
    lk.unlock();
    auto sendResult = _conn->send(std::vector<uint8_t>(request.begin(), request.end()));
    lk.lock();
    if (!sendResult.success()) {
        // Fail fast on send error
        if (_active) {
            _active->failed = true;
            _active->failureReason = "send failed: " + sendResult.errorMessage;
            _active->cv.notify_one();
        }
    }

    // Wait
    const bool timedout =
        !_active->cv.wait_for(lk, _cfg.requestTimeout, [this] { return _active->done || _active->failed; });

    Response out;
    if (timedout) {
        out.statusCode = 0;
        out.statusMessage = "timeout";
    } else if (_active->failed) {
        out.statusCode = 0;
        out.statusMessage = _active->failureReason;
    } else {
        out = std::move(_active->resp);
    }

    _active.reset();
    return out;
}

void WebDAVConnection::onDataReceived(const std::vector<uint8_t>& bytes) {
    // Guard lifetime across callback execution
    _inCallback.fetch_add(1, std::memory_order_acq_rel);
    struct Guard
    {
        std::atomic<int>& c;
        ~Guard() {
            c.fetch_sub(1, std::memory_order_acq_rel);
        }
    } guard{_inCallback};
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // ignore incoming data during shutdown
    }

    std::lock_guard<std::mutex> lk(_reqMutex);

    if (_activeStream) {
        // If parser is paused due to backpressure, buffer incoming bytes until resumed
        if (_activeStream->parserPaused) {
            _pausedRemainder.insert(_pausedRemainder.end(), bytes.begin(), bytes.end());
            return;
        }

        const char* dataPtr = reinterpret_cast<const char*>(bytes.data());
        size_t dataLen = bytes.size();
        auto err = llhttp_execute(&_parser, dataPtr, dataLen);
        if (err == HPE_PAUSED) {
            // Store unconsumed portion for later when consumer frees space
            const char* pos = llhttp_get_error_pos(&_parser);
            size_t consumed = 0;
            if (pos && pos >= dataPtr && pos <= dataPtr + dataLen) {
                consumed = static_cast<size_t>(pos - dataPtr);
            }
            if (consumed < dataLen) {
                _pausedRemainder.insert(_pausedRemainder.end(), bytes.begin() + consumed, bytes.end());
            }
            _activeStream->parserPaused = true;
        } else if (err != HPE_OK) {
            _activeStream->failed = true;
            _activeStream->failureReason = std::string("llhttp error: ") + llhttp_errno_name(err);
            _activeStream->cv.notify_all();
        }
        return;
    }

    if (_active) {
        auto err = llhttp_execute(&_parser, reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (err != HPE_OK && err != HPE_PAUSED) {
            _active->failed = true;
            _active->failureReason = std::string("llhttp error: ") + llhttp_errno_name(err);
            _active->cv.notify_one();
        }
        return;
    }

    // No active request: accumulate leftover up to a small cap
    constexpr size_t ABS_CAP = 1u << 20;  // 1 MiB
    const size_t cap = std::min<std::size_t>(ABS_CAP, _cfg.maxBodyBytes / 4);
    if (_leftover.size() + bytes.size() > cap) {
        _leftover.clear();
        return;
    }
    _leftover.insert(_leftover.end(), bytes.begin(), bytes.end());
}

WebDAVConnection::Response WebDAVConnection::get(const std::string& path,
                                                 const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    auto req = buildRequest("GET", path, extraHeaders, {});
    return sendAndReceive(std::move(req));
}

WebDAVConnection::Response WebDAVConnection::head(
    const std::string& path, const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    auto req = buildRequest("HEAD", path, extraHeaders, {});
    return sendAndReceive(std::move(req));
}

WebDAVConnection::Response WebDAVConnection::propfind(const std::string& path, int depth, const std::string& bodyXml) {
    std::vector<std::pair<std::string, std::string>> hdrs{{"Depth", std::to_string(depth)},
                                                          {"Content-Type", "application/xml; charset=utf-8"},
                                                          {"Accept", "application/xml, text/xml; q=0.9, */*; q=0.1"}};
    auto req = buildRequest("PROPFIND", path, hdrs, bodyXml);
    return sendAndReceive(std::move(req));
}

WebDAVConnection::StreamHandle WebDAVConnection::openGetStream(const std::string& path, const StreamConfig& cfg) {
    std::unique_lock<std::mutex> lk(_reqMutex);

    // Only one active operation at a time per connection
    if (_active || _activeStream) {
        throw std::runtime_error("Another HTTP request is already active on this connection");
    }

    _activeStream = std::make_shared<StreamState>();
    _activeStream->capacity = std::max<size_t>(cfg.bufferBytes, 64 * 1024);
    _activeStream->buf.resize(_activeStream->capacity);
    _activeStream->head = 0;
    _activeStream->tail = 0;
    _activeStream->size = 0;
    _activeStream->maxBodyBytes = _cfg.maxBodyBytes;

    llhttp_init(&_parser, HTTP_RESPONSE, &_streamSettings);
    _parser.data = _activeStream.get();

    // Feed leftover bytes, if any
    if (!_leftover.empty()) {
        const char* dataPtr = reinterpret_cast<const char*>(_leftover.data());
        size_t dataLen = _leftover.size();
        auto err = llhttp_execute(&_parser, dataPtr, dataLen);
        if (err == HPE_PAUSED) {
            // Compute unconsumed remainder starting from error pos
            const char* pos = llhttp_get_error_pos(&_parser);
            size_t consumed = 0;
            if (pos && pos >= dataPtr && pos <= dataPtr + dataLen) {
                consumed = static_cast<size_t>(pos - dataPtr);
            }
            if (consumed < dataLen) {
                _pausedRemainder.insert(_pausedRemainder.end(), _leftover.begin() + consumed, _leftover.end());
            }
            _activeStream->parserPaused = true;
        } else if (err != HPE_OK) {
            _activeStream->failed = true;
            _activeStream->failureReason = std::string("llhttp error on leftover: ") + llhttp_errno_name(err);
            _activeStream->cv.notify_all();
        }
        _leftover.clear();
    }

    // Build and send the request without holding the mutex to avoid deadlock with callback
    std::vector<std::pair<std::string, std::string>> hdrs = cfg.headers;  // copy
    auto req = buildRequest("GET", path, hdrs, {});
    lk.unlock();
    auto sendResult = _conn->send(std::vector<uint8_t>(req.begin(), req.end()));
    lk.lock();
    if (!sendResult.success()) {
        _activeStream->failed = true;
        _activeStream->failureReason = "send failed: " + sendResult.errorMessage;
        _activeStream->cv.notify_all();
    }

    return StreamHandle(_activeStream);
}

void WebDAVConnection::resumeIfPaused() {
    std::lock_guard<std::mutex> lk(_reqMutex);
    if (!_activeStream) return;
    if (!_activeStream->parserPaused) return;

    // Attempt to resume parser
    _activeStream->parserPaused = false;
    llhttp_resume(&_parser);

    if (!_pausedRemainder.empty()) {
        const char* dataPtr = reinterpret_cast<const char*>(_pausedRemainder.data());
        size_t dataLen = _pausedRemainder.size();
        auto err = llhttp_execute(&_parser, dataPtr, dataLen);
        if (err == HPE_PAUSED) {
            // Compute consumed part and keep the rest for next resume
            const char* pos = llhttp_get_error_pos(&_parser);
            size_t consumed = 0;
            if (pos && pos >= dataPtr && pos <= dataPtr + dataLen) {
                consumed = static_cast<size_t>(pos - dataPtr);
            }
            if (consumed > 0) {
                _pausedRemainder.erase(_pausedRemainder.begin(), _pausedRemainder.begin() + consumed);
            }
            _activeStream->parserPaused = true;  // still paused
        } else if (err != HPE_OK) {
            _activeStream->failed = true;
            _activeStream->failureReason = std::string("llhttp error on resume: ") + llhttp_errno_name(err);
            _activeStream->cv.notify_all();
        } else {
            // All paused remainder was processed
            _pausedRemainder.clear();
        }
    }
}

void WebDAVConnection::abortActiveRequest() {
    std::lock_guard<std::mutex> lk(_reqMutex);
    if (_active) {
        _active->failed = true;
        _active->failureReason = "aborted";
        _active->cv.notify_one();
        _active.reset();
    }
    if (_activeStream) {
        _activeStream->failed = true;
        _activeStream->failureReason = "aborted";
        _activeStream->cv.notify_all();
        _activeStream.reset();
    }
}

}  // namespace EntropyEngine::Networking::WebDAV
