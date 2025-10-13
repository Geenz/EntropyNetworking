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
#include "../Core/ErrorCodes.h"
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <string>

namespace EntropyEngine::Networking {

using ConnectionId = uint64_t;

/**
 * ConnectionManager - Manages all network connections
 *
 * Thread-safe manager for creating, tracking, and destroying connections.
 * Uses ref counting through NetworkConnection's EntropyObject base.
 */
class ConnectionManager {
public:
    ConnectionManager() = default;
    ~ConnectionManager();

    // Connection creation
    Result<ConnectionId> createUnixSocketConnection(const std::string& socketPath);
    Result<ConnectionId> createWebRTCConnection(
        const struct WebRTCConfig& config,
        const struct SignalingCallbacks& callbacks,
        const std::string& dataChannelLabel = "entropy-data"
    );

    // Connection access
    NetworkConnection* getConnection(ConnectionId id);
    Result<void> removeConnection(ConnectionId id);

    // Bulk operations
    void disconnectAll();
    size_t getConnectionCount() const;
    std::vector<ConnectionId> getAllConnectionIds() const;

private:
    ConnectionId generateConnectionId();

    mutable std::shared_mutex _mutex;
    std::unordered_map<ConnectionId, NetworkConnection*> _connections;
    std::atomic<ConnectionId> _nextConnectionId{1};
};

} // namespace EntropyEngine::Networking
