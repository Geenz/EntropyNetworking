/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NetworkSession.h"
#include "src/Networking/Protocol/entropy.capnp.h"
#include <capnp/message.h>
#include <capnp/serialize.h>

namespace EntropyEngine::Networking {

NetworkSession::NetworkSession(NetworkConnection* connection)
    : _connection(connection)
{
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

Result<void> NetworkSession::sendEntityCreated(
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    uint64_t parentId)
{
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
        auto entityCreated = message.initEntityCreated();

        entityCreated.setEntityId(entityId);
        entityCreated.setAppId(appId);
        entityCreated.setTypeName(typeName);
        entityCreated.setParentId(parentId);
        // Properties will be sent separately via property updates

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // EntityCreated goes on reliable channel
        return _connection->send(serialized.value);

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::SerializationFailed, e.what());
    }
}

Result<void> NetworkSession::sendEntityDestroyed(uint64_t entityId) {
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
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
    uint64_t entityId,
    const std::string& propertyName,
    const PropertyValue& value)
{
    // For now, this is a simple single-property update
    // In Phase 4, this will be batched by BatchManager
    if (!_connection || !_connection->isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    try {
        capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
        auto batch = message.initPropertyUpdateBatch();

        // Single update batch
        batch.setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
        batch.setSequence(0); // TODO: sequence tracking

        auto updates = batch.initUpdates(1);
        auto update = updates[0];

        // Compute property hash
        // TODO: Get appId and typeName from entity registry
        auto hash = computePropertyHash(entityId, "unknown", "unknown", propertyName);
        update.getPropertyHash().setHigh(hash.high);
        update.getPropertyHash().setLow(hash.low);

        // Set type and value
        PropertyType type = getPropertyType(value);
        update.setExpectedType(static_cast<::PropertyType>(type));

        // TODO: Set property value from PropertyValue variant

        auto serialized = serialize(builder);
        if (serialized.failed()) {
            return Result<void>::err(serialized.error, serialized.errorMessage);
        }

        // PropertyUpdate goes on unreliable channel
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
