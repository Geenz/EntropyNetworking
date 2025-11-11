/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file ConnectionManager.h
 * @brief Platform-agnostic connection manager with slot-based allocation
 *
 * This file contains ConnectionManager, the core of the transport layer. It provides
 * lock-free connection management with generation-stamped handles for safe concurrent access.
 */

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
 * Thread Safety: All public methods are thread-safe. Internal operations use
 * lock-free algorithms or minimal per-slot locking. Callbacks are invoked using
 * lock-free atomic shared_ptr access to prevent deadlocks.
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
     * @brief Opens a remote connection using WebRTC with internal signaling
     *
     * Simplified client-side API - WebSocket signaling handled internally.
     * @param signalingUrl WebSocket URL for signaling (e.g., "ws://localhost:8080")
     * @return ConnectionHandle for operations, or invalid if full
     */
    ConnectionHandle openRemoteConnection(const std::string& signalingUrl);

    /**
     * @brief Opens a remote connection using WebRTC with explicit callbacks
     *
     * Advanced API for server-side or custom signaling.
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
    void setMessageCallback(const ConnectionHandle& handle, std::function<void(const std::vector<uint8_t>&)> callback) noexcept;

    /**
     * @brief Sets state callback (called by handle.setStateCallback())
     * @param handle Connection handle
     * @param callback Function called when connection state changes
     */
    void setStateCallback(const ConnectionHandle& handle, std::function<void(ConnectionState)> callback) noexcept;

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
        std::atomic<uint32_t> generation{1};                                        ///< Generation counter for handle validation
        std::atomic<ConnectionState> state{ConnectionState::Disconnected};          ///< Current connection state (query source)
        std::atomic<ConnectionState> lastPublishedState{ConnectionState::Disconnected}; ///< Last state sent to metrics (dedup tracking)
        std::unique_ptr<NetworkConnection> connection;                              ///< Backend implementation (Unix/WebRTC/XPC)
        ConnectionType type;                                                        ///< Connection type (Local or Remote)
        std::atomic<uint32_t> nextFree{INVALID_INDEX};                              ///< Next free slot index (free list)

        // User-provided callbacks (set via ConnectionHandle); accessed via atomic_load/atomic_store on shared_ptr for lock-free fanout
        std::shared_ptr<std::function<void(const std::vector<uint8_t>&)>> userMessageCb;  ///< User message callback
        std::shared_ptr<std::function<void(ConnectionState)>> userStateCb;                ///< User state callback

        std::mutex mutex;  ///< Per-slot mutex for connection operations
    };

    const size_t _capacity;                      ///< Maximum number of connections
    std::vector<ConnectionSlot> _connectionSlots; ///< Pre-allocated connection slots
    std::atomic<uint64_t> _freeListHead{0};      ///< Free list head (packed: tag(32) | index(32))
    std::atomic<size_t> _activeCount{0};         ///< Currently allocated connections

    // Handle validation
    bool validateHandle(const ConnectionHandle& handle) const noexcept;

    // Centralized state publish + metrics de-duplication
    void handleStatePublish(uint32_t index, ConnectionState newState) noexcept;

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
        std::atomic<uint64_t> totalBytesSent{0};        ///< Total bytes sent across all connections
        std::atomic<uint64_t> totalBytesReceived{0};    ///< Total bytes received across all connections
        std::atomic<uint64_t> totalMessagesSent{0};     ///< Total messages sent
        std::atomic<uint64_t> totalMessagesReceived{0}; ///< Total messages received
        std::atomic<uint64_t> connectionsOpened{0};     ///< Count of successful connections
        std::atomic<uint64_t> connectionsFailed{0};     ///< Count of failed connection attempts
        std::atomic<uint64_t> connectionsClosed{0};     ///< Count of closed connections
        std::atomic<uint64_t> wouldBlockSends{0};       ///< Count of WouldBlock from trySend()
    };

    mutable MetricsCounters _metrics;  ///< Aggregate observability metrics
};

} // namespace EntropyEngine::Networking
