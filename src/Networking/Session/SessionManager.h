/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file SessionManager.h
 * @brief Slot-based session manager for protocol-level operations
 *
 * This file contains SessionManager, which manages protocol sessions with entity/property
 * synchronization. Builds on ConnectionManager to provide high-level messaging.
 */

#pragma once

#include <EntropyCore.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "../Core/ErrorCodes.h"
#include "../Transport/ConnectionHandle.h"
#include "../Transport/ConnectionManager.h"
#include "NetworkSession.h"
#include "SessionHandle.h"

namespace EntropyEngine::Networking
{

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
 * Thread Safety: All public methods are thread-safe. Operations use minimal
 * per-slot locking for session access.
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
class SessionManager : public Core::EntropyObject
{
public:
    // Re-export PropertyRegistrationInfo for callback usage
    using PropertyRegistrationInfo = NetworkSession::PropertyRegistrationInfo;

    // Message type callbacks
    using ComponentGroupData = NetworkSession::ComponentGroupData;
    using EntityCreatedCallback = NetworkSession::EntityCreatedCallback;
    using EntityDestroyedCallback = std::function<void(uint64_t entityId)>;
    using PropertyUpdateCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using SceneSnapshotCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using HandshakeCallback = std::function<void(const std::string& clientType, const std::string& clientId)>;
    using ErrorCallback = std::function<void(NetworkError error, const std::string& message)>;
    using HeartbeatCallback = std::function<void(uint64_t timestamp)>;

    // Re-export asset types from NetworkSession for convenience
    using AssetEntryData = NetworkSession::AssetEntryData;
    using AssetResolveResponseData = NetworkSession::AssetResolveResponseData;

    // Asset message callbacks (server-side - receiving requests from clients)
    using AssetAdvertiseCallback =
        std::function<void(const std::string& appId, const std::vector<AssetEntryData>& entries, uint64_t requestId)>;
    using AssetWithdrawCallback =
        std::function<void(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId)>;
    using AssetWithdrawAllCallback = std::function<void(const std::string& appId, uint64_t requestId)>;
    using AssetResolveCallback = std::function<void(const std::array<uint8_t, 32>& assetId, uint64_t requestId)>;
    using AssetResolveBatchCallback =
        std::function<void(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId)>;
    using AssetProvideKeyCallback = std::function<void(const std::array<uint8_t, 32>& assetId,
                                                       const std::array<uint8_t, 32>& key, uint64_t requestId)>;
    using AssetUploadCallback =
        std::function<void(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                           bool persistent, uint64_t requestId, const NetworkSession::AssetMetadataData& metadata)>;
    using AssetFetchCallback = std::function<void(const std::array<uint8_t, 32>& assetId, uint64_t requestId)>;

    // Asset response callbacks (for clients receiving responses)
    using AssetAdvertiseResponseCallback =
        std::function<void(uint64_t requestId, bool success, const std::string& errorMessage)>;
    using AssetWithdrawResponseCallback =
        std::function<void(uint64_t requestId, bool success, uint32_t removedCount, const std::string& errorMessage)>;
    using AssetWithdrawAllResponseCallback =
        std::function<void(uint64_t requestId, bool success, uint32_t removedCount, const std::string& errorMessage)>;
    using AssetResolveResponseCallback =
        std::function<void(uint64_t requestId, const AssetResolveResponseData& response)>;
    using AssetResolveBatchResponseCallback =
        std::function<void(uint64_t requestId, const std::vector<AssetResolveResponseData>& responses)>;
    using AssetProvideKeyResponseCallback =
        std::function<void(uint64_t requestId, bool success, const std::string& errorMessage)>;
    using AssetUploadResponseCallback =
        std::function<void(uint64_t requestId, bool success, const std::array<uint8_t, 32>& assetId,
                           const std::string& uri, const std::string& errorMessage)>;
    using AssetFetchResponseCallback = std::function<void(
        uint64_t requestId, bool found, const std::vector<uint8_t>& data, const std::string& errorMessage)>;

    // Chunked upload callbacks
    using AssetUploadBeginResponseCallback =
        std::function<void(const NetworkSession::AssetUploadBeginResponseData& data)>;
    using AssetUploadChunkResponseCallback =
        std::function<void(const NetworkSession::AssetUploadChunkResponseData& data)>;
    using AssetUploadCompleteResponseCallback =
        std::function<void(const NetworkSession::AssetUploadCompleteResponseData& data)>;
    using AssetUploadCancelResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;

