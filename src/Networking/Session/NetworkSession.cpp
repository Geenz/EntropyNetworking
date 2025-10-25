/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NetworkSession.h"
#include "../Core/TimeUtils.h"
#include "src/Networking/Protocol/entropy.capnp.h"
#include <capnp/message.h>
#include <capnp/serialize.h>

namespace EntropyEngine::Networking {

std::string NetworkSession::generateSessionId() {
    static std::atomic<uint64_t> ctr{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "session-" + std::to_string(now) + "-" + std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
}

NetworkSession::NetworkSession(NetworkConnection* connection, PropertyRegistry* externalRegistry)
    : _connection(connection)
    , _propertyRegistry(externalRegistry)
    , _sessionId(generateSessionId())
{
    // If no external registry provided, create and own one
    if (!_propertyRegistry) {
        _ownedRegistry = std::make_unique<PropertyRegistry>();
        _propertyRegistry = _ownedRegistry.get();
    }

    if (_connection) {
        _connection->retain();

        // Set up callbacks
        _connection->setMessageCallback([this](const std::vector<uint8_t>& data) {
            onMessageReceived(data);
        });

        _connection->setStateCallback([this](ConnectionState state) {
            onConnectionStateChanged(state);
        });
    }
}

NetworkSession::~NetworkSession() {
    if (_connection) {
        _connection->release();
    }
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

bool NetworkSession::isConnected() const {
    return _connection && _connection->isConnected();
}

ConnectionState NetworkSession::getState() const {
    return _state;
}

Result<void> NetworkSession::performHandshake(const std::string& clientType, const std::string& clientId) {
    _clientType = clientType;
    _clientId = clientId;

    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto msg = builder.initRoot<Message>();
        auto hs = msg.initHandshake();
        hs.setProtocolVersion(1);
        hs.setClientType(clientType);
        hs.setClientId(clientId);

        auto ser = serialize(builder);
        if (ser.failed()) {
            return Result<void>::err(ser.error, ser.errorMessage);
        }

        return _connection->send(ser.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendEntityCreated(
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    uint64_t parentId,
    const std::vector<PropertyMetadata>& properties)
{
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
        auto ec = message.initEntityCreated();
        ec.setEntityId(entityId);
        ec.setAppId(appId);
        ec.setTypeName(typeName);
        ec.setParentId(parentId);

        auto list = ec.initProperties(properties.size());
        for (size_t i = 0; i < properties.size(); ++i) {
            const auto& pm = properties[i];
            auto pr = list[i];
            auto ph = pr.initPropertyHash();
            ph.setHigh(pm.hash.high);
            ph.setLow(pm.hash.low);
            pr.setEntityId(pm.entityId);
            pr.setComponentType(pm.componentType);
            pr.setPropertyName(pm.propertyName);
            pr.setType(static_cast<::PropertyType>(pm.type));
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

Result<void> NetworkSession::sendEntityDestroyed(uint64_t entityId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_handshakeComplete) {
        return Result<void>::err(NetworkError::HandshakeFailed, "Handshake not complete");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
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

Result<void> NetworkSession::sendPropertyUpdate(
    PropertyHash hash,
    PropertyType type,
    const PropertyValue& value)
{
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

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
        auto batch = message.initPropertyUpdateBatch();

        batch.setTimestamp(getCurrentTimestampMicros());
        batch.setSequence(_nextSendSequence.fetch_add(1, std::memory_order_relaxed));

        auto updates = batch.initUpdates(1);
        auto update = updates[0];

        auto ph = update.initPropertyHash();
        ph.setHigh(hash.high);
        ph.setLow(hash.low);
        update.setExpectedType(static_cast<::PropertyType>(type));

        auto valueBuilder = update.initValue();
        std::visit([&valueBuilder](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t>) valueBuilder.setInt32(v);
            else if constexpr (std::is_same_v<T, int64_t>) valueBuilder.setInt64(v);
            else if constexpr (std::is_same_v<T, float>) valueBuilder.setFloat32(v);
            else if constexpr (std::is_same_v<T, double>) valueBuilder.setFloat64(v);
            else if constexpr (std::is_same_v<T, Vec2>) { auto b = valueBuilder.initVec2(); b.setX(v.x); b.setY(v.y); }
            else if constexpr (std::is_same_v<T, Vec3>) { auto b = valueBuilder.initVec3(); b.setX(v.x); b.setY(v.y); b.setZ(v.z); }
            else if constexpr (std::is_same_v<T, Vec4>) { auto b = valueBuilder.initVec4(); b.setX(v.x); b.setY(v.y); b.setZ(v.z); b.setW(v.w); }
            else if constexpr (std::is_same_v<T, Quat>) { auto b = valueBuilder.initQuat(); b.setX(v.x); b.setY(v.y); b.setZ(v.z); b.setW(v.w); }
            else if constexpr (std::is_same_v<T, std::string>) valueBuilder.setString(v);
            else if constexpr (std::is_same_v<T, bool>) valueBuilder.setBool(v);
            else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) valueBuilder.setBytes(kj::arrayPtr(v.data(), v.size()));
        }, value);

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

void NetworkSession::setEntityCreatedCallback(EntityCreatedCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _entityCreatedCallback = std::move(callback);
}

void NetworkSession::setEntityDestroyedCallback(EntityDestroyedCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _entityDestroyedCallback = std::move(callback);
}

void NetworkSession::setPropertyUpdateCallback(PropertyUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _propertyUpdateCallback = std::move(callback);
}

void NetworkSession::setSceneSnapshotCallback(SceneSnapshotCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _sceneSnapshotCallback = std::move(callback);
}

void NetworkSession::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _errorCallback = std::move(callback);
}

ConnectionStats NetworkSession::getStats() const {
    if (!_connection) {
        return ConnectionStats{};
    }
    return _connection->getStats();
}

void NetworkSession::onMessageReceived(const std::vector<uint8_t>& data) {
    handleReceivedMessage(data);
}

void NetworkSession::onConnectionStateChanged(ConnectionState state) {
    _state = state;
}

void NetworkSession::handleReceivedMessage(const std::vector<uint8_t>& data) {
    try {
        // Deserialize the message
        auto deserialized = deserialize(data);
        if (deserialized.failed()) {
            if (_errorCallback) {
                _errorCallback(deserialized.error, deserialized.errorMessage);
            }
            return;
        }

        // Read the message
        kj::ArrayPtr<const ::capnp::word> words(
            reinterpret_cast<const ::capnp::word*>(deserialized.value.begin()),
            deserialized.value.size()
        );

        ::capnp::FlatArrayMessageReader reader(words);
        auto message = reader.getRoot<Message>();

        // Dispatch based on message type
        switch (message.which()) {
            case Message::ENTITY_CREATED: {
                if (_entityCreatedCallback) {
                    auto entityCreated = message.getEntityCreated();
                    _entityCreatedCallback(
                        entityCreated.getEntityId(),
                        std::string(entityCreated.getAppId().cStr()),
                        std::string(entityCreated.getTypeName().cStr()),
                        entityCreated.getParentId()
                    );
                }
                break;
            }

            case Message::ENTITY_DESTROYED: {
                if (_entityDestroyedCallback) {
                    auto entityDestroyed = message.getEntityDestroyed();
                    _entityDestroyedCallback(entityDestroyed.getEntityId());
                }
                break;
            }

            case Message::PROPERTY_UPDATE_BATCH: {
                auto batch = message.getPropertyUpdateBatch();
                uint32_t seq = batch.getSequence();
                uint32_t last = _lastReceivedSequence.load(std::memory_order_relaxed);

                if (seq <= last) {
                    // Duplicate or old packet received - track for diagnostics
                    _duplicatePacketsReceived.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (seq > last + 1) {
                    // Gap detected; packet loss event - track for diagnostics
                    _packetLossEvents.fetch_add(1, std::memory_order_relaxed);
                }
                _lastReceivedSequence.store(seq, std::memory_order_relaxed);

                if (_propertyUpdateCallback) {
                    _propertyUpdateCallback(data);
                }
                break;
            }

            case Message::SCENE_SNAPSHOT_CHUNK: {
                if (_sceneSnapshotCallback) {
                    _sceneSnapshotCallback(data);
                }
                break;
            }

            case Message::HANDSHAKE_RESPONSE: {
                auto resp = message.getHandshakeResponse();
                if (resp.getSuccess()) {
                    _handshakeComplete = true;
                } else {
                    if (_errorCallback) {
                        _errorCallback(NetworkError::HandshakeFailed,
                                     std::string(resp.getErrorMessage().cStr()));
                    }
                    disconnect();
                }
                break;
            }

            default:
                // Unknown or unhandled message type
                if (_errorCallback) {
                    _errorCallback(NetworkError::InvalidMessage, "Unknown message type");
                }
                break;
        }

    } catch (const std::exception& e) {
        if (_errorCallback) {
            _errorCallback(NetworkError::DeserializationFailed, e.what());
        }
    }
}

} // namespace EntropyEngine::Networking
