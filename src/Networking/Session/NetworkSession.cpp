/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NetworkSession.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "../Core/TimeUtils.h"
#include "../Protocol/ComponentSchemaSerializer.h"
#include "Networking/Protocol/entropy.capnp.h"

namespace EntropyEngine::Networking
{

std::string NetworkSession::generateSessionId() {
    static std::atomic<uint64_t> ctr{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "session-" + std::to_string(now) + "-" + std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
}

NetworkSession::NetworkSession(NetworkConnection* connection, PropertyRegistry* externalRegistry,
                               ComponentSchemaRegistry* schemaRegistry)
    : _connection(connection),
      _propertyRegistry(externalRegistry),
      _schemaRegistry(schemaRegistry),
      _sessionId(generateSessionId()) {
    // If no external registry provided, create and own one
    if (!_propertyRegistry) {
        _ownedRegistry = std::make_unique<PropertyRegistry>();
        _propertyRegistry = _ownedRegistry.get();
    }

    ENTROPY_LOG_DEBUG(std::format("NetworkSession: Constructor called with connection {}, session {}",
                                  (void*)_connection, (void*)this));

    if (_connection) {
        _connection->retain();

        // Note: Callbacks are NOT set here. SessionManager will register our callbacks
        // with ConnectionManager's fan-out system after construction.
    }

    // Start the message worker thread - this decouples receiving from processing
    // so slow callbacks don't block the receive path
    startMessageWorker();
}

NetworkSession::~NetworkSession() {
    // Set shutdown flag to prevent new callback invocations and new messages from being queued
    _shuttingDown.store(true, std::memory_order_release);

    // Stop the message worker thread - this will drain remaining messages
    stopMessageWorker();

    // Clear connection callbacks FIRST to prevent new invocations
    if (_connection) {
        _connection->setMessageCallback(nullptr);
        _connection->setStateCallback(nullptr);

        // Wait for active callbacks to complete
        while (_activeCallbacks.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }

        // Release our reference to the connection
        _connection->release();
        _connection = nullptr;
    }

    // Clear all callbacks - safe now that active count is zero
    _entityCreatedCallback = nullptr;
    _entityDestroyedCallback = nullptr;
    _propertyUpdateCallback = nullptr;
    _sceneSnapshotCallback = nullptr;
    _handshakeCallback = nullptr;
    _errorCallback = nullptr;
    _registerSchemaResponseCallback = nullptr;
    _queryPublicSchemasResponseCallback = nullptr;
    _publishSchemaResponseCallback = nullptr;
    _unpublishSchemaResponseCallback = nullptr;
    _schemaNackCallback = nullptr;
    _schemaAdvertisementCallback = nullptr;
    _heartbeatCallback = nullptr;
    _heartbeatResponseCallback = nullptr;
}

Result<void> NetworkSession::connect() {
    if (!_connection) {
        return Result<void>::err(NetworkError::InvalidParameter, "No connection");
    }

    return _connection->connect();
}

Result<void> NetworkSession::disconnect() {
    if (!_connection) {
        return Result<void>::ok();
    }

    return _connection->disconnect();
}

void NetworkSession::setupCallbacks() {
    if (!_connection) {
        return;
    }

    // Register message and state callbacks with the connection
    // This is what SessionManager does automatically, but direct users must call manually
    _connection->setMessageCallback([this](const std::vector<uint8_t>& data) { this->onMessageReceived(data); });

    _connection->setStateCallback([this](ConnectionState state) { this->onConnectionStateChanged(state); });
}

bool NetworkSession::isConnected() const {
    return _connection && _connection->isConnected();
}

ConnectionState NetworkSession::getState() const {
    return _state;
}

bool NetworkSession::supportsMultipleChannels() const {
    return _connection && _connection->supportsMultipleChannels();
}

Result<void> NetworkSession::openChannel(const std::string& channel) {
    if (!_connection) {
        return Result<void>::err(NetworkError::ConnectionClosed, "No connection");
    }
    return _connection->openChannel(channel);
}

Result<void> NetworkSession::performHandshake(const std::string& clientType, const std::string& clientId) {
    _clientType = clientType;
    _clientId = clientId;

    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto msg = builder.initRoot<Protocol::Message>();
        auto hs = msg.initHandshake();
        hs.setProtocolVersion(1);
        hs.setClientType(clientType);
        hs.setClientId(clientId);

        // Set capability flags
        hs.setSupportsSchemaMetadata(true);
        hs.setSupportsSchemaAck(true);
        hs.setSupportsSchemaAdvert(true);

        auto ser = serialize(builder);
        if (ser.failed()) {
            return Result<void>::err(ser.error, ser.errorMessage);
        }

        return _connection->send(ser.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendEntityCreated(uint64_t entityId, const std::string& appId, const std::string& typeName,
                                               uint64_t parentId, const std::vector<ComponentGroupData>& components,
                                               uint64_t targetSceneId, const std::string& entityName) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto ec = message.initEntityCreated();
        ec.setEntityId(entityId);
        ec.setAppId(appId);
        ec.setTypeName(typeName);
        ec.setParentId(parentId);
        ec.setTargetSceneId(targetSceneId);
        ec.setEntityName(entityName);

        // Build component groups
        auto componentList = ec.initComponents(components.size());
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& comp = components[i];
            auto cg = componentList[i];

            // Set component type hash
            auto th = cg.initTypeHash();
            th.setHigh(comp.typeHash.high);
            th.setLow(comp.typeHash.low);

            // Set component name
            cg.setComponentName(comp.componentName);

            // Build properties within this component
            auto propList = cg.initProperties(comp.properties.size());
            for (size_t j = 0; j < comp.properties.size(); ++j) {
                const auto& pm = comp.properties[j];
                auto pr = propList[j];
                auto ph = pr.initPropertyHash();
                ph.setHigh(pm.hash.high);
                ph.setLow(pm.hash.low);
                pr.setEntityId(pm.entityId);
                auto ct = pr.initComponentType();
                ct.setHigh(pm.componentType.high);
                ct.setLow(pm.componentType.low);
                pr.setPropertyName(pm.propertyName);
                pr.setType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(pm.type)));
                pr.setRegisteredAt(pm.registeredAt);
            }
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendComponentAdded(uint64_t entityId, const ComponentGroupData& component) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto ca = message.initComponentAdded();
        ca.setEntityId(entityId);

        // Build component group
        auto cg = ca.initComponent();
        auto th = cg.initTypeHash();
        th.setHigh(component.typeHash.high);
        th.setLow(component.typeHash.low);
        cg.setComponentName(component.componentName);

        // Build properties
        auto propList = cg.initProperties(component.properties.size());
        for (size_t i = 0; i < component.properties.size(); ++i) {
            const auto& pm = component.properties[i];
            auto pr = propList[i];
            auto ph = pr.initPropertyHash();
            ph.setHigh(pm.hash.high);
            ph.setLow(pm.hash.low);
            pr.setEntityId(pm.entityId);
            auto ct = pr.initComponentType();
            ct.setHigh(pm.componentType.high);
            ct.setLow(pm.componentType.low);
            pr.setPropertyName(pm.propertyName);
            pr.setType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(pm.type)));
            pr.setRegisteredAt(pm.registeredAt);
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendComponentRemoved(uint64_t entityId, ComponentTypeHash typeHash) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto cr = message.initComponentRemoved();
        cr.setEntityId(entityId);

        auto th = cr.initTypeHash();
        th.setHigh(typeHash.high);
        th.setLow(typeHash.low);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendEntityDestroyed(uint64_t entityId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto entityDestroyed = message.initEntityDestroyed();
        entityDestroyed.setEntityId(entityId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // EntityDestroyed goes on reliable channel
        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendPropertyUpdate(PropertyHash hash, PropertyType type, const PropertyValue& value) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    // Optional: validate against registry before send
    auto validateResult = _propertyRegistry->validatePropertyValue(hash, value);
    if (validateResult.failed()) {
        return validateResult;
    }

    // Check if batching is enabled
    if (_batchingEnabled.load(std::memory_order_relaxed)) {
        // Accumulate for batching
        std::lock_guard<std::mutex> lock(_pendingUpdatesMutex);

        // Check if this property already has a pending update (deduplication)
        auto it = _pendingPropertyUpdates.find(hash);
        if (it != _pendingPropertyUpdates.end()) {
            // Update exists, replace value
            it->second.value = value;
            it->second.timestamp = std::chrono::steady_clock::now();

            std::lock_guard<std::mutex> statsLock(_batchStatsMutex);
            _batchStats.updatesDeduped++;
        } else {
            // New update
            _pendingPropertyUpdates[hash] = PendingPropertyUpdate{type, value, std::chrono::steady_clock::now()};
        }

        return Result<void>::ok();
    }

    // Batching disabled - send immediately (original behavior)
    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto batch = message.initPropertyUpdateBatch();

        batch.setTimestamp(getCurrentTimestampMicros());
        batch.setSequence(_nextSendSequence.fetch_add(1, std::memory_order_relaxed));

        auto updates = batch.initUpdates(1);
        auto update = updates[0];

        auto ph = update.initPropertyHash();
        ph.setHigh(hash.high);
        ph.setLow(hash.low);
        update.setExpectedType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(type)));

        auto valueBuilder = update.initValue();
        std::visit(
            [&valueBuilder](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>)
                    valueBuilder.setInt32(v);
                else if constexpr (std::is_same_v<T, int64_t>)
                    valueBuilder.setInt64(v);
                else if constexpr (std::is_same_v<T, float>)
                    valueBuilder.setFloat32(v);
                else if constexpr (std::is_same_v<T, double>)
                    valueBuilder.setFloat64(v);
                else if constexpr (std::is_same_v<T, Vec2>) {
                    auto b = valueBuilder.initVec2();
                    b.setX(v.x);
                    b.setY(v.y);
                } else if constexpr (std::is_same_v<T, Vec3>) {
                    auto b = valueBuilder.initVec3();
                    b.setX(v.x);
                    b.setY(v.y);
                    b.setZ(v.z);
                } else if constexpr (std::is_same_v<T, Vec4>) {
                    auto b = valueBuilder.initVec4();
                    b.setX(v.x);
                    b.setY(v.y);
                    b.setZ(v.z);
                    b.setW(v.w);
                } else if constexpr (std::is_same_v<T, Quat>) {
                    auto b = valueBuilder.initQuat();
                    b.setX(v.x);
                    b.setY(v.y);
                    b.setZ(v.z);
                    b.setW(v.w);
                } else if constexpr (std::is_same_v<T, std::string>)
                    valueBuilder.setString(v);
                else if constexpr (std::is_same_v<T, bool>)
                    valueBuilder.setBool(v);
                else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                    valueBuilder.setBytes(kj::arrayPtr(v.data(), v.size()));
                else if constexpr (std::is_same_v<T, AssetId>)
                    valueBuilder.setAssetId(kj::arrayPtr(v.hash.data(), v.hash.size()));
                else if constexpr (std::is_same_v<T, std::vector<AssetId>>) {
                    auto arr = valueBuilder.initAssetIdArray(v.size());
                    for (size_t i = 0; i < v.size(); ++i) {
                        arr.set(i, kj::arrayPtr(v[i].hash.data(), v[i].hash.size()));
                    }
                }
            },
            value);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->sendUnreliable(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    // PropertyUpdateBatch goes on unreliable channel
    return _connection->sendUnreliable(batchData);
}

Result<void> NetworkSession::sendSceneSnapshot(const std::vector<uint8_t>& snapshotData) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    // SceneSnapshot goes on reliable channel
    return _connection->send(snapshotData);
}

