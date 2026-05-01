/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../Core/ConnectionTypes.h"
#include "NetworkConnection.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#endif

namespace EntropyEngine::Networking
{

#if defined(__APPLE__)

/**
 * @brief XPC-based connection for iOS/visionOS/macOS IPC
 *
 * XPCConnection provides Inter-Process Communication using Apple's XPC framework.
 * This is the primary IPC mechanism on iOS/visionOS/iPadOS where Unix sockets
 * are unavailable due to sandboxing.
 *
 * Platform support:
 * - iOS/iPadOS/tvOS/watchOS/visionOS: Required (no Unix sockets)
 * - macOS: Optional (Unix sockets also available)
 * - Catalyst: Supported
 *
 * Two connection modes:
 * 1. Named service: Connect to XPC service by bundle ID
 *    Example: "com.example.MyApp.networking"
 *
 * 2. Anonymous endpoint: Connect to XPC endpoint from accept()
 *    Used by XPCServer to wrap accepted connections
 *
 * Message format:
 * - Wraps vector<uint8_t> in XPC dictionary with "payload" key
 * - Uses xpc_data_t for efficient binary transfer
 *
 * Thread safety:
 * - Uses GCD (Grand Central Dispatch) for event handling
 * - All callbacks invoked on private serial queue
 * - Thread-safe from external callers
 */
class XPCConnection : public NetworkConnection
{
public:
    /**
     * @brief Client-side constructor: connects to named XPC service
     * @param serviceName Bundle ID of XPC service (e.g., "com.example.MyApp.xpc")
     */
    explicit XPCConnection(std::string serviceName);

    /**
     * @brief Client-side constructor with configuration
     */
    XPCConnection(std::string serviceName, const struct ConnectionConfig* cfg);

    /**
     * @brief Server-side constructor: wraps accepted XPC connection
     * @param connection XPC connection from xpc_connection_create()
     * @param peerInfo Description of peer for debugging
     */
    XPCConnection(xpc_connection_t connection, std::string peerInfo);

    ~XPCConnection() override;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override;
    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
    ConnectionState getState() const override;
    ConnectionType getType() const override {
        return ConnectionType::Local;
    }
    ConnectionStats getStats() const override;

    // XPC-specific request/response API (Apple-only)
#if defined(__APPLE__)
    Result<std::vector<uint8_t>> sendWithReply(const std::vector<uint8_t>& data, std::chrono::milliseconds timeout);
#endif

    // EntropyObject interface
    const char* className() const noexcept override {
        return "XPCConnection";
    }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    std::string _serviceName;
    xpc_connection_t _connection{nullptr};
    dispatch_queue_t _queue{nullptr};

    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
    std::atomic<bool> _shouldStop{false};

    // Configuration (limits)
    size_t _maxMessageSize{64ull * 1024ull * 1024ull};
    int _defaultReplyTimeoutMs{5000};

    // Statistics (atomic for thread-safety)
    std::atomic<uint64_t> _bytesSent{0};
    std::atomic<uint64_t> _bytesReceived{0};
    std::atomic<uint64_t> _messagesSent{0};
    std::atomic<uint64_t> _messagesReceived{0};
    std::atomic<uint64_t> _connectTime{0};
    std::atomic<uint64_t> _lastActivityTime{0};

    // Helper methods
    void setupEventHandler();
    void handleMessage(xpc_object_t message);
    void handleError(xpc_object_t error);
    void setState(ConnectionState newState);
};

#endif  // __APPLE__

}  // namespace EntropyEngine::Networking
