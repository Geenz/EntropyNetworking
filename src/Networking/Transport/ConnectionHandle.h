/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file ConnectionHandle.h
 * @brief Generation-stamped handle for network connections
 *
 * This file contains ConnectionHandle, which provides the primary API for connection
 * operations. Follows the WorkContractHandle pattern with safe handle invalidation.
 */

#pragma once

#include <EntropyCore.h>
#include "../Core/ConnectionTypes.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <cstdint>

namespace EntropyEngine::Networking {

// Forward declaration
class ConnectionManager;

/**
 * @brief EntropyObject-stamped handle for network connections
 *
 * ConnectionHandle follows the WorkContractHandle pattern - it's an EntropyObject
 * stamped with (owner + index + generation) that delegates operations to ConnectionManager.
 *
 * The handle is the primary API for connection operations:
 * - connect() / disconnect()
 * - send() / sendUnreliable()
 * - State queries (isConnected, getState, getStats)
 * - Generation-based validation (valid())
 *
 * Copy semantics:
 * - Copying a handle copies its stamped identity (not ownership transfer)
 * - The ConnectionManager owns lifetime; handles become invalid when freed
 *
 * Typical workflow:
 * 1. Create via ConnectionManager::openLocalConnection() or openRemoteConnection()
 * 2. Call connect(), then send operations
 * 3. After disconnect or release, valid() returns false
 *
 * @code
 * ConnectionManager connMgr(1024);
 * auto h = connMgr.openLocalConnection("/tmp/entropy.sock");
 * h.connect().wait();
 * if (h.isConnected()) {
 *     h.send(data);
 * }
 * @endcode
 */
class ConnectionHandle : public Core::EntropyObject {
private:
    friend class ConnectionManager;

    // Private constructor for ConnectionManager to stamp identity
    ConnectionHandle(ConnectionManager* manager, uint32_t index, uint32_t generation) {
        Core::HandleAccess::set(*this, manager, index, generation);
    }

public:
    // Default: invalid (no stamped identity)
    ConnectionHandle() = default;

    // Copy constructor: create a new handle object stamped with the same identity
    ConnectionHandle(const ConnectionHandle& other) noexcept {
        if (other.hasHandle()) {
            Core::HandleAccess::set(*this,
                              const_cast<void*>(other.handleOwner()),
                              other.handleIndex(),
                              other.handleGeneration());
        }
    }

    // Copy assignment
    ConnectionHandle& operator=(const ConnectionHandle& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                Core::HandleAccess::set(*this,
                                  const_cast<void*>(other.handleOwner()),
                                  other.handleIndex(),
                                  other.handleGeneration());
            } else {
                Core::HandleAccess::clear(*this);
            }
        }
        return *this;
    }

    // Move constructor
    ConnectionHandle(ConnectionHandle&& other) noexcept {
        if (other.hasHandle()) {
            Core::HandleAccess::set(*this,
                              const_cast<void*>(other.handleOwner()),
                              other.handleIndex(),
                              other.handleGeneration());
        }
    }

    // Move assignment
    ConnectionHandle& operator=(ConnectionHandle&& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                Core::HandleAccess::set(*this,
                                  const_cast<void*>(other.handleOwner()),
                                  other.handleIndex(),
                                  other.handleGeneration());
            } else {
                Core::HandleAccess::clear(*this);
            }
        }
        return *this;
    }

    // Connection operations

    /**
     * @brief Initiates connection to the endpoint
     *
     * Transitions from Disconnected to Connecting state, then to Connected when ready.
     * @return Result indicating success or failure reason
     */
    Result<void> connect();

    /**
     * @brief Disconnects from the endpoint
     *
     * Gracefully closes the connection and transitions to Disconnected state.
     * Does NOT free the slot - use close() for that.
     * @return Result indicating success or failure reason
     */
    Result<void> disconnect();

    /**
     * @brief Closes connection and frees the slot
     *
     * Disconnects (if connected) and returns the slot to the free list.
     * After calling close(), valid() will return false and the handle cannot be reused.
     * @return Result indicating success or failure reason
     */
    Result<void> close();

    /**
     * @brief Sends data over the reliable channel
     *
     * Data is guaranteed to arrive in order and without loss.
     * @param data Bytes to send
     * @return Result indicating success or failure reason
     */
    Result<void> send(const std::vector<uint8_t>& data);

    /**
     * @brief Non-blocking send that returns WouldBlock on backpressure
     * @param data Bytes to send
     */
    Result<void> trySend(const std::vector<uint8_t>& data);

    /**
     * @brief Sends data over the unreliable channel (if available)
     *
     * Best-effort delivery with no ordering guarantees. Falls back to reliable
     * channel if unreliable is not supported by the backend.
     * @param data Bytes to send
     * @return Result indicating success or failure reason
     */
    Result<void> sendUnreliable(const std::vector<uint8_t>& data);

    // Queries

    /**
     * @brief Checks if connection is established and ready
     * @return true if state is Connected
     */
    bool isConnected() const;

    /**
     * @brief Gets the current connection state
     * @return Current connection state, or Disconnected if handle is invalid
     */
    ConnectionState getState() const;

    /**
     * @brief Gets connection statistics
     * @return Stats structure with byte/message counts
     */
    ConnectionStats getStats() const;

    /**
     * @brief Gets the connection type (Local or Remote)
     * @return Connection type determined at creation
     */
    ConnectionType getType() const;

    /**
     * @brief Checks whether this handle still refers to a live connection
     *
     * Validates that the handle's owner, index, and generation match the
     * ConnectionManager's current slot state.
     * @return true if handle is valid and refers to an allocated connection
     */
    bool valid() const;

    // Callback setters

    /**
     * @brief Sets callback for incoming messages
     *
     * Convenience method that delegates to ConnectionManager. Avoids exposing
     * NetworkConnection backend directly.
     * @param callback Function called when messages are received
     */
    void setMessageCallback(std::function<void(const std::vector<uint8_t>&)> callback) noexcept;

    /**
     * @brief Sets callback for connection state changes
     *
     * Convenience method that delegates to ConnectionManager. Avoids exposing
     * NetworkConnection backend directly.
     * @param callback Function called when connection state changes
     */
    void setStateCallback(std::function<void(ConnectionState)> callback) noexcept;

    // EntropyObject interface
    const char* className() const noexcept override { return "ConnectionHandle"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    ConnectionManager* manager() const;
};

} // namespace EntropyEngine::Networking