    // Scene management callbacks (server-side - receiving requests from clients)
    using CreateSceneCallback = std::function<void(const std::string& sceneName, bool transient)>;
    using CreateSceneResponseCallback =
        std::function<void(bool success, uint64_t sceneId, const std::string& errorMessage)>;
    using DestroySceneCallback = std::function<void(uint64_t sceneId)>;
    using DestroySceneResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using SetSceneEnabledCallback = std::function<void(uint64_t sceneId, bool enabled)>;
    using SetSceneEnabledResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using AddEntityToSceneCallback = std::function<void(uint64_t entityId, uint64_t sceneId)>;
    using AddEntityToSceneResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;

    // Re-export material types from NetworkSession
    using MaterialPropertyData = NetworkSession::MaterialPropertyData;
    using MaterialAssetData = NetworkSession::MaterialAssetData;

    // Material system callbacks (server-side - receiving requests from clients)
    using CreateMaterialCallback = NetworkSession::CreateMaterialCallback;
    using CreateMaterialResponseCallback = NetworkSession::CreateMaterialResponseCallback;
    using UpdateMaterialPropertyCallback = NetworkSession::UpdateMaterialPropertyCallback;
    using UpdateMaterialPropertyResponseCallback = NetworkSession::UpdateMaterialPropertyResponseCallback;
    using UpdateMaterialPropertiesBatchCallback = NetworkSession::UpdateMaterialPropertiesBatchCallback;
    using UpdateMaterialPropertiesBatchResponseCallback = NetworkSession::UpdateMaterialPropertiesBatchResponseCallback;
    using MaterialPropertyUpdateCallback = NetworkSession::MaterialPropertyUpdateCallback;
    using MaterialSubscribeCallback = NetworkSession::MaterialSubscribeCallback;
    using MaterialSubscribeResponseCallback = NetworkSession::MaterialSubscribeResponseCallback;
    using MaterialUnsubscribeCallback = NetworkSession::MaterialUnsubscribeCallback;
    using MaterialUnsubscribeResponseCallback = NetworkSession::MaterialUnsubscribeResponseCallback;
    using GetMaterialCallback = NetworkSession::GetMaterialCallback;
    using GetMaterialResponseCallback = NetworkSession::GetMaterialResponseCallback;
    using MaterialResolvedCallback = NetworkSession::MaterialResolvedCallback;

    /**
     * @brief Constructs session manager with specified capacity
     *
     * @param connectionManager Reference to connection manager (must outlive SessionManager)
     * @param capacity Maximum number of sessions (typically 512-2048)
     * @param schemaRegistry Optional ComponentSchemaRegistry for schema operations (must outlive SessionManager)
     */
    explicit SessionManager(ConnectionManager* connectionManager, size_t capacity,
                            ComponentSchemaRegistry* schemaRegistry = nullptr);
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
     * @param externalRegistry Optional external PropertyRegistry to share across sessions.
     *                        If nullptr, each session creates its own internal registry.
     * @return SessionHandle for operations, or invalid if full or connection invalid
     */
    SessionHandle createSession(ConnectionHandle connection, PropertyRegistry* externalRegistry = nullptr);

    /**
     * @brief Destroys a session and returns its slot to the free list
     *
     * This should be called when a session is no longer needed (e.g., after disconnect).
     * After this call, the handle becomes invalid.
     *
     * @param handle Session handle to destroy
     * @return Result indicating success or failure
     */
    Result<void> destroySession(const SessionHandle& handle);

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
     * @brief Sets callback for Handshake completion (server-side)
     * @param handle Session handle
     * @param callback Callback function invoked when handshake completes
     * @return Result indicating success or failure
     */
    Result<void> setHandshakeCallback(const SessionHandle& handle, HandshakeCallback callback);

    /**
     * @brief Sets callback for error conditions
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setErrorCallback(const SessionHandle& handle, ErrorCallback callback);

    /**
     * @brief Sets callback for Heartbeat messages
     *
     * Callback is invoked when a heartbeat is received from the peer.
     * Server-side uses this to track session liveness for timeout detection.
     *
     * @param handle Session handle
     * @param callback Callback function invoked with heartbeat timestamp
     * @return Result indicating success or failure
     */
    Result<void> setHeartbeatCallback(const SessionHandle& handle, HeartbeatCallback callback);

    /**
     * @brief Sets callback for connection disconnect/failure
     *
     * Called immediately when connection dies (broken pipe, EOF, etc).
     * Enables immediate session cleanup for local IPC instead of waiting for heartbeat timeout.
     * @param handle Session handle
     * @param callback Callback function invoked with state and reason
     * @return Result indicating success or failure
     */
    Result<void> setDisconnectCallback(const SessionHandle& handle, NetworkSession::DisconnectCallback callback);

    // Asset callback setters

