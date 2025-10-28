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
#include "../Core/ComponentSchemaRegistry.h"
#include "../Core/SchemaNackTracker.h"
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

    // Schema message callbacks
    using RegisterSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using QueryPublicSchemasResponseCallback = std::function<void(const std::vector<ComponentSchema>& schemas)>;
    using PublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using UnpublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using SchemaNackCallback = std::function<void(ComponentTypeHash typeHash, const std::string& reason, uint64_t timestamp)>;
    using SchemaAdvertisementCallback = std::function<void(ComponentTypeHash typeHash, const std::string& appId,
                                                            const std::string& componentName, uint32_t schemaVersion)>;

    /**
     * @brief Construct a NetworkSession
     * @param connection Network connection to wrap
     * @param externalRegistry Optional external PropertyRegistry to share across sessions.
     *                        If nullptr, creates an internal registry (for single-session use).
     * @param schemaRegistry Optional ComponentSchemaRegistry for schema operations.
     *                      If nullptr, schema operations will not be available.
     */
    NetworkSession(NetworkConnection* connection,
                   PropertyRegistry* externalRegistry = nullptr,
                   ComponentSchemaRegistry* schemaRegistry = nullptr);
    ~NetworkSession() override;

    // Connection management
    Result<void> connect();
    Result<void> disconnect();
    bool isConnected() const;
    ConnectionState getState() const;

    // Handshake
    Result<void> performHandshake(const std::string& clientType, const std::string& clientId);
    bool isHandshakeComplete() const { return _handshakeComplete; }

    // Diagnostics
    const std::string& getSessionId() const noexcept { return _sessionId; }

    // Send protocol messages
    Result<void> sendEntityCreated(uint64_t entityId, const std::string& appId,
                                   const std::string& typeName, uint64_t parentId,
                                   const std::vector<PropertyMetadata>& properties = {});
    Result<void> sendEntityDestroyed(uint64_t entityId);
    Result<void> sendPropertyUpdate(PropertyHash hash, PropertyType type,
                                    const PropertyValue& value);
    Result<void> sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData);
    Result<void> sendSceneSnapshot(const std::vector<uint8_t>& snapshotData);

    // Send schema protocol messages
    Result<void> sendRegisterSchema(const ComponentSchema& schema);
    Result<void> sendQueryPublicSchemas();
    Result<void> sendPublishSchema(ComponentTypeHash typeHash);
    Result<void> sendUnpublishSchema(ComponentTypeHash typeHash);
    Result<void> sendSchemaNack(ComponentTypeHash typeHash, const std::string& reason);
    Result<void> sendSchemaAdvertisement(ComponentTypeHash typeHash, const std::string& appId,
                                          const std::string& componentName, uint32_t schemaVersion);

    // Message callbacks
    void setEntityCreatedCallback(EntityCreatedCallback callback);
    void setEntityDestroyedCallback(EntityDestroyedCallback callback);
    void setPropertyUpdateCallback(PropertyUpdateCallback callback);
    void setSceneSnapshotCallback(SceneSnapshotCallback callback);
    void setErrorCallback(ErrorCallback callback);

    // Schema message callbacks
    void setRegisterSchemaResponseCallback(RegisterSchemaResponseCallback callback);
    void setQueryPublicSchemasResponseCallback(QueryPublicSchemasResponseCallback callback);
    void setPublishSchemaResponseCallback(PublishSchemaResponseCallback callback);
    void setUnpublishSchemaResponseCallback(UnpublishSchemaResponseCallback callback);
    void setSchemaNackCallback(SchemaNackCallback callback);
    void setSchemaAdvertisementCallback(SchemaAdvertisementCallback callback);

    // Property registry access (always valid after construction)
    PropertyRegistry& getPropertyRegistry() { return *_propertyRegistry; }
    const PropertyRegistry& getPropertyRegistry() const { return *_propertyRegistry; }

    // Schema registry access (may be nullptr if not configured)
    ComponentSchemaRegistry* getSchemaRegistry() { return _schemaRegistry; }
    const ComponentSchemaRegistry* getSchemaRegistry() const { return _schemaRegistry; }

    // Statistics
    ConnectionStats getStats() const;

    // Network diagnostics
    uint64_t getDuplicatePacketCount() const { return _duplicatePacketsReceived.load(std::memory_order_relaxed); }
    uint64_t getPacketLossEventCount() const { return _packetLossEvents.load(std::memory_order_relaxed); }
    uint64_t getSequenceUpdateFailureCount() const { return _sequenceUpdateFailures.load(std::memory_order_relaxed); }

private:
    static std::string generateSessionId();

    void onMessageReceived(const std::vector<uint8_t>& data);
    void onConnectionStateChanged(ConnectionState state);
    void handleReceivedMessage(const std::vector<uint8_t>& data);

    NetworkConnection* _connection; // Not owned, managed by ConnectionManager

    // Registry ownership model: external (non-owning) or internal (owned)
    PropertyRegistry* _propertyRegistry{nullptr};
    std::unique_ptr<PropertyRegistry> _ownedRegistry; // used when no external provided

    // Schema registry: external (non-owning), optional
    ComponentSchemaRegistry* _schemaRegistry{nullptr};

    // NACK tracking
    SchemaNackTracker _nackTracker;

    std::string _sessionId;

    // Callbacks
    EntityCreatedCallback _entityCreatedCallback;
    EntityDestroyedCallback _entityDestroyedCallback;
    PropertyUpdateCallback _propertyUpdateCallback;
    SceneSnapshotCallback _sceneSnapshotCallback;
    ErrorCallback _errorCallback;

    // Schema callbacks
    RegisterSchemaResponseCallback _registerSchemaResponseCallback;
    QueryPublicSchemasResponseCallback _queryPublicSchemasResponseCallback;
    PublishSchemaResponseCallback _publishSchemaResponseCallback;
    UnpublishSchemaResponseCallback _unpublishSchemaResponseCallback;
    SchemaNackCallback _schemaNackCallback;
    SchemaAdvertisementCallback _schemaAdvertisementCallback;

    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
    std::atomic<uint32_t> _nextSendSequence{0};
    std::atomic<uint32_t> _lastReceivedSequence{0};

    // Network diagnostics counters
    std::atomic<uint64_t> _duplicatePacketsReceived{0};
    std::atomic<uint64_t> _packetLossEvents{0};
    std::atomic<uint64_t> _sequenceUpdateFailures{0};  // CAS retry exhaustion

    // Handshake state
    bool _handshakeComplete{false};
    std::string _clientType;
    std::string _clientId;

    mutable std::mutex _mutex;
};

} // namespace EntropyEngine::Networking
