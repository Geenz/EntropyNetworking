/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <EntropyCore.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "../Core/ComponentSchemaRegistry.h"
#include "../Core/ErrorCodes.h"
#include "../Core/PropertyRegistry.h"
#include "../Core/SchemaNackPolicy.h"
#include "../Core/SchemaNackTracker.h"
#include "../Protocol/MessageSerializer.h"
#include "../Transport/NetworkConnection.h"

namespace EntropyEngine::Networking
{

/**
 * NetworkSession - High-level session that manages a peer connection
 *
 * Wraps a NetworkConnection and provides protocol-level functionality:
 * - Message serialization/deserialization
 * - Automatic routing (reliable vs unreliable channels)
 * - Property registry management
 * - Protocol message callbacks
 */
class NetworkSession : public Core::EntropyObject
{
    friend class SessionManager;  // Allow SessionManager to register callbacks via fan-out
public:
    // Message type callbacks
    using EntityCreatedCallback = std::function<void(uint64_t entityId, const std::string& appId,
                                                     const std::string& typeName, uint64_t parentId)>;
    using EntityDestroyedCallback = std::function<void(uint64_t entityId)>;
    using PropertyUpdateCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using SceneSnapshotCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using HandshakeCallback = std::function<void(const std::string& clientType, const std::string& clientId)>;
    using ErrorCallback = std::function<void(NetworkError error, const std::string& message)>;

    // Schema message callbacks
    using RegisterSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using QueryPublicSchemasResponseCallback = std::function<void(const std::vector<ComponentSchema>& schemas)>;
    using PublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using UnpublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using SchemaNackCallback =
        std::function<void(ComponentTypeHash typeHash, const std::string& reason, uint64_t timestamp)>;
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
    NetworkSession(NetworkConnection* connection, PropertyRegistry* externalRegistry = nullptr,
                   ComponentSchemaRegistry* schemaRegistry = nullptr);
    ~NetworkSession() override;

    // Connection management
    Result<void> connect();
    Result<void> disconnect();
    bool isConnected() const;
    ConnectionState getState() const;

    /**
     * @brief Set up connection callbacks to route messages to this session
     *
     * This method registers this session's message and state callbacks with the
     * underlying connection. SessionManager calls this automatically, but direct
     * users (like tests) must call it manually after construction.
     *
     * @note Must be called before any messages will be received
     */
    void setupCallbacks();

    // Handshake
    Result<void> performHandshake(const std::string& clientType, const std::string& clientId);
    bool isHandshakeComplete() const {
        return _handshakeComplete;
    }

    // Diagnostics
    const std::string& getSessionId() const noexcept {
        return _sessionId;
    }

    // Send protocol messages
    Result<void> sendEntityCreated(uint64_t entityId, const std::string& appId, const std::string& typeName,
                                   uint64_t parentId, const std::vector<PropertyMetadata>& properties = {});
    Result<void> sendEntityDestroyed(uint64_t entityId);
    Result<void> sendPropertyUpdate(PropertyHash hash, PropertyType type, const PropertyValue& value);
    Result<void> sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData);
    Result<void> sendSceneSnapshot(const std::vector<uint8_t>& snapshotData);

    // Send schema protocol messages
    Result<void> sendRegisterSchema(const ComponentSchema& schema);
    Result<void> sendQueryPublicSchemas();
    Result<void> sendPublishSchema(ComponentTypeHash typeHash);
    Result<void> sendUnpublishSchema(ComponentTypeHash typeHash);

    /**
     * @brief Send NACK for unknown schema (optional feedback)
     *
     * Sends a SchemaNack message to notify the peer about an unknown ComponentTypeHash.
     * This is OPTIONAL feedback controlled by SchemaNackPolicy:
     * - Only sent when SchemaNackPolicy::instance().isEnabled() == true
     * - Subject to per-schema rate limiting via SchemaNackTracker (default: 1000ms interval)
     * - Uses non-blocking reliable send path (MPSC queue)
     *
     * Typical usage: Called automatically by handleUnknownSchema() when processing
     * ENTITY_CREATED messages with unknown ComponentTypeHash values.
     *
     * @param typeHash Unknown ComponentTypeHash that triggered the NACK
     * @param reason Human-readable reason (e.g., "Schema not found in registry")
     * @return Result indicating success or error (Ok if policy disabled or rate limited)
     */
    Result<void> sendSchemaNack(ComponentTypeHash typeHash, const std::string& reason);

    Result<void> sendSchemaAdvertisement(ComponentTypeHash typeHash, const std::string& appId,
                                         const std::string& componentName, uint32_t schemaVersion);

    // Message callbacks
    void setEntityCreatedCallback(EntityCreatedCallback callback);
    void setEntityDestroyedCallback(EntityDestroyedCallback callback);
    void setPropertyUpdateCallback(PropertyUpdateCallback callback);
    void setSceneSnapshotCallback(SceneSnapshotCallback callback);
    void setHandshakeCallback(HandshakeCallback callback);
    void setErrorCallback(ErrorCallback callback);

