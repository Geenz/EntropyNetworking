// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <EntropyCore.h>
#include "ConnectionHandle.h"
#include "NetworkConnection.h"
#include "../Core/ConnectionTypes.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>

namespace EntropyEngine::Networking {

/**
 * @brief Slot-based connection manager with platform-agnostic API
 *
 * ConnectionManager follows the WorkContractGroup pattern - it owns connection
 * slots and returns generation-stamped handles. Provides platform abstraction
 * for write-once-deploy-everywhere:
 *
 * - Local connections: Auto-selects Unix socket (Linux/macOS) or Named pipe (Windows)
 * - Remote connections: Uses WebRTC on all platforms
 *
 * Handle lifecycle:
 * 1. Create connection via openLocalConnection() or openRemoteConnection()
 * 2. Returns ConnectionHandle stamped with (manager + index + generation)
 * 3. Use handle for all operations (connect, send, etc.)
 * 4. Handle becomes invalid after disconnect or release
 *
 * Thread-safe: All operations are lock-free or use minimal locking.
 *
 * @code
 * ConnectionManager connMgr(1024);  // capacity: 1024 connections
 *
 * // Platform-agnostic local connection
 * auto local = connMgr.openLocalConnection("/tmp/entropy.sock");
 * local.connect().wait();
 *
 * // Cross-platform remote connection
 * auto remote = connMgr.openRemoteConnection(server, webrtcConfig, callbacks);
 * remote.connect().wait();
 * @endcode
 */
class ConnectionManager : public Core::EntropyObject {
public:
    /**
     * @brief Constructs connection manager with specified capacity
     *
     * Pre-allocates all slots for lock-free operation. Choose capacity
     * based on maximum concurrent connections.
     * @param capacity Maximum number of connections (typically 1024-4096)
     */
    explicit ConnectionManager(size_t capacity);
    ~ConnectionManager();

    // Delete copy operations
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // High-level platform-agnostic API

    /**
     * @brief Opens a local connection with platform-appropriate backend
     *
     * Platform auto-selection:
     * - Linux/macOS: Unix domain socket
     * - Windows: Named pipe
     * - macOS (future): Can use XPC with config
     *
     * @param endpoint Path to socket/pipe
     * @return ConnectionHandle for operations, or invalid if full
     */
    ConnectionHandle openLocalConnection(const std::string& endpoint);

    /**
     * @brief Opens a remote connection using WebRTC
     *
     * Cross-platform WebRTC data channel connection.
     * @param signalingServer WebSocket URL for signaling
     * @param config WebRTC configuration (ICE servers, etc.)
     * @param callbacks Signaling callbacks for SDP/ICE exchange
     * @return ConnectionHandle for operations, or invalid if full
     */
    ConnectionHandle openRemoteConnection(
        const std::string& signalingServer,
        WebRTCConfig config,
        SignalingCallbacks callbacks
    );

    /**
     * @brief Opens connection with explicit configuration
     *
     * Advanced API for full control over backend selection.
     * @param config Connection configuration
     * @return ConnectionHandle for operations, or invalid if full
     */
    ConnectionHandle openConnection(ConnectionConfig config);

    /**
     * @brief Adopts a pre-constructed NetworkConnection backend
     *
     * Used by LocalServer to wrap accepted connections. Allocates a slot,
     * installs the backend, and wires up state synchronization.
     * @param backend Already-constructed connection (e.g., from accept())
     * @param type Connection type (Local or Remote)
     * @return ConnectionHandle for operations, or invalid if full
     */
    ConnectionHandle adoptConnection(std::unique_ptr<NetworkConnection> backend, ConnectionType type);

    // Internal operations called by ConnectionHandle

    /**
     * @brief Initiates connection (called by handle.connect())
     * @param handle Connection handle
     * @return Result indicating success or failure
     */
    Result<void> connect(const ConnectionHandle& handle);

    /**
     * @brief Disconnects connection (called by handle.disconnect())
     * @param handle Connection handle
     * @return Result indicating success or failure
     */
    Result<void> disconnect(const ConnectionHandle& handle);

    /**
     * @brief Closes connection and frees slot (called by handle.close())
     *
     * Disconnects the connection (if connected) and returns the slot to the free list.
     * After calling this, the handle becomes invalid and the slot can be reused.
     * @param handle Connection handle
     * @return Result indicating success or failure
     */
    Result<void> closeConnection(const ConnectionHandle& handle);

    /**
     * @brief Sends data over reliable channel (called by handle.send())
     * @param handle Connection handle
     * @param data Bytes to send
     * @return Result indicating success or failure
     */
    Result<void> send(const ConnectionHandle& handle, const std::vector<uint8_t>& data);

    /**
     * @brief Non-blocking send that returns WouldBlock on backpressure
     */
    Result<void> trySend(const ConnectionHandle& handle, const std::vector<uint8_t>& data);

    /**
     * @brief Sends data over unreliable channel (called by handle.sendUnreliable())
     * @param handle Connection handle
     * @param data Bytes to send
     * @return Result indicating success or failure
     */
    Result<void> sendUnreliable(const ConnectionHandle& handle, const std::vector<uint8_t>& data);

