/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "NetworkConnection.h"
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

namespace EntropyEngine::Networking {

/**
 * UnixSocketConnection - Unix domain socket implementation
 *
 * Used for local IPC, particularly for signaling with local signaling server.
 * Provides reliable, ordered message delivery.
 */
class UnixSocketConnection : public NetworkConnection {
public:
    UnixSocketConnection(std::string socketPath);
    ~UnixSocketConnection() override;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override { return _state.load() == ConnectionState::Connected; }

    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;

    ConnectionState getState() const override { return _state.load(); }
    ConnectionType getType() const override { return ConnectionType::UnixSocket; }
    ConnectionStats getStats() const override;

private:
    void receiveLoop();
    Result<void> sendInternal(const std::vector<uint8_t>& data);

    std::string _socketPath;
    int _socket{-1};
    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};

    std::thread _receiveThread;
    std::atomic<bool> _shouldStop{false};

    mutable std::mutex _sendMutex;
    ConnectionStats _stats;
};

} // namespace EntropyEngine::Networking