    /**
     * @brief Sets callback for AssetAdvertise messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetAdvertiseCallback(const SessionHandle& handle, AssetAdvertiseCallback callback);

    /**
     * @brief Sets callback for AssetWithdraw messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetWithdrawCallback(const SessionHandle& handle, AssetWithdrawCallback callback);

    /**
     * @brief Sets callback for AssetWithdrawAll messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetWithdrawAllCallback(const SessionHandle& handle, AssetWithdrawAllCallback callback);

    /**
     * @brief Sets callback for AssetResolve messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetResolveCallback(const SessionHandle& handle, AssetResolveCallback callback);

    /**
     * @brief Sets callback for AssetResolveBatch messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetResolveBatchCallback(const SessionHandle& handle, AssetResolveBatchCallback callback);

    /**
     * @brief Sets callback for AssetProvideKey messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetProvideKeyCallback(const SessionHandle& handle, AssetProvideKeyCallback callback);

    /**
     * @brief Sets callback for AssetUpload messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetUploadCallback(const SessionHandle& handle, AssetUploadCallback callback);

    /**
     * @brief Sets callback for AssetFetch messages
     * @param handle Session handle
     * @param callback Callback function
     * @return Result indicating success or failure
     */
    Result<void> setAssetFetchCallback(const SessionHandle& handle, AssetFetchCallback callback);

    // Asset response callback setters (for clients receiving responses)

    /**
     * @brief Sets callback for AssetAdvertiseResponse messages
     */
    Result<void> setAssetAdvertiseResponseCallback(const SessionHandle& handle,
                                                   AssetAdvertiseResponseCallback callback);

    /**
     * @brief Sets callback for AssetWithdrawResponse messages
     */
    Result<void> setAssetWithdrawResponseCallback(const SessionHandle& handle, AssetWithdrawResponseCallback callback);

    /**
     * @brief Sets callback for AssetWithdrawAllResponse messages
     */
    Result<void> setAssetWithdrawAllResponseCallback(const SessionHandle& handle,
                                                     AssetWithdrawAllResponseCallback callback);

    /**
     * @brief Sets callback for AssetResolveResponse messages
     */
    Result<void> setAssetResolveResponseCallback(const SessionHandle& handle, AssetResolveResponseCallback callback);

    /**
     * @brief Sets callback for AssetResolveBatchResponse messages
     */
    Result<void> setAssetResolveBatchResponseCallback(const SessionHandle& handle,
                                                      AssetResolveBatchResponseCallback callback);

    /**
     * @brief Sets callback for AssetProvideKeyResponse messages
     */
    Result<void> setAssetProvideKeyResponseCallback(const SessionHandle& handle,
                                                    AssetProvideKeyResponseCallback callback);

    /**
     * @brief Sets callback for AssetUploadResponse messages
     */
    Result<void> setAssetUploadResponseCallback(const SessionHandle& handle, AssetUploadResponseCallback callback);

    /**
     * @brief Sets callback for AssetFetchResponse messages
     */
    Result<void> setAssetFetchResponseCallback(const SessionHandle& handle, AssetFetchResponseCallback callback);

    // Asset metadata callback setters

    /**
     * @brief Sets callback for AssetMetadataRequest messages
     */
    Result<void> setAssetMetadataCallback(const SessionHandle& handle, NetworkSession::AssetMetadataCallback callback);

    /**
     * @brief Sets callback for AssetMetadataResponse messages
     */
    Result<void> setAssetMetadataResponseCallback(const SessionHandle& handle,
                                                  NetworkSession::AssetMetadataResponseCallback callback);

    /**
     * @brief Sets callback for AssetUploadBeginResponse messages
     */
    Result<void> setAssetUploadBeginResponseCallback(const SessionHandle& handle,
                                                     AssetUploadBeginResponseCallback callback);

    /**
     * @brief Sets callback for AssetUploadChunkResponse messages
     */
    Result<void> setAssetUploadChunkResponseCallback(const SessionHandle& handle,
                                                     AssetUploadChunkResponseCallback callback);

    /**
     * @brief Sets callback for AssetUploadCompleteResponse messages
     */
    Result<void> setAssetUploadCompleteResponseCallback(const SessionHandle& handle,
                                                        AssetUploadCompleteResponseCallback callback);

    /**
     * @brief Sets callback for AssetUploadCancelResponse messages
     */
    Result<void> setAssetUploadCancelResponseCallback(const SessionHandle& handle,
                                                      AssetUploadCancelResponseCallback callback);

    // Scene management callback setters

    /**
     * @brief Sets callback for CreateSceneRequest messages
     * @param handle Session handle
     * @param callback Callback function invoked when CreateSceneRequest is received
     * @return Result indicating success or failure
     */
    Result<void> setCreateSceneCallback(const SessionHandle& handle, CreateSceneCallback callback);

