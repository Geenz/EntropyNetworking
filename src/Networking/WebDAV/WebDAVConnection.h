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
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <atomic>

#include "Networking/Transport/NetworkConnection.h"
#include <llhttp.h>

namespace EntropyEngine::Networking::WebDAV {

class WebDAVConnection {
public:
    struct Config {
        std::string host;                      // required for Host header
        std::string userAgent = "EntropyWebDAV/1.0";
        std::string authHeader;                // optional: "Bearer ..." or "Basic ..."
        std::chrono::milliseconds requestTimeout{30000};
        size_t maxBodyBytes = 128ull * 1024 * 1024; // 128 MiB cap
    };

    struct Response {
        int statusCode = 0;
        std::string statusMessage;
        std::unordered_map<std::string, std::string> headers; // lowercase keys
        std::vector<uint8_t> body;
        bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
    };

    WebDAVConnection(std::shared_ptr<EntropyEngine::Networking::NetworkConnection> nc, Config cfg);
    ~WebDAVConnection();

    Response get(const std::string& path,
                 const std::vector<std::pair<std::string,std::string>>& extraHeaders = {});
    Response head(const std::string& path,
                  const std::vector<std::pair<std::string,std::string>>& extraHeaders = {});
    Response propfind(const std::string& path, int depth, const std::string& bodyXml);

    bool isConnected() const { return _conn && _conn->isConnected(); }

public:
    struct PendingResponse {
        std::condition_variable cv;
        bool done = false;
        bool failed = false;
        std::string failureReason;
        Response resp;
        std::string curHeaderField;  // accumulator for header field (may be chunked)
        std::string curHeaderValue;  // accumulator for header value (may be chunked)
        std::string statusText;      // accumulated status text
        size_t maxBodyBytes = 128ull * 1024 * 1024; // default cap; overridden per request
    };

private:
    std::shared_ptr<EntropyEngine::Networking::NetworkConnection> _conn;
    Config _cfg;

    std::mutex _reqMutex;                           // serialize requests and protect parser
    std::unique_ptr<PendingResponse> _active;       // state for current request
    llhttp_t _parser{};
    llhttp_settings_t _settings{};
    std::vector<uint8_t> _leftover;                 // bytes received when no request active

    // Shutdown/lifetime guards for receive callback
    std::atomic<bool> _shuttingDown{false};
    std::atomic<int>  _inCallback{0};

    void onDataReceived(const std::vector<uint8_t>& bytes);
    Response sendAndReceive(std::string request);
    std::string buildRequest(const char* method,
                             const std::string& path,
                             const std::vector<std::pair<std::string,std::string>>& headers,
                             const std::string& body);
};

} // namespace EntropyEngine::Networking::WebDAV
