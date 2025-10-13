// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ConnectionManager.h"
#include "UnixSocketConnection.h"
#include "WebRTCConnection.h"
#include <format>
#include <stdexcept>

namespace EntropyEngine::Networking {

ConnectionManager::ConnectionManager(size_t capacity)
    : _capacity(capacity)
    , _connectionSlots(capacity)
{
    // Initialize lock-free free list
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

std::unique_ptr<NetworkConnection> ConnectionManager::createLocalBackend(const ConnectionConfig& config) {
    if (config.backend == ConnectionBackend::Auto) {
#if defined(__unix__) || defined(__APPLE__)
        return std::make_unique<UnixSocketConnection>(config.endpoint);
#elif defined(_WIN32)
        throw std::runtime_error("Named pipe backend not yet implemented");
#else
        throw std::runtime_error("No local backend available for this platform");
#endif
    }

    switch (config.backend) {
        case ConnectionBackend::UnixSocket:
#if defined(__unix__) || defined(__APPLE__)
            return std::make_unique<UnixSocketConnection>(config.endpoint);
#else
            throw std::runtime_error("Unix sockets not supported on this platform");
#endif

        case ConnectionBackend::NamedPipe:
            throw std::runtime_error("Named pipe backend not yet implemented");

        case ConnectionBackend::XPC:
            throw std::runtime_error("XPC backend not yet implemented");

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

    // Initialize state snapshot and callback wiring
    slot.state.store(slot.connection->getState(), std::memory_order_release);
    slot.connection->setStateCallback([this, index](ConnectionState newState) {
        _connectionSlots[index].state.store(newState, std::memory_order_release);
    });

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

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (!slot.connection) {
        return Result<void>::err(NetworkError::InvalidParameter, "Connection not initialized");
    }

    auto result = slot.connection->connect();

    // Update state to match backend's actual state
    // For WebRTC this will be Connecting; for Unix socket it may be Connected or Connecting
    slot.state.store(slot.connection->getState(), std::memory_order_release);

    // Wire up state callback to keep manager's state synchronized
    slot.connection->setStateCallback([this, index](ConnectionState newState) {
        // Update slot state atomically
        _connectionSlots[index].state.store(newState, std::memory_order_release);
    });

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

    return slot.connection->send(data);
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

    return slot.connection->sendUnreliable(data);
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

void ConnectionManager::setMessageCallback(const ConnectionHandle& handle, std::function<void(const std::vector<uint8_t>&)> callback) {
    if (!validateHandle(handle)) return;

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (slot.connection) {
        slot.connection->setMessageCallback(std::move(callback));
    }
}

void ConnectionManager::setStateCallback(const ConnectionHandle& handle, std::function<void(ConnectionState)> callback) {
    if (!validateHandle(handle)) return;

    uint32_t index = handle.handleIndex();
    auto& slot = _connectionSlots[index];

    std::lock_guard<std::mutex> lock(slot.mutex);

    if (slot.connection) {
        slot.connection->setStateCallback(std::move(callback));
    }
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

} // namespace EntropyEngine::Networking