    /**
     * @brief Sets callback for CreateSceneResponse messages
     * @param handle Session handle
     * @param callback Callback function invoked when CreateSceneResponse is received
     * @return Result indicating success or failure
     */
    Result<void> setCreateSceneResponseCallback(const SessionHandle& handle, CreateSceneResponseCallback callback);

    /**
     * @brief Sets callback for DestroySceneRequest messages
     * @param handle Session handle
     * @param callback Callback function invoked when DestroySceneRequest is received
     * @return Result indicating success or failure
     */
    Result<void> setDestroySceneCallback(const SessionHandle& handle, DestroySceneCallback callback);

    /**
     * @brief Sets callback for DestroySceneResponse messages
     * @param handle Session handle
     * @param callback Callback function invoked when DestroySceneResponse is received
     * @return Result indicating success or failure
     */
    Result<void> setDestroySceneResponseCallback(const SessionHandle& handle, DestroySceneResponseCallback callback);

    /**
     * @brief Sets callback for SetSceneEnabledRequest messages
     * @param handle Session handle
     * @param callback Callback function invoked when SetSceneEnabledRequest is received
     * @return Result indicating success or failure
     */
    Result<void> setSetSceneEnabledCallback(const SessionHandle& handle, SetSceneEnabledCallback callback);

    /**
     * @brief Sets callback for SetSceneEnabledResponse messages
     * @param handle Session handle
     * @param callback Callback function invoked when SetSceneEnabledResponse is received
     * @return Result indicating success or failure
     */
    Result<void> setSetSceneEnabledResponseCallback(const SessionHandle& handle,
                                                    SetSceneEnabledResponseCallback callback);

    /**
     * @brief Sets callback for AddEntityToSceneRequest messages
     * @param handle Session handle
     * @param callback Callback function invoked when AddEntityToSceneRequest is received
     * @return Result indicating success or failure
     */
    Result<void> setAddEntityToSceneCallback(const SessionHandle& handle, AddEntityToSceneCallback callback);

    /**
     * @brief Sets callback for AddEntityToSceneResponse messages
     * @param handle Session handle
     * @param callback Callback function invoked when AddEntityToSceneResponse is received
     * @return Result indicating success or failure
     */
    Result<void> setAddEntityToSceneResponseCallback(const SessionHandle& handle,
                                                     AddEntityToSceneResponseCallback callback);

    // Material system callback setters

    /**
     * @brief Sets callback for CreateMaterial requests
     * @param handle Session handle
     * @param callback Callback function invoked when CreateMaterialRequest is received
     * @return Result indicating success or failure
     */
    Result<void> setCreateMaterialCallback(const SessionHandle& handle, CreateMaterialCallback callback);

    /**
     * @brief Sets callback for CreateMaterial responses
     */
    Result<void> setCreateMaterialResponseCallback(const SessionHandle& handle,
                                                   CreateMaterialResponseCallback callback);

    /**
     * @brief Sets callback for UpdateMaterialProperty requests
     */
    Result<void> setUpdateMaterialPropertyCallback(const SessionHandle& handle,
                                                   UpdateMaterialPropertyCallback callback);

    /**
     * @brief Sets callback for UpdateMaterialProperty responses
     */
    Result<void> setUpdateMaterialPropertyResponseCallback(const SessionHandle& handle,
                                                           UpdateMaterialPropertyResponseCallback callback);

    /**
     * @brief Sets callback for UpdateMaterialPropertiesBatch requests
     */
    Result<void> setUpdateMaterialPropertiesBatchCallback(const SessionHandle& handle,
                                                          UpdateMaterialPropertiesBatchCallback callback);

    /**
     * @brief Sets callback for UpdateMaterialPropertiesBatch responses
     */
    Result<void> setUpdateMaterialPropertiesBatchResponseCallback(
        const SessionHandle& handle, UpdateMaterialPropertiesBatchResponseCallback callback);

    /**
     * @brief Sets callback for MaterialPropertyUpdate broadcasts (from other clients)
     */
    Result<void> setMaterialPropertyUpdateCallback(const SessionHandle& handle,
                                                   MaterialPropertyUpdateCallback callback);

    /**
     * @brief Sets callback for MaterialSubscribe requests
     */
    Result<void> setMaterialSubscribeCallback(const SessionHandle& handle, MaterialSubscribeCallback callback);

    /**
     * @brief Sets callback for MaterialSubscribe responses
     */
    Result<void> setMaterialSubscribeResponseCallback(const SessionHandle& handle,
                                                      MaterialSubscribeResponseCallback callback);

    /**
     * @brief Sets callback for MaterialUnsubscribe requests
     */
    Result<void> setMaterialUnsubscribeCallback(const SessionHandle& handle, MaterialUnsubscribeCallback callback);

