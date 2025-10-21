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
#include "Networking/HTTP/HttpConnection.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <optional>

namespace EntropyEngine::Networking::HTTP {

struct ClientConfig {
    size_t maxConnectionsPerHost = 6;
};

// Simple TCP socket pool keyed by host:port
class SocketPool {
public:
    explicit SocketPool(size_t maxPerHost) : _maxPerHost(maxPerHost) {}

    // Acquire a connected socket to host:port, creating a new one if needed
    int acquire(const std::string& host, uint16_t port, const RequestOptions& opts, bool wait);
    void release(const std::string& host, uint16_t port, int sock, bool keepAlive);
    void shutdown();

private:
    struct HostKey {
        std::string host;
        uint16_t port;
        bool operator==(const HostKey& o) const { return port==o.port && host==o.host; }
    };
    struct HostKeyHash { size_t operator()(const HostKey& k) const { return std::hash<std::string>{}(k.host) ^ (std::hash<uint16_t>{}(k.port)<<1); } };

    struct Bucket {
        std::vector<int> idle; // sockets ready for reuse
        size_t total = 0;      // idle + busy
    };

    size_t _maxPerHost;
    std::mutex _m;
    std::unordered_map<HostKey, Bucket, HostKeyHash> _buckets;
    bool _shutdown = false;

    static int openSocket(const std::string& host, uint16_t port, const RequestOptions& opts);
};

class HttpClient {
public:
    explicit HttpClient(ClientConfig cfg = {}) : _cfg(cfg), _pool(cfg.maxConnectionsPerHost) {}

    // Aggregated execute (http only, no TLS yet). host string may contain ":port"; if none provided, default 80.
    HttpResponse execute(const HttpRequest& req, const RequestOptions& opts = {});

    void closeIdle() { _pool.shutdown(); }

private:
    ClientConfig _cfg;
    SocketPool _pool;
    HttpConnection _conn; // stateless helper

    static std::pair<std::string,uint16_t> splitHostPort(const std::string& host);
};

} // namespace EntropyEngine::Networking::HTTP
