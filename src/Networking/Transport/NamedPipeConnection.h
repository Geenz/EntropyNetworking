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
#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine::Networking {

/**
 * NamedPipeConnection - Windows named pipe implementation
 *
 * Used for local IPC on Windows. Provides reliable, ordered message delivery
 * with simple length-prefixed framing: [uint32_t little-endian length][payload].
 */
class NamedPipeConnection : public NetworkConnection {
public:
    // Client-side constructor: connects to endpoint (\\\\.\\pipe\\NAME or full path)
    explicit NamedPipeConnection(std::string pipeName);
    // Client-side constructor with configuration
    NamedPipeConnection(std::string pipeName, const struct ConnectionConfig* cfg);

#ifdef _WIN32
    // Server-side constructor: wraps already-connected pipe HANDLE
    NamedPipeConnection(HANDLE connectedPipe, std::string peerInfo);
#endif

    ~NamedPipeConnection() override;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override { return _state.load() == ConnectionState::Connected; }

    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
    Result<void> trySend(const std::vector<uint8_t>& data) override;

    ConnectionState getState() const override { return _state.load(); }
    ConnectionType getType() const override { return ConnectionType::Local; }
    ConnectionStats getStats() const override;

private:
    void receiveLoop();
    Result<void> sendInternal(const std::vector<uint8_t>& data);

    std::wstring toWide(const std::string& s) const;

    std::string _pipeName;
#ifdef _WIN32
    HANDLE _pipe{INVALID_HANDLE_VALUE};
#endif
    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};

    std::thread _receiveThread;
    std::atomic<bool> _shouldStop{false};

    mutable std::mutex _sendMutex;

    // Configurable parameters (initialized from ConnectionConfig or defaults)
    int _connectTimeoutMs{5000};
    int _sendPollTimeoutMs{100};
    int _sendMaxPolls{20};
    int _recvIdlePollMs{-1};
    size_t _maxMessageSize{16ull * 1024ull * 1024ull};

    // Atomic stats to avoid data races between send/receive threads
    std::atomic<uint64_t> _bytesSent{0};
    std::atomic<uint64_t> _bytesReceived{0};
    std::atomic<uint64_t> _messagesSent{0};
    std::atomic<uint64_t> _messagesReceived{0};
    std::atomic<uint64_t> _connectTime{0};
    std::atomic<uint64_t> _lastActivityTime{0};
};

} // namespace EntropyEngine::Networking