    // Schema message callbacks
    void setRegisterSchemaResponseCallback(RegisterSchemaResponseCallback callback);
    void setQueryPublicSchemasResponseCallback(QueryPublicSchemasResponseCallback callback);
    void setPublishSchemaResponseCallback(PublishSchemaResponseCallback callback);
    void setUnpublishSchemaResponseCallback(UnpublishSchemaResponseCallback callback);
    void setSchemaNackCallback(SchemaNackCallback callback);
    void setSchemaAdvertisementCallback(SchemaAdvertisementCallback callback);

    // Property registry access (always valid after construction)
    PropertyRegistry& getPropertyRegistry() {
        return *_propertyRegistry;
    }
    const PropertyRegistry& getPropertyRegistry() const {
        return *_propertyRegistry;
    }

    // Schema registry access (may be nullptr if not configured)
    ComponentSchemaRegistry* getSchemaRegistry() {
        return _schemaRegistry;
    }
    const ComponentSchemaRegistry* getSchemaRegistry() const {
        return _schemaRegistry;
    }

    // Statistics
    ConnectionStats getStats() const;

    // Network diagnostics
    uint64_t getDuplicatePacketCount() const {
        return _duplicatePacketsReceived.load(std::memory_order_relaxed);
    }
    uint64_t getPacketLossEventCount() const {
        return _packetLossEvents.load(std::memory_order_relaxed);
    }
    uint64_t getSequenceUpdateFailureCount() const {
        return _sequenceUpdateFailures.load(std::memory_order_relaxed);
    }
    uint64_t getUnknownSchemaDropCount() const {
        return _unknownSchemaDrops.load(std::memory_order_relaxed);
    }

    // Property update batching
    void setBatchingEnabled(bool enabled);
    bool isBatchingEnabled() const {
        return _batchingEnabled.load(std::memory_order_relaxed);
    }
    Result<void> flushPropertyUpdates();

    struct PropertyBatchStats
    {
        uint64_t totalBatchesSent{0};
        uint64_t totalUpdatesSent{0};
        uint64_t updatesDeduped{0};
        uint64_t averageBatchSize{0};
    };
    PropertyBatchStats getPropertyBatchStats() const;
    size_t getPendingPropertyUpdateCount() const;

private:
    static std::string generateSessionId();

    void onMessageReceived(const std::vector<uint8_t>& data);
    void onConnectionStateChanged(ConnectionState state);
    void handleReceivedMessage(const std::vector<uint8_t>& data);
    void handleUnknownSchema(ComponentTypeHash typeHash);

    NetworkConnection* _connection;  // Not owned, managed by ConnectionManager

    // Registry ownership model: external (non-owning) or internal (owned)
    PropertyRegistry* _propertyRegistry{nullptr};
    std::unique_ptr<PropertyRegistry> _ownedRegistry;  // used when no external provided

    // Schema registry: external (non-owning), optional
    ComponentSchemaRegistry* _schemaRegistry{nullptr};

    // NACK tracking
    SchemaNackTracker _nackTracker;

    // Unknown schema logging rate limiter
    struct LogRateLimiter
    {
        std::unordered_map<ComponentTypeHash, std::chrono::steady_clock::time_point> lastLogTimes;
        std::mutex mutex;

        bool shouldLog(ComponentTypeHash typeHash, std::chrono::milliseconds interval) {
            std::lock_guard<std::mutex> lock(mutex);
            auto now = std::chrono::steady_clock::now();
            auto it = lastLogTimes.find(typeHash);

            if (it == lastLogTimes.end()) {
                lastLogTimes[typeHash] = now;
                return true;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            if (elapsed >= interval) {
                it->second = now;
                return true;
            }
            return false;
        }
    };
    LogRateLimiter _logRateLimiter;

    std::string _sessionId;

    // Callbacks
    EntityCreatedCallback _entityCreatedCallback;
    EntityDestroyedCallback _entityDestroyedCallback;
    PropertyUpdateCallback _propertyUpdateCallback;
    SceneSnapshotCallback _sceneSnapshotCallback;
    HandshakeCallback _handshakeCallback;
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
    std::atomic<uint64_t> _unknownSchemaDrops{0};      // Count of unknown schemas encountered

    // Handshake state
    std::atomic<bool> _handshakeComplete{false};
    std::string _clientType;
    std::string _clientId;

    // Shutdown coordination
    std::atomic<bool> _shuttingDown{false};
    std::atomic<uint32_t> _activeCallbacks{0};

    // Property update batching
    std::atomic<bool> _batchingEnabled{false};

    struct PendingPropertyUpdate
    {
        PropertyType type;
        PropertyValue value;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::unordered_map<PropertyHash, PendingPropertyUpdate> _pendingPropertyUpdates;
    mutable std::mutex _pendingUpdatesMutex;

    std::atomic<uint32_t> _batchSequenceNumber{0};

    // Batch statistics
    mutable std::mutex _batchStatsMutex;
    PropertyBatchStats _batchStats;

    mutable std::mutex _mutex;
};

}  // namespace EntropyEngine::Networking
