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
#include <sstream>

#include "EntropyCore.h"

namespace EntropyEngine::Networking::WebDAV {

static inline void toLowerInPlace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
}

// llhttp C callbacks
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
    pr->resp.body.insert(pr->resp.body.end(),
                         reinterpret_cast<const uint8_t*>(at),
                         reinterpret_cast<const uint8_t*>(at) + len);
    return 0;
}

static int on_message_complete_cb(llhttp_t* p) {
    auto* pr = static_cast<WebDAVConnection::PendingResponse*>(p->data);
    pr->done = true;
    pr->cv.notify_one();
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

    // Install permanent callback that feeds data into the parser of the active request
    _conn->setMessageCallback([this](const std::vector<uint8_t>& bytes){ onDataReceived(bytes); });
}

std::string WebDAVConnection::buildRequest(const char* method,
                                           const std::string& path,
                                           const std::vector<std::pair<std::string,std::string>>& headers,
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
        auto err = llhttp_execute(&_parser,
                                  reinterpret_cast<const char*>(_leftover.data()),
                                  _leftover.size());
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
    const bool timedout = !_active->cv.wait_for(lk, _cfg.requestTimeout, [this]{
        return _active->done || _active->failed; });

    Response out;
    if (timedout) {
        out.statusCode = 0; out.statusMessage = "timeout";
    } else if (_active->failed) {
        out.statusCode = 0; out.statusMessage = _active->failureReason;
    } else {
        out = std::move(_active->resp);
    }

    _active.reset();
    return out;
}

void WebDAVConnection::onDataReceived(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(_reqMutex);
    if (!_active) {
        _leftover.insert(_leftover.end(), bytes.begin(), bytes.end());
        return;
    }

    auto err = llhttp_execute(&_parser, reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (err != HPE_OK && err != HPE_PAUSED) {
        _active->failed = true;
        _active->failureReason = std::string("llhttp error: ") + llhttp_errno_name(err);
        _active->cv.notify_one();
    }
}

WebDAVConnection::Response WebDAVConnection::get(const std::string& path,
                 const std::vector<std::pair<std::string,std::string>>& extraHeaders) {
    auto req = buildRequest("GET", path, extraHeaders, {});
    return sendAndReceive(std::move(req));
}

WebDAVConnection::Response WebDAVConnection::head(const std::string& path,
                  const std::vector<std::pair<std::string,std::string>>& extraHeaders) {
    auto req = buildRequest("HEAD", path, extraHeaders, {});
    return sendAndReceive(std::move(req));
}

WebDAVConnection::Response WebDAVConnection::propfind(const std::string& path, int depth, const std::string& bodyXml) {
    std::vector<std::pair<std::string,std::string>> hdrs{
        {"Depth", std::to_string(depth)},
        {"Content-Type", "application/xml; charset=utf-8"},
        {"Accept", "application/xml, text/xml; q=0.9, */*; q=0.1"}
    };
    auto req = buildRequest("PROPFIND", path, hdrs, bodyXml);
    return sendAndReceive(std::move(req));
}

} // namespace EntropyEngine::Networking::WebDAV
