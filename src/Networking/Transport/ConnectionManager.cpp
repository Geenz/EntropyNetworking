/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ConnectionManager.h"
#include "UnixSocketConnection.h"
#include "WebRTCConnection.h"

namespace EntropyEngine::Networking {

ConnectionManager::~ConnectionManager() {
    disconnectAll();
}

Result<ConnectionId> ConnectionManager::createUnixSocketConnection(const std::string& socketPath) {
    auto* connection = new UnixSocketConnection(socketPath);
    connection->retain(); // Initial ref count for the manager

    ConnectionId id = generateConnectionId();

    {
        std::unique_lock lock(_mutex);
        _connections[id] = connection;
    }

    return Result<ConnectionId>::ok(id);
}

Result<ConnectionId> ConnectionManager::createWebRTCConnection(
    const WebRTCConfig& config,
    const SignalingCallbacks& callbacks,
    const std::string& dataChannelLabel
) {
    auto* connection = new WebRTCConnection(config, callbacks, dataChannelLabel);
    connection->retain(); // Initial ref count for the manager

    ConnectionId id = generateConnectionId();

    {
        std::unique_lock lock(_mutex);
        _connections[id] = connection;
    }

    return Result<ConnectionId>::ok(id);
}

NetworkConnection* ConnectionManager::getConnection(ConnectionId id) {
    std::shared_lock lock(_mutex);

    auto it = _connections.find(id);
    if (it == _connections.end()) {
        return nullptr;
    }

    auto* connection = it->second;
    connection->tryRetain(); // Caller must release when done
    return connection;
}

Result<void> ConnectionManager::removeConnection(ConnectionId id) {
    std::unique_lock lock(_mutex);

    auto it = _connections.find(id);
    if (it == _connections.end()) {
        return Result<void>::err(NetworkError::EntityNotFound, "Connection not found");
    }

    auto* connection = it->second;
    connection->disconnect();
    connection->release(); // Release manager's ref count

    _connections.erase(it);

    return Result<void>::ok();
}

void ConnectionManager::disconnectAll() {
    std::unique_lock lock(_mutex);

    for (auto& [id, connection] : _connections) {
        connection->disconnect();
        connection->release();
    }

    _connections.clear();
}

size_t ConnectionManager::getConnectionCount() const {
    std::shared_lock lock(_mutex);
    return _connections.size();
}

std::vector<ConnectionId> ConnectionManager::getAllConnectionIds() const {
    std::shared_lock lock(_mutex);

    std::vector<ConnectionId> ids;
    ids.reserve(_connections.size());

    for (const auto& [id, _] : _connections) {
        ids.push_back(id);
    }

    return ids;
}

ConnectionId ConnectionManager::generateConnectionId() {
    return _nextConnectionId.fetch_add(1, std::memory_order_relaxed);
}

} // namespace EntropyEngine::Networking