    /**
     * @brief Sets callback for MaterialUnsubscribe responses
     */
    Result<void> setMaterialUnsubscribeResponseCallback(const SessionHandle& handle,
                                                        MaterialUnsubscribeResponseCallback callback);

    /**
     * @brief Sets callback for GetMaterial requests
     */
    Result<void> setGetMaterialCallback(const SessionHandle& handle, GetMaterialCallback callback);

    /**
     * @brief Sets callback for GetMaterial responses
     */
    Result<void> setGetMaterialResponseCallback(const SessionHandle& handle, GetMaterialResponseCallback callback);

    /**
     * @brief Sets callback for MaterialResolved notifications (server push of material data)
     */
    Result<void> setMaterialResolvedCallback(const SessionHandle& handle, MaterialResolvedCallback callback);

    /**
     * @brief Sets callback for MeshMaterialBinding requests (server receives binding requests)
     */
    Result<void> setMeshMaterialBindingCallback(const SessionHandle& handle,
                                                NetworkSession::MeshMaterialBindingCallback callback);

    /**
     * @brief Sets callback for MeshMaterialBinding responses (client receives results)
     */
    Result<void> setMeshMaterialBindingResponseCallback(const SessionHandle& handle,
                                                        NetworkSession::MeshMaterialBindingResponseCallback callback);

    /**
     * @brief Sets callback for MeshMaterialBinding updates (clients receive broadcast)
     */
    Result<void> setMeshMaterialBindingUpdateCallback(const SessionHandle& handle,
                                                      NetworkSession::MeshMaterialBindingUpdateCallback callback);

    // =========================================================================
    // Shader Callbacks
    // =========================================================================

    /**
     * @brief Sets callback for GetShader requests (server receives shader queries)
     */
    Result<void> setGetShaderCallback(const SessionHandle& handle, NetworkSession::GetShaderCallback callback);

    /**
     * @brief Sets callback for GetShader responses (client receives shader data)
     */
    Result<void> setGetShaderResponseCallback(const SessionHandle& handle,
                                              NetworkSession::GetShaderResponseCallback callback);

    // Asset send methods (for clients sending requests)

    /**
     * @brief Sends AssetAdvertise request
     */
    Result<void> sendAssetAdvertise(const SessionHandle& handle, const std::string& appId,
                                    const std::vector<NetworkSession::AssetEntryData>& entries, uint64_t requestId = 0);

    /**
     * @brief Sends AssetWithdraw request
     */
    Result<void> sendAssetWithdraw(const SessionHandle& handle, const std::vector<std::array<uint8_t, 32>>& assetIds,
                                   uint64_t requestId = 0);

    /**
     * @brief Sends AssetWithdrawAll request
     */
    Result<void> sendAssetWithdrawAll(const SessionHandle& handle, const std::string& appId, uint64_t requestId = 0);

    /**
     * @brief Sends AssetResolve request
     */
    Result<void> sendAssetResolve(const SessionHandle& handle, const std::array<uint8_t, 32>& assetId,
                                  uint64_t requestId = 0);

    /**
     * @brief Sends AssetResolveBatch request
     */
    Result<void> sendAssetResolveBatch(const SessionHandle& handle,
                                       const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId = 0);

    /**
     * @brief Sends AssetProvideKey request
     */
    Result<void> sendAssetProvideKey(const SessionHandle& handle, const std::array<uint8_t, 32>& assetId,
                                     const std::array<uint8_t, 32>& key, uint64_t requestId = 0);

    /**
     * @brief Sends AssetUpload request
     */
    Result<void> sendAssetUpload(const SessionHandle& handle, const std::string& appId,
                                 const std::vector<uint8_t>& data, uint8_t contentType, bool persistent,
                                 uint64_t requestId = 0);

    /**
     * @brief Sends AssetUpload request with metadata
     * @param metadata Type-specific metadata (shader, texture, etc.)
     */
    Result<void> sendAssetUpload(const SessionHandle& handle, const std::string& appId,
                                 const std::vector<uint8_t>& data, uint8_t contentType, bool persistent,
                                 uint64_t requestId, const NetworkSession::AssetMetadataData& metadata);

    /**
     * @brief Sends AssetFetch request
     */
    Result<void> sendAssetFetch(const SessionHandle& handle, const std::array<uint8_t, 32>& assetId,
                                uint64_t requestId = 0);

    /**
     * @brief Begins a chunked upload session
     */
    Result<void> sendAssetUploadBegin(const SessionHandle& handle, const NetworkSession::AssetUploadBeginData& data);

    /**
     * @brief Sends a chunk during chunked upload
     */
    Result<void> sendAssetUploadChunk(const SessionHandle& handle, const NetworkSession::AssetUploadChunkData& data);

