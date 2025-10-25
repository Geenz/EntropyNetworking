/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "SessionManager.h"
#include <format>

namespace EntropyEngine::Networking {

SessionManager::SessionManager(ConnectionManager* connectionManager, size_t capacity)
    : _connectionManager(connectionManager)
    , _capacity(capacity)
    , _sessionSlots(capacity)
{
    // Initialize lock-free free list
    for (size_t i = 0; i < _capacity - 1; ++i) {
        _sessionSlots[i].nextFree.store(static_cast<uint32_t>(i + 1), std::memory_order_relaxed);
    }
    _sessionSlots[_capacity - 1].nextFree.store(INVALID_INDEX, std::memory_order_relaxed);
    _freeListHead.store(0, std::memory_order_relaxed);
}

SessionManager::~SessionManager() {
    // Clean up all sessions
    for (auto& slot : _sessionSlots) {
        slot.session.reset();
    }
}

uint32_t SessionManager::allocateSlot() {
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t head = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        uint32_t idx = headIndex(head);
        if (idx == INVALID_INDEX) {
            return INVALID_INDEX;  // No free slots
        }
        uint32_t next = _sessionSlots[idx].nextFree.load(std::memory_order_acquire);
        uint64_t newHead = packHead(next, headTag(head) + 1);
        if (_freeListHead.compare_exchange_weak(head, newHead,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            _activeCount.fetch_add(1, std::memory_order_acq_rel);
            return idx;
        }
    }
}

void SessionManager::returnSlotToFreeList(uint32_t index) {
    auto& slot = _sessionSlots[index];

    // Increment generation
    slot.generation.fetch_add(1, std::memory_order_acq_rel);

    // Clear session
    slot.session.reset();

    // Decrement active count
    _activeCount.fetch_sub(1, std::memory_order_acq_rel);

    // Push back to free list
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t old = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        uint32_t oldIdx = headIndex(old);
        slot.nextFree.store(oldIdx, std::memory_order_release);
        uint64_t newH = packHead(index, headTag(old) + 1);
        if (_freeListHead.compare_exchange_weak(old, newH,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            break;
        }
    }
}

SessionHandle SessionManager::createSession(ConnectionHandle connection, PropertyRegistry* externalRegistry) {
    if (!connection.valid()) {
        return SessionHandle();  // Invalid connection
    }

    uint32_t index = allocateSlot();
    if (index == INVALID_INDEX) {
        return SessionHandle();  // Full
    }

    auto& slot = _sessionSlots[index];
    uint32_t generation = slot.generation.load(std::memory_order_acquire);

    try {
        // Store connection handle
        slot.connection = connection;

        // Get underlying connection pointer
        NetworkConnection* connPtr = _connectionManager->getConnectionPointer(connection);
        if (!connPtr) {
            returnSlotToFreeList(index);
            return SessionHandle();
        }

        // Create NetworkSession wrapping the connection and optional external registry
        slot.session = std::make_unique<NetworkSession>(connPtr, externalRegistry);

        return SessionHandle(this, index, generation);
    } catch (const std::exception& e) {
        // Failed to create session - return slot to free list
        returnSlotToFreeList(index);
        return SessionHandle();
    }
}

bool SessionManager::validateHandle(const SessionHandle& handle) const noexcept {
    if (handle.handleOwner() != static_cast<const void*>(this)) return false;

    uint32_t index = handle.handleIndex();
    if (index >= _capacity) return false;

    uint32_t currentGen = _sessionSlots[index].generation.load(std::memory_order_acquire);
    return currentGen == handle.handleGeneration();
}

bool SessionManager::isValidHandle(const SessionHandle& handle) const noexcept {
    return validateHandle(handle);
}

Result<void> SessionManager::setEntityCreatedCallback(const SessionHandle& handle, EntityCreatedCallback callback) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    slot.session->setEntityCreatedCallback(std::move(callback));
    return Result<void>::ok();
}

Result<void> SessionManager::setEntityDestroyedCallback(const SessionHandle& handle, EntityDestroyedCallback callback) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    slot.session->setEntityDestroyedCallback(std::move(callback));
    return Result<void>::ok();
}

Result<void> SessionManager::setPropertyUpdateCallback(const SessionHandle& handle, PropertyUpdateCallback callback) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    slot.session->setPropertyUpdateCallback(std::move(callback));
    return Result<void>::ok();
}

Result<void> SessionManager::setSceneSnapshotCallback(const SessionHandle& handle, SceneSnapshotCallback callback) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    slot.session->setSceneSnapshotCallback(std::move(callback));
    return Result<void>::ok();
}

Result<void> SessionManager::setErrorCallback(const SessionHandle& handle, ErrorCallback callback) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    slot.session->setErrorCallback(std::move(callback));
    return Result<void>::ok();
}

Result<void> SessionManager::sendEntityCreated(
    const SessionHandle& handle,
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    uint64_t parentId
) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    return slot.session->sendEntityCreated(entityId, appId, typeName, parentId);
}

Result<void> SessionManager::sendEntityDestroyed(const SessionHandle& handle, uint64_t entityId) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    return slot.session->sendEntityDestroyed(entityId);
}

Result<void> SessionManager::sendPropertyUpdate(
    const SessionHandle& handle,
    PropertyHash hash,
    PropertyType type,
    const PropertyValue& value
) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    return slot.session->sendPropertyUpdate(hash, type, value);
}

Result<void> SessionManager::sendPropertyUpdateBatch(
    const SessionHandle& handle,
    const std::vector<uint8_t>& batchData
) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    return slot.session->sendPropertyUpdateBatch(batchData);
}

Result<void> SessionManager::sendSceneSnapshot(
    const SessionHandle& handle,
    const std::vector<uint8_t>& snapshotData
) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.session) {
        return Result<void>::err(NetworkError::InvalidParameter, "Session not initialized");
    }

    return slot.session->sendSceneSnapshot(snapshotData);
}

bool SessionManager::isConnected(const SessionHandle& handle) const {
    if (!validateHandle(handle)) return false;

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    if (!slot.session) return false;
    return slot.session->isConnected();
}

ConnectionState SessionManager::getConnectionState(const SessionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionState::Disconnected;

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    if (!slot.session) return ConnectionState::Disconnected;
    return slot.session->getState();
}

ConnectionStats SessionManager::getConnectionStats(const SessionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionStats{};

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    if (!slot.session) return ConnectionStats{};
    return slot.session->getStats();
}

ConnectionHandle SessionManager::getConnection(const SessionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionHandle();

    uint32_t index = handle.handleIndex();
    return _sessionSlots[index].connection;
}

PropertyRegistry& SessionManager::getPropertyRegistry(const SessionHandle& handle) {
    if (!validateHandle(handle)) {
        static PropertyRegistry dummy;
        return dummy;
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    if (!slot.session) {
        static PropertyRegistry dummy;
        return dummy;
    }

    return slot.session->getPropertyRegistry();
}

const PropertyRegistry& SessionManager::getPropertyRegistry(const SessionHandle& handle) const {
    if (!validateHandle(handle)) {
        static PropertyRegistry dummy;
        return dummy;
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _sessionSlots[index];

    if (!slot.session) {
        static PropertyRegistry dummy;
        return dummy;
    }

    return slot.session->getPropertyRegistry();
}

uint64_t SessionManager::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<SessionManager>().id
    );
    return hash;
}

std::string SessionManager::toString() const {
    return std::format("{}@{}(cap={}, active={})",
                       className(),
                       static_cast<const void*>(this),
                       _capacity,
                       _activeCount.load(std::memory_order_relaxed));
}

} // namespace EntropyEngine::Networking
