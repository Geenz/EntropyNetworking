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

#if defined(__APPLE__)
#include "XPCConnection.h"
#include <TargetConditionals.h>
#endif

#include <format>
#include <stdexcept>

namespace EntropyEngine::Networking {

ConnectionManager::ConnectionManager(size_t capacity)
    : _capacity(capacity)
    , _connectionSlots(capacity)
{
    // Initialize lock-free free list
    if (_capacity == 0) {
        // Set free list head to INVALID_INDEX to indicate no available slots
        uint64_t headVal = (static_cast<uint64_t>(0) << 32) | static_cast<uint64_t>(INVALID_INDEX);
        _freeListHead.store(headVal, std::memory_order_relaxed);
        return;
    }
    for (size_t i = 0; i < _capacity - 1; ++i) {
        _connectionSlots[i].nextFree.store(static_cast<uint32_t>(i + 1), std::memory_order_relaxed);
    }
    _connectionSlots[_capacity - 1].nextFree.store(INVALID_INDEX, std::memory_order_relaxed);
    _freeListHead.store(0, std::memory_order_relaxed);
}

ConnectionManager::~ConnectionManager() {
    // Disconnect all connections
    for (auto& slot : _connectionSlots) {
        if (slot.connection) {
            slot.connection->disconnect();
        }
    }
}

uint32_t ConnectionManager::allocateSlot() {
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
        uint32_t next = _connectionSlots[idx].nextFree.load(std::memory_order_acquire);
        uint64_t newHead = packHead(next, headTag(head) + 1);
        if (_freeListHead.compare_exchange_weak(head, newHead,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            _activeCount.fetch_add(1, std::memory_order_acq_rel);
            return idx;
        }
    }
}

void ConnectionManager::returnSlotToFreeList(uint32_t index) {
    auto& slot = _connectionSlots[index];

    // Increment generation
    slot.generation.fetch_add(1, std::memory_order_acq_rel);

    // Clear connection
    slot.connection.reset();
    // Reset published state for next allocation
    slot.lastPublishedState.store(ConnectionState::Disconnected, std::memory_order_release);

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

void ConnectionManager::handleStatePublish(uint32_t index, ConnectionState newState) noexcept {
    auto& slot = _connectionSlots[index];
    // Publish latest state for queries
    slot.state.store(newState, std::memory_order_release);

    // Deduplicate metrics on actual transitions only
    for (;;) {
        ConnectionState prev = slot.lastPublishedState.load(std::memory_order_acquire);
        if (prev == newState) {
            return; // no change
        }
        if (slot.lastPublishedState.compare_exchange_weak(
                prev, newState,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            switch (newState) {
                case ConnectionState::Connected:
                    _metrics.connectionsOpened.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ConnectionState::Failed:
                    _metrics.connectionsFailed.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ConnectionState::Disconnected:
                    _metrics.connectionsClosed.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    break;
            }
            return;
        }
    }
}

std::unique_ptr<NetworkConnection> ConnectionManager::createLocalBackend(const ConnectionConfig& config) {
    if (config.backend == ConnectionBackend::Auto) {
        // Automatic platform detection
#if defined(__APPLE__)
        #if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION
            // iOS family - use XPC (Unix sockets unavailable due to sandboxing)
            return std::make_unique<XPCConnection>(config.endpoint, &config);
        #else
            // macOS - prefer Unix sockets for simplicity (XPC available via explicit backend)
            return std::make_unique<UnixSocketConnection>(config.endpoint, &config);
        #endif
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
        // Linux/Android - use Unix sockets
        return std::make_unique<UnixSocketConnection>(config.endpoint, &config);
#elif defined(_WIN32)
        throw std::runtime_error("Named pipe backend not yet implemented");
#else
        throw std::runtime_error("No local backend available for this platform");
#endif
    }

    switch (config.backend) {
        case ConnectionBackend::UnixSocket:
#if defined(__unix__) || defined(__APPLE__)
            return std::make_unique<UnixSocketConnection>(config.endpoint, &config);
#else
            throw std::runtime_error("Unix sockets not supported on this platform");
#endif

        case ConnectionBackend::NamedPipe:
            throw std::runtime_error("Named pipe backend not yet implemented");

        case ConnectionBackend::XPC:
#if defined(__APPLE__)
            return std::make_unique<XPCConnection>(config.endpoint, &config);
#else
            throw std::runtime_error("XPC not supported on this platform");
#endif

        default:
            throw std::runtime_error("Invalid backend for local connection");
    }
}

std::unique_ptr<NetworkConnection> ConnectionManager::createRemoteBackend(const ConnectionConfig& config) {
    return std::make_unique<WebRTCConnection>(
        config.webrtcConfig,
        config.signalingCallbacks,
        config.dataChannelLabel
    );
}

ConnectionHandle ConnectionManager::openLocalConnection(const std::string& endpoint) {
    ConnectionConfig config;
    config.type = ConnectionType::Local;
    config.endpoint = endpoint;
    return openConnection(config);
}

ConnectionHandle ConnectionManager::openRemoteConnection(
    const std::string& signalingServer,
    WebRTCConfig config,
    SignalingCallbacks callbacks
) {
    ConnectionConfig connConfig;
    connConfig.type = ConnectionType::Remote;
    connConfig.endpoint = signalingServer;
    connConfig.webrtcConfig = std::move(config);
    connConfig.signalingCallbacks = std::move(callbacks);
    return openConnection(connConfig);
}

ConnectionHandle ConnectionManager::openConnection(ConnectionConfig config) {
    uint32_t index = allocateSlot();
    if (index == INVALID_INDEX) {
        return ConnectionHandle();  // Full - return invalid handle
    }

    auto& slot = _connectionSlots[index];
    uint32_t generation = slot.generation.load(std::memory_order_acquire);

    try {
        // Create backend based on type
        if (config.type == ConnectionType::Local) {
            slot.connection = createLocalBackend(config);
        } else {
            slot.connection = createRemoteBackend(config);
        }

        slot.type = config.type;
        slot.state.store(ConnectionState::Disconnected, std::memory_order_release);

        return ConnectionHandle(this, index, generation);
    } catch (const std::exception& e) {
        // Failed to create backend - return slot to free list
        returnSlotToFreeList(index);
        return ConnectionHandle();  // Return invalid handle
    }
}

ConnectionHandle ConnectionManager::adoptConnection(std::unique_ptr<NetworkConnection> backend, ConnectionType type) {
    if (!backend) {
        return ConnectionHandle();  // Invalid backend
    }

    uint32_t index = allocateSlot();
    if (index == INVALID_INDEX) {
        return ConnectionHandle();  // Full - return invalid handle
    }

    auto& slot = _connectionSlots[index];
    uint32_t generation = slot.generation.load(std::memory_order_acquire);

    // Install backend
    slot.connection = std::move(backend);
    slot.type = type;

    // Wire manager-owned callbacks BEFORE taking the initial state snapshot
    slot.connection->setStateCallback([this, index](ConnectionState newState) noexcept {
        // Publish state and update metrics on real transitions only
        this->handleStatePublish(index, newState);
        // Fan out to user callback using atomic_load on shared_ptr (no slot mutex)
        auto cbptr = std::atomic_load(&_connectionSlots[index].userStateCb);
        if (cbptr && *cbptr) {
            (*cbptr)(newState);
        }
    });

    // Manager-owned message callback: increment metrics and fan out to user callback
    slot.connection->setMessageCallback([this, index](const std::vector<uint8_t>& data) noexcept {
        _metrics.totalBytesReceived.fetch_add(data.size(), std::memory_order_relaxed);
        _metrics.totalMessagesReceived.fetch_add(1, std::memory_order_relaxed);
        auto cbptr = std::atomic_load(&_connectionSlots[index].userMessageCb);
        if (cbptr && *cbptr) {
            (*cbptr)(data);
        }
    });

    // Take and publish current state snapshot after wiring callbacks to avoid races
    {
        ConnectionState st = slot.connection->getState();
        handleStatePublish(index, st);
        auto cbptr2 = std::atomic_load(&_connectionSlots[index].userStateCb);
        if (cbptr2 && *cbptr2) {
            (*cbptr2)(st);
        }
    }

    return ConnectionHandle(this, index, generation);
}

bool ConnectionManager::validateHandle(const ConnectionHandle& handle) const noexcept {
    if (handle.handleOwner() != static_cast<const void*>(this)) return false;

    uint32_t index = handle.handleIndex();
    if (index >= _capacity) return false;

    uint32_t currentGen = _connectionSlots[index].generation.load(std::memory_order_acquire);
    return currentGen == handle.handleGeneration();
}

bool ConnectionManager::isValidHandle(const ConnectionHandle& handle) const noexcept {
    return validateHandle(handle);
}

Result<void> ConnectionManager::connect(const ConnectionHandle& handle) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    // Check connection exists (with lock)
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.connection) {
            return Result<void>::err(NetworkError::InvalidParameter, "Connection not initialized");
        }
    }
    // Lock released - now safe to set up callbacks that may fire synchronously

