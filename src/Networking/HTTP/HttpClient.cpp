/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "Networking/HTTP/HttpClient.h"
#include <sstream>
#include <chrono>
#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace EntropyEngine::Networking::HTTP {

// ---- SocketPool ----

int SocketPool::openSocket(const std::string& host, uint16_t port, const RequestOptions& opts) {
    (void)opts; // TODO: honor connect timeout via non-blocking + select if needed
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return -1;
    }

    // Timeouts (5s) as safety net
#ifdef _WIN32
    DWORD timeoutMs = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
#else
    timeval tv{}; tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    return sock;
}

int SocketPool::acquire(const std::string& host, uint16_t port, const RequestOptions& opts, bool wait) {
    HostKey key{host, port};
    std::unique_lock<std::mutex> lk(_m);
    while (true) {
        if (_shutdown) return -1;
        Bucket& b = _buckets[key];
        if (!b.idle.empty()) {
            int s = b.idle.back(); b.idle.pop_back();
            return s;
        }
        if (b.total < _maxPerHost) {
            // create outside lock
            lk.unlock();
            int s = openSocket(host, port, opts);
            lk.lock();
            if (s >= 0) { ++b.total; return s; }
            if (!wait) return -1;
        } else {
            if (!wait) return -1;
        }
        // brief wait before retry
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        lk.lock();
    }
}

void SocketPool::release(const std::string& host, uint16_t port, int sock, bool keepAlive) {
    HostKey key{host, port};
    std::lock_guard<std::mutex> lk(_m);
    auto it = _buckets.find(key);
    if (it == _buckets.end()) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return;
    }
    Bucket& b = it->second;
    if (keepAlive && !_shutdown) {
        b.idle.push_back(sock);
    } else {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        if (b.total > 0) --b.total;
    }
}

void SocketPool::shutdown() {
    std::lock_guard<std::mutex> lk(_m);
    _shutdown = true;
    for (auto& kv : _buckets) {
        for (int s : kv.second.idle) {
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
        }
        kv.second.idle.clear();
        kv.second.total = 0;
    }
}

// ---- HttpClient ----

static void toLowerInPlace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
}

std::pair<std::string,uint16_t> HttpClient::splitHostPort(const std::string& host) {
    auto pos = host.find(':');
    if (pos == std::string::npos) return {host, (uint16_t)80};
    std::string h = host.substr(0, pos);
    uint16_t port = (uint16_t)std::stoi(host.substr(pos+1));
    return {h, port};
}

HttpResponse HttpClient::execute(const HttpRequest& inReq, const RequestOptions& opts) {
    HttpRequest req = inReq; // copy to modify headers
    // normalize header keys to lowercase
    HttpHeaders lower;
    for (auto& kv : req.headers) {
        std::string k = kv.first; toLowerInPlace(k);
        lower[k] = kv.second;
    }
    req.headers.swap(lower);

    auto [hostOnly, port] = splitHostPort(req.host);

    // Acquire a socket (wait if pool exhausted)
    int sock = _pool.acquire(hostOnly, port, opts, /*wait*/true);
    if (sock < 0) {
        return HttpResponse{0, "socket acquire failed", {}, {}};
    }

    HttpResponse resp;
    bool keep = _conn.executeOnSocket(sock, req, opts, resp);

    // Release according to keepAlive decision
    _pool.release(hostOnly, port, sock, keep);

    return resp;
}

} // namespace EntropyEngine::Networking::HTTP
