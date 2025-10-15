/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file SessionHandle.h
 * @brief Generation-stamped handle for protocol-level network sessions
 *
 * This file contains SessionHandle, which provides the primary API for entity and
 * property synchronization operations over network connections.
 */

#pragma once

#include <EntropyCore.h>
#include "../Transport/ConnectionHandle.h"
#include "../Core/PropertyRegistry.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <cstdint>
#include <string>

namespace EntropyEngine::Networking {

// Forward declaration
class SessionManager;

/**
 * @brief EntropyObject-stamped handle for network sessions
 *
 * SessionHandle is the primary entry point for protocol-level operations.
 * It wraps a ConnectionHandle and provides high-level message sending:
 * - sendEntityCreated() / sendEntityDestroyed()
 * - sendPropertyUpdate() / sendPropertyUpdateBatch()
 * - sendSceneSnapshot()
 *
 * The handle follows the WorkContractHandle pattern - stamped with
 * (manager + index + generation) and delegates to SessionManager.
 *
 * Copy semantics:
 * - Copying a handle copies its stamped identity (not ownership transfer)
 * - SessionManager owns lifetime; handles become invalid when freed
 *
 * Typical workflow:
 * 1. Create via SessionManager::createSession(connectionHandle)
 * 2. Use for protocol operations (sendEntityCreated, etc.)
 * 3. After release, valid() returns false
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
 * @endcode
 */
class SessionHandle : public Core::EntropyObject {
private:
    friend class SessionManager;

    // Private constructor for SessionManager to stamp identity
    SessionHandle(SessionManager* manager, uint32_t index, uint32_t generation) {
        Core::HandleAccess::set(*this, manager, index, generation);
    }

public:
    // Default: invalid (no stamped identity)
    SessionHandle() = default;

    // Copy constructor: create a new handle object stamped with the same identity
    SessionHandle(const SessionHandle& other) noexcept {
        if (other.hasHandle()) {
            Core::HandleAccess::set(*this,
                              const_cast<void*>(other.handleOwner()),
                              other.handleIndex(),
                              other.handleGeneration());
        }
    }

    // Copy assignment
    SessionHandle& operator=(const SessionHandle& other) noexcept {
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
    SessionHandle(SessionHandle&& other) noexcept {
        if (other.hasHandle()) {
            Core::HandleAccess::set(*this,
                              const_cast<void*>(other.handleOwner()),
                              other.handleIndex(),
                              other.handleGeneration());
        }
    }

    // Move assignment
    SessionHandle& operator=(SessionHandle&& other) noexcept {
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

    // Protocol operations

    /**
     * @brief Sends EntityCreated protocol message
     *
     * Notifies remote peer of new entity creation.
     * @param entityId Unique entity identifier
     * @param appId Application identifier
     * @param typeName Entity type name
     * @param parentId Parent entity ID (0 for root)
     * @return Result indicating success or failure
     */
    Result<void> sendEntityCreated(
        uint64_t entityId,
        const std::string& appId,
        const std::string& typeName,
        uint64_t parentId
    ) const;

    /**
     * @brief Sends EntityDestroyed protocol message
     *
     * Notifies remote peer of entity destruction.
     * @param entityId Entity to destroy
     * @return Result indicating success or failure
     */
    Result<void> sendEntityDestroyed(uint64_t entityId) const;

    /**
     * @brief Sends single property update
     *
     * Sends individual property change. For bulk updates, use sendPropertyUpdateBatch().
     * @param entityId Entity owning the property
     * @param propertyName Name of the property
     * @param value Property value
     * @return Result indicating success or failure
     */
    Result<void> sendPropertyUpdate(
        uint64_t entityId,
        const std::string& propertyName,
        const PropertyValue& value
    ) const;

    /**
     * @brief Sends batched property updates
     *
     * Sends pre-serialized batch of property updates. Used by BatchManager.
     * @param batchData Serialized property update batch
     * @return Result indicating success or failure
     */
    Result<void> sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData) const;

    /**
     * @brief Sends scene snapshot
     *
     * Sends complete scene state for initialization or synchronization.
     * @param snapshotData Serialized scene snapshot
     * @return Result indicating success or failure
     */
    Result<void> sendSceneSnapshot(const std::vector<uint8_t>& snapshotData) const;

    // Connection queries (pass-through to underlying connection)

    /**
     * @brief Checks if underlying connection is established
     * @return true if connection is in Connected state
     */
    bool isConnected() const;

    /**
     * @brief Gets underlying connection state
     * @return Connection state, or Disconnected if handle is invalid
     */
    ConnectionState getConnectionState() const;

    /**
     * @brief Gets connection statistics
     * @return Stats with byte/message counts
     */
    ConnectionStats getConnectionStats() const;

    /**
     * @brief Gets the underlying connection handle
     *
     * For advanced use cases that need direct connection access.
     * @return ConnectionHandle for this session
     */
    ConnectionHandle getConnection() const;

    // Property registry access

    /**
     * @brief Gets the property registry for this session
     *
     * Registry tracks entity properties and provides hash-based lookups.
     * @return Reference to property registry
     */
    PropertyRegistry& getPropertyRegistry();

    /**
     * @brief Gets the property registry (const version)
     * @return Const reference to property registry
     */
    const PropertyRegistry& getPropertyRegistry() const;

    /**
     * @brief Checks whether this handle still refers to a live session
     *
     * Validates that the handle's owner, index, and generation match the
     * SessionManager's current slot state.
     * @return true if handle is valid and refers to an allocated session
     */
    bool valid() const;

    // EntropyObject interface
    const char* className() const noexcept override { return "SessionHandle"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    SessionManager* manager() const;
};

} // namespace EntropyEngine::Networking