    // Wire up manager-owned callbacks BEFORE connecting to ensure we capture all state transitions
    // This is critical for synchronous connections (Unix sockets) where connect() completes immediately
    slot.connection->setStateCallback([this, index](ConnectionState newState) noexcept {
        // Publish state and update metrics on real transitions only
        this->handleStatePublish(index, newState);
        // Fan out to user callback using atomic_load on shared_ptr (no slot mutex)
        auto cbptr = std::atomic_load(&_connectionSlots[index].userStateCb);
        if (cbptr && *cbptr) {
            (*cbptr)(newState);
        }
    });

    slot.connection->setMessageCallback([this, index](const std::vector<uint8_t>& data) noexcept {
        _metrics.totalBytesReceived.fetch_add(data.size(), std::memory_order_relaxed);
        _metrics.totalMessagesReceived.fetch_add(1, std::memory_order_relaxed);
        auto cbptr = std::atomic_load(&_connectionSlots[index].userMessageCb);
        if (cbptr && *cbptr) {
            (*cbptr)(data);
        }
    });

    // Now that callbacks are registered, initiate the connection
    // State transitions will be captured by the callbacks above
    auto result = slot.connection->connect();

    // Sync initial state in case the callback hasn't fired yet or for immediate transitions
    // For WebRTC this will be Connecting; for Unix socket it may be Connected
    auto initialState = slot.connection->getState();
    handleStatePublish(index, initialState);

