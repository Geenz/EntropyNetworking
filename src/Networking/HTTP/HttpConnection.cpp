/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "Networking/HTTP/HttpConnection.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace EntropyEngine::Networking::HTTP {

namespace {
struct ParsedResponse {
    int statusCode = 0;
    std::string statusMessage;
    HttpHeaders headers;
    std::vector<uint8_t> body;

    std::string curHeaderField;
    std::string curHeaderValue;
    bool complete = false;
};

static int on_status_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<ParsedResponse*>(p->data);
    pr->statusMessage.append(at, len);
    return 0;
}
static int on_header_field_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<ParsedResponse*>(p->data);
    if (!pr->curHeaderField.empty() && !pr->curHeaderValue.empty()) {
        std::string name = pr->curHeaderField;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        pr->headers[name] = pr->curHeaderValue;
        pr->curHeaderField.clear();
        pr->curHeaderValue.clear();
    }
    pr->curHeaderField.append(at, len);
    return 0;
}
static int on_header_value_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<ParsedResponse*>(p->data);
    pr->curHeaderValue.append(at, len);
    return 0;
}
static int on_headers_complete_cb(llhttp_t* p) {
    auto* pr = static_cast<ParsedResponse*>(p->data);
    if (!pr->curHeaderField.empty()) {
        std::string name = pr->curHeaderField;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        pr->headers[name] = pr->curHeaderValue;
        pr->curHeaderField.clear();
        pr->curHeaderValue.clear();
    }
    pr->statusCode = p->status_code;
    return 0;
}
static int on_body_cb(llhttp_t* p, const char* at, size_t len) {
    auto* pr = static_cast<ParsedResponse*>(p->data);
    pr->body.insert(pr->body.end(), reinterpret_cast<const uint8_t*>(at), reinterpret_cast<const uint8_t*>(at)+len);
    return 0;
}
static int on_message_complete_cb(llhttp_t* p) { auto* pr = static_cast<ParsedResponse*>(p->data); pr->complete = true; return 0; }

// Send all bytes, handling partial sends and EWOULDBLOCK/EINTR
static bool sendAll(int sock, const char* data, size_t len) {
    size_t sentTotal = 0;
    while (sentTotal < len) {
        int n = ::send(sock, data + sentTotal, (int)(len - sentTotal), 0);
        if (n > 0) {
            sentTotal += (size_t)n;
            continue;
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR) continue;
#else
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) continue;
#endif
        return false;
    }
    return true;
}
}

HttpConnection::HttpConnection() {
#ifdef _WIN32
    static bool wsaInit = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (!wsaInit) {
        WSADATA wsaData; WSAStartup(MAKEWORD(2,2), &wsaData); wsaInit = true;
    }
#endif
}

std::string HttpConnection::methodToString(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE_: return "DELETE";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::PROPFIND: return "PROPFIND";
        default: return "GET";
    }
}

void HttpConnection::toLowerInPlace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
}

bool HttpConnection::executeOnSocket(int sock, const HttpRequest& req, const RequestOptions& opts, HttpResponse& out) {
    (void)opts; // minimal initial implementation ignores per-request timeouts at the socket level

    // Build request text (origin-form target)
    std::ostringstream o;
    o << methodToString(req.method) << ' ' << (req.path.empty()? "/" : req.path) << " HTTP/1.1\r\n";
    // Host header: use req.host verbatim
    o << "Host: " << req.host << "\r\n";
    // Default UA
    if (req.headers.find("user-agent") == req.headers.end()) {
        o << "User-Agent: EntropyHTTP/1.0\r\n";
    }
    // Default Connection: keep-alive
    bool hasConn = (req.headers.find("connection") != req.headers.end());
    if (!hasConn) o << "Connection: keep-alive\r\n";

    // Additional headers
    for (auto& kv : req.headers) {
        std::string key = kv.first; // assume already lowercase; write as-is
        // Capitalization does not matter on wire; keep as provided
        o << key << ": " << kv.second << "\r\n";
    }

    if (!req.body.empty()) {
        o << "Content-Length: " << req.body.size() << "\r\n";
    }
    o << "\r\n";

    std::string head = o.str();

    // Send request
    if (!head.empty()) {
        if (!sendAll(sock, head.c_str(), head.size())) return false;
    }
    if (!req.body.empty()) {
        if (!sendAll(sock, reinterpret_cast<const char*>(req.body.data()), req.body.size())) return false;
    }

    // Parse response
    ParsedResponse pr;
    llhttp_settings_t settings{};
    llhttp_settings_init(&settings);
    settings.on_status = on_status_cb;
    settings.on_header_field = on_header_field_cb;
    settings.on_header_value = on_header_value_cb;
    settings.on_headers_complete = on_headers_complete_cb;
    settings.on_body = on_body_cb;
    settings.on_message_complete = on_message_complete_cb;

    llhttp_t parser{};
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    parser.data = &pr;

    std::vector<uint8_t> buf(8192);
    size_t total = 0;

    const auto deadline = std::chrono::steady_clock::now() + opts.totalDeadline;

    while (!pr.complete) {
        // Basic deadline check
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        int n = recv(sock, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0);
        if (n == 0) {
            // Peer closed connection; may indicate end-of-message when close-delimited
            llhttp_finish(&parser);
            break;
        }
        if (n < 0) {
            break; // timeout or error
        }
        total += (size_t)n;
        auto err = llhttp_execute(&parser, reinterpret_cast<const char*>(buf.data()), n);
        if (err != HPE_OK && err != HPE_PAUSED) {
            break;
        }
        // Stop if we exceed max response size safety bound
        if (total > opts.maxResponseBytes) {
            break;
        }
    }

    out.statusCode = pr.statusCode;
    out.statusMessage = pr.statusMessage;
    out.headers = std::move(pr.headers);
    out.body = std::move(pr.body);

    // Determine keep-alive: HTTP/1.1 default keep-alive unless Connection: close
    bool keep = true;
    auto it = out.headers.find("connection");
    if (it != out.headers.end()) {
        std::string v = it->second; toLowerInPlace(v);
        if (v.find("close") != std::string::npos) keep = false;
    }
    return keep;
}