Result<void> NetworkSession::sendRegisterSchema(const ComponentSchema& schema) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initRegisterSchemaRequest();

        // Serialize the schema using ComponentSchemaSerializer
        auto schemaBuilder = request.initSchema();
        serializeComponentSchema(schema, schemaBuilder);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Schema messages go on reliable channel
        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendQueryPublicSchemas() {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        message.initQueryPublicSchemasRequest();

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendPublishSchema(ComponentTypeHash typeHash) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initPublishSchemaRequest();

        auto hashBuilder = request.initTypeHash();
        hashBuilder.setHigh(typeHash.high);
        hashBuilder.setLow(typeHash.low);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendUnpublishSchema(ComponentTypeHash typeHash) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initUnpublishSchemaRequest();

        auto hashBuilder = request.initTypeHash();
        hashBuilder.setHigh(typeHash.high);
        hashBuilder.setLow(typeHash.low);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendSchemaNack(ComponentTypeHash typeHash, const std::string& reason) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    // Check global policy - if disabled, don't send NACK but still count it
    auto& policy = SchemaNackPolicy::instance();
    if (!policy.isEnabled()) {
        // Policy disabled - metrics counted, but no NACK sent
        return Result<void>::ok();
    }

    // Check if we should send NACK (rate limiting)
    if (!_nackTracker.shouldSendNack(typeHash)) {
        // Rate limited - silently skip
        return Result<void>::ok();
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto nack = message.initSchemaNack();

        auto hashBuilder = nack.initTypeHash();
        hashBuilder.setHigh(typeHash.high);
        hashBuilder.setLow(typeHash.low);
        nack.setReason(reason);
        nack.setTimestamp(getCurrentTimestampMicros());

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        auto result = _connection->send(serialized.value);
        if (result.success()) {
            // Record that we sent the NACK
            _nackTracker.recordNackSent(typeHash);
        }

        return result;

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendSchemaAdvertisement(ComponentTypeHash typeHash, const std::string& appId,
                                                     const std::string& componentName, uint32_t schemaVersion) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto advert = message.initSchemaAdvertisement();

        auto hashBuilder = advert.initTypeHash();
        hashBuilder.setHigh(typeHash.high);
        hashBuilder.setLow(typeHash.low);
        advert.setAppId(appId);
        advert.setComponentName(componentName);
        advert.setSchemaVersion(schemaVersion);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendHeartbeat() {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto heartbeat = message.initHeartbeat();

        heartbeat.setTimestamp(getCurrentTimestampMicros());

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Heartbeats go on reliable channel
        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendHeartbeatResponse(uint64_t clientTimestamp) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initHeartbeatResponse();

        response.setTimestamp(clientTimestamp);
        response.setServerTime(getCurrentTimestampMicros());

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Heartbeat responses go on reliable channel
        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

// ============================================================================
// Asset Protocol Messages
// ============================================================================

Result<void> NetworkSession::sendAssetAdvertise(const std::string& appId, const std::vector<AssetEntryData>& entries,
                                                uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetAdvertiseRequest();
        request.setAppId(appId);
        request.setRequestId(requestId);

        auto entriesList = request.initEntries(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            auto capnpEntry = entriesList[i];
            capnpEntry.setId(kj::arrayPtr(entry.id.data(), entry.id.size()));
            capnpEntry.setUri(entry.uri);
            capnpEntry.setContentType(entry.contentType);
            capnpEntry.setSizeBytes(entry.sizeBytes);
            capnpEntry.setEncrypted(entry.encrypted);
            capnpEntry.setPlaintextHash(kj::arrayPtr(entry.plaintextHash.data(), entry.plaintextHash.size()));
            capnpEntry.setAppId(entry.appId);
            capnpEntry.setPersistent(entry.persistent);
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetAdvertiseResponse(bool success, const std::string& errorMessage,
                                                        uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetAdvertiseResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetWithdraw(const std::vector<std::array<uint8_t, 32>>& assetIds,
                                               uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetWithdrawRequest();
        request.setRequestId(requestId);
        auto list = request.initAssetIds(assetIds.size());
        for (size_t i = 0; i < assetIds.size(); ++i) {
            list.set(i, kj::arrayPtr(assetIds[i].data(), 32));
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetWithdrawResponse(bool success, uint32_t removedCount,
                                                       const std::string& errorMessage, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetWithdrawResponse();
        response.setSuccess(success);
        response.setRemovedCount(removedCount);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetWithdrawAll(const std::string& appId, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetWithdrawAllRequest();
        request.setAppId(appId);
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetWithdrawAllResponse(bool success, uint32_t removedCount,
                                                          const std::string& errorMessage, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetWithdrawAllResponse();
        response.setSuccess(success);
        response.setRemovedCount(removedCount);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetResolve(const std::array<uint8_t, 32>& assetId, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetResolveRequest();
        request.setAssetId(kj::arrayPtr(assetId.data(), 32));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetResolveResponse(const AssetResolveResponseData& response) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto resp = message.initAssetResolveResponse();
        resp.setFound(response.found);
        resp.setRequestId(response.requestId);

        if (response.found) {
            auto entry = resp.initEntry();
            entry.setId(kj::arrayPtr(response.entry.id.data(), response.entry.id.size()));
            entry.setUri(response.entry.uri);
            entry.setContentType(response.entry.contentType);
            entry.setSizeBytes(response.entry.sizeBytes);
            entry.setEncrypted(response.entry.encrypted);
            entry.setPlaintextHash(
                kj::arrayPtr(response.entry.plaintextHash.data(), response.entry.plaintextHash.size()));
            entry.setAppId(response.entry.appId);
            entry.setPersistent(response.entry.persistent);
        }

        resp.setHasKey(response.hasKey);
        if (response.hasKey) {
            resp.setKey(kj::arrayPtr(response.key.data(), 32));
        }
        resp.setDeliveryMethod(response.deliveryMethod);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetResolveBatch(const std::vector<std::array<uint8_t, 32>>& assetIds,
                                                   uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetResolveBatchRequest();
        request.setRequestId(requestId);
        auto list = request.initAssetIds(assetIds.size());
        for (size_t i = 0; i < assetIds.size(); ++i) {
            list.set(i, kj::arrayPtr(assetIds[i].data(), 32));
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetResolveBatchResponse(const std::vector<AssetResolveResponseData>& responses,
                                                           uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto batchResp = message.initAssetResolveBatchResponse();
        batchResp.setRequestId(requestId);
        auto responsesList = batchResp.initResponses(responses.size());

        for (size_t i = 0; i < responses.size(); ++i) {
            const auto& r = responses[i];
            auto resp = responsesList[i];
            resp.setFound(r.found);

            if (r.found) {
                auto entry = resp.initEntry();
                entry.setId(kj::arrayPtr(r.entry.id.data(), r.entry.id.size()));
                entry.setUri(r.entry.uri);
                entry.setContentType(r.entry.contentType);
                entry.setSizeBytes(r.entry.sizeBytes);
                entry.setEncrypted(r.entry.encrypted);
                entry.setPlaintextHash(kj::arrayPtr(r.entry.plaintextHash.data(), r.entry.plaintextHash.size()));
                entry.setAppId(r.entry.appId);
                entry.setPersistent(r.entry.persistent);
            }

            resp.setHasKey(r.hasKey);
            if (r.hasKey) {
                resp.setKey(kj::arrayPtr(r.key.data(), 32));
            }
            resp.setDeliveryMethod(r.deliveryMethod);
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetProvideKey(const std::array<uint8_t, 32>& assetId,
                                                 const std::array<uint8_t, 32>& key, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetProvideKeyRequest();
        request.setAssetId(kj::arrayPtr(assetId.data(), 32));
        request.setKey(kj::arrayPtr(key.data(), 32));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetProvideKeyResponse(bool success, const std::string& errorMessage,
                                                         uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetProvideKeyResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

namespace
{
// Helper to serialize a PropertyValue into a Cap'n Proto PropertyValue builder
// (Duplicated here because the main serializePropertyValue is defined later in the file)
void serializePropertyValueForMetadata(Protocol::PropertyValue::Builder& builder, const PropertyValue& value) {
    std::visit(
        [&builder](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t>)
                builder.setInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                builder.setInt64(v);
            else if constexpr (std::is_same_v<T, float>)
                builder.setFloat32(v);
            else if constexpr (std::is_same_v<T, double>)
                builder.setFloat64(v);
            else if constexpr (std::is_same_v<T, Vec2>) {
                auto b = builder.initVec2();
                b.setX(v.x);
                b.setY(v.y);
            } else if constexpr (std::is_same_v<T, Vec3>) {
                auto b = builder.initVec3();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
            } else if constexpr (std::is_same_v<T, Vec4>) {
                auto b = builder.initVec4();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
                b.setW(v.w);
            } else if constexpr (std::is_same_v<T, Quat>) {
                auto b = builder.initQuat();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
                b.setW(v.w);
            } else if constexpr (std::is_same_v<T, std::string>)
                builder.setString(v);
            else if constexpr (std::is_same_v<T, bool>)
                builder.setBool(v);
            else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                builder.setBytes(kj::arrayPtr(v.data(), v.size()));
            else if constexpr (std::is_same_v<T, AssetId>)
                builder.setAssetId(kj::arrayPtr(v.hash.data(), v.hash.size()));
            else if constexpr (std::is_same_v<T, Mat3>) {
                auto b = builder.initMat3();
                auto col0 = b.initCol0();
                col0.setX(v[0].x);
                col0.setY(v[0].y);
                col0.setZ(v[0].z);
                auto col1 = b.initCol1();
                col1.setX(v[1].x);
                col1.setY(v[1].y);
                col1.setZ(v[1].z);
                auto col2 = b.initCol2();
                col2.setX(v[2].x);
                col2.setY(v[2].y);
                col2.setZ(v[2].z);
            } else if constexpr (std::is_same_v<T, Mat4>) {
                auto b = builder.initMat4();
                auto col0 = b.initCol0();
                col0.setX(v[0].x);
                col0.setY(v[0].y);
                col0.setZ(v[0].z);
                col0.setW(v[0].w);
                auto col1 = b.initCol1();
                col1.setX(v[1].x);
                col1.setY(v[1].y);
                col1.setZ(v[1].z);
                col1.setW(v[1].w);
                auto col2 = b.initCol2();
                col2.setX(v[2].x);
                col2.setY(v[2].y);
                col2.setZ(v[2].z);
                col2.setW(v[2].w);
                auto col3 = b.initCol3();
                col3.setX(v[3].x);
                col3.setY(v[3].y);
                col3.setZ(v[3].z);
                col3.setW(v[3].w);
            }
        },
        value);
}
}  // namespace

Result<void> NetworkSession::sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data,
                                             uint8_t contentType, bool persistent, uint64_t requestId) {
    return sendAssetUpload(appId, data, contentType, persistent, requestId, AssetMetadataData{});
}

Result<void> NetworkSession::sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data,
                                             uint8_t contentType, bool persistent, uint64_t requestId,
                                             const AssetMetadataData& metadata) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetUploadRequest();
        request.setAppId(appId);
        request.setData(kj::arrayPtr(data.data(), data.size()));
        request.setContentType(contentType);
        request.setPersistent(persistent);
        request.setRequestId(requestId);

        // Serialize metadata if present
        auto metaBuilder = request.initMetadata();
        if (metadata.type == AssetMetadataType::Shader && metadata.shaderMetadata) {
            auto shader = metaBuilder.initShader();
            const auto& sm = *metadata.shaderMetadata;
            shader.setName(sm.name);
            shader.setDescription(sm.description);
            shader.setRenderQueue(sm.renderQueue);
            shader.setCastsShadows(sm.castsShadows);
            shader.setTransparent(sm.transparent);
            shader.setAuthor(sm.author);

            auto keywordsBuilder = shader.initKeywords(sm.keywords.size());
            for (size_t i = 0; i < sm.keywords.size(); ++i) {
                keywordsBuilder.set(i, sm.keywords[i]);
            }

            auto paramsBuilder = shader.initParameters(sm.parameters.size());
            for (size_t i = 0; i < sm.parameters.size(); ++i) {
                auto& param = sm.parameters[i];
                auto paramBuilder = paramsBuilder[i];
                paramBuilder.setName(param.name);
                paramBuilder.setDisplayName(param.displayName);
                paramBuilder.setType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(param.type)));
                if (param.defaultValue) {
                    auto valueBuilder = paramBuilder.initDefaultValue();
                    serializePropertyValueForMetadata(valueBuilder, *param.defaultValue);
                }
                auto attrsBuilder = paramBuilder.initAttributes(param.attributes.size());
                size_t j = 0;
                for (const auto& [key, val] : param.attributes) {
                    attrsBuilder[j].setKey(key);
                    attrsBuilder[j].setValue(val);
                    ++j;
                }
            }
        } else if (metadata.type == AssetMetadataType::Texture && metadata.textureMetadata) {
            auto tex = metaBuilder.initTexture();
            const auto& tm = *metadata.textureMetadata;
            tex.setWidth(tm.width);
            tex.setHeight(tm.height);
            tex.setDepth(tm.depth);
            tex.setMipLevels(tm.mipLevels);
            tex.setArrayLayers(tm.arrayLayers);
            tex.setTextureType(tm.textureType);
            tex.setFormat(tm.format);
            tex.setColorSpace(tm.colorSpace);
            tex.setGenerateMips(tm.generateMips);
            tex.setSourceFile(tm.sourceFile);
        }
        // else: metadata.type == None, metaBuilder defaults to none

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadResponse(bool success, const std::array<uint8_t, 32>& assetId,
                                                     const std::string& uri, const std::string& errorMessage,
                                                     uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetUploadResponse();
        response.setSuccess(success);
        response.setAssetId(kj::arrayPtr(assetId.data(), 32));
        response.setUri(uri);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetFetch(const std::array<uint8_t, 32>& assetId, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetFetchRequest();
        request.setAssetId(kj::arrayPtr(assetId.data(), 32));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetFetchResponse(bool found, const std::vector<uint8_t>& data,
                                                    const std::string& errorMessage, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetFetchResponse();
        response.setFound(found);
        response.setData(kj::arrayPtr(data.data(), data.size()));
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Route bulk asset data to dedicated download channel
        // Falls back to default channel if multi-channel not supported
        return _connection->sendOnChannel(NetworkConnection::CHANNEL_ASSET_DOWNLOAD, serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

// ============================================================================
// Chunked Upload Send Methods
// ============================================================================

Result<void> NetworkSession::sendAssetUploadBegin(const AssetUploadBeginData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetUploadBeginRequest();
        request.setAppId(data.appId);
        request.setTotalSize(data.totalSize);
        request.setContentType(data.contentType);
        request.setPersistent(data.persistent);
        request.setChunkSize(data.chunkSize);
        request.setEncrypted(data.encrypted);
        request.setPlaintextHash(kj::arrayPtr(data.plaintextHash.data(), data.plaintextHash.size()));
        request.setRequestId(data.requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadBeginResponse(const AssetUploadBeginResponseData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetUploadBeginResponse();
        response.setSuccess(data.success);
        response.setUploadId(kj::arrayPtr(data.uploadId.data(), data.uploadId.size()));
        response.setChunkSize(data.chunkSize);
        response.setErrorMessage(data.errorMessage);
        response.setRequestId(data.requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadChunk(const AssetUploadChunkData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetUploadChunkRequest();
        request.setUploadId(kj::arrayPtr(data.uploadId.data(), data.uploadId.size()));
        request.setOffset(data.offset);
        request.setData(kj::arrayPtr(data.data.data(), data.data.size()));
        request.setSequence(data.sequence);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Route chunk data to dedicated channel to avoid blocking control messages
        // Falls back to default channel if multi-channel not supported
        return _connection->sendOnChannel(NetworkConnection::CHANNEL_ASSET_UPLOAD, serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadChunkResponse(const AssetUploadChunkResponseData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetUploadChunkResponse();
        response.setSuccess(data.success);
        response.setUploadId(kj::arrayPtr(data.uploadId.data(), data.uploadId.size()));
        response.setBytesReceived(data.bytesReceived);
        response.setErrorMessage(data.errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Route on same channel as request for consistency
        return _connection->sendOnChannel(NetworkConnection::CHANNEL_ASSET_UPLOAD, serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadComplete(const AssetUploadCompleteData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetUploadCompleteRequest();
        request.setUploadId(kj::arrayPtr(data.uploadId.data(), data.uploadId.size()));
        request.setTotalChunks(data.totalChunks);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadCompleteResponse(const AssetUploadCompleteResponseData& data) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetUploadCompleteResponse();
        response.setSuccess(data.success);
        response.setAssetId(kj::arrayPtr(data.assetId.data(), data.assetId.size()));
        response.setUri(data.uri);
        response.setBytesStored(data.bytesStored);
        response.setErrorMessage(data.errorMessage);
        response.setUploadId(kj::arrayPtr(data.uploadId.data(), data.uploadId.size()));

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadCancel(const std::array<uint8_t, 16>& uploadId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAssetUploadCancelRequest();
        request.setUploadId(kj::arrayPtr(uploadId.data(), uploadId.size()));

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAssetUploadCancelResponse(bool success, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAssetUploadCancelResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

// =============================================================================
// Scene Management Messages
// =============================================================================

Result<void> NetworkSession::sendCreateSceneRequest(const std::string& sceneName, bool transient) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initCreateSceneRequest();
        request.setSceneName(sceneName);
        request.setTransient(transient);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendCreateSceneResponse(bool success, uint64_t sceneId, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initCreateSceneResponse();
        response.setSuccess(success);
        response.setSceneId(sceneId);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendDestroySceneRequest(uint64_t sceneId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initDestroySceneRequest();
        request.setSceneId(sceneId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendDestroySceneResponse(bool success, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initDestroySceneResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendSetSceneEnabledRequest(uint64_t sceneId, bool enabled) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initSetSceneEnabledRequest();
        request.setSceneId(sceneId);
        request.setEnabled(enabled);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendSetSceneEnabledResponse(bool success, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initSetSceneEnabledResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAddEntityToSceneRequest(uint64_t entityId, uint64_t sceneId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initAddEntityToSceneRequest();
        request.setEntityId(entityId);
        request.setSceneId(sceneId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendAddEntityToSceneResponse(bool success, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initAddEntityToSceneResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

// ============================================================================
// Material System Send Methods
// ============================================================================

namespace
{

// Helper to serialize a PropertyValue into a Cap'n Proto PropertyValue builder
void serializePropertyValue(Protocol::PropertyValue::Builder& builder, const PropertyValue& value) {
    std::visit(
        [&builder](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t>)
                builder.setInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                builder.setInt64(v);
            else if constexpr (std::is_same_v<T, float>)
                builder.setFloat32(v);
            else if constexpr (std::is_same_v<T, double>)
                builder.setFloat64(v);
            else if constexpr (std::is_same_v<T, Vec2>) {
                auto b = builder.initVec2();
                b.setX(v.x);
                b.setY(v.y);
            } else if constexpr (std::is_same_v<T, Vec3>) {
                auto b = builder.initVec3();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
            } else if constexpr (std::is_same_v<T, Vec4>) {
                auto b = builder.initVec4();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
                b.setW(v.w);
            } else if constexpr (std::is_same_v<T, Quat>) {
                auto b = builder.initQuat();
                b.setX(v.x);
                b.setY(v.y);
                b.setZ(v.z);
                b.setW(v.w);
            } else if constexpr (std::is_same_v<T, std::string>)
                builder.setString(v);
            else if constexpr (std::is_same_v<T, bool>)
                builder.setBool(v);
            else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                builder.setBytes(kj::arrayPtr(v.data(), v.size()));
            else if constexpr (std::is_same_v<T, AssetId>)
                builder.setAssetId(kj::arrayPtr(v.hash.data(), v.hash.size()));
            else if constexpr (std::is_same_v<T, Mat3>) {
                auto b = builder.initMat3();
                auto col0 = b.initCol0();
                col0.setX(v[0].x);
                col0.setY(v[0].y);
                col0.setZ(v[0].z);
                auto col1 = b.initCol1();
                col1.setX(v[1].x);
                col1.setY(v[1].y);
                col1.setZ(v[1].z);
                auto col2 = b.initCol2();
                col2.setX(v[2].x);
                col2.setY(v[2].y);
                col2.setZ(v[2].z);
            } else if constexpr (std::is_same_v<T, Mat4>) {
                auto b = builder.initMat4();
                auto col0 = b.initCol0();
                col0.setX(v[0].x);
                col0.setY(v[0].y);
                col0.setZ(v[0].z);
                col0.setW(v[0].w);
                auto col1 = b.initCol1();
                col1.setX(v[1].x);
                col1.setY(v[1].y);
                col1.setZ(v[1].z);
                col1.setW(v[1].w);
                auto col2 = b.initCol2();
                col2.setX(v[2].x);
                col2.setY(v[2].y);
                col2.setZ(v[2].z);
                col2.setW(v[2].w);
                auto col3 = b.initCol3();
                col3.setX(v[3].x);
                col3.setY(v[3].y);
                col3.setZ(v[3].z);
                col3.setW(v[3].w);
            }
        },
        value);
}

// Helper to serialize MaterialAssetData into a Cap'n Proto MaterialAssetData builder
void serializeMaterialAssetData(Protocol::MaterialAssetData::Builder& builder,
                                const NetworkSession::MaterialAssetData& data) {
    builder.setName(data.name);
    builder.setShaderAssetId(kj::arrayPtr(data.shaderAssetId.data(), data.shaderAssetId.size()));

    auto propsBuilder = builder.initProperties(data.properties.size());
    for (size_t i = 0; i < data.properties.size(); ++i) {
        propsBuilder[i].setName(data.properties[i].name);
        auto valueBuilder = propsBuilder[i].initValue();
        serializePropertyValue(valueBuilder, data.properties[i].value);
    }

    auto keywordsBuilder = builder.initEnabledKeywords(data.enabledKeywords.size());
    for (size_t i = 0; i < data.enabledKeywords.size(); ++i) {
        keywordsBuilder.set(i, data.enabledKeywords[i]);
    }

    builder.setRenderQueue(data.renderQueue);
    builder.setCastsShadows(data.castsShadows);
    builder.setReceivesShadows(data.receivesShadows);
    builder.setDepthWrite(data.depthWrite);
    builder.setCreatorSessionId(data.creatorSessionId);
    builder.setVersion(data.version);
    builder.setModifiedAt(data.modifiedAt);
    builder.setAppId(data.appId);
}

}  // namespace

Result<void> NetworkSession::sendCreateMaterialRequest(const MaterialAssetData& material, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initCreateMaterialRequest();
        auto matBuilder = request.initMaterial();
        serializeMaterialAssetData(matBuilder, material);
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendCreateMaterialResponse(bool success, const std::array<uint8_t, 32>& materialId,
                                                        const std::string& errorMessage, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initCreateMaterialResponse();
        response.setSuccess(success);
        response.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendUpdateMaterialPropertyRequest(const std::array<uint8_t, 32>& materialId,
                                                               const std::string& propertyName,
                                                               const PropertyValue& value, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initUpdateMaterialPropertyRequest();
        request.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        request.setPropertyName(propertyName);
        auto valueBuilder = request.initValue();
        serializePropertyValue(valueBuilder, value);
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendUpdateMaterialPropertyResponse(bool success, uint64_t newVersion,
                                                                const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initUpdateMaterialPropertyResponse();
        response.setSuccess(success);
        response.setNewVersion(newVersion);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendUpdateMaterialPropertiesBatchRequest(
    const std::array<uint8_t, 32>& materialId, const std::vector<MaterialPropertyData>& properties,
    uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initUpdateMaterialPropertiesBatchRequest();
        request.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));

        auto propsBuilder = request.initProperties(properties.size());
        for (size_t i = 0; i < properties.size(); ++i) {
            propsBuilder[i].setName(properties[i].name);
            auto valueBuilder = propsBuilder[i].initValue();
            serializePropertyValue(valueBuilder, properties[i].value);
        }

        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendUpdateMaterialPropertiesBatchResponse(bool success, uint64_t newVersion,
                                                                       const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initUpdateMaterialPropertiesBatchResponse();
        response.setSuccess(success);
        response.setNewVersion(newVersion);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialPropertyUpdate(const std::array<uint8_t, 32>& materialId,
                                                        const std::string& propertyName, const PropertyValue& value,
                                                        uint64_t newVersion, uint64_t originSessionId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto update = message.initMaterialPropertyUpdate();
        update.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        update.setPropertyName(propertyName);
        auto valueBuilder = update.initValue();
        serializePropertyValue(valueBuilder, value);
        update.setVersion(newVersion);
        update.setOriginSessionId(originSessionId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialSubscribeRequest(const std::array<uint8_t, 32>& materialId,
                                                          uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initMaterialSubscribeRequest();
        request.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialSubscribeResponse(bool success, const MaterialAssetData& material,
                                                           const std::string& errorMessage, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initMaterialSubscribeResponse();
        response.setSuccess(success);
        if (success) {
            auto matBuilder = response.initMaterial();
            serializeMaterialAssetData(matBuilder, material);
        }
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialUnsubscribeRequest(const std::array<uint8_t, 32>& materialId,
                                                            uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initMaterialUnsubscribeRequest();
        request.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialUnsubscribeResponse(bool success, const std::string& errorMessage) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initMaterialUnsubscribeResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendGetMaterialRequest(const std::array<uint8_t, 32>& materialId, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initGetMaterialRequest();
        request.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendGetMaterialResponse(bool success, const MaterialAssetData& material,
                                                     const std::string& /*errorMessage*/) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initGetMaterialResponse();
        response.setFound(success);
        if (success) {
            auto matBuilder = response.initMaterial();
            serializeMaterialAssetData(matBuilder, material);
        }
        // Note: errorMessage not in current schema, ignored for now
        response.setRequestId(0);  // No request correlation in current signature

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMaterialResolved(const std::array<uint8_t, 32>& materialId,
                                                  const MaterialAssetData& material) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto resolved = message.initMaterialResolved();
        resolved.setMaterialId(kj::arrayPtr(materialId.data(), materialId.size()));
        auto matBuilder = resolved.initMaterial();
        serializeMaterialAssetData(matBuilder, material);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMeshMaterialBindingRequest(uint64_t entityId,
                                                            const std::vector<std::array<uint8_t, 32>>& materialIds,
                                                            uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initMeshMaterialBindingRequest();
        request.setEntityId(entityId);
        request.setRequestId(requestId);

        auto matIdsBuilder = request.initMaterialIds(materialIds.size());
        for (size_t i = 0; i < materialIds.size(); ++i) {
            matIdsBuilder.set(i, kj::arrayPtr(materialIds[i].data(), materialIds[i].size()));
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMeshMaterialBindingResponse(bool success, const std::string& errorMessage,
                                                             uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto response = message.initMeshMaterialBindingResponse();
        response.setSuccess(success);
        response.setErrorMessage(errorMessage);
        response.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendMeshMaterialBindingUpdate(uint64_t entityId,
                                                           const std::vector<std::array<uint8_t, 32>>& materialIds,
                                                           uint64_t originSessionId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto update = message.initMeshMaterialBindingUpdate();
        update.setEntityId(entityId);
        update.setOriginSessionId(originSessionId);

        auto matIdsBuilder = update.initMaterialIds(materialIds.size());
        for (size_t i = 0; i < materialIds.size(); ++i) {
            matIdsBuilder.set(i, kj::arrayPtr(materialIds[i].data(), materialIds[i].size()));
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

//=============================================================================
// Shader Protocol Messages
//=============================================================================

Result<void> NetworkSession::sendGetShaderRequest(const std::array<uint8_t, 32>& shaderAssetId, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto request = message.initGetShaderRequest();
        request.setShaderAssetId(kj::arrayPtr(shaderAssetId.data(), shaderAssetId.size()));
        request.setRequestId(requestId);

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendGetShaderResponse(const GetShaderResponseData& response, uint64_t requestId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto resp = message.initGetShaderResponse();
        resp.setFound(response.found);
        resp.setIsBuiltin(response.isBuiltin);
        resp.setMainSource(response.mainSource);
        resp.setRequestId(requestId);

        // Modules
        auto modulesBuilder = resp.initModules(response.modules.size());
        for (size_t i = 0; i < response.modules.size(); ++i) {
            modulesBuilder[i].setModuleName(response.modules[i].moduleName);
            modulesBuilder[i].setSource(response.modules[i].source);
        }

        // Metadata
        auto metaBuilder = resp.initMetadata();
        metaBuilder.setName(response.metadata.name);
        metaBuilder.setDescription(response.metadata.description);
        metaBuilder.setRenderQueue(response.metadata.renderQueue);
        metaBuilder.setCastsShadows(response.metadata.castsShadows);
        metaBuilder.setTransparent(response.metadata.transparent);
        metaBuilder.setAuthor(response.metadata.author);

        // Keywords
        auto keywordsBuilder = metaBuilder.initKeywords(response.metadata.keywords.size());
        for (size_t i = 0; i < response.metadata.keywords.size(); ++i) {
            keywordsBuilder.set(i, response.metadata.keywords[i]);
        }

        // Parameters (native typed)
        auto paramsBuilder = metaBuilder.initParameters(response.metadata.parameters.size());
        for (size_t i = 0; i < response.metadata.parameters.size(); ++i) {
            const auto& param = response.metadata.parameters[i];
            paramsBuilder[i].setName(param.name);
            paramsBuilder[i].setDisplayName(param.displayName);
            paramsBuilder[i].setType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(param.type)));
            // Serialize defaultValue if present
            if (param.defaultValue.has_value()) {
                auto valueBuilder = paramsBuilder[i].initDefaultValue();
                serializePropertyValue(valueBuilder, param.defaultValue.value());
            }
            // Serialize KeyValue attributes
            auto attrsBuilder = paramsBuilder[i].initAttributes(param.attributes.size());
            size_t j = 0;
            for (const auto& [key, value] : param.attributes) {
                attrsBuilder[j].setKey(key);
                attrsBuilder[j].setValue(value);
                ++j;
            }
        }

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }
        return _connection->send(serialized.value);
    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

std::chrono::steady_clock::time_point NetworkSession::getLastHeartbeatReceived() const {
    uint64_t ms = _lastHeartbeatReceivedMs.load(std::memory_order_relaxed);
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(ms));
}

void NetworkSession::setEntityCreatedCallback(EntityCreatedCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _entityCreatedCallback = std::move(callback);
}

void NetworkSession::setEntityDestroyedCallback(EntityDestroyedCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _entityDestroyedCallback = std::move(callback);
}

void NetworkSession::setPropertyUpdateCallback(PropertyUpdateCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _propertyUpdateCallback = std::move(callback);
}

void NetworkSession::setSceneSnapshotCallback(SceneSnapshotCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _sceneSnapshotCallback = std::move(callback);
}

void NetworkSession::setHandshakeCallback(HandshakeCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _handshakeCallback = std::move(callback);
}

void NetworkSession::setErrorCallback(ErrorCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _errorCallback = std::move(callback);
}

void NetworkSession::setRegisterSchemaResponseCallback(RegisterSchemaResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _registerSchemaResponseCallback = std::move(callback);
}

void NetworkSession::setQueryPublicSchemasResponseCallback(QueryPublicSchemasResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _queryPublicSchemasResponseCallback = std::move(callback);
}

void NetworkSession::setPublishSchemaResponseCallback(PublishSchemaResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _publishSchemaResponseCallback = std::move(callback);
}

void NetworkSession::setUnpublishSchemaResponseCallback(UnpublishSchemaResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _unpublishSchemaResponseCallback = std::move(callback);
}

void NetworkSession::setSchemaNackCallback(SchemaNackCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _schemaNackCallback = std::move(callback);
}

void NetworkSession::setSchemaAdvertisementCallback(SchemaAdvertisementCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _schemaAdvertisementCallback = std::move(callback);
}

void NetworkSession::setHeartbeatCallback(HeartbeatCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _heartbeatCallback = std::move(callback);
}

void NetworkSession::setHeartbeatResponseCallback(HeartbeatResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _heartbeatResponseCallback = std::move(callback);
}

void NetworkSession::setDisconnectCallback(DisconnectCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;  // Don't set callbacks during shutdown
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _disconnectCallback = std::move(callback);
}

void NetworkSession::setAssetAdvertiseCallback(AssetAdvertiseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetAdvertiseCallback = std::move(callback);
}

void NetworkSession::setAssetAdvertiseResponseCallback(AssetAdvertiseResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetAdvertiseResponseCallback = std::move(callback);
}

void NetworkSession::setAssetWithdrawCallback(AssetWithdrawCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetWithdrawCallback = std::move(callback);
}

void NetworkSession::setAssetWithdrawResponseCallback(AssetWithdrawResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetWithdrawResponseCallback = std::move(callback);
}

void NetworkSession::setAssetWithdrawAllCallback(AssetWithdrawAllCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetWithdrawAllCallback = std::move(callback);
}

void NetworkSession::setAssetWithdrawAllResponseCallback(AssetWithdrawAllResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetWithdrawAllResponseCallback = std::move(callback);
}

void NetworkSession::setAssetResolveCallback(AssetResolveCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetResolveCallback = std::move(callback);
}

void NetworkSession::setAssetResolveResponseCallback(AssetResolveResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetResolveResponseCallback = std::move(callback);
}

void NetworkSession::setAssetResolveBatchCallback(AssetResolveBatchCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetResolveBatchCallback = std::move(callback);
}

void NetworkSession::setAssetResolveBatchResponseCallback(AssetResolveBatchResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetResolveBatchResponseCallback = std::move(callback);
}

void NetworkSession::setAssetProvideKeyCallback(AssetProvideKeyCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetProvideKeyCallback = std::move(callback);
}

void NetworkSession::setAssetProvideKeyResponseCallback(AssetProvideKeyResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetProvideKeyResponseCallback = std::move(callback);
}

void NetworkSession::setAssetUploadCallback(AssetUploadCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadCallback = std::move(callback);
}

void NetworkSession::setAssetUploadResponseCallback(AssetUploadResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadResponseCallback = std::move(callback);
}

void NetworkSession::setAssetFetchCallback(AssetFetchCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetFetchCallback = std::move(callback);
}

void NetworkSession::setAssetFetchResponseCallback(AssetFetchResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetFetchResponseCallback = std::move(callback);
}

void NetworkSession::setAssetUploadBeginCallback(AssetUploadBeginCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadBeginCallback = std::move(callback);
}

void NetworkSession::setAssetUploadBeginResponseCallback(AssetUploadBeginResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadBeginResponseCallback = std::move(callback);
}

void NetworkSession::setAssetUploadChunkCallback(AssetUploadChunkCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadChunkCallback = std::move(callback);
}

void NetworkSession::setAssetUploadChunkResponseCallback(AssetUploadChunkResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadChunkResponseCallback = std::move(callback);
}

void NetworkSession::setAssetUploadCompleteCallback(AssetUploadCompleteCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadCompleteCallback = std::move(callback);
}

void NetworkSession::setAssetUploadCompleteResponseCallback(AssetUploadCompleteResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadCompleteResponseCallback = std::move(callback);
}

void NetworkSession::setAssetUploadCancelCallback(AssetUploadCancelCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadCancelCallback = std::move(callback);
}

void NetworkSession::setAssetUploadCancelResponseCallback(AssetUploadCancelResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _assetUploadCancelResponseCallback = std::move(callback);
}

// Scene management callback setters
void NetworkSession::setCreateSceneCallback(CreateSceneCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _createSceneCallback = std::move(callback);
}

void NetworkSession::setCreateSceneResponseCallback(CreateSceneResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _createSceneResponseCallback = std::move(callback);
}

void NetworkSession::setDestroySceneCallback(DestroySceneCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _destroySceneCallback = std::move(callback);
}

void NetworkSession::setDestroySceneResponseCallback(DestroySceneResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _destroySceneResponseCallback = std::move(callback);
}

void NetworkSession::setSetSceneEnabledCallback(SetSceneEnabledCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _setSceneEnabledCallback = std::move(callback);
}

void NetworkSession::setSetSceneEnabledResponseCallback(SetSceneEnabledResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _setSceneEnabledResponseCallback = std::move(callback);
}

void NetworkSession::setAddEntityToSceneCallback(AddEntityToSceneCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _addEntityToSceneCallback = std::move(callback);
}

void NetworkSession::setAddEntityToSceneResponseCallback(AddEntityToSceneResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _addEntityToSceneResponseCallback = std::move(callback);
}

// Material system callback setters
void NetworkSession::setCreateMaterialCallback(CreateMaterialCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _createMaterialCallback = std::move(callback);
}

void NetworkSession::setCreateMaterialResponseCallback(CreateMaterialResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _createMaterialResponseCallback = std::move(callback);
}

void NetworkSession::setUpdateMaterialPropertyCallback(UpdateMaterialPropertyCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _updateMaterialPropertyCallback = std::move(callback);
}

void NetworkSession::setUpdateMaterialPropertyResponseCallback(UpdateMaterialPropertyResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _updateMaterialPropertyResponseCallback = std::move(callback);
}

void NetworkSession::setUpdateMaterialPropertiesBatchCallback(UpdateMaterialPropertiesBatchCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _updateMaterialPropertiesBatchCallback = std::move(callback);
}

void NetworkSession::setUpdateMaterialPropertiesBatchResponseCallback(
    UpdateMaterialPropertiesBatchResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _updateMaterialPropertiesBatchResponseCallback = std::move(callback);
}

void NetworkSession::setMaterialPropertyUpdateCallback(MaterialPropertyUpdateCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialPropertyUpdateCallback = std::move(callback);
}

void NetworkSession::setMaterialSubscribeCallback(MaterialSubscribeCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialSubscribeCallback = std::move(callback);
}

void NetworkSession::setMaterialSubscribeResponseCallback(MaterialSubscribeResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialSubscribeResponseCallback = std::move(callback);
}

void NetworkSession::setMaterialUnsubscribeCallback(MaterialUnsubscribeCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialUnsubscribeCallback = std::move(callback);
}

void NetworkSession::setMaterialUnsubscribeResponseCallback(MaterialUnsubscribeResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialUnsubscribeResponseCallback = std::move(callback);
}

void NetworkSession::setGetMaterialCallback(GetMaterialCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _getMaterialCallback = std::move(callback);
}

void NetworkSession::setGetMaterialResponseCallback(GetMaterialResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _getMaterialResponseCallback = std::move(callback);
}

void NetworkSession::setMaterialResolvedCallback(MaterialResolvedCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _materialResolvedCallback = std::move(callback);
}

void NetworkSession::setMeshMaterialBindingCallback(MeshMaterialBindingCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _meshMaterialBindingCallback = std::move(callback);
}

void NetworkSession::setMeshMaterialBindingResponseCallback(MeshMaterialBindingResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _meshMaterialBindingResponseCallback = std::move(callback);
}

void NetworkSession::setMeshMaterialBindingUpdateCallback(MeshMaterialBindingUpdateCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _meshMaterialBindingUpdateCallback = std::move(callback);
}

void NetworkSession::setGetShaderCallback(GetShaderCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _getShaderCallback = std::move(callback);
}

void NetworkSession::setGetShaderResponseCallback(GetShaderResponseCallback callback) {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _getShaderResponseCallback = std::move(callback);
}

void NetworkSession::clearCallbacks() {
    // Mark as shutting down to prevent new callbacks
    _shuttingDown.store(true, std::memory_order_release);

    // Wait for active callbacks to complete
    while (_activeCallbacks.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Clear all callbacks
    std::lock_guard<std::mutex> lock(_mutex);
    _entityCreatedCallback = nullptr;
    _entityDestroyedCallback = nullptr;
    _propertyUpdateCallback = nullptr;
    _sceneSnapshotCallback = nullptr;
    _handshakeCallback = nullptr;
    _errorCallback = nullptr;
    _heartbeatCallback = nullptr;
    _heartbeatResponseCallback = nullptr;
    _registerSchemaResponseCallback = nullptr;
    _queryPublicSchemasResponseCallback = nullptr;
    _publishSchemaResponseCallback = nullptr;
    _unpublishSchemaResponseCallback = nullptr;
    _schemaNackCallback = nullptr;
    _schemaAdvertisementCallback = nullptr;

    // Asset callbacks
    _assetAdvertiseCallback = nullptr;
    _assetAdvertiseResponseCallback = nullptr;
    _assetWithdrawCallback = nullptr;
    _assetWithdrawResponseCallback = nullptr;
    _assetWithdrawAllCallback = nullptr;
    _assetWithdrawAllResponseCallback = nullptr;
    _assetResolveCallback = nullptr;
    _assetResolveResponseCallback = nullptr;
    _assetResolveBatchCallback = nullptr;
    _assetResolveBatchResponseCallback = nullptr;
    _assetProvideKeyCallback = nullptr;
    _assetProvideKeyResponseCallback = nullptr;
    _assetUploadCallback = nullptr;
    _assetUploadResponseCallback = nullptr;
    _assetFetchCallback = nullptr;
    _assetFetchResponseCallback = nullptr;

    // Chunked upload callbacks
    _assetUploadBeginCallback = nullptr;
    _assetUploadBeginResponseCallback = nullptr;
    _assetUploadChunkCallback = nullptr;
    _assetUploadChunkResponseCallback = nullptr;
    _assetUploadCompleteCallback = nullptr;
    _assetUploadCompleteResponseCallback = nullptr;
    _assetUploadCancelCallback = nullptr;
    _assetUploadCancelResponseCallback = nullptr;

    // Scene management callbacks
    _createSceneCallback = nullptr;
    _createSceneResponseCallback = nullptr;
    _destroySceneCallback = nullptr;
    _destroySceneResponseCallback = nullptr;
    _setSceneEnabledCallback = nullptr;
    _setSceneEnabledResponseCallback = nullptr;
    _addEntityToSceneCallback = nullptr;
    _addEntityToSceneResponseCallback = nullptr;

    // Material system callbacks
    _createMaterialCallback = nullptr;
    _createMaterialResponseCallback = nullptr;
    _updateMaterialPropertyCallback = nullptr;
    _updateMaterialPropertyResponseCallback = nullptr;
    _updateMaterialPropertiesBatchCallback = nullptr;
    _updateMaterialPropertiesBatchResponseCallback = nullptr;
    _materialPropertyUpdateCallback = nullptr;
    _materialSubscribeCallback = nullptr;
    _materialSubscribeResponseCallback = nullptr;
    _materialUnsubscribeCallback = nullptr;
    _materialUnsubscribeResponseCallback = nullptr;
    _getMaterialCallback = nullptr;
    _getMaterialResponseCallback = nullptr;
    _materialResolvedCallback = nullptr;

    // Mesh-material binding callbacks
    _meshMaterialBindingCallback = nullptr;
    _meshMaterialBindingResponseCallback = nullptr;
    _meshMaterialBindingUpdateCallback = nullptr;

    // Shader callbacks
    _getShaderCallback = nullptr;
    _getShaderResponseCallback = nullptr;
}

void NetworkSession::handleUnknownSchema(ComponentTypeHash typeHash) {
    // ALWAYS increment unknown schema counter, regardless of policy
    _unknownSchemaDrops.fetch_add(1, std::memory_order_relaxed);

    // ALWAYS emit rate-limited log
    auto& policy = SchemaNackPolicy::instance();
    auto logInterval = std::chrono::milliseconds(policy.getLogIntervalMs());
    if (_logRateLimiter.shouldLog(typeHash, logInterval)) {
        std::string typeHashStr = Networking::toString(typeHash);
        std::string logMessage = "Unknown schema encountered: " + typeHashStr +
                                 " (error: " + std::string(errorToString(NetworkError::SchemaNotFound)) + ")";
        ENTROPY_LOG_WARNING(logMessage);
    }

    // If policy enabled, enqueue NACK feedback asynchronously (non-blocking)
    if (policy.isEnabled()) {
        // sendSchemaNack already handles rate limiting via SchemaNackTracker
        // and uses the non-blocking reliable send path (_connection->send())
        // The send() call enqueues the message without blocking the caller
        sendSchemaNack(typeHash, "Schema not found in registry");
    }
}

ConnectionStats NetworkSession::getStats() const {
    if (!_connection) {
        return ConnectionStats{};
    }
    return _connection->getStats();
}

// ============================================================================
// Async Message Queue Implementation
// ============================================================================
// Decouples the receive thread from message processing. This ensures:
// 1. Slow callbacks don't block the receive thread
// 2. Heartbeat detection works correctly even when app is busy
// 3. Connection can detect broken pipes immediately
// ============================================================================

void NetworkSession::startMessageWorker() {
    bool expected = false;
    if (!_messageWorkerRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Already running
        return;
    }

    _messageWorkerThread = std::thread([this]() { messageWorkerLoop(); });

    ENTROPY_LOG_DEBUG(std::format("NetworkSession {}: Message worker thread started", _sessionId));
}

void NetworkSession::stopMessageWorker() {
    bool expected = true;
    if (!_messageWorkerRunning.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        // Not running or already stopping
        return;
    }

    // Wake up the worker thread
    {
        std::lock_guard<std::mutex> lock(_messageQueueMutex);
        _messageQueueCV.notify_one();
    }

    // Wait for worker to finish
    if (_messageWorkerThread.joinable()) {
        _messageWorkerThread.join();
    }

    // Drain any remaining messages (discard since we're shutting down)
    {
        std::lock_guard<std::mutex> lock(_messageQueueMutex);
        _messageQueue.clear();
    }

    ENTROPY_LOG_DEBUG(std::format("NetworkSession {}: Message worker thread stopped", _sessionId));
}

void NetworkSession::messageWorkerLoop() {
    while (_messageWorkerRunning.load(std::memory_order_acquire)) {
        std::vector<uint8_t> message;

        {
            std::unique_lock<std::mutex> lock(_messageQueueMutex);

            // Wait for messages or shutdown signal
            _messageQueueCV.wait(lock, [this]() {
                return !_messageQueue.empty() || !_messageWorkerRunning.load(std::memory_order_acquire);
            });

            // Check if we should exit
            if (!_messageWorkerRunning.load(std::memory_order_acquire) && _messageQueue.empty()) {
                break;
            }

            // Pop message from queue
            if (!_messageQueue.empty()) {
                message = std::move(_messageQueue.front());
                _messageQueue.pop_front();
            }
        }

        // Process the message outside the lock
        if (!message.empty()) {
            handleReceivedMessage(message);
        }
    }
}

void NetworkSession::onMessageReceived(const std::vector<uint8_t>& data) {
    // Check shutdown flag to prevent further processing during destruction
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    // Queue the message for the worker thread to process
    // This keeps the receive thread responsive for immediate disconnect detection
    {
        std::lock_guard<std::mutex> lock(_messageQueueMutex);
        _messageQueue.push_back(data);
    }
    _messageQueueCV.notify_one();
}

void NetworkSession::onConnectionStateChanged(ConnectionState state) {
    // Check shutdown flag to prevent further processing during destruction
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    ConnectionState prevState = _state;
    _state = state;

    // Invoke disconnect callback immediately when connection dies
    // This enables immediate session cleanup for local IPC instead of waiting for heartbeat timeout
    if ((state == ConnectionState::Disconnected || state == ConnectionState::Failed) &&
        prevState != ConnectionState::Disconnected && prevState != ConnectionState::Failed) {
        DisconnectCallback cb;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            cb = _disconnectCallback;
        }
        if (cb) {
            std::string reason = (state == ConnectionState::Failed) ? "Connection failed" : "Connection closed";
            cb(state, reason);
        }
    }
}

void NetworkSession::handleReceivedMessage(const std::vector<uint8_t>& data) {
    // Check shutdown flag with acquire ordering - if true, we see all callback clears
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    try {
        // Deserialize the message
        auto deserialized = deserialize(data);
        if (deserialized.failed()) {
            _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
            if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                _errorCallback(deserialized.error, deserialized.errorMessage);
            }
            _activeCallbacks.fetch_sub(1, std::memory_order_release);
            return;
        }

        // Read the message
        kj::ArrayPtr<const capnp::word> words(reinterpret_cast<const capnp::word*>(deserialized.value.begin()),
                                              deserialized.value.size());

        capnp::FlatArrayMessageReader reader(words);
        auto message = reader.getRoot<Protocol::Message>();

        // Dispatch based on message type
        switch (message.which()) {
            case Protocol::Message::ENTITY_CREATED:
            {
                auto entityCreated = message.getEntityCreated();

                // Extract component groups with their properties
                std::vector<ComponentGroupData> componentGroups;
                auto componentsReader = entityCreated.getComponents();

                // Build ComponentGroupData for each component
                for (auto compGroup : componentsReader) {
                    ComponentGroupData groupData;

                    // Get component type hash
                    if (compGroup.hasTypeHash()) {
                        auto thReader = compGroup.getTypeHash();
                        groupData.typeHash = ComponentTypeHash{thReader.getHigh(), thReader.getLow()};
                    }

                    // Get component name
                    if (compGroup.hasComponentName()) {
                        groupData.componentName = compGroup.getComponentName().cStr();
                    }

                    // Validate component schema if registry is available
                    if (_schemaRegistry && !groupData.typeHash.isNull()) {
                        if (!_schemaRegistry->isRegistered(groupData.typeHash)) {
                            handleUnknownSchema(groupData.typeHash);
                        }
                    }

                    // Extract properties from this component group
                    auto propsReader = compGroup.getProperties();
                    for (auto prop : propsReader) {
                        PropertyMetadata metadata;

                        // Extract property hash
                        if (prop.hasPropertyHash()) {
                            auto hashReader = prop.getPropertyHash();
                            metadata.hash = PropertyHash{hashReader.getHigh(), hashReader.getLow()};
                        }

                        // Extract entity ID
                        metadata.entityId = prop.getEntityId();

                        // Use component type from the group
                        metadata.componentType = groupData.typeHash;

                        // Extract property name
                        if (prop.hasPropertyName()) {
                            metadata.propertyName = prop.getPropertyName().cStr();
                        }

                        // Extract property type
                        metadata.type = static_cast<PropertyType>(prop.getType());

                        groupData.properties.push_back(std::move(metadata));
                    }

                    componentGroups.push_back(std::move(groupData));
                }

                // Invoke callback with component groups
                // Application layer decides how to handle entities with unknown schemas
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _entityCreatedCallback) {
                    std::string entityName;
                    if (entityCreated.hasEntityName()) {
                        entityName = entityCreated.getEntityName().cStr();
                    }
                    _entityCreatedCallback(entityCreated.getEntityId(), std::string(entityCreated.getAppId().cStr()),
                                           std::string(entityCreated.getTypeName().cStr()), entityCreated.getParentId(),
                                           componentGroups, entityCreated.getTargetSceneId(), entityName);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ENTITY_DESTROYED:
            {
                auto entityDestroyed = message.getEntityDestroyed();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _entityDestroyedCallback) {
                    _entityDestroyedCallback(entityDestroyed.getEntityId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::PROPERTY_UPDATE_BATCH:
            {
                auto batch = message.getPropertyUpdateBatch();
                uint32_t seq = batch.getSequence();

                // Atomically check and update sequence number to avoid race conditions
                // Use retry limit to prevent livelock under extreme contention
                constexpr int MAX_CAS_RETRIES = 100;
                bool updateSucceeded = false;
                for (int retry = 0; retry < MAX_CAS_RETRIES; ++retry) {
                    uint32_t last = _lastReceivedSequence.load(std::memory_order_relaxed);

                    // Track packet loss before attempting CAS
                    bool isGap = (seq > last + 1);
                    if (isGap) {
                        _packetLossEvents.fetch_add(1, std::memory_order_relaxed);
                    }

                    // Atomically update if still valid
                    if (_lastReceivedSequence.compare_exchange_weak(last, seq, std::memory_order_relaxed,
                                                                    std::memory_order_relaxed)) {
                        updateSucceeded = true;
                        break;
                    }

                    // CAS failed - verify if packet is truly a duplicate
                    uint32_t current = _lastReceivedSequence.load(std::memory_order_relaxed);
                    if (seq <= current) {
                        // Packet is genuinely old/duplicate relative to current state
                        _duplicatePacketsReceived.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    // Otherwise, another thread raced ahead - retry with new value
                }

                // Track CAS retry exhaustion for diagnostics
                if (!updateSucceeded) {
                    _sequenceUpdateFailures.fetch_add(1, std::memory_order_relaxed);
                    // Packet dropped - do not invoke callback
                    return;
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _propertyUpdateCallback) {
                    _propertyUpdateCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::SCENE_SNAPSHOT_CHUNK:
            {
                ENTROPY_LOG_INFO(std::format("NetworkSession: Received SCENE_SNAPSHOT_CHUNK ({} bytes), callback={}",
                                             data.size(), _sceneSnapshotCallback ? "set" : "null"));
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _sceneSnapshotCallback) {
                    _sceneSnapshotCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::HANDSHAKE:
            {
                // Server-side handshake handling: automatically respond
                auto handshake = message.getHandshake();
                std::string clientType = handshake.getClientType().cStr();
                std::string clientId = handshake.getClientId().cStr();

                try {
                    capnp::MallocMessageBuilder builder;
                    auto msg = builder.initRoot<Protocol::Message>();
                    auto response = msg.initHandshakeResponse();

                    response.setSuccess(true);
                    response.setServerId(_sessionId);  // Use session ID as server ID
                    response.setErrorMessage("");

                    // Echo capability support back to client
                    response.setSupportsSchemaMetadata(handshake.getSupportsSchemaMetadata());
                    response.setSupportsSchemaAck(handshake.getSupportsSchemaAck());
                    response.setSupportsSchemaAdvert(handshake.getSupportsSchemaAdvert());

                    auto serialized = serialize(builder);
                    ENTROPY_LOG_DEBUG(std::format("Handshake serialized, success={}", serialized.success()));
                    if (serialized.success()) {
                        ENTROPY_LOG_DEBUG("Sending HANDSHAKE_RESPONSE");
                        _connection->send(serialized.value);
                        ENTROPY_LOG_DEBUG("HANDSHAKE_RESPONSE sent");
                        _handshakeComplete = true;

                        // Auto-send all public schemas to newly connected client
                        if (_schemaRegistry) {
                            auto publicSchemas = _schemaRegistry->getPublicSchemas();
                            for (const auto& schema : publicSchemas) {
                                // Send schema advertisement for each public schema
                                auto result = sendSchemaAdvertisement(schema.typeHash, schema.appId,
                                                                      schema.componentName, schema.schemaVersion);

                                // Log errors but continue with other schemas
                                if (result.failed()) {
                                    ENTROPY_LOG_WARNING_CAT(
                                        "NetworkSession",
                                        std::format("Failed to auto-send schema {}.{}: {}", schema.appId,
                                                    schema.componentName, result.errorMessage));
                                }
                            }
                        }

                        // Notify application that handshake is complete
                        // Copy callback under mutex to avoid race with setHandshakeCallback
                        HandshakeCallback callback;
                        {
                            std::lock_guard<std::mutex> lock(_mutex);
                            callback = _handshakeCallback;
                        }

                        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                        if (!_shuttingDown.load(std::memory_order_acquire) && callback) {
                            callback(clientType, clientId);
                        }
                        _activeCallbacks.fetch_sub(1, std::memory_order_release);
                    } else {
                        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                        if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                            _errorCallback(NetworkError::SerializationFailed, serialized.errorMessage);
                        }
                        _activeCallbacks.fetch_sub(1, std::memory_order_release);
                    }
                } catch (const std::exception& e) {
                    _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                    if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                        _errorCallback(NetworkError::SerializationFailed, e.what());
                    }
                    _activeCallbacks.fetch_sub(1, std::memory_order_release);
                }
                break;
            }

            case Protocol::Message::HANDSHAKE_RESPONSE:
            {
                auto resp = message.getHandshakeResponse();
                ENTROPY_LOG_DEBUG("Received HANDSHAKE_RESPONSE");
                if (resp.getSuccess()) {
                    _handshakeComplete = true;
                    ENTROPY_LOG_DEBUG("Handshake marked complete, checking callback...");

                    // Copy callback under mutex to avoid race with setHandshakeCallback
                    HandshakeCallback callback;
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        callback = _handshakeCallback;
                    }

                    bool hasCallback = static_cast<bool>(callback);
                    bool shuttingDown = _shuttingDown.load(std::memory_order_acquire);
                    ENTROPY_LOG_DEBUG(std::format("hasCallback={}, shuttingDown={}", hasCallback, shuttingDown));

                    // Notify application that handshake is complete (client-side)
                    _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                    if (!_shuttingDown.load(std::memory_order_acquire) && callback) {
                        ENTROPY_LOG_DEBUG("About to invoke handshake callback");
                        callback(_clientType, _clientId);
                        ENTROPY_LOG_DEBUG("Handshake callback invoked");
                    } else {
                        ENTROPY_LOG_DEBUG("Skipping callback invocation");
                    }
                    _activeCallbacks.fetch_sub(1, std::memory_order_release);
                } else {
                    _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                    if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                        _errorCallback(NetworkError::HandshakeFailed, std::string(resp.getErrorMessage().cStr()));
                    }
                    _activeCallbacks.fetch_sub(1, std::memory_order_release);
                    disconnect();
                }
                break;
            }

            case Protocol::Message::REGISTER_SCHEMA_RESPONSE:
            {
                auto resp = message.getRegisterSchemaResponse();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _registerSchemaResponseCallback) {
                    _registerSchemaResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::QUERY_PUBLIC_SCHEMAS_RESPONSE:
            {
                auto resp = message.getQueryPublicSchemasResponse();
                auto schemasReader = resp.getSchemas();

                std::vector<ComponentSchema> schemas;
                schemas.reserve(schemasReader.size());

                for (auto schemaReader : schemasReader) {
                    auto result = deserializeComponentSchema(schemaReader);
                    if (result.success()) {
                        schemas.push_back(std::move(result.value));
                    } else {
                        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                        if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                            _errorCallback(result.error, result.errorMessage);
                        }
                        _activeCallbacks.fetch_sub(1, std::memory_order_release);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _queryPublicSchemasResponseCallback) {
                    _queryPublicSchemasResponseCallback(schemas);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::PUBLISH_SCHEMA_RESPONSE:
            {
                auto resp = message.getPublishSchemaResponse();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _publishSchemaResponseCallback) {
                    _publishSchemaResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::UNPUBLISH_SCHEMA_RESPONSE:
            {
                auto resp = message.getUnpublishSchemaResponse();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _unpublishSchemaResponseCallback) {
                    _unpublishSchemaResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::SCHEMA_NACK:
            {
                auto nack = message.getSchemaNack();
                auto typeHashReader = nack.getTypeHash();
                ComponentTypeHash typeHash{typeHashReader.getHigh(), typeHashReader.getLow()};
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _schemaNackCallback) {
                    _schemaNackCallback(typeHash, std::string(nack.getReason().cStr()), nack.getTimestamp());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::SCHEMA_ADVERTISEMENT:
            {
                auto advert = message.getSchemaAdvertisement();
                auto typeHashReader = advert.getTypeHash();
                ComponentTypeHash typeHash{typeHashReader.getHigh(), typeHashReader.getLow()};
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _schemaAdvertisementCallback) {
                    _schemaAdvertisementCallback(typeHash, std::string(advert.getAppId().cStr()),
                                                 std::string(advert.getComponentName().cStr()),
                                                 advert.getSchemaVersion());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::HEARTBEAT:
            {
                auto heartbeat = message.getHeartbeat();
                uint64_t clientTimestamp = heartbeat.getTimestamp();

                // Update last heartbeat received time
                auto now = std::chrono::steady_clock::now();
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                _lastHeartbeatReceivedMs.store(static_cast<uint64_t>(nowMs), std::memory_order_relaxed);

                // Send heartbeat response back to client
                sendHeartbeatResponse(clientTimestamp);

                // Invoke callback if set (for server-side tracking)
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _heartbeatCallback) {
                    _heartbeatCallback(clientTimestamp);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::HEARTBEAT_RESPONSE:
            {
                auto response = message.getHeartbeatResponse();
                uint64_t clientTimestamp = response.getTimestamp();
                uint64_t serverTime = response.getServerTime();

                // Invoke callback for RTT calculation (client-side)
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _heartbeatResponseCallback) {
                    _heartbeatResponseCallback(clientTimestamp, serverTime);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

                // ========================================================================
                // Asset System Messages
                // ========================================================================

            case Protocol::Message::ASSET_ADVERTISE_REQUEST:
            {
                auto req = message.getAssetAdvertiseRequest();
                std::string appId(req.getAppId().cStr());
                uint64_t requestId = req.getRequestId();
                auto entriesReader = req.getEntries();

                std::vector<AssetEntryData> entries;
                entries.reserve(entriesReader.size());
                for (auto src : entriesReader) {
                    AssetEntryData entry;
                    auto idData = src.getId();
                    if (idData.size() == 32) {
                        std::memcpy(entry.id.data(), idData.begin(), 32);
                    }
                    entry.uri = std::string(src.getUri().cStr());
                    entry.contentType = src.getContentType();
                    entry.sizeBytes = src.getSizeBytes();
                    entry.encrypted = src.getEncrypted();
                    auto hashData = src.getPlaintextHash();
                    if (hashData.size() == 32) {
                        std::memcpy(entry.plaintextHash.data(), hashData.begin(), 32);
                    }
                    entry.appId = std::string(src.getAppId().cStr());
                    entry.persistent = src.getPersistent();
                    entries.push_back(std::move(entry));
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetAdvertiseCallback) {
                    _assetAdvertiseCallback(appId, entries, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_ADVERTISE_RESPONSE:
            {
                auto resp = message.getAssetAdvertiseResponse();
                uint64_t requestId = resp.getRequestId();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetAdvertiseResponseCallback) {
                    _assetAdvertiseResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()),
                                                    requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_WITHDRAW_REQUEST:
            {
                auto req = message.getAssetWithdrawRequest();
                uint64_t requestId = req.getRequestId();
                auto idsReader = req.getAssetIds();

                std::vector<std::array<uint8_t, 32>> assetIds;
                assetIds.reserve(idsReader.size());
                for (auto idData : idsReader) {
                    if (idData.size() == 32) {
                        std::array<uint8_t, 32> id;
                        std::memcpy(id.data(), idData.begin(), 32);
                        assetIds.push_back(id);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetWithdrawCallback) {
                    _assetWithdrawCallback(assetIds, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_WITHDRAW_RESPONSE:
            {
                auto resp = message.getAssetWithdrawResponse();
                uint64_t requestId = resp.getRequestId();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetWithdrawResponseCallback) {
                    _assetWithdrawResponseCallback(resp.getSuccess(), resp.getRemovedCount(),
                                                   std::string(resp.getErrorMessage().cStr()), requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_WITHDRAW_ALL_REQUEST:
            {
                auto req = message.getAssetWithdrawAllRequest();
                std::string appId(req.getAppId().cStr());
                uint64_t requestId = req.getRequestId();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetWithdrawAllCallback) {
                    _assetWithdrawAllCallback(appId, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_WITHDRAW_ALL_RESPONSE:
            {
                auto resp = message.getAssetWithdrawAllResponse();
                uint64_t requestId = resp.getRequestId();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetWithdrawAllResponseCallback) {
                    _assetWithdrawAllResponseCallback(resp.getSuccess(), resp.getRemovedCount(),
                                                      std::string(resp.getErrorMessage().cStr()), requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_RESOLVE_REQUEST:
            {
                auto req = message.getAssetResolveRequest();
                auto idData = req.getAssetId();
                uint64_t requestId = req.getRequestId();

                std::array<uint8_t, 32> assetId{};
                if (idData.size() == 32) {
                    std::memcpy(assetId.data(), idData.begin(), 32);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetResolveCallback) {
                    _assetResolveCallback(assetId, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_RESOLVE_RESPONSE:
            {
                auto resp = message.getAssetResolveResponse();

                AssetResolveResponseData responseData;
                responseData.found = resp.getFound();
                responseData.deliveryMethod = resp.getDeliveryMethod();
                responseData.requestId = resp.getRequestId();

                if (resp.getFound()) {
                    auto entry = resp.getEntry();
                    auto idData = entry.getId();
                    if (idData.size() == 32) {
                        std::memcpy(responseData.entry.id.data(), idData.begin(), 32);
                    }
                    responseData.entry.uri = std::string(entry.getUri().cStr());
                    responseData.entry.contentType = entry.getContentType();
                    responseData.entry.sizeBytes = entry.getSizeBytes();
                    responseData.entry.encrypted = entry.getEncrypted();
                    auto hashData = entry.getPlaintextHash();
                    if (hashData.size() == 32) {
                        std::memcpy(responseData.entry.plaintextHash.data(), hashData.begin(), 32);
                    }
                    responseData.entry.appId = std::string(entry.getAppId().cStr());
                    responseData.entry.persistent = entry.getPersistent();
                }

                responseData.hasKey = resp.getHasKey();
                if (resp.getHasKey()) {
                    auto keyData = resp.getKey();
                    if (keyData.size() == 32) {
                        std::memcpy(responseData.key.data(), keyData.begin(), 32);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetResolveResponseCallback) {
                    _assetResolveResponseCallback(responseData);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_RESOLVE_BATCH_REQUEST:
            {
                auto req = message.getAssetResolveBatchRequest();
                uint64_t requestId = req.getRequestId();
                auto idsReader = req.getAssetIds();

                std::vector<std::array<uint8_t, 32>> assetIds;
                assetIds.reserve(idsReader.size());
                for (auto idData : idsReader) {
                    if (idData.size() == 32) {
                        std::array<uint8_t, 32> id;
                        std::memcpy(id.data(), idData.begin(), 32);
                        assetIds.push_back(id);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetResolveBatchCallback) {
                    _assetResolveBatchCallback(assetIds, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_RESOLVE_BATCH_RESPONSE:
            {
                auto resp = message.getAssetResolveBatchResponse();
                uint64_t requestId = resp.getRequestId();
                auto responsesReader = resp.getResponses();

                std::vector<AssetResolveResponseData> responses;
                responses.reserve(responsesReader.size());

                for (auto src : responsesReader) {
                    AssetResolveResponseData responseData;
                    responseData.found = src.getFound();
                    responseData.deliveryMethod = src.getDeliveryMethod();

                    if (src.getFound()) {
                        auto entry = src.getEntry();
                        auto idData = entry.getId();
                        if (idData.size() == 32) {
                            std::memcpy(responseData.entry.id.data(), idData.begin(), 32);
                        }
                        responseData.entry.uri = std::string(entry.getUri().cStr());
                        responseData.entry.contentType = entry.getContentType();
                        responseData.entry.sizeBytes = entry.getSizeBytes();
                        responseData.entry.encrypted = entry.getEncrypted();
                        auto hashData = entry.getPlaintextHash();
                        if (hashData.size() == 32) {
                            std::memcpy(responseData.entry.plaintextHash.data(), hashData.begin(), 32);
                        }
                        responseData.entry.appId = std::string(entry.getAppId().cStr());
                        responseData.entry.persistent = entry.getPersistent();
                    }

                    responseData.hasKey = src.getHasKey();
                    if (src.getHasKey()) {
                        auto keyData = src.getKey();
                        if (keyData.size() == 32) {
                            std::memcpy(responseData.key.data(), keyData.begin(), 32);
                        }
                    }

                    responses.push_back(std::move(responseData));
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetResolveBatchResponseCallback) {
                    _assetResolveBatchResponseCallback(responses, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_PROVIDE_KEY_REQUEST:
            {
                auto req = message.getAssetProvideKeyRequest();
                auto idData = req.getAssetId();
                auto keyData = req.getKey();
                uint64_t requestId = req.getRequestId();

                std::array<uint8_t, 32> assetId{};
                std::array<uint8_t, 32> key{};

                if (idData.size() == 32) {
                    std::memcpy(assetId.data(), idData.begin(), 32);
                }
                if (keyData.size() == 32) {
                    std::memcpy(key.data(), keyData.begin(), 32);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetProvideKeyCallback) {
                    _assetProvideKeyCallback(assetId, key, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_PROVIDE_KEY_RESPONSE:
            {
                auto resp = message.getAssetProvideKeyResponse();
                uint64_t requestId = resp.getRequestId();
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetProvideKeyResponseCallback) {
                    _assetProvideKeyResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()),
                                                     requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_REQUEST:
            {
                auto req = message.getAssetUploadRequest();
                std::string appId(req.getAppId().cStr());
                auto dataReader = req.getData();
                std::vector<uint8_t> data(dataReader.begin(), dataReader.end());
                uint8_t contentType = req.getContentType();
                bool persistent = req.getPersistent();
                uint64_t requestId = req.getRequestId();

                // Parse asset metadata
                AssetMetadataData metadata;
                if (req.hasMetadata()) {
                    auto metaReader = req.getMetadata();
                    if (metaReader.isShader()) {
                        metadata.type = AssetMetadataType::Shader;
                        auto shaderMeta = metaReader.getShader();
                        ShaderMetadataData shaderData;
                        shaderData.name = shaderMeta.getName().cStr();
                        shaderData.description = shaderMeta.getDescription().cStr();
                        shaderData.renderQueue = shaderMeta.getRenderQueue();
                        shaderData.castsShadows = shaderMeta.getCastsShadows();
                        shaderData.transparent = shaderMeta.getTransparent();
                        shaderData.author = shaderMeta.getAuthor().cStr();
                        for (auto kw : shaderMeta.getKeywords()) {
                            shaderData.keywords.push_back(kw.cStr());
                        }
                        for (auto param : shaderMeta.getParameters()) {
                            ShaderParameterDefData paramData;
                            paramData.name = param.getName().cStr();
                            paramData.displayName = param.getDisplayName().cStr();
                            paramData.type = fromCapnpPropertyType(static_cast<uint16_t>(param.getType()));
                            if (param.hasDefaultValue()) {
                                paramData.defaultValue = deserializePropertyValue(param.getDefaultValue());
                            }
                            for (auto attr : param.getAttributes()) {
                                paramData.attributes[attr.getKey().cStr()] = attr.getValue().cStr();
                            }
                            shaderData.parameters.push_back(std::move(paramData));
                        }
                        metadata.shaderMetadata = std::move(shaderData);
                    } else if (metaReader.isTexture()) {
                        metadata.type = AssetMetadataType::Texture;
                        auto texMeta = metaReader.getTexture();
                        TextureMetadataData texData;
                        texData.width = texMeta.getWidth();
                        texData.height = texMeta.getHeight();
                        texData.depth = texMeta.getDepth();
                        texData.mipLevels = texMeta.getMipLevels();
                        texData.arrayLayers = texMeta.getArrayLayers();
                        texData.textureType = texMeta.getTextureType();
                        texData.format = texMeta.getFormat();
                        texData.colorSpace = texMeta.getColorSpace();
                        texData.generateMips = texMeta.getGenerateMips();
                        texData.sourceFile = texMeta.getSourceFile().cStr();
                        metadata.textureMetadata = std::move(texData);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadCallback) {
                    _assetUploadCallback(appId, data, contentType, persistent, requestId, metadata);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_RESPONSE:
            {
                auto resp = message.getAssetUploadResponse();
                auto idData = resp.getAssetId();
                uint64_t requestId = resp.getRequestId();

                std::array<uint8_t, 32> assetId{};
                if (idData.size() == 32) {
                    std::memcpy(assetId.data(), idData.begin(), 32);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadResponseCallback) {
                    _assetUploadResponseCallback(resp.getSuccess(), assetId, std::string(resp.getUri().cStr()),
                                                 std::string(resp.getErrorMessage().cStr()), requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_FETCH_REQUEST:
            {
                auto req = message.getAssetFetchRequest();
                auto idData = req.getAssetId();
                uint64_t requestId = req.getRequestId();

                std::array<uint8_t, 32> assetId{};
                if (idData.size() == 32) {
                    std::memcpy(assetId.data(), idData.begin(), 32);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetFetchCallback) {
                    _assetFetchCallback(assetId, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_FETCH_RESPONSE:
            {
                auto resp = message.getAssetFetchResponse();
                auto dataReader = resp.getData();
                std::vector<uint8_t> data(dataReader.begin(), dataReader.end());
                uint64_t requestId = resp.getRequestId();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetFetchResponseCallback) {
                    _assetFetchResponseCallback(resp.getFound(), data, std::string(resp.getErrorMessage().cStr()),
                                                requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

                // ================================================================
                // Chunked Upload Messages
                // ================================================================

            case Protocol::Message::ASSET_UPLOAD_BEGIN_REQUEST:
            {
                auto req = message.getAssetUploadBeginRequest();
                AssetUploadBeginData data;
                data.appId = std::string(req.getAppId().cStr());
                data.totalSize = req.getTotalSize();
                data.contentType = req.getContentType();
                data.persistent = req.getPersistent();
                data.chunkSize = req.getChunkSize();
                data.encrypted = req.getEncrypted();
                auto hashData = req.getPlaintextHash();
                if (hashData.size() == 32) {
                    std::memcpy(data.plaintextHash.data(), hashData.begin(), 32);
                }
                data.requestId = req.getRequestId();

                // Parse asset metadata
                if (req.hasMetadata()) {
                    auto metaReader = req.getMetadata();
                    if (metaReader.isShader()) {
                        data.metadata.type = AssetMetadataType::Shader;
                        auto shaderMeta = metaReader.getShader();
                        ShaderMetadataData shaderData;
                        shaderData.name = shaderMeta.getName().cStr();
                        shaderData.description = shaderMeta.getDescription().cStr();
                        shaderData.renderQueue = shaderMeta.getRenderQueue();
                        shaderData.castsShadows = shaderMeta.getCastsShadows();
                        shaderData.transparent = shaderMeta.getTransparent();
                        shaderData.author = shaderMeta.getAuthor().cStr();
                        for (auto kw : shaderMeta.getKeywords()) {
                            shaderData.keywords.push_back(kw.cStr());
                        }
                        for (auto param : shaderMeta.getParameters()) {
                            ShaderParameterDefData paramData;
                            paramData.name = param.getName().cStr();
                            paramData.displayName = param.getDisplayName().cStr();
                            paramData.type = fromCapnpPropertyType(static_cast<uint16_t>(param.getType()));
                            if (param.hasDefaultValue()) {
                                paramData.defaultValue = deserializePropertyValue(param.getDefaultValue());
                            }
                            for (auto attr : param.getAttributes()) {
                                paramData.attributes[attr.getKey().cStr()] = attr.getValue().cStr();
                            }
                            shaderData.parameters.push_back(std::move(paramData));
                        }
                        data.metadata.shaderMetadata = std::move(shaderData);
                    } else if (metaReader.isTexture()) {
                        data.metadata.type = AssetMetadataType::Texture;
                        auto texMeta = metaReader.getTexture();
                        TextureMetadataData texData;
                        texData.width = texMeta.getWidth();
                        texData.height = texMeta.getHeight();
                        texData.depth = texMeta.getDepth();
                        texData.mipLevels = texMeta.getMipLevels();
                        texData.arrayLayers = texMeta.getArrayLayers();
                        texData.textureType = texMeta.getTextureType();
                        texData.format = texMeta.getFormat();
                        texData.colorSpace = texMeta.getColorSpace();
                        texData.generateMips = texMeta.getGenerateMips();
                        texData.sourceFile = texMeta.getSourceFile().cStr();
                        data.metadata.textureMetadata = std::move(texData);
                    }
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadBeginCallback) {
                    _assetUploadBeginCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_BEGIN_RESPONSE:
            {
                auto resp = message.getAssetUploadBeginResponse();
                AssetUploadBeginResponseData data;
                data.success = resp.getSuccess();
                auto uploadIdData = resp.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(data.uploadId.data(), uploadIdData.begin(), 16);
                }
                data.chunkSize = resp.getChunkSize();
                data.errorMessage = std::string(resp.getErrorMessage().cStr());
                data.requestId = resp.getRequestId();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadBeginResponseCallback) {
                    _assetUploadBeginResponseCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_CHUNK_REQUEST:
            {
                auto req = message.getAssetUploadChunkRequest();
                AssetUploadChunkData data;
                auto uploadIdData = req.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(data.uploadId.data(), uploadIdData.begin(), 16);
                }
                data.offset = req.getOffset();
                auto chunkData = req.getData();
                data.data.assign(chunkData.begin(), chunkData.end());
                data.sequence = req.getSequence();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadChunkCallback) {
                    _assetUploadChunkCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_CHUNK_RESPONSE:
            {
                auto resp = message.getAssetUploadChunkResponse();
                AssetUploadChunkResponseData data;
                data.success = resp.getSuccess();
                auto uploadIdData = resp.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(data.uploadId.data(), uploadIdData.begin(), 16);
                }
                data.bytesReceived = resp.getBytesReceived();
                data.errorMessage = std::string(resp.getErrorMessage().cStr());

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadChunkResponseCallback) {
                    _assetUploadChunkResponseCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_COMPLETE_REQUEST:
            {
                auto req = message.getAssetUploadCompleteRequest();
                AssetUploadCompleteData data;
                auto uploadIdData = req.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(data.uploadId.data(), uploadIdData.begin(), 16);
                }
                data.totalChunks = req.getTotalChunks();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadCompleteCallback) {
                    _assetUploadCompleteCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_COMPLETE_RESPONSE:
            {
                auto resp = message.getAssetUploadCompleteResponse();
                AssetUploadCompleteResponseData data;
                data.success = resp.getSuccess();
                auto assetIdData = resp.getAssetId();
                if (assetIdData.size() == 32) {
                    std::memcpy(data.assetId.data(), assetIdData.begin(), 32);
                }
                data.uri = std::string(resp.getUri().cStr());
                data.bytesStored = resp.getBytesStored();
                data.errorMessage = std::string(resp.getErrorMessage().cStr());
                auto uploadIdData = resp.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(data.uploadId.data(), uploadIdData.begin(), 16);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadCompleteResponseCallback) {
                    _assetUploadCompleteResponseCallback(data);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_CANCEL_REQUEST:
            {
                auto req = message.getAssetUploadCancelRequest();
                std::array<uint8_t, 16> uploadId{};
                auto uploadIdData = req.getUploadId();
                if (uploadIdData.size() == 16) {
                    std::memcpy(uploadId.data(), uploadIdData.begin(), 16);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadCancelCallback) {
                    _assetUploadCancelCallback(uploadId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ASSET_UPLOAD_CANCEL_RESPONSE:
            {
                auto resp = message.getAssetUploadCancelResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _assetUploadCancelResponseCallback) {
                    _assetUploadCancelResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

                // ================================================================
                // Scene Management Messages
                // ================================================================

            case Protocol::Message::CREATE_SCENE_REQUEST:
            {
                auto req = message.getCreateSceneRequest();
                std::string sceneName(req.getSceneName().cStr());
                bool transient = req.getTransient();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _createSceneCallback) {
                    _createSceneCallback(sceneName, transient);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::CREATE_SCENE_RESPONSE:
            {
                auto resp = message.getCreateSceneResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _createSceneResponseCallback) {
                    _createSceneResponseCallback(resp.getSuccess(), resp.getSceneId(),
                                                 std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::DESTROY_SCENE_REQUEST:
            {
                auto req = message.getDestroySceneRequest();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _destroySceneCallback) {
                    _destroySceneCallback(req.getSceneId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::DESTROY_SCENE_RESPONSE:
            {
                auto resp = message.getDestroySceneResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _destroySceneResponseCallback) {
                    _destroySceneResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::SET_SCENE_ENABLED_REQUEST:
            {
                auto req = message.getSetSceneEnabledRequest();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _setSceneEnabledCallback) {
                    _setSceneEnabledCallback(req.getSceneId(), req.getEnabled());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::SET_SCENE_ENABLED_RESPONSE:
            {
                auto resp = message.getSetSceneEnabledResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _setSceneEnabledResponseCallback) {
                    _setSceneEnabledResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ADD_ENTITY_TO_SCENE_REQUEST:
            {
                auto req = message.getAddEntityToSceneRequest();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _addEntityToSceneCallback) {
                    _addEntityToSceneCallback(req.getEntityId(), req.getSceneId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::ADD_ENTITY_TO_SCENE_RESPONSE:
            {
                auto resp = message.getAddEntityToSceneResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _addEntityToSceneResponseCallback) {
                    _addEntityToSceneResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

                // ============================================================================
                // Material System Messages
                // ============================================================================

            case Protocol::Message::CREATE_MATERIAL_REQUEST:
            {
                auto req = message.getCreateMaterialRequest();
                auto matReader = req.getMaterial();

                MaterialAssetData material;
                material.name = matReader.getName().cStr();

                auto shaderIdData = matReader.getShaderAssetId();
                if (shaderIdData.size() == 32) {
                    std::copy(shaderIdData.begin(), shaderIdData.end(), material.shaderAssetId.begin());
                }

                for (auto propReader : matReader.getProperties()) {
                    MaterialPropertyData prop;
                    prop.name = propReader.getName().cStr();
                    prop.value = deserializePropertyValue(propReader.getValue());
                    material.properties.push_back(std::move(prop));
                }

                for (auto kw : matReader.getEnabledKeywords()) {
                    material.enabledKeywords.push_back(kw.cStr());
                }

                material.renderQueue = matReader.getRenderQueue();
                material.castsShadows = matReader.getCastsShadows();
                material.receivesShadows = matReader.getReceivesShadows();
                material.depthWrite = matReader.getDepthWrite();
                material.creatorSessionId = matReader.getCreatorSessionId();
                material.version = matReader.getVersion();
                material.modifiedAt = matReader.getModifiedAt();
                material.appId = matReader.getAppId().cStr();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _createMaterialCallback) {
                    _createMaterialCallback(material, req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::CREATE_MATERIAL_RESPONSE:
            {
                auto resp = message.getCreateMaterialResponse();
                std::array<uint8_t, 32> materialId{};
                auto idData = resp.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }
                uint64_t requestId = resp.getRequestId();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _createMaterialResponseCallback) {
                    _createMaterialResponseCallback(resp.getSuccess(), materialId,
                                                    std::string(resp.getErrorMessage().cStr()), requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::UPDATE_MATERIAL_PROPERTY_REQUEST:
            {
                auto req = message.getUpdateMaterialPropertyRequest();
                std::array<uint8_t, 32> materialId{};
                auto idData = req.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                PropertyValue value = deserializePropertyValue(req.getValue());

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _updateMaterialPropertyCallback) {
                    _updateMaterialPropertyCallback(materialId, std::string(req.getPropertyName().cStr()), value,
                                                    req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::UPDATE_MATERIAL_PROPERTY_RESPONSE:
            {
                auto resp = message.getUpdateMaterialPropertyResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _updateMaterialPropertyResponseCallback) {
                    _updateMaterialPropertyResponseCallback(resp.getSuccess(), resp.getNewVersion(),
                                                            std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::UPDATE_MATERIAL_PROPERTIES_BATCH_REQUEST:
            {
                auto req = message.getUpdateMaterialPropertiesBatchRequest();
                std::array<uint8_t, 32> materialId{};
                auto idData = req.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                std::vector<MaterialPropertyData> properties;
                for (auto propReader : req.getProperties()) {
                    MaterialPropertyData prop;
                    prop.name = propReader.getName().cStr();
                    prop.value = deserializePropertyValue(propReader.getValue());
                    properties.push_back(std::move(prop));
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _updateMaterialPropertiesBatchCallback) {
                    _updateMaterialPropertiesBatchCallback(materialId, properties, req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::UPDATE_MATERIAL_PROPERTIES_BATCH_RESPONSE:
            {
                auto resp = message.getUpdateMaterialPropertiesBatchResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _updateMaterialPropertiesBatchResponseCallback) {
                    _updateMaterialPropertiesBatchResponseCallback(resp.getSuccess(), resp.getNewVersion(),
                                                                   std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_PROPERTY_UPDATE:
            {
                auto update = message.getMaterialPropertyUpdate();
                std::array<uint8_t, 32> materialId{};
                auto idData = update.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                PropertyValue value = deserializePropertyValue(update.getValue());

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialPropertyUpdateCallback) {
                    _materialPropertyUpdateCallback(materialId, std::string(update.getPropertyName().cStr()), value,
                                                    update.getVersion(), update.getOriginSessionId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_SUBSCRIBE_REQUEST:
            {
                auto req = message.getMaterialSubscribeRequest();
                std::array<uint8_t, 32> materialId{};
                auto idData = req.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialSubscribeCallback) {
                    _materialSubscribeCallback(materialId, req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_SUBSCRIBE_RESPONSE:
            {
                auto resp = message.getMaterialSubscribeResponse();

                MaterialAssetData material;
                if (resp.getSuccess() && resp.hasMaterial()) {
                    auto matReader = resp.getMaterial();
                    material.name = matReader.getName().cStr();

                    auto shaderIdData = matReader.getShaderAssetId();
                    if (shaderIdData.size() == 32) {
                        std::copy(shaderIdData.begin(), shaderIdData.end(), material.shaderAssetId.begin());
                    }

                    for (auto propReader : matReader.getProperties()) {
                        MaterialPropertyData prop;
                        prop.name = propReader.getName().cStr();
                        prop.value = deserializePropertyValue(propReader.getValue());
                        material.properties.push_back(std::move(prop));
                    }

                    for (auto kw : matReader.getEnabledKeywords()) {
                        material.enabledKeywords.push_back(kw.cStr());
                    }

                    material.renderQueue = matReader.getRenderQueue();
                    material.castsShadows = matReader.getCastsShadows();
                    material.receivesShadows = matReader.getReceivesShadows();
                    material.depthWrite = matReader.getDepthWrite();
                    material.creatorSessionId = matReader.getCreatorSessionId();
                    material.version = matReader.getVersion();
                    material.modifiedAt = matReader.getModifiedAt();
                    material.appId = matReader.getAppId().cStr();
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialSubscribeResponseCallback) {
                    _materialSubscribeResponseCallback(resp.getSuccess(), material,
                                                       std::string(resp.getErrorMessage().cStr()), resp.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_UNSUBSCRIBE_REQUEST:
            {
                auto req = message.getMaterialUnsubscribeRequest();
                std::array<uint8_t, 32> materialId{};
                auto idData = req.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialUnsubscribeCallback) {
                    _materialUnsubscribeCallback(materialId, req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_UNSUBSCRIBE_RESPONSE:
            {
                auto resp = message.getMaterialUnsubscribeResponse();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialUnsubscribeResponseCallback) {
                    _materialUnsubscribeResponseCallback(resp.getSuccess(), std::string(resp.getErrorMessage().cStr()));
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::GET_MATERIAL_REQUEST:
            {
                auto req = message.getGetMaterialRequest();
                std::array<uint8_t, 32> materialId{};
                auto idData = req.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _getMaterialCallback) {
                    _getMaterialCallback(materialId, req.getRequestId());
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::GET_MATERIAL_RESPONSE:
            {
                auto resp = message.getGetMaterialResponse();

                MaterialAssetData material;
                if (resp.getFound() && resp.hasMaterial()) {
                    auto matReader = resp.getMaterial();
                    material.name = matReader.getName().cStr();

                    auto shaderIdData = matReader.getShaderAssetId();
                    if (shaderIdData.size() == 32) {
                        std::copy(shaderIdData.begin(), shaderIdData.end(), material.shaderAssetId.begin());
                    }

                    for (auto propReader : matReader.getProperties()) {
                        MaterialPropertyData prop;
                        prop.name = propReader.getName().cStr();
                        prop.value = deserializePropertyValue(propReader.getValue());
                        material.properties.push_back(std::move(prop));
                    }

                    for (auto kw : matReader.getEnabledKeywords()) {
                        material.enabledKeywords.push_back(kw.cStr());
                    }

                    material.renderQueue = matReader.getRenderQueue();
                    material.castsShadows = matReader.getCastsShadows();
                    material.receivesShadows = matReader.getReceivesShadows();
                    material.depthWrite = matReader.getDepthWrite();
                    material.creatorSessionId = matReader.getCreatorSessionId();
                    material.version = matReader.getVersion();
                    material.modifiedAt = matReader.getModifiedAt();
                    material.appId = matReader.getAppId().cStr();
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _getMaterialResponseCallback) {
                    // Note: Cap'n Proto schema has requestId but callback expects errorMessage
                    // Pass empty error message when found, generic error when not found
                    std::string errorMessage = resp.getFound() ? "" : "Material not found";
                    _getMaterialResponseCallback(resp.getFound(), material, errorMessage);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MATERIAL_RESOLVED:
            {
                auto resolved = message.getMaterialResolved();
                std::array<uint8_t, 32> materialId{};
                auto idData = resolved.getMaterialId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), materialId.begin());
                }

                MaterialAssetData material;
                if (resolved.hasMaterial()) {
                    auto matReader = resolved.getMaterial();
                    material.name = matReader.getName().cStr();

                    auto shaderIdData = matReader.getShaderAssetId();
                    if (shaderIdData.size() == 32) {
                        std::copy(shaderIdData.begin(), shaderIdData.end(), material.shaderAssetId.begin());
                    }

                    for (auto propReader : matReader.getProperties()) {
                        MaterialPropertyData prop;
                        prop.name = propReader.getName().cStr();
                        prop.value = deserializePropertyValue(propReader.getValue());
                        material.properties.push_back(std::move(prop));
                    }

                    for (auto kw : matReader.getEnabledKeywords()) {
                        material.enabledKeywords.push_back(kw.cStr());
                    }

                    material.renderQueue = matReader.getRenderQueue();
                    material.castsShadows = matReader.getCastsShadows();
                    material.receivesShadows = matReader.getReceivesShadows();
                    material.depthWrite = matReader.getDepthWrite();
                    material.creatorSessionId = matReader.getCreatorSessionId();
                    material.version = matReader.getVersion();
                    material.modifiedAt = matReader.getModifiedAt();
                    material.appId = matReader.getAppId().cStr();
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _materialResolvedCallback) {
                    _materialResolvedCallback(materialId, material);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MESH_MATERIAL_BINDING_REQUEST:
            {
                auto request = message.getMeshMaterialBindingRequest();
                uint64_t entityId = request.getEntityId();
                uint64_t requestId = request.getRequestId();

                std::vector<std::array<uint8_t, 32>> materialIds;
                for (auto matIdData : request.getMaterialIds()) {
                    std::array<uint8_t, 32> matId{};
                    if (matIdData.size() == 32) {
                        std::copy(matIdData.begin(), matIdData.end(), matId.begin());
                    }
                    materialIds.push_back(matId);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _meshMaterialBindingCallback) {
                    _meshMaterialBindingCallback(entityId, materialIds, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MESH_MATERIAL_BINDING_RESPONSE:
            {
                auto response = message.getMeshMaterialBindingResponse();
                bool success = response.getSuccess();
                std::string errorMessage = response.getErrorMessage().cStr();
                uint64_t requestId = response.getRequestId();

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _meshMaterialBindingResponseCallback) {
                    _meshMaterialBindingResponseCallback(success, errorMessage, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::MESH_MATERIAL_BINDING_UPDATE:
            {
                auto update = message.getMeshMaterialBindingUpdate();
                uint64_t entityId = update.getEntityId();
                uint64_t originSessionId = update.getOriginSessionId();

                std::vector<std::array<uint8_t, 32>> materialIds;
                for (auto matIdData : update.getMaterialIds()) {
                    std::array<uint8_t, 32> matId{};
                    if (matIdData.size() == 32) {
                        std::copy(matIdData.begin(), matIdData.end(), matId.begin());
                    }
                    materialIds.push_back(matId);
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _meshMaterialBindingUpdateCallback) {
                    _meshMaterialBindingUpdateCallback(entityId, materialIds, originSessionId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

                // ==================================================================
                // Shader Protocol Messages
                // ==================================================================

            case Protocol::Message::GET_SHADER_REQUEST:
            {
                auto request = message.getGetShaderRequest();
                uint64_t requestId = request.getRequestId();

                std::array<uint8_t, 32> shaderAssetId{};
                auto idData = request.getShaderAssetId();
                if (idData.size() == 32) {
                    std::copy(idData.begin(), idData.end(), shaderAssetId.begin());
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _getShaderCallback) {
                    _getShaderCallback(shaderAssetId, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            case Protocol::Message::GET_SHADER_RESPONSE:
            {
                auto response = message.getGetShaderResponse();
                uint64_t requestId = response.getRequestId();

                GetShaderResponseData data;
                data.found = response.getFound();
                data.isBuiltin = response.getIsBuiltin();
                data.mainSource = response.getMainSource().cStr();

                // Modules
                for (auto mod : response.getModules()) {
                    ShaderModuleData moduleData;
                    moduleData.moduleName = mod.getModuleName().cStr();
                    moduleData.source = mod.getSource().cStr();
                    data.modules.push_back(std::move(moduleData));
                }

                // Metadata (native typed)
                auto meta = response.getMetadata();
                data.metadata.name = meta.getName().cStr();
                data.metadata.description = meta.getDescription().cStr();
                data.metadata.renderQueue = meta.getRenderQueue();  // int32_t
                data.metadata.castsShadows = meta.getCastsShadows();
                data.metadata.transparent = meta.getTransparent();
                data.metadata.author = meta.getAuthor().cStr();

                for (auto kw : meta.getKeywords()) {
                    data.metadata.keywords.push_back(kw.cStr());
                }

                for (auto param : meta.getParameters()) {
                    ShaderParameterDefData paramData;
                    paramData.name = param.getName().cStr();
                    paramData.displayName = param.getDisplayName().cStr();
                    paramData.type = fromCapnpPropertyType(static_cast<uint16_t>(param.getType()));
                    // Deserialize defaultValue if present
                    if (param.hasDefaultValue()) {
                        paramData.defaultValue = deserializePropertyValue(param.getDefaultValue());
                    }
                    // Parse KeyValue attributes
                    for (auto attr : param.getAttributes()) {
                        paramData.attributes[attr.getKey().cStr()] = attr.getValue().cStr();
                    }
                    data.metadata.parameters.push_back(std::move(paramData));
                }

                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _getShaderResponseCallback) {
                    _getShaderResponseCallback(data, requestId);
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
            }

            default:
                // Unknown or unhandled message type
                _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
                if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
                    _errorCallback(NetworkError::InvalidMessage, "Unknown message type");
                }
                _activeCallbacks.fetch_sub(1, std::memory_order_release);
                break;
        }

    } catch (const std::exception& e) {
        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
        if (!_shuttingDown.load(std::memory_order_acquire) && _errorCallback) {
            _errorCallback(NetworkError::DeserializationFailed, e.what());
        }
        _activeCallbacks.fetch_sub(1, std::memory_order_release);
    }
}

// Property update batching methods

void NetworkSession::setBatchingEnabled(bool enabled) {
    _batchingEnabled.store(enabled, std::memory_order_relaxed);
}

Result<void> NetworkSession::flushPropertyUpdates() {
    // Move pending updates out of accumulator
    std::unordered_map<PropertyHash, PendingPropertyUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(_pendingUpdatesMutex);
        if (_pendingPropertyUpdates.empty()) {
            return Result<void>::ok();  // Nothing to flush
        }
        updates = std::move(_pendingPropertyUpdates);
        _pendingPropertyUpdates.clear();
    }

    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        // Build Cap'n Proto PropertyUpdateBatch
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Protocol::Message>();
        auto batch = message.initPropertyUpdateBatch();

        // Set timestamp and sequence
        batch.setTimestamp(getCurrentTimestampMicros());
        batch.setSequence(_batchSequenceNumber.fetch_add(1, std::memory_order_relaxed));

        // Add all updates
        auto updatesList = batch.initUpdates(updates.size());
        size_t index = 0;
        for (const auto& [hash, pending] : updates) {
            auto update = updatesList[index++];

            // Set property hash
            auto ph = update.initPropertyHash();
            ph.setHigh(hash.high);
            ph.setLow(hash.low);

            // Set type
            update.setExpectedType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(pending.type)));

            // Set value based on type
            auto valueBuilder = update.initValue();
            std::visit(
                [&valueBuilder](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, int32_t>)
                        valueBuilder.setInt32(v);
                    else if constexpr (std::is_same_v<T, int64_t>)
                        valueBuilder.setInt64(v);
                    else if constexpr (std::is_same_v<T, float>)
                        valueBuilder.setFloat32(v);
                    else if constexpr (std::is_same_v<T, double>)
                        valueBuilder.setFloat64(v);
                    else if constexpr (std::is_same_v<T, Vec2>) {
                        auto b = valueBuilder.initVec2();
                        b.setX(v.x);
                        b.setY(v.y);
                    } else if constexpr (std::is_same_v<T, Vec3>) {
                        auto b = valueBuilder.initVec3();
                        b.setX(v.x);
                        b.setY(v.y);
                        b.setZ(v.z);
                    } else if constexpr (std::is_same_v<T, Vec4>) {
                        auto b = valueBuilder.initVec4();
                        b.setX(v.x);
                        b.setY(v.y);
                        b.setZ(v.z);
                        b.setW(v.w);
                    } else if constexpr (std::is_same_v<T, Quat>) {
                        auto b = valueBuilder.initQuat();
                        b.setX(v.x);
                        b.setY(v.y);
                        b.setZ(v.z);
                        b.setW(v.w);
                    } else if constexpr (std::is_same_v<T, std::string>)
                        valueBuilder.setString(v);
                    else if constexpr (std::is_same_v<T, bool>)
                        valueBuilder.setBool(v);
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        valueBuilder.setBytes(kj::arrayPtr(v.data(), v.size()));
                    else if constexpr (std::is_same_v<T, AssetId>)
                        valueBuilder.setAssetId(kj::arrayPtr(v.hash.data(), v.hash.size()));
                },
                pending.value);
        }

        // Serialize
        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // Send on unreliable channel
        auto result = _connection->sendUnreliable(serialized.value);

        // Update statistics
        std::lock_guard<std::mutex> lock(_batchStatsMutex);
        if (result.success()) {
            _batchStats.totalBatchesSent++;
            _batchStats.totalUpdatesSent += updates.size();
            _batchStats.averageBatchSize =
                _batchStats.totalUpdatesSent / std::max(_batchStats.totalBatchesSent, static_cast<uint64_t>(1));
        }

        return result;

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

NetworkSession::PropertyBatchStats NetworkSession::getPropertyBatchStats() const {
    std::lock_guard<std::mutex> lock(_batchStatsMutex);
    return _batchStats;
}

size_t NetworkSession::getPendingPropertyUpdateCount() const {
    std::lock_guard<std::mutex> lock(_pendingUpdatesMutex);
    return _pendingPropertyUpdates.size();
}

// ============================================================================
// EntityBuilder Implementation
// ============================================================================

NetworkSession::EntityBuilder::EntityBuilder(NetworkSession& session, uint64_t entityId, const std::string& appId,
                                             const std::string& typeName, uint64_t parentId)
    : _session(session), _entityId(entityId), _appId(appId), _typeName(typeName), _parentId(parentId) {}

NetworkSession::ComponentHandle NetworkSession::EntityBuilder::attach(const ComponentSchema& schema) {
    return ComponentHandle(*this, schema);
}

Result<void> NetworkSession::EntityBuilder::sync() {
    if (_synced) {
        return Result<void>::err(NetworkError::AlreadyExists, "Entity already synced");
    }
    _synced = true;

    // Group properties by component type
    std::unordered_map<ComponentTypeHash, ComponentGroupData> componentGroups;

    for (const auto& prop : _properties) {
        auto& group = componentGroups[prop.componentType];
        if (group.typeHash.isNull()) {
            group.typeHash = prop.componentType;
            // Try to get component name from schema registry
            if (_session._schemaRegistry) {
                auto schema = _session._schemaRegistry->getSchema(prop.componentType);
                if (schema.has_value()) {
                    group.componentName = schema->componentName;
                } else {
                    group.componentName = "Unknown";
                }
            } else {
                group.componentName = "Unknown";
            }
        }
        group.properties.push_back(prop);
    }

    // Convert to vector
    std::vector<ComponentGroupData> components;
    components.reserve(componentGroups.size());
    for (auto& [hash, group] : componentGroups) {
        components.push_back(std::move(group));
    }

    // Send EntityCreated with component groups
    auto result = _session.sendEntityCreated(_entityId, _appId, _typeName, _parentId, components);
    if (result.failed()) {
        return result;
    }

    // Send pending property updates
    for (const auto& [hash, type, value] : _pendingUpdates) {
        auto updateResult = _session.sendPropertyUpdate(hash, type, value);
        if (updateResult.failed()) {
            ENTROPY_LOG_WARNING(std::format("Failed to send property update: {}", updateResult.errorMessage));
        }
    }

    return Result<void>::ok();
}

NetworkSession::EntityBuilder NetworkSession::createEntity(const std::string& typeName, const std::string& appId,
                                                           uint64_t parentId) {
    return EntityBuilder(*this, nextEntityId(), appId, typeName, parentId);
}

// ============================================================================
// ComponentHandle Implementation
// ============================================================================

NetworkSession::ComponentHandle::ComponentHandle(EntityBuilder& entity, const ComponentSchema& schema)
    : _entity(entity), _schema(schema) {}

template <typename T>
NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set(const std::string& propertyName, const T& value) {
    // Find property in schema
    for (const auto& propDef : _schema.properties) {
        if (propDef.name == propertyName) {
            // Compute property hash
            auto hash = computePropertyHash(_entity._entityId, _schema.typeHash, propertyName);

            // Create PropertyMetadata for registration
            PropertyMetadata meta;
            meta.hash = hash;
            meta.entityId = _entity._entityId;
            meta.componentType = _schema.typeHash;
            meta.propertyName = propertyName;
            meta.type = propDef.type;
            meta.registeredAt = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

            _entity._properties.push_back(meta);

            // Queue property update
            _entity._pendingUpdates.emplace_back(hash, propDef.type, PropertyValue(value));
            return *this;
        }
    }

    ENTROPY_LOG_WARNING(std::format("Property '{}' not found in schema '{}'", propertyName, _schema.componentName));
    return *this;
}

// Explicit template instantiations for common types
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<int32_t>(const std::string&,
                                                                                        const int32_t&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<int64_t>(const std::string&,
                                                                                        const int64_t&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<float>(const std::string&, const float&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<double>(const std::string&,
                                                                                       const double&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<Vec2>(const std::string&, const Vec2&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<Vec3>(const std::string&, const Vec3&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<Vec4>(const std::string&, const Vec4&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<Quat>(const std::string&, const Quat&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<std::string>(const std::string&,
                                                                                            const std::string&);
template NetworkSession::ComponentHandle& NetworkSession::ComponentHandle::set<bool>(const std::string&, const bool&);

}  // namespace EntropyEngine::Networking
