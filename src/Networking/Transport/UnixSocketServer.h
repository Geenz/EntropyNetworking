// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "LocalServer.h"
#include <string>
#include <atomic>

namespace EntropyEngine::Networking {

/**
 * @brief Unix domain socket server implementation
 *
 * Provides server-side Unix socket functionality for Linux/macOS.
 * Handles socket creation, binding, listening, and accepting connections.
 */
class UnixSocketServer : public LocalServer {
public:
    UnixSocketServer(ConnectionManager* connMgr, std::string socketPath);
    ~UnixSocketServer() override;

    // LocalServer interface
    Result<void> listen() override;
    ConnectionHandle accept() override;
    Result<void> close() override;
    bool isListening() const override { return _listening.load(std::memory_order_acquire); }

    // EntropyObject interface
    const char* className() const noexcept override { return "UnixSocketServer"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    ConnectionManager* _connMgr;
    std::string _socketPath;
    int _serverSocket{-1};
    std::atomic<bool> _listening{false};
};

} // namespace EntropyEngine::Networking