    // Fan out current state to user callback to avoid races with immediate transitions
    {
        auto cbptr = std::atomic_load(&_connectionSlots[index].userStateCb);
        if (cbptr && *cbptr) {
            (*cbptr)(initialState);
        }
    }

    return result;
}

Result<void> ConnectionManager::disconnect(const ConnectionHandle& handle) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.connection) {
        return Result<void>::ok();  // Already disconnected
    }

    auto result = slot.connection->disconnect();
    slot.state.store(ConnectionState::Disconnected, std::memory_order_release);

    return result;
}

Result<void> ConnectionManager::closeConnection(const ConnectionHandle& handle) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    {
        std::lock_guard<std::mutex> lock(slot.mutex);

        // Disconnect if still connected
        if (slot.connection) {
            slot.connection->disconnect();
            slot.state.store(ConnectionState::Disconnected, std::memory_order_release);
        }
    }

    // Return slot to free list (outside the lock to avoid deadlock)
    returnSlotToFreeList(index);

    return Result<void>::ok();
}

Result<void> ConnectionManager::send(const ConnectionHandle& handle, const std::vector<uint8_t>& data) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.connection) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not initialized");
    }

    auto r = slot.connection->send(data);
    if (r.success()) {
        _metrics.totalBytesSent.fetch_add(data.size(), std::memory_order_relaxed);
        _metrics.totalMessagesSent.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

Result<void> ConnectionManager::sendUnreliable(const ConnectionHandle& handle, const std::vector<uint8_t>& data) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.connection) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not initialized");
    }

    auto r = slot.connection->sendUnreliable(data);
    if (r.success()) {
        _metrics.totalBytesSent.fetch_add(data.size(), std::memory_order_relaxed);
        _metrics.totalMessagesSent.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

bool ConnectionManager::isConnected(const ConnectionHandle& handle) const {
    if (!validateHandle(handle)) return false;

    uint32_t index = handle.handleIndex();
    return _connectionSlots[index].state.load(std::memory_order_acquire) == ConnectionState::Connected;
}

ConnectionState ConnectionManager::getState(const ConnectionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionState::Disconnected;

    uint32_t index = handle.handleIndex();
    return _connectionSlots[index].state.load(std::memory_order_acquire);
}

ConnectionStats ConnectionManager::getStats(const ConnectionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionStats{};

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    if (!slot.connection) return ConnectionStats{};
    return slot.connection->getStats();
}

ConnectionType ConnectionManager::getConnectionType(const ConnectionHandle& handle) const {
    if (!validateHandle(handle)) return ConnectionType::Local;

    uint32_t index = handle.handleIndex();
    return _connectionSlots[index].type;
}

NetworkConnection* ConnectionManager::getConnectionPointer(const ConnectionHandle& handle) {
    if (!validateHandle(handle)) return nullptr;

    uint32_t index = handle.handleIndex();
    return _connectionSlots[index].connection.get();
}

void ConnectionManager::setMessageCallback(const ConnectionHandle& handle, std::function<void(const std::vector<uint8_t>&)> callback) noexcept {
    if (!validateHandle(handle)) return;

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    // Store user callback; backend remains bound to manager-owned fan-out
    auto sp = std::make_shared<std::function<void(const std::vector<uint8_t>&)>>(std::move(callback));
    std::atomic_store(&slot.userMessageCb, std::move(sp));
}

void ConnectionManager::setStateCallback(const ConnectionHandle& handle, std::function<void(ConnectionState)> callback) noexcept {
    if (!validateHandle(handle)) return;

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    // Store user callback; backend remains bound to manager-owned fan-out
    auto sp = std::make_shared<std::function<void(ConnectionState)>>(std::move(callback));
    std::atomic_store(&slot.userStateCb, std::move(sp));
}

uint64_t ConnectionManager::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<ConnectionManager>().id
    );
    return hash;
}

