/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace EntropyEngine::Networking::HTTP {

// Lowercase header keys map
using HttpHeaders = std::unordered_map<std::string, std::string>;

enum class HttpMethod {
    GET, HEAD, POST, PUT, DELETE_, OPTIONS, PATCH, PROPFIND
};

struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
    std::string scheme = "https"; // "http" or "https" - defaults to HTTPS for security
    std::string host;      // may include ":port"
    std::string path;      // origin-form path + optional query
    HttpHeaders headers;   // lowercase keys; Host/User-Agent auto-filled if missing
    std::vector<uint8_t> body;
};

struct HttpResponse {
    int statusCode = 0;
    std::string statusMessage;
    HttpHeaders headers;   // lowercase keys
    std::vector<uint8_t> body; // aggregated body

    bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
};

enum class ProxyPolicy { Auto, DirectOnly, ForceProxy };

struct RequestOptions {
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds writeTimeout{10000};
    std::chrono::milliseconds readIdleTimeout{15000};
    std::chrono::milliseconds totalDeadline{30000};
    size_t maxResponseBytes = 128ull * 1024ull * 1024ull;
    bool followRedirects = false; // not implemented yet
    // Proxy behaviour (auto = env/system detection; DirectOnly = bypass; ForceProxy = expect proxy)
    ProxyPolicy proxyPolicy = ProxyPolicy::Auto;
    std::optional<std::string> explicitProxy; // per-request override, e.g., "http://proxy:8080"
};

} // namespace EntropyEngine::Networking::HTTP