    /**
     * @brief Completes a chunked upload session
     */
    Result<void> sendAssetUploadComplete(const SessionHandle& handle,
                                         const NetworkSession::AssetUploadCompleteData& data);

    /**
     * @brief Cancels an in-progress chunked upload
     */
    Result<void> sendAssetUploadCancel(const SessionHandle& handle, const std::array<uint8_t, 16>& uploadId);

    /**
     * @brief Check if session's connection supports multiple data channels
     */
    bool supportsMultipleChannels(const SessionHandle& handle);

    /**
     * @brief Open a named data channel for bulk data transfer
     */
    Result<void> openChannel(const SessionHandle& handle, const std::string& channel);

    // Internal operations called by SessionHandle

    /**
     * @brief Sends EntityCreated message (called by handle.sendEntityCreated())
     */
    Result<void> sendEntityCreated(const SessionHandle& handle, uint64_t entityId, const std::string& appId,
                                   const std::string& typeName, uint64_t parentId,
                                   const std::vector<NetworkSession::ComponentGroupData>& components = {},
                                   uint64_t targetSceneId = 0, const std::string& entityName = "");

    /**
     * @brief Sends EntityDestroyed message (called by handle.sendEntityDestroyed())
     */
    Result<void> sendEntityDestroyed(const SessionHandle& handle, uint64_t entityId);

    /**
     * @brief Sends ComponentAdded message (called by handle.sendComponentAdded())
     */
    Result<void> sendComponentAdded(const SessionHandle& handle, uint64_t entityId,
                                    const NetworkSession::ComponentGroupData& component);

    /**
     * @brief Sends ComponentRemoved message (called by handle.sendComponentRemoved())
     */
    Result<void> sendComponentRemoved(const SessionHandle& handle, uint64_t entityId, ComponentTypeHash typeHash);

    /**
     * @brief Sends PropertyUpdate message (called by handle.sendPropertyUpdate())
     */
    Result<void> sendPropertyUpdate(const SessionHandle& handle, PropertyHash hash, PropertyType type,
                                    const PropertyValue& value);

    /**
     * @brief Sends PropertyUpdateBatch message (called by handle.sendPropertyUpdateBatch())
     */
    Result<void> sendPropertyUpdateBatch(const SessionHandle& handle, const std::vector<uint8_t>& batchData);

    /**
     * @brief Sends SceneSnapshot message (called by handle.sendSceneSnapshot())
     */
    Result<void> sendSceneSnapshot(const SessionHandle& handle, const std::vector<uint8_t>& snapshotData);

    /**
     * @brief Sends Heartbeat message (called by handle.sendHeartbeat())
     */
    Result<void> sendHeartbeat(const SessionHandle& handle);

    // Asset send methods

    /**
     * @brief Sends AssetResolveResponse message
     */
    Result<void> sendAssetResolveResponse(const SessionHandle& handle, bool found, const AssetEntryData& entry,
                                          bool hasKey, const std::array<uint8_t, 32>& key, uint8_t deliveryMethod,
                                          uint64_t requestId = 0);

    /**
     * @brief Sends AssetUploadResponse message
     */
    Result<void> sendAssetUploadResponse(const SessionHandle& handle, bool success,
                                         const std::array<uint8_t, 32>& assetId, const std::string& uri,
                                         const std::string& errorMessage, uint64_t requestId = 0);

    /**
     * @brief Sends AssetAdvertiseResponse message
     */
    Result<void> sendAssetAdvertiseResponse(const SessionHandle& handle, bool success, const std::string& errorMessage,
                                            uint64_t requestId = 0);

    /**
     * @brief Sends AssetFetchResponse message
     */
    Result<void> sendAssetFetchResponse(const SessionHandle& handle, bool found, const std::vector<uint8_t>& data,
                                        const std::string& errorMessage, uint64_t requestId = 0);

    /**
     * @brief Sends AssetMetadataRequest message
     */
    Result<void> sendAssetMetadataRequest(const SessionHandle& handle, const std::array<uint8_t, 32>& assetId,
                                          uint64_t requestId = 0);

    /**
     * @brief Sends AssetMetadataResponse message
     */
    Result<void> sendAssetMetadataResponse(const SessionHandle& handle, bool found,
                                           const NetworkSession::AssetMetadataData& metadata, uint64_t requestId = 0);

    /**
     * @brief Initiates handshake (called by handle.performHandshake())
     */
    Result<void> performHandshake(const SessionHandle& handle, const std::string& clientType,
                                  const std::string& clientId);

    // Scene management send methods

    /**
     * @brief Sends CreateSceneRequest message
     */
    Result<void> sendCreateSceneRequest(const SessionHandle& handle, const std::string& sceneName, bool transient);