std::string ConnectionManager::toString() const {
    return std::format("{}@{}(cap={}, active={})",
                       className(),
                       static_cast<const void*>(this),
                       _capacity,
                       _activeCount.load(std::memory_order_relaxed));
}

ConnectionManager::ManagerMetrics ConnectionManager::getManagerMetrics() const noexcept {
    ManagerMetrics m;
    m.totalBytesSent = _metrics.totalBytesSent.load(std::memory_order_relaxed);
    m.totalBytesReceived = _metrics.totalBytesReceived.load(std::memory_order_relaxed);
    m.totalMessagesSent = _metrics.totalMessagesSent.load(std::memory_order_relaxed);
    m.totalMessagesReceived = _metrics.totalMessagesReceived.load(std::memory_order_relaxed);
    m.connectionsOpened = _metrics.connectionsOpened.load(std::memory_order_relaxed);
    m.connectionsFailed = _metrics.connectionsFailed.load(std::memory_order_relaxed);
    m.connectionsClosed = _metrics.connectionsClosed.load(std::memory_order_relaxed);
    m.wouldBlockSends = _metrics.wouldBlockSends.load(std::memory_order_relaxed);
    return m;
}

Result<void> ConnectionManager::trySend(const ConnectionHandle& handle, const std::vector<uint8_t>& data) {
    if (!validateHandle(handle)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid connection handle");
    }

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.connection) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not initialized");
    }

    auto r = slot.connection->trySend(data);
    if (r.success()) {
        _metrics.totalBytesSent.fetch_add(data.size(), std::memory_order_relaxed);
        _metrics.totalMessagesSent.fetch_add(1, std::memory_order_relaxed);
    } else if (r.error == NetworkError::WouldBlock) {
        _metrics.wouldBlockSends.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

} // namespace EntropyEngine::Networking

