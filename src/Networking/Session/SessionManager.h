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
#include "SessionHandle.h"
#include "NetworkSession.h"
#include "../Transport/ConnectionManager.h"
#include "../Transport/ConnectionHandle.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

namespace EntropyEngine::Networking {

/**
 * @brief Slot-based session manager for protocol-level operations
 *
 * SessionManager follows the WorkContractGroup pattern - it owns session
 * slots and returns generation-stamped handles. Builds on ConnectionManager
 * to provide protocol-level messaging (EntityCreated, PropertyUpdate, etc.).
 *
 * Handle lifecycle:
 * 1. Create session via createSession(connectionHandle)
 * 2. Returns SessionHandle stamped with (manager + index + generation)
 * 3. Use handle for protocol operations
 * 4. Handle becomes invalid after release
 *
 * Session structure:
 * - Each session wraps a ConnectionHandle
 * - Sessions maintain PropertyRegistry for entity tracking
 * - Callbacks can be set for incoming protocol messages
 *
 * Thread-safe: All operations are lock-free or use minimal locking.
 *
 * @code
 * ConnectionManager connMgr(1024);
 * SessionManager sessMgr(&connMgr, 512);
 *
 * auto conn = connMgr.openLocalConnection("/tmp/entropy.sock");
 * conn.connect().wait();
 *
 * auto sess = sessMgr.createSession(conn);
 * sess.sendEntityCreated(entityId, appId, typeName, parentId);
 *
 * // Set up callbacks
 * sessMgr.setEntityCreatedCallback(sess, [](auto...) { ... });
 * @endcode
 */
class SessionManager : public Core::EntropyObject {
public:
    // Message type callbacks
    using EntityCreatedCallback = std::function<void(
        uint64_t entityId,
        const std::string& appId,
        const std::string& typeName,
        uint64_t parentId
    )>;
    using EntityDestroyedCallback = std::function<void(uint64_t entityId)>;
    using PropertyUpdateCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using SceneSnapshotCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using ErrorCallback = std::function<void(NetworkError error, const std::string& message)>;

    /**
     * @brief Constructs session manager with specified capacity
     *
     * @param connectionManager Reference to connection manager (must outlive SessionManager)
     * @param capacity Maximum number of sessions (typically 512-2048)
     */
    explicit SessionManager(ConnectionManager* connectionManager, size_t capacity);
    ~SessionManager();

    // Delete copy operations
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Session creation

    /**
     * @brief Creates a new session wrapping the given connection
     *
     * The session stores a copy of the connection handle and creates a
     * NetworkSession instance internally.
     *
     * @param connection Connection handle to wrap
     * @return SessionHandle for operations, or invalid if full or connection invalid
     */
    SessionHandle createSession(ConnectionHandle connection);

    // Callback configuration

    /**
     * @brief Sets callback for EntityCreated messages
     *
     * Callback is invoked when remote peer sends EntityCreated message.
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setEntityCreatedCallback(const SessionHandle& handle, EntityCreatedCallback callback);

    /**
     * @brief Sets callback for EntityDestroyed messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setEntityDestroyedCallback(const SessionHandle& handle, EntityDestroyedCallback callback);

    /**
     * @brief Sets callback for PropertyUpdate batches
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setPropertyUpdateCallback(const SessionHandle& handle, PropertyUpdateCallback callback);

    /**
     * @brief Sets callback for SceneSnapshot messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setSceneSnapshotCallback(const SessionHandle& handle, SceneSnapshotCallback callback);

    /**
     * @brief Sets callback for error conditions
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setErrorCallback(const SessionHandle& handle, ErrorCallback callback);

    // Internal operations called by SessionHandle

    /**
     * @brief Sends EntityCreated message (called by handle.sendEntityCreated())
     */
    Result<void> sendEntityCreated(
        const SessionHandle& handle,
        uint64_t entityId,
        const std::string& appId,
        const std::string& typeName,
        uint64_t parentId
    );

    /**
     * @brief Sends EntityDestroyed message (called by handle.sendEntityDestroyed())
     */
    Result<void> sendEntityDestroyed(const SessionHandle& handle, uint64_t entityId);

    /**
     * @brief Sends PropertyUpdate message (called by handle.sendPropertyUpdate())
     */
    Result<void> sendPropertyUpdate(
        const SessionHandle& handle,
        uint64_t entityId,
        const std::string& propertyName,
        const PropertyValue& value
    );

    /**
     * @brief Sends PropertyUpdateBatch message (called by handle.sendPropertyUpdateBatch())
     */
    Result<void> sendPropertyUpdateBatch(
        const SessionHandle& handle,
        const std::vector<uint8_t>& batchData
    );

    /**
     * @brief Sends SceneSnapshot message (called by handle.sendSceneSnapshot())
     */
    Result<void> sendSceneSnapshot(
        const SessionHandle& handle,
        const std::vector<uint8_t>& snapshotData
    );

    /**
     * @brief Checks if connected (called by handle.isConnected())
     */
    bool isConnected(const SessionHandle& handle) const;

    /**
     * @brief Gets connection state (called by handle.getConnectionState())
     */
    ConnectionState getConnectionState(const SessionHandle& handle) const;

    /**
     * @brief Gets connection stats (called by handle.getConnectionStats())
     */
    ConnectionStats getConnectionStats(const SessionHandle& handle) const;

    /**
     * @brief Gets connection handle (called by handle.getConnection())
     */
    ConnectionHandle getConnection(const SessionHandle& handle) const;

    /**
     * @brief Gets property registry (called by handle.getPropertyRegistry())
     */
    PropertyRegistry& getPropertyRegistry(const SessionHandle& handle);

    /**
     * @brief Gets property registry const (called by handle.getPropertyRegistry())
     */
    const PropertyRegistry& getPropertyRegistry(const SessionHandle& handle) const;

    /**
     * @brief Validates handle (called by handle.valid())
     */
    bool isValidHandle(const SessionHandle& handle) const noexcept;

    /**
     * @brief Gets active session count
     * @return Number of currently allocated sessions
     */
    size_t activeCount() const noexcept {
        return _activeCount.load(std::memory_order_acquire);
    }

    /**
     * @brief Gets maximum capacity
     * @return Maximum number of sessions this manager can handle
     */
    size_t capacity() const noexcept { return _capacity; }

    // EntropyObject interface
    const char* className() const noexcept override { return "SessionManager"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    /// Sentinel value for invalid slot index
    static constexpr uint32_t INVALID_INDEX = ~0u;

    /**
     * @brief Internal storage for a session slot
     */
    struct SessionSlot {
        std::atomic<uint32_t> generation{1};
        ConnectionHandle connection;                        // Stored connection handle
        std::unique_ptr<NetworkSession> session;            // Protocol layer
        std::atomic<uint32_t> nextFree{INVALID_INDEX};
        std::mutex mutex;  // Per-slot mutex for session operations
    };

    ConnectionManager* _connectionManager;  // Not owned
    const size_t _capacity;
    std::vector<SessionSlot> _sessionSlots;
    std::atomic<uint64_t> _freeListHead{0};  // Packed: tag(32) | index(32)
    std::atomic<size_t> _activeCount{0};

    // Handle validation
    bool validateHandle(const SessionHandle& handle) const noexcept;

    // Slot allocation
    uint32_t allocateSlot();
    void returnSlotToFreeList(uint32_t index);

    friend class SessionHandle;
};

} // namespace EntropyEngine::Networking