    /**
     * @brief Checks if connected (called by handle.isConnected())
     * @param handle Connection handle
     * @return true if connection is established
     */
    bool isConnected(const ConnectionHandle& handle) const;

    /**
     * @brief Gets connection state (called by handle.getState())
     * @param handle Connection handle
     * @return Current connection state
     */
    ConnectionState getState(const ConnectionHandle& handle) const;

    /**
     * @brief Gets connection statistics (called by handle.getStats())
     * @param handle Connection handle
     * @return Statistics structure
     */
    ConnectionStats getStats(const ConnectionHandle& handle) const;

    /**
     * @brief Gets connection type (called by handle.getType())
     * @param handle Connection handle
     * @return ConnectionType (Local or Remote)
     */
    ConnectionType getConnectionType(const ConnectionHandle& handle) const;

    /**
     * @brief Validates handle (called by handle.valid())
     * @param handle Connection handle
     * @return true if handle is valid and refers to allocated connection
     */
    bool isValidHandle(const ConnectionHandle& handle) const noexcept;

    /**
     * @brief Sets message callback (called by handle.setMessageCallback())
     * @param handle Connection handle
     * @param callback Function called when messages are received
     */
    void setMessageCallback(const ConnectionHandle& handle, std::function<void(const std::vector<uint8_t>&)> callback);

    /**
     * @brief Sets state callback (called by handle.setStateCallback())
     * @param handle Connection handle
     * @param callback Function called when connection state changes
     */
    void setStateCallback(const ConnectionHandle& handle, std::function<void(ConnectionState)> callback);

    /**
     * @brief Gets active connection count
     * @return Number of currently allocated connections
     */
    size_t activeCount() const noexcept {
        return _activeCount.load(std::memory_order_acquire);
    }

    /**
     * @brief Gets maximum capacity
     * @return Maximum number of connections this manager can handle
     */
    size_t capacity() const noexcept { return _capacity; }

    /**
     * @brief Lightweight aggregate metrics snapshot for observability
     */
    struct ManagerMetrics {
        uint64_t totalBytesSent = 0;
        uint64_t totalBytesReceived = 0;
        uint64_t totalMessagesSent = 0;
        uint64_t totalMessagesReceived = 0;
        uint64_t connectionsOpened = 0;
        uint64_t connectionsFailed = 0;
        uint64_t connectionsClosed = 0;
        uint64_t wouldBlockSends = 0;
    };

    /**
     * @brief Get a snapshot of aggregate metrics across all connections
     */
    ManagerMetrics getManagerMetrics() const noexcept;

    // EntropyObject interface
    const char* className() const noexcept override { return "ConnectionManager"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

    // Internal: For SessionManager to access underlying connection
    NetworkConnection* getConnectionPointer(const ConnectionHandle& handle);

private:
    /// Sentinel value for invalid slot index
    static constexpr uint32_t INVALID_INDEX = ~0u;

    /**
     * @brief Internal storage for a connection slot
     */
    struct ConnectionSlot {
        std::atomic<uint32_t> generation{1};
        std::atomic<ConnectionState> state{ConnectionState::Disconnected};
        std::unique_ptr<NetworkConnection> connection;
        ConnectionType type;
        std::atomic<uint32_t> nextFree{INVALID_INDEX};

        // User-provided callbacks (set via ConnectionHandle); accessed under mutex
        std::function<void(const std::vector<uint8_t>&)> userMessageCb;
        std::function<void(ConnectionState)> userStateCb;

        std::mutex mutex;  // Per-slot mutex for connection operations
    };

    const size_t _capacity;
    std::vector<ConnectionSlot> _connectionSlots;
    std::atomic<uint64_t> _freeListHead{0};  // Packed: tag(32) | index(32)
    std::atomic<size_t> _activeCount{0};

    // Handle validation
    bool validateHandle(const ConnectionHandle& handle) const noexcept;

    // Slot allocation
    uint32_t allocateSlot();
    void returnSlotToFreeList(uint32_t index);

    // Backend creation (platform-specific)
    std::unique_ptr<NetworkConnection> createLocalBackend(const ConnectionConfig& config);
    std::unique_ptr<NetworkConnection> createRemoteBackend(const ConnectionConfig& config);

    friend class ConnectionHandle;
    friend class SessionManager;

    // Lightweight aggregate metrics counters (atomic)
    struct MetricsCounters {
        std::atomic<uint64_t> totalBytesSent{0};
        std::atomic<uint64_t> totalBytesReceived{0};
        std::atomic<uint64_t> totalMessagesSent{0};
        std::atomic<uint64_t> totalMessagesReceived{0};
        std::atomic<uint64_t> connectionsOpened{0};
        std::atomic<uint64_t> connectionsFailed{0};
        std::atomic<uint64_t> connectionsClosed{0};
        std::atomic<uint64_t> wouldBlockSends{0};
    };

    mutable MetricsCounters _metrics;
};

} // namespace EntropyEngine::Networking
