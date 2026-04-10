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

#include <cstdint>
#include <string>
#include <vector>

#include "../Core/ErrorCodes.h"
#include "../Core/PropertyRegistry.h"
#include "../Transport/ConnectionHandle.h"
#include "NetworkSession.h"

namespace EntropyEngine::Networking
{

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
class SessionHandle : public Core::EntropyObject
{
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
            Core::HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                                    other.handleGeneration());
        }
    }

    // Copy assignment
    SessionHandle& operator=(const SessionHandle& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                Core::HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
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
            Core::HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                                    other.handleGeneration());
        }
    }

    // Move assignment
    SessionHandle& operator=(SessionHandle&& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                Core::HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
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
     * @param components Component groups with their properties
     * @param targetSceneId Scene to add entity to (0 = use session's default scene)
     * @param entityName Flecs entity name for client-side identification
     * @return Result indicating success or failure
     */
    Result<void> sendEntityCreated(uint64_t entityId, const std::string& appId, const std::string& typeName,
                                   uint64_t parentId,
                                   const std::vector<NetworkSession::ComponentGroupData>& components = {},
                                   uint64_t targetSceneId = 0, const std::string& entityName = "") const;

    /**
     * @brief Sends EntityDestroyed protocol message
     *
     * Notifies remote peer of entity destruction.
     * @param entityId Entity to destroy
     * @return Result indicating success or failure
     */
    Result<void> sendEntityDestroyed(uint64_t entityId) const;

    /**
     * @brief Sends ComponentAdded protocol message
     *
     * Notifies remote peer that a component was added to an entity.
     * @param entityId Entity that received the component
     * @param component Component data including type hash and properties
     * @return Result indicating success or failure
     */
    Result<void> sendComponentAdded(uint64_t entityId, const NetworkSession::ComponentGroupData& component) const;

    /**
     * @brief Sends ComponentRemoved protocol message
     *
     * Notifies remote peer that a component was removed from an entity.
     * @param entityId Entity that lost the component
     * @param typeHash Type hash of the removed component
     * @return Result indicating success or failure
     */
    Result<void> sendComponentRemoved(uint64_t entityId, ComponentTypeHash typeHash) const;

    /**
     * @brief Sends single property update
     *
     * Sends individual property change. For bulk updates, use sendPropertyUpdateBatch().
     * @param hash Pre-computed property hash
     * @param type Property type
     * @param value Property value
     * @return Result indicating success or failure
     */
    Result<void> sendPropertyUpdate(PropertyHash hash, PropertyType type, const PropertyValue& value) const;

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

    /**
     * @brief Sends heartbeat message
     *
     * Sends a heartbeat to the remote peer. The peer will respond with
     * a HeartbeatResponse. Used for connection liveness detection.
     * @return Result indicating success or failure
     */
    Result<void> sendHeartbeat() const;

    // Asset protocol operations

    /**
     * @brief Sends AssetAdvertise request
     * @param appId Application identifier
     * @param entries Asset entries to advertise
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetAdvertise(const std::string& appId,
                                    const std::vector<NetworkSession::AssetEntryData>& entries,
                                    uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetWithdraw request
     * @param assetIds Asset IDs to withdraw
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetWithdraw(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetWithdrawAll request
     * @param appId Application identifier
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetWithdrawAll(const std::string& appId, uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetResolve request
     * @param assetId Asset ID to resolve
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetResolve(const std::array<uint8_t, 32>& assetId, uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetResolveBatch request
     * @param assetIds Asset IDs to resolve
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetResolveBatch(const std::vector<std::array<uint8_t, 32>>& assetIds,
                                       uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetProvideKey request
     * @param assetId Asset ID
     * @param key 32-byte encryption key
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetProvideKey(const std::array<uint8_t, 32>& assetId, const std::array<uint8_t, 32>& key,
                                     uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetUpload request
     * @param appId Application identifier
     * @param data Asset data
     * @param contentType Content type
     * @param persistent Whether asset survives app disconnect
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                                 bool persistent, uint64_t requestId = 0) const;

    /**
     * @brief Sends AssetUpload request with metadata
     * @param appId Application identifier
     * @param data Asset data
     * @param contentType Content type
     * @param persistent Whether asset survives app disconnect
     * @param requestId Request ID for response correlation
     * @param metadata Type-specific metadata (shader, texture, etc.)
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                                 bool persistent, uint64_t requestId,
                                 const NetworkSession::AssetMetadataData& metadata) const;

    /**
     * @brief Sends AssetFetch request (for WebRTC delivery)
     * @param assetId Asset ID to fetch
     * @param requestId Request ID for response correlation
     * @return Result indicating success or failure
     */
    Result<void> sendAssetFetch(const std::array<uint8_t, 32>& assetId, uint64_t requestId = 0) const;

    // Chunked upload operations

    /**
     * @brief Begins a chunked upload session
     * @param data Upload parameters
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUploadBegin(const NetworkSession::AssetUploadBeginData& data) const;

    /**
     * @brief Sends a chunk of data during chunked upload
     * @param data Chunk data with upload ID, offset, and payload
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUploadChunk(const NetworkSession::AssetUploadChunkData& data) const;

    /**
     * @brief Completes a chunked upload session
     * @param data Completion data with upload ID and total chunks
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUploadComplete(const NetworkSession::AssetUploadCompleteData& data) const;

    /**
     * @brief Cancels an in-progress chunked upload
     * @param uploadId 16-byte upload session ID
     * @return Result indicating success or failure
     */
    Result<void> sendAssetUploadCancel(const std::array<uint8_t, 16>& uploadId) const;

    // Multi-channel support

    /**
     * @brief Check if connection supports multiple data channels
     * @return true if WebRTC-style multi-channel is available
     */
    bool supportsMultipleChannels() const;

    /**
     * @brief Open a named data channel for bulk data transfer
     *
     * For WebRTC connections, creates a dedicated data channel.
     * For other backends, this is a no-op.
     *
     * @param channel Channel name (use NetworkConnection::CHANNEL_* constants)
     * @return Result indicating success or failure
     */
    Result<void> openChannel(const std::string& channel) const;

    // Permission system operations

    /**
     * @brief Sends PermissionRequest to remote peer
     * @param requestId Correlation ID for this request
     * @param requestingSessionId Session originating the request
     * @param appId Human-readable name of the requesting application
     * @param key Permission category/permission pair
     * @param reason Human-readable reason shown to user
     * @return Result indicating success or failure
     */
    Result<void> sendPermissionRequest(uint64_t requestId, uint64_t requestingSessionId, const std::string& appId,
                                       PermissionKey key, const std::string& reason) const;

    /**
     * @brief Sends PermissionResponse to remote peer
     * @param requestId Correlates to the original PermissionRequest
     * @param respondingSessionId Session that is responding (portal)
     * @param key Permission key being responded to
     * @param granted User's decision
     * @return Result indicating success or failure
     */
    Result<void> sendHeadPosePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false) const;

    Result<void> sendIdentityPermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& identityHash = "") const;

    Result<void> sendUsernamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& username = "") const;

    Result<void> sendHostnamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& hostname = "") const;

    /**
     * @brief Sends PermissionRevoked notification
     * @param portalSessionId Portal that disconnected
     * @param key Which permission is revoked
     * @return Result indicating success or failure
     */
    Result<void> sendPermissionRevoked(uint64_t portalSessionId, PermissionKey key) const;

    /**
     * @brief Sends PermissionRequestCancelled notification
     * @param requestId Which request was cancelled
     * @param key Which permission
     * @return Result indicating success or failure
     */
    Result<void> sendPermissionRequestCancelled(uint64_t requestId, PermissionKey key) const;

    // Spatial anchoring operations

    Result<void> sendRegisterLandmarkRequest(uint64_t requestId, const std::vector<uint8_t>& definitionMsgData) const;
    Result<void> sendRegisterLandmarkResponse(uint64_t requestId, bool success, uint64_t landmarkId,
                                              const std::string& errorMessage = "") const;
    Result<void> sendLandmarkObservation(uint64_t landmarkId, uint64_t observerSessionId, bool detected,
                                         const LandmarkPose& pose, float confidence, uint64_t timestamp) const;
    Result<void> sendLandmarkStateUpdate(uint64_t landmarkId, uint8_t state, const LandmarkPose& pose,
                                         uint16_t observerCount) const;
    Result<void> sendUnregisterLandmarkRequest(uint64_t requestId, uint64_t landmarkId) const;
    Result<void> sendUnregisterLandmarkResponse(uint64_t requestId, bool success,
                                                const std::string& errorMessage = "") const;
    Result<void> sendLandmarkSnapshot(const std::vector<uint8_t>& snapshotData) const;

    // Handshake operations

    /**
     * @brief Initiates protocol handshake with remote peer
     *
     * Sends handshake message to establish protocol-level communication.
     * This method should be called AFTER setting up callbacks (setHandshakeCallback,
     * setEntityCreatedCallback, etc.) to ensure notifications are received.
     *
     * Server-side: NetworkSession automatically responds to incoming handshakes,
     * so this method is typically not needed on the server.
     *
     * Client-side: Call this method after the underlying connection is established
     * and callbacks are configured.
     *
     * @param clientType Type identifier for this client (e.g., "EntropyClient", "CanvasViewer")
     * @param clientId Unique identifier for this client instance
     * @return Result indicating success or failure
     */
    Result<void> performHandshake(const std::string& clientType, const std::string& clientId) const;

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
    const char* className() const noexcept override {
        return "SessionHandle";
    }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    SessionManager* manager() const;
};

}  // namespace EntropyEngine::Networking