    /**
     * @brief Sends CreateSceneResponse message
     */
    Result<void> sendCreateSceneResponse(const SessionHandle& handle, bool success, uint64_t sceneId,
                                         const std::string& errorMessage);

    /**
     * @brief Sends DestroySceneRequest message
     */
    Result<void> sendDestroySceneRequest(const SessionHandle& handle, uint64_t sceneId);

    /**
     * @brief Sends DestroySceneResponse message
     */
    Result<void> sendDestroySceneResponse(const SessionHandle& handle, bool success, const std::string& errorMessage);

    /**
     * @brief Sends SetSceneEnabledRequest message
     */
    Result<void> sendSetSceneEnabledRequest(const SessionHandle& handle, uint64_t sceneId, bool enabled);

    /**
     * @brief Sends SetSceneEnabledResponse message
     */
    Result<void> sendSetSceneEnabledResponse(const SessionHandle& handle, bool success,
                                             const std::string& errorMessage);

    /**
     * @brief Sends AddEntityToSceneRequest message
     */
    Result<void> sendAddEntityToSceneRequest(const SessionHandle& handle, uint64_t entityId, uint64_t sceneId);

    /**
     * @brief Sends AddEntityToSceneResponse message
     */
    Result<void> sendAddEntityToSceneResponse(const SessionHandle& handle, bool success,
                                              const std::string& errorMessage);

    // Material system send methods

    /**
     * @brief Sends CreateMaterialRequest message
     */
    Result<void> sendCreateMaterialRequest(const SessionHandle& handle, const MaterialAssetData& material,
                                           uint64_t requestId = 0);

    /**
     * @brief Sends CreateMaterialResponse message
     */
    Result<void> sendCreateMaterialResponse(const SessionHandle& handle, bool success,
                                            const std::array<uint8_t, 32>& materialId, const std::string& errorMessage,
                                            uint64_t requestId = 0);

    /**
     * @brief Sends UpdateMaterialPropertyRequest message
     */
    Result<void> sendUpdateMaterialPropertyRequest(const SessionHandle& handle,
                                                   const std::array<uint8_t, 32>& materialId,
                                                   const std::string& propertyName, const PropertyValue& value,
                                                   uint64_t requestId = 0);

    /**
     * @brief Sends UpdateMaterialPropertyResponse message
     */
    Result<void> sendUpdateMaterialPropertyResponse(const SessionHandle& handle, bool success, uint64_t newVersion,
                                                    const std::string& errorMessage);

    /**
     * @brief Sends UpdateMaterialPropertiesBatchRequest message
     */
    Result<void> sendUpdateMaterialPropertiesBatchRequest(const SessionHandle& handle,
                                                          const std::array<uint8_t, 32>& materialId,
                                                          const std::vector<MaterialPropertyData>& properties,
                                                          uint64_t requestId = 0);

    /**
     * @brief Sends UpdateMaterialPropertiesBatchResponse message
     */
    Result<void> sendUpdateMaterialPropertiesBatchResponse(const SessionHandle& handle, bool success,
                                                           uint64_t newVersion, const std::string& errorMessage);

    /**
     * @brief Sends MaterialPropertyUpdate broadcast message
     */
    Result<void> sendMaterialPropertyUpdate(const SessionHandle& handle, const std::array<uint8_t, 32>& materialId,
                                            const std::string& propertyName, const PropertyValue& value,
                                            uint64_t newVersion, uint64_t originSessionId);

    /**
     * @brief Sends MaterialSubscribeRequest message
     */
    Result<void> sendMaterialSubscribeRequest(const SessionHandle& handle, const std::array<uint8_t, 32>& materialId,
                                              uint64_t requestId = 0);

    /**
     * @brief Sends MaterialSubscribeResponse message
     */
    Result<void> sendMaterialSubscribeResponse(const SessionHandle& handle, bool success,
                                               const MaterialAssetData& material, const std::string& errorMessage,
                                               uint64_t requestId = 0);

    /**
     * @brief Sends MaterialUnsubscribeRequest message
     */
    Result<void> sendMaterialUnsubscribeRequest(const SessionHandle& handle, const std::array<uint8_t, 32>& materialId,
                                                uint64_t requestId = 0);

    /**
     * @brief Sends MaterialUnsubscribeResponse message
     */
    Result<void> sendMaterialUnsubscribeResponse(const SessionHandle& handle, bool success,
                                                 const std::string& errorMessage);

    /**
     * @brief Sends GetMaterialRequest message
     */
    Result<void> sendGetMaterialRequest(const SessionHandle& handle, const std::array<uint8_t, 32>& materialId,
                                        uint64_t requestId = 0);

    /**
     * @brief Sends GetMaterialResponse message
     */
    Result<void> sendGetMaterialResponse(const SessionHandle& handle, bool success, const MaterialAssetData& material,
                                         const std::string& errorMessage);

