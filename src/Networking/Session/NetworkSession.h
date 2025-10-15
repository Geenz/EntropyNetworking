/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "../Transport/NetworkConnection.h"
#include "../Protocol/MessageSerializer.h"
#include "../Core/PropertyRegistry.h"
#include "../Core/ErrorCodes.h"
#include <EntropyCore.h>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

namespace EntropyEngine::Networking {

/**
 * NetworkSession - High-level session that manages a peer connection
 *
 * Wraps a NetworkConnection and provides protocol-level functionality:
 * - Message serialization/deserialization
 * - Automatic routing (reliable vs unreliable channels)
 * - Property registry management
 * - Protocol message callbacks
 */
class NetworkSession : public Core::EntropyObject {
public:
    // Message type callbacks
    using EntityCreatedCallback = std::function<void(uint64_t entityId, const std::string& appId,
                                                      const std::string& typeName, uint64_t parentId)>;
    using EntityDestroyedCallback = std::function<void(uint64_t entityId)>;
    using PropertyUpdateCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using SceneSnapshotCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using ErrorCallback = std::function<void(NetworkError error, const std::string& message)>;

    NetworkSession(NetworkConnection* connection);
    ~NetworkSession() override;

    // Connection management
    Result<void> connect();
    Result<void> disconnect();
    bool isConnected() const;
    ConnectionState getState() const;

    // Send protocol messages
    Result<void> sendEntityCreated(uint64_t entityId, const std::string& appId,
                                   const std::string& typeName, uint64_t parentId);
    Result<void> sendEntityDestroyed(uint64_t entityId);
    Result<void> sendPropertyUpdate(uint64_t entityId, const std::string& propertyName,
                                    const PropertyValue& value);
    Result<void> sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData);
    Result<void> sendSceneSnapshot(const std::vector<uint8_t>& snapshotData);

    // Message callbacks
    void setEntityCreatedCallback(EntityCreatedCallback callback);
    void setEntityDestroyedCallback(EntityDestroyedCallback callback);
    void setPropertyUpdateCallback(PropertyUpdateCallback callback);
    void setSceneSnapshotCallback(SceneSnapshotCallback callback);
    void setErrorCallback(ErrorCallback callback);

    // Property registry access
    PropertyRegistry& getPropertyRegistry() { return _propertyRegistry; }
    const PropertyRegistry& getPropertyRegistry() const { return _propertyRegistry; }

    // Statistics
    ConnectionStats getStats() const;

private:
    void onMessageReceived(const std::vector<uint8_t>& data);
    void onConnectionStateChanged(ConnectionState state);
    void handleReceivedMessage(const std::vector<uint8_t>& data);

    NetworkConnection* _connection; // Not owned, managed by ConnectionManager
    PropertyRegistry _propertyRegistry;

    // Callbacks
    EntityCreatedCallback _entityCreatedCallback;
    EntityDestroyedCallback _entityDestroyedCallback;
    PropertyUpdateCallback _propertyUpdateCallback;
    SceneSnapshotCallback _sceneSnapshotCallback;
    ErrorCallback _errorCallback;

    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
    mutable std::mutex _mutex;
};

} // namespace EntropyEngine::Networking