namespace {
// Send all bytes over TLS, handling WANT_READ/WANT_WRITE and EINTR
static bool sendAllSsl(SSL* ssl, const char* data, size_t len) {
    size_t sentTotal = 0;
    while (sentTotal < len) {
        int n = ::SSL_write(ssl, data + sentTotal, (int)(len - sentTotal));
        if (n > 0) {
            sentTotal += (size_t)n;
            continue;
        }
        int err = ::SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // Retry; rely on socket timeouts to bound wait
            continue;
        }
        return false;
    }
    return true;
}
}

bool HttpConnection::executeOnSsl(SSL* ssl, const HttpRequest& req, const RequestOptions& opts, HttpResponse& out) {
    (void)opts;
    // Build request text
    std::ostringstream o;
    o << methodToString(req.method) << ' ' << (req.path.empty()? "/" : req.path) << " HTTP/1.1\r\n";
    o << "Host: " << req.host << "\r\n";
    if (req.headers.find("user-agent") == req.headers.end()) {
        o << "User-Agent: EntropyHTTP/1.0\r\n";
    }
    bool hasConn = (req.headers.find("connection") != req.headers.end());
    if (!hasConn) o << "Connection: keep-alive\r\n";
    for (auto& kv : req.headers) {
        o << kv.first << ": " << kv.second << "\r\n";
    }
    if (!req.body.empty()) {
        o << "Content-Length: " << req.body.size() << "\r\n";
    }
    o << "\r\n";
    std::string head = o.str();

    if (!sendAllSsl(ssl, head.c_str(), head.size())) return false;
    if (!req.body.empty()) {
        if (!sendAllSsl(ssl, reinterpret_cast<const char*>(req.body.data()), req.body.size())) return false;
    }

    // Parse response
    ParsedResponse pr;
    llhttp_settings_t settings{};
    llhttp_settings_init(&settings);
    settings.on_status = on_status_cb;
    settings.on_header_field = on_header_field_cb;
    settings.on_header_value = on_header_value_cb;
    settings.on_headers_complete = on_headers_complete_cb;
    settings.on_body = on_body_cb;
    settings.on_message_complete = on_message_complete_cb;

    llhttp_t parser{};
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    parser.data = &pr;

    std::vector<uint8_t> buf(8192);
    size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + opts.totalDeadline;

    while (!pr.complete) {
        if (std::chrono::steady_clock::now() > deadline) break;
        int n = ::SSL_read(ssl, reinterpret_cast<char*>(buf.data()), (int)buf.size());
        if (n == 0) {
            // TLS close notify or EOF
            llhttp_finish(&parser);
            break;
        }
        if (n < 0) {
            int err = ::SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue; // retry until deadline/timeout
            }
            break;
        }
        total += (size_t)n;
        auto perr = llhttp_execute(&parser, reinterpret_cast<const char*>(buf.data()), n);
        if (perr != HPE_OK && perr != HPE_PAUSED) {
            break;
        }
        if (total > opts.maxResponseBytes) break;
    }

    out.statusCode = pr.statusCode;
    out.statusMessage = pr.statusMessage;
    out.headers = std::move(pr.headers);
    out.body = std::move(pr.body);

    bool keep = true;
    auto it = out.headers.find("connection");
    if (it != out.headers.end()) {
        std::string v = it->second; toLowerInPlace(v);
        if (v.find("close") != std::string::npos) keep = false;
    }
    return keep;
}

} // namespace EntropyEngine::Networking::HTTP