    /**
     * @brief Sends MaterialResolved notification
     */
    Result<void> sendMaterialResolved(const SessionHandle& handle, const std::array<uint8_t, 32>& materialId,
                                      const MaterialAssetData& material);

    /**
     * @brief Sends MeshMaterialBindingRequest
     */
    Result<void> sendMeshMaterialBindingRequest(const SessionHandle& handle, uint64_t entityId,
                                                const std::vector<std::array<uint8_t, 32>>& materialIds,
                                                uint64_t requestId = 0);

    /**
     * @brief Sends MeshMaterialBindingResponse
     */
    Result<void> sendMeshMaterialBindingResponse(const SessionHandle& handle, bool success,
                                                 const std::string& errorMessage, uint64_t requestId = 0);

    /**
     * @brief Sends MeshMaterialBindingUpdate (broadcast)
     */
    Result<void> sendMeshMaterialBindingUpdate(const SessionHandle& handle, uint64_t entityId,
                                               const std::vector<std::array<uint8_t, 32>>& materialIds,
                                               uint64_t originSessionId);

    // =========================================================================
    // Shader Send Methods
    // =========================================================================

    /**
     * @brief Sends GetShaderRequest (client requests shader info)
     */
    Result<void> sendGetShaderRequest(const SessionHandle& handle, const std::array<uint8_t, 32>& shaderAssetId,
                                      uint64_t requestId = 0);

    /**
     * @brief Sends GetShaderResponse (server responds with shader data)
     */
    Result<void> sendGetShaderResponse(const SessionHandle& handle,
                                       const NetworkSession::GetShaderResponseData& response, uint64_t requestId = 0);

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
    size_t capacity() const noexcept {
        return _capacity;
    }

    /**
     * @brief Get schema registry (if configured)
     * @return ComponentSchemaRegistry pointer or nullptr
     */
    ComponentSchemaRegistry* getSchemaRegistry() const noexcept {
        return _schemaRegistry;
    }

    /**
     * @brief Broadcast schema advertisement to all connected sessions
     *
     * Called automatically when a schema is published via registry callback.
     * Sends SchemaAdvertisement message to all sessions with completed handshake.
     *
     * @param typeHash Component type hash
     * @param schema Component schema that was published
     */
    void broadcastSchemaAdvertisement(ComponentTypeHash typeHash, const ComponentSchema& schema);

    /**
     * @brief Broadcast schema unpublish notification to all connected sessions
     *
     * Called automatically when a schema is unpublished via registry callback.
     * Sends UnpublishSchema message to all sessions with completed handshake.
     *
     * @param typeHash Component type hash
     */
    void broadcastSchemaUnpublish(ComponentTypeHash typeHash);

    /**
     * @brief Flush property update batches for all connected sessions
     *
     * Iterates through all active sessions and calls flushPropertyUpdates() on each.
     * This is useful for server applications that need to synchronize state updates
     * to hundreds of connected clients efficiently.
     *
     * Non-blocking: uses try_to_lock to avoid holding up other operations.
     * Continues flushing even if individual sessions fail.
     */
    void flushAllPropertyBatches();

    /**
     * @brief Broadcast material property update to all subscribed sessions except origin
     *
     * Used by servers to fan-out material property changes to all clients that have
     * subscribed to the material, excluding the session that originated the change.
     *
     * @param materialId The material AssetId that was updated
     * @param propertyName Name of the property that changed
     * @param value New property value
     * @param newVersion New material version after this update
     * @param originSessionId Session that made the change (will be excluded from broadcast)
     * @param subscribedSessionIds List of session IDs subscribed to this material
     */
    void broadcastMaterialPropertyUpdate(const std::array<uint8_t, 32>& materialId, const std::string& propertyName,
                                         const PropertyValue& value, uint64_t newVersion, uint64_t originSessionId,
                                         const std::vector<uint64_t>& subscribedSessionIds);

    // EntropyObject interface
    const char* className() const noexcept override {
        return "SessionManager";
    }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    /// Sentinel value for invalid slot index
    static constexpr uint32_t INVALID_INDEX = ~0u;

    /**
     * @brief Internal storage for a session slot
     */
    struct SessionSlot
    {
        std::atomic<uint32_t> generation{1};
        ConnectionHandle connection;              // Stored connection handle
        std::unique_ptr<NetworkSession> session;  // Protocol layer
        std::atomic<uint32_t> nextFree{INVALID_INDEX};
        std::mutex mutex;  // Per-slot mutex for session operations
    };

    ConnectionManager* _connectionManager;     // Not owned
    ComponentSchemaRegistry* _schemaRegistry;  // Not owned, optional
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

}  // namespace EntropyEngine::Networking
