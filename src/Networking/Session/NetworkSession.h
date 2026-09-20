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

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "../Core/ComponentSchemaRegistry.h"
#include "../Core/ErrorCodes.h"
#include "../Core/PermissionTypes.h"
#include "../Core/PropertyRegistry.h"
#include "../Core/SchemaNackPolicy.h"
#include "../Core/SchemaNackTracker.h"
#include "../Protocol/MessageSerializer.h"
#include "../Transport/NetworkConnection.h"

namespace EntropyEngine::Networking
{

/// Compact pose for landmark network messages.
/// Avoids decomposed float parameters that invite transposition bugs.
struct LandmarkPose
{
    float posX = 0, posY = 0, posZ = 0;
    float oriX = 0, oriY = 0, oriZ = 0, oriW = 1;

    /// Construct from glm types (quat component order: w,x,y,z)
    static LandmarkPose fromGlm(const glm::vec3& pos, const glm::quat& ori) {
        return {pos.x, pos.y, pos.z, ori.x, ori.y, ori.z, ori.w};
    }

    [[nodiscard]] glm::vec3 position() const {
        return {posX, posY, posZ};
    }
    [[nodiscard]] glm::quat orientation() const {
        return {oriW, oriX, oriY, oriZ};
    }
};

/**
 * NetworkSession - High-level session that manages a peer connection
 *
 * Wraps a NetworkConnection and provides protocol-level functionality:
 * - Message serialization/deserialization
 * - Automatic routing (reliable vs unreliable channels)
 * - Property registry management
 * - Protocol message callbacks
 */
class NetworkSession : public Core::EntropyObject
{
    friend class SessionManager;  // Allow SessionManager to register callbacks via fan-out
public:
    /**
     * Property registration info passed with EntityCreated callback.
     * Contains all the metadata needed to reconstruct property deltas from hashes.
     */
    struct PropertyRegistrationInfo
    {
        PropertyHash propertyHash;                // 128-bit hash for this property
        uint64_t entityId = 0;                    // Entity this property belongs to
        ComponentTypeHash componentType;          // Component type hash
        std::string propertyName;                 // Property name (e.g., "position")
        PropertyType type = PropertyType::Int32;  // Property type (default to Int32)
    };

    /**
     * Component group data for wire protocol - groups properties under a component.
     */
    struct ComponentGroupData
    {
        ComponentTypeHash typeHash;                // ComponentTypeHash
        std::string componentName;                 // Human-readable name (e.g., "Transform")
        std::vector<PropertyMetadata> properties;  // Properties within this component
    };

    // Message type callbacks
    using EntityCreatedCallback = std::function<void(
        uint64_t entityId, const std::string& appId, const std::string& typeName, uint64_t parentId,
        const std::vector<ComponentGroupData>& components, uint64_t targetSceneId, const std::string& entityName)>;
    using EntityDestroyedCallback = std::function<void(uint64_t entityId)>;
    using PropertyUpdateCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using SceneSnapshotCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using HandshakeCallback = std::function<void(const std::string& clientType, const std::string& clientId)>;
    using ErrorCallback = std::function<void(NetworkError error, const std::string& message)>;
    using HeartbeatCallback = std::function<void(uint64_t timestamp)>;
    using HeartbeatResponseCallback = std::function<void(uint64_t clientTimestamp, uint64_t serverTime)>;
    using DisconnectCallback = std::function<void(ConnectionState state, const std::string& reason)>;

    // Schema message callbacks
    using RegisterSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using QueryPublicSchemasResponseCallback = std::function<void(const std::vector<ComponentSchema>& schemas)>;
    using PublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using UnpublishSchemaResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using SchemaNackCallback =
        std::function<void(ComponentTypeHash typeHash, const std::string& reason, uint64_t timestamp)>;
    using SchemaAdvertisementCallback = std::function<void(ComponentTypeHash typeHash, const std::string& appId,
                                                           const std::string& componentName, uint32_t schemaVersion)>;

    // =========================================================================
    // Asset Metadata Data Structures
    // =========================================================================

    // Shader parameter definition for metadata
    struct ShaderParameterDefData
    {
        std::string name;
        std::string displayName;
        PropertyType type = PropertyType::Float32;
        std::optional<PropertyValue> defaultValue;
        std::map<std::string, std::string> attributes;  // key -> value
    };

    // Shader metadata
    struct ShaderMetadataData
    {
        std::string name;
        std::string description;
        std::vector<std::string> keywords;
        std::vector<ShaderParameterDefData> parameters;
        int32_t renderQueue = 2000;
        bool castsShadows = true;
        bool transparent = false;
        std::string author;
    };

    // Texture metadata (mirrors TextureMetadata in SDK)
    struct TextureMetadataData
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        uint8_t textureType = 1;  // TextureType enum
        uint8_t format = 3;       // TextureFormat enum (RGBA8 = 3)
        uint8_t colorSpace = 1;   // ColorSpace enum (sRGB = 1)
        bool generateMips = false;
        std::string sourceFile;
    };

    // Asset metadata variant - supports shader and texture metadata
    enum class AssetMetadataType : uint8_t
    {
        None = 0,
        Shader = 1,
        Texture = 2
    };

    struct AssetMetadataData
    {
        AssetMetadataType type = AssetMetadataType::None;
        std::optional<ShaderMetadataData> shaderMetadata;
        std::optional<TextureMetadataData> textureMetadata;
    };

    // Asset entry data structure (mirrors AssetEntry in entropy.capnp)
    struct AssetEntryData
    {
        std::array<uint8_t, 32> id{};
        std::string uri;
        uint8_t contentType = 0;
        uint64_t sizeBytes = 0;
        bool encrypted = false;
        std::array<uint8_t, 32> plaintextHash{};
        std::string appId;
        bool persistent = false;
        AssetMetadataData metadata;  // Type-specific metadata
    };

    // Asset resolve response data structure
    struct AssetResolveResponseData
    {
        bool found = false;
        AssetEntryData entry;
        bool hasKey = false;
        std::array<uint8_t, 32> key{};
        uint8_t deliveryMethod = 0;
        uint64_t requestId = 0;  // For response correlation
    };

    // Asset message callbacks (requestId added for correlation)
    using AssetAdvertiseCallback =
        std::function<void(const std::string& appId, const std::vector<AssetEntryData>& entries, uint64_t requestId)>;
    using AssetAdvertiseResponseCallback =
        std::function<void(bool success, const std::string& errorMessage, uint64_t requestId)>;
    using AssetWithdrawCallback =
        std::function<void(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId)>;
    using AssetWithdrawResponseCallback =
        std::function<void(bool success, uint32_t removedCount, const std::string& errorMessage, uint64_t requestId)>;
    using AssetWithdrawAllCallback = std::function<void(const std::string& appId, uint64_t requestId)>;
    using AssetWithdrawAllResponseCallback =
        std::function<void(bool success, uint32_t removedCount, const std::string& errorMessage, uint64_t requestId)>;
    using AssetResolveCallback = std::function<void(const std::array<uint8_t, 32>& assetId, uint64_t requestId)>;
    using AssetResolveResponseCallback =
        std::function<void(const AssetResolveResponseData& response)>;  // requestId in data struct
    using AssetResolveBatchCallback =
        std::function<void(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId)>;
    using AssetResolveBatchResponseCallback =
        std::function<void(const std::vector<AssetResolveResponseData>& responses, uint64_t requestId)>;
    using AssetProvideKeyCallback = std::function<void(const std::array<uint8_t, 32>& assetId,
                                                       const std::array<uint8_t, 32>& key, uint64_t requestId)>;
    using AssetProvideKeyResponseCallback =
        std::function<void(bool success, const std::string& errorMessage, uint64_t requestId)>;
    using AssetUploadCallback =
        std::function<void(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                           bool persistent, uint64_t requestId, const AssetMetadataData& metadata)>;
    using AssetUploadResponseCallback =
        std::function<void(bool success, const std::array<uint8_t, 32>& assetId, const std::string& uri,
                           const std::string& errorMessage, uint64_t requestId)>;
    using AssetFetchCallback = std::function<void(const std::array<uint8_t, 32>& assetId, uint64_t requestId)>;
    using AssetFetchResponseCallback = std::function<void(bool found, const std::vector<uint8_t>& data,
                                                          const std::string& errorMessage, uint64_t requestId)>;

    // Chunked download callbacks
    using AssetFetchBeginCallback = std::function<void(uint64_t requestId, uint64_t totalSize, uint32_t chunkCount)>;
    using AssetFetchChunkCallback =
        std::function<void(uint64_t requestId, uint32_t sequence, const std::vector<uint8_t>& data)>;

    // Asset metadata callbacks
    using AssetMetadataCallback = std::function<void(const std::array<uint8_t, 32>& assetId, uint64_t requestId)>;
    using AssetMetadataResponseCallback =
        std::function<void(bool found, const AssetMetadataData& metadata, uint64_t requestId)>;

    // Chunked upload data structures
    struct AssetUploadBeginData
    {
        std::string appId;
        uint64_t totalSize = 0;
        uint8_t contentType = 0;
        bool persistent = false;
        uint32_t chunkSize = 0;
        bool encrypted = false;
        std::array<uint8_t, 32> plaintextHash{};
        uint64_t requestId = 0;      // For response correlation
        AssetMetadataData metadata;  // Type-specific metadata
    };

    struct AssetUploadBeginResponseData
    {
        bool success = false;
        std::array<uint8_t, 16> uploadId{};
        uint32_t chunkSize = 0;
        std::string errorMessage;
        uint64_t requestId = 0;  // Echo back for correlation
    };

    struct AssetUploadChunkData
    {
        std::array<uint8_t, 16> uploadId{};
        uint64_t offset = 0;
        std::vector<uint8_t> data;
        uint32_t sequence = 0;
    };

    struct AssetUploadChunkResponseData
    {
        bool success = false;
        std::array<uint8_t, 16> uploadId{};
        uint64_t bytesReceived = 0;
        std::string errorMessage;
    };

    struct AssetUploadCompleteData
    {
        std::array<uint8_t, 16> uploadId{};
        uint32_t totalChunks = 0;
    };

    struct AssetUploadCompleteResponseData
    {
        bool success = false;
        std::array<uint8_t, 32> assetId{};
        std::string uri;
        uint64_t bytesStored = 0;
        std::string errorMessage;
        std::array<uint8_t, 16> uploadId{};  // For correlation with pending uploads
    };

    // Chunked upload callbacks
    using AssetUploadBeginCallback = std::function<void(const AssetUploadBeginData& data)>;
    using AssetUploadBeginResponseCallback = std::function<void(const AssetUploadBeginResponseData& data)>;
    using AssetUploadChunkCallback = std::function<void(const AssetUploadChunkData& data)>;
    using AssetUploadChunkResponseCallback = std::function<void(const AssetUploadChunkResponseData& data)>;
    using AssetUploadCompleteCallback = std::function<void(const AssetUploadCompleteData& data)>;
    using AssetUploadCompleteResponseCallback = std::function<void(const AssetUploadCompleteResponseData& data)>;
    using AssetUploadCancelCallback = std::function<void(const std::array<uint8_t, 16>& uploadId)>;
    using AssetUploadCancelResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;

    // Scene management callbacks
    using CreateSceneCallback = std::function<void(const std::string& sceneName, bool transient)>;
    using CreateSceneResponseCallback =
        std::function<void(bool success, uint64_t sceneId, const std::string& errorMessage)>;
    using DestroySceneCallback = std::function<void(uint64_t sceneId)>;
    using DestroySceneResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using SetSceneEnabledCallback = std::function<void(uint64_t sceneId, bool enabled)>;
    using SetSceneEnabledResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;
    using AddEntityToSceneCallback = std::function<void(uint64_t entityId, uint64_t sceneId)>;
    using AddEntityToSceneResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;

    // Material system callbacks
    struct MaterialPropertyData
    {
        std::string name;
        PropertyValue value;
    };

    struct SamplerOverrideData
    {
        std::string slotName;
        std::array<uint8_t, 32> samplerAssetId{};
    };

    /// Blend-state mirror carried over the wire with MaterialAssetData.
    /// Factor codes match MTLBlendFactor / VkBlendFactor values; op codes:
    /// 0=Add, 1=Subtract, 2=ReverseSubtract, 3=Min, 4=Max.
    struct BlendStateData
    {
        bool enabled = false;
        uint32_t srcColor = 1;  // One
        uint32_t dstColor = 0;  // Zero
        uint32_t srcAlpha = 1;  // One
        uint32_t dstAlpha = 0;  // Zero
        uint32_t colorOp = 0;   // Add
        uint32_t alphaOp = 0;   // Add
    };

    struct MaterialAssetData
    {
        std::string name;
        std::array<uint8_t, 32> shaderAssetId{};
        std::vector<MaterialPropertyData> properties;
        int32_t renderQueue = 2000;
        bool castsShadows = true;
        bool receivesShadows = true;
        bool depthWrite = true;
        uint64_t creatorSessionId = 0;
        uint64_t version = 0;
        uint64_t modifiedAt = 0;
        std::string appId;
        std::vector<std::string> enabledKeywords;
        std::vector<SamplerOverrideData> samplerOverrides;
        bool useOIT = false;
        BlendStateData blendState;
    };

    using CreateMaterialCallback = std::function<void(const MaterialAssetData& material, uint64_t requestId)>;
    using CreateMaterialResponseCallback = std::function<void(bool success, const std::array<uint8_t, 32>& materialId,
                                                              const std::string& errorMessage, uint64_t requestId)>;

    using UpdateMaterialPropertyCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId, const std::string& propertyName,
                           const PropertyValue& value, uint64_t requestId)>;
    using UpdateMaterialPropertyResponseCallback =
        std::function<void(bool success, uint64_t newVersion, const std::string& errorMessage)>;

    using UpdateMaterialPropertiesBatchCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId,
                           const std::vector<MaterialPropertyData>& properties, uint64_t requestId)>;
    using UpdateMaterialPropertiesBatchResponseCallback =
        std::function<void(bool success, uint64_t newVersion, const std::string& errorMessage)>;

    using MaterialPropertyUpdateCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId, const std::string& propertyName,
                           const PropertyValue& value, uint64_t newVersion, uint64_t originSessionId)>;

    using MaterialSubscribeCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId, uint64_t requestId)>;
    using MaterialSubscribeResponseCallback = std::function<void(bool success, const MaterialAssetData& material,
                                                                 const std::string& errorMessage, uint64_t requestId)>;

    using MaterialUnsubscribeCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId, uint64_t requestId)>;
    using MaterialUnsubscribeResponseCallback = std::function<void(bool success, const std::string& errorMessage)>;

    using GetMaterialCallback = std::function<void(const std::array<uint8_t, 32>& materialId, uint64_t requestId)>;
    using GetMaterialResponseCallback =
        std::function<void(bool success, const MaterialAssetData& material, const std::string& errorMessage)>;

    using MaterialResolvedCallback =
        std::function<void(const std::array<uint8_t, 32>& materialId, const MaterialAssetData& material)>;

    // Mesh-material binding callbacks
    using MeshMaterialBindingCallback = std::function<void(
        uint64_t entityId, const std::vector<std::array<uint8_t, 32>>& materialIds, uint64_t requestId)>;
    using MeshMaterialBindingResponseCallback =
        std::function<void(bool success, const std::string& errorMessage, uint64_t requestId)>;
    using MeshMaterialBindingUpdateCallback = std::function<void(
        uint64_t entityId, const std::vector<std::array<uint8_t, 32>>& materialIds, uint64_t originSessionId)>;

    // =========================================================================
    // Shader Protocol Types
    // =========================================================================

    /**
     * @brief Shader module for network transfer
     */
    struct ShaderModuleData
    {
        std::string moduleName;
        std::string source;
    };

    /**
     * @brief Get shader response data
     *
     * Uses ShaderMetadataData (defined above) for native typed metadata.
     */
    struct GetShaderResponseData
    {
        bool found = false;
        bool isBuiltin = false;
        std::string mainSource;
        std::vector<ShaderModuleData> modules;
        ShaderMetadataData metadata;  // Uses native PropertyType-based parameters
    };

    // Get shader callbacks
    using GetShaderCallback = std::function<void(const std::array<uint8_t, 32>& shaderAssetId, uint64_t requestId)>;
    using GetShaderResponseCallback = std::function<void(const GetShaderResponseData& response, uint64_t requestId)>;

    // =========================================================================
    // Per-Permission Response Callbacks
    // =========================================================================

    // Permission system callbacks
    using PermissionRequestCallback =
        std::function<void(uint64_t requestId, uint64_t requestingSessionId, const std::string& appId,
                           PermissionKey key, const std::string& reason)>;

    using HeadPosePermissionResponseCallback = std::function<void(uint64_t requestId, uint64_t respondingSessionId,
                                                                  bool granted, uint64_t grantToken, bool isNewGrant)>;

    using IdentityPermissionResponseCallback =
        std::function<void(uint64_t requestId, uint64_t respondingSessionId, bool granted, uint64_t grantToken,
                           bool isNewGrant, const std::string& identityHash)>;

    using UsernamePermissionResponseCallback =
        std::function<void(uint64_t requestId, uint64_t respondingSessionId, bool granted, uint64_t grantToken,
                           bool isNewGrant, const std::string& username)>;

    using HostnamePermissionResponseCallback =
        std::function<void(uint64_t requestId, uint64_t respondingSessionId, bool granted, uint64_t grantToken,
                           bool isNewGrant, const std::string& hostname)>;

    using PermissionRevokedCallback = std::function<void(uint64_t portalSessionId, PermissionKey key)>;

    using PermissionRequestCancelledCallback = std::function<void(uint64_t requestId, PermissionKey key)>;

    // Spatial anchoring callbacks
    using RegisterLandmarkRequestCallback =
        std::function<void(uint64_t requestId, const std::vector<uint8_t>& definitionMsgData)>;
    using RegisterLandmarkResponseCallback =
        std::function<void(uint64_t requestId, bool success, uint64_t landmarkId, const std::string& errorMessage)>;
    using LandmarkObservationMsgCallback =
        std::function<void(uint64_t landmarkId, uint64_t observerSessionId, bool detected, const LandmarkPose& pose,
                           float confidence, uint64_t timestamp)>;
    using LandmarkStateUpdateMsgCallback =
        std::function<void(uint64_t landmarkId, uint8_t state, const LandmarkPose& pose, uint16_t observerCount)>;
    using UnregisterLandmarkRequestCallback = std::function<void(uint64_t requestId, uint64_t landmarkId)>;
    using UnregisterLandmarkResponseCallback =
        std::function<void(uint64_t requestId, bool success, const std::string& errorMessage)>;
    using LandmarkSnapshotMsgCallback = std::function<void(const std::vector<uint8_t>& snapshotData)>;

    /**
     * @brief Construct a NetworkSession
     * @param connection Network connection to wrap
     * @param externalRegistry Optional external PropertyRegistry to share across sessions.
     *                        If nullptr, creates an internal registry (for single-session use).
     * @param schemaRegistry Optional ComponentSchemaRegistry for schema operations.
     *                      If nullptr, schema operations will not be available.
     */
    NetworkSession(NetworkConnection* connection, PropertyRegistry* externalRegistry = nullptr,
                   ComponentSchemaRegistry* schemaRegistry = nullptr);
    ~NetworkSession() override;

    // Connection management
    Result<void> connect();
    Result<void> disconnect();
    bool isConnected() const;
    ConnectionState getState() const;

    // Multi-channel support (for WebRTC bulk data isolation)
    bool supportsMultipleChannels() const;
    Result<void> openChannel(const std::string& channel);

    /**
     * @brief Set up connection callbacks to route messages to this session
     *
     * This method registers this session's message and state callbacks with the
     * underlying connection. SessionManager calls this automatically, but direct
     * users (like tests) must call it manually after construction.
     *
     * @note Must be called before any messages will be received
     */
    void setupCallbacks();

    // Handshake
    Result<void> performHandshake(const std::string& clientType, const std::string& clientId);
    bool isHandshakeComplete() const {
        return _handshakeComplete;
    }

    // Diagnostics
    const std::string& getSessionId() const noexcept {
        return _sessionId;
    }

    // Send protocol messages
    Result<void> sendEntityCreated(uint64_t entityId, const std::string& appId, const std::string& typeName,
                                   uint64_t parentId, const std::vector<ComponentGroupData>& components = {},
                                   uint64_t targetSceneId = 0, const std::string& entityName = "");
    Result<void> sendEntityDestroyed(uint64_t entityId);
    Result<void> sendComponentAdded(uint64_t entityId, const ComponentGroupData& component);
    Result<void> sendComponentRemoved(uint64_t entityId, ComponentTypeHash typeHash);
    Result<void> sendPropertyUpdate(PropertyHash hash, PropertyType type, const PropertyValue& value);
    Result<void> sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData);
    Result<void> sendSceneSnapshot(const std::vector<uint8_t>& snapshotData);

    // =========================================================================
    // ECS-Style Entity API
    // =========================================================================
    //
    // Clean, compositional API for creating entities with components:
    //
    //   auto entity = session.createEntity("Mesh");
    //   entity.attach(transformSchema)
    //         .set("position", Vec3{0, 1, 0})
    //         .set("rotation", Quat{1, 0, 0, 0})
    //         .set("scale", Vec3{1, 1, 1});
    //   entity.sync();
    //
    // Or fluent one-liner:
    //   session.createEntity("Light")
    //          .attach(transformSchema).set("position", Vec3{0, 5, 0})
    //          .sync();

    class EntityBuilder;

    /**
     * @brief Component attachment on an entity, allows setting property values
     */
    class ComponentHandle
    {
    public:
        ComponentHandle(EntityBuilder& entity, const ComponentSchema& schema);

        /**
         * @brief Set a property value on this component
         * @param propertyName Name of the property (must exist in schema)
         * @param value Property value (type must match schema)
         * @return Reference to this handle for chaining
         */
        template <typename T>
        ComponentHandle& set(const std::string& propertyName, const T& value);

        /**
         * @brief Return to entity for attaching more components
         */
        EntityBuilder& done() {
            return _entity;
        }

    private:
        EntityBuilder& _entity;
        const ComponentSchema& _schema;
    };

    /**
     * @brief Builder for creating entities with components in ECS style
     */
    class EntityBuilder
    {
        friend class NetworkSession;
        friend class ComponentHandle;

    public:
        /**
         * @brief Attach a component schema to this entity
         * @param schema Component schema to attach
         * @return ComponentHandle for setting property values
         */
        ComponentHandle attach(const ComponentSchema& schema);

        /**
         * @brief Get the entity ID
         */
        uint64_t id() const {
            return _entityId;
        }

        /**
         * @brief Send entity creation and all pending property updates
         * @return Result indicating success/failure
         */
        Result<void> sync();

    private:
        EntityBuilder(NetworkSession& session, uint64_t entityId, const std::string& appId, const std::string& typeName,
                      uint64_t parentId);

        NetworkSession& _session;
        uint64_t _entityId;
        std::string _appId;
        std::string _typeName;
        uint64_t _parentId;
        std::vector<PropertyMetadata> _properties;
        std::vector<std::tuple<PropertyHash, PropertyType, PropertyValue>> _pendingUpdates;
        bool _synced = false;
    };

    /**
     * @brief Create a new entity with ECS-style API
     * @param typeName Entity type name (e.g., "Mesh", "Light", "Camera")
     * @param appId Application ID (default: "CanvasEngine")
     * @param parentId Parent entity ID (default: 0 = root)
     * @return EntityBuilder for attaching components and setting properties
     */
    EntityBuilder createEntity(const std::string& typeName, const std::string& appId = "CanvasEngine",
                               uint64_t parentId = 0);

    /**
     * @brief Get the next available entity ID
     * @return Unique entity ID for this session
     */
    uint64_t nextEntityId() {
        return _nextEntityId++;
    }

    // Heartbeat protocol
    Result<void> sendHeartbeat();
    Result<void> sendHeartbeatResponse(uint64_t clientTimestamp);
    std::chrono::steady_clock::time_point getLastHeartbeatReceived() const;

    // Send schema protocol messages
    Result<void> sendRegisterSchema(const ComponentSchema& schema);
    Result<void> sendQueryPublicSchemas();
    Result<void> sendPublishSchema(ComponentTypeHash typeHash);
    Result<void> sendUnpublishSchema(ComponentTypeHash typeHash);

    /**
     * @brief Send NACK for unknown schema (optional feedback)
     *
     * Sends a SchemaNack message to notify the peer about an unknown ComponentTypeHash.
     * This is OPTIONAL feedback controlled by SchemaNackPolicy:
     * - Only sent when SchemaNackPolicy::instance().isEnabled() == true
     * - Subject to per-schema rate limiting via SchemaNackTracker (default: 1000ms interval)
     * - Uses non-blocking reliable send path (MPSC queue)
     *
     * Typical usage: Called automatically by handleUnknownSchema() when processing
     * ENTITY_CREATED messages with unknown ComponentTypeHash values.
     *
     * @param typeHash Unknown ComponentTypeHash that triggered the NACK
     * @param reason Human-readable reason (e.g., "Schema not found in registry")
     * @return Result indicating success or error (Ok if policy disabled or rate limited)
     */
    Result<void> sendSchemaNack(ComponentTypeHash typeHash, const std::string& reason);

    Result<void> sendSchemaAdvertisement(ComponentTypeHash typeHash, const std::string& appId,
                                         const std::string& componentName, uint32_t schemaVersion);

    // Send asset protocol messages (requestId for correlation)
    Result<void> sendAssetAdvertise(const std::string& appId, const std::vector<AssetEntryData>& entries,
                                    uint64_t requestId = 0);
    Result<void> sendAssetAdvertiseResponse(bool success, const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendAssetWithdraw(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId = 0);
    Result<void> sendAssetWithdrawResponse(bool success, uint32_t removedCount, const std::string& errorMessage,
                                           uint64_t requestId = 0);
    Result<void> sendAssetWithdrawAll(const std::string& appId, uint64_t requestId = 0);
    Result<void> sendAssetWithdrawAllResponse(bool success, uint32_t removedCount, const std::string& errorMessage,
                                              uint64_t requestId = 0);
    Result<void> sendAssetResolve(const std::array<uint8_t, 32>& assetId, uint64_t requestId = 0);
    Result<void> sendAssetResolveResponse(const AssetResolveResponseData& response);  // requestId in struct
    Result<void> sendAssetResolveBatch(const std::vector<std::array<uint8_t, 32>>& assetIds, uint64_t requestId = 0);
    Result<void> sendAssetResolveBatchResponse(const std::vector<AssetResolveResponseData>& responses,
                                               uint64_t requestId = 0);
    Result<void> sendAssetProvideKey(const std::array<uint8_t, 32>& assetId, const std::array<uint8_t, 32>& key,
                                     uint64_t requestId = 0);
    Result<void> sendAssetProvideKeyResponse(bool success, const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                                 bool persistent, uint64_t requestId = 0);
    Result<void> sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data, uint8_t contentType,
                                 bool persistent, uint64_t requestId, const AssetMetadataData& metadata);
    Result<void> sendAssetUploadResponse(bool success, const std::array<uint8_t, 32>& assetId, const std::string& uri,
                                         const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendAssetFetch(const std::array<uint8_t, 32>& assetId, uint64_t requestId = 0);
    Result<void> sendAssetFetchResponse(bool found, const std::vector<uint8_t>& data, const std::string& errorMessage,
                                        uint64_t requestId = 0);

    // Chunked download send methods
    Result<void> sendAssetFetchBegin(uint64_t requestId, uint64_t totalSize, uint32_t chunkCount, uint8_t contentType);
    Result<void> sendAssetFetchChunk(uint64_t requestId, uint32_t sequence, const std::vector<uint8_t>& data);

    // Chunked upload send methods
    Result<void> sendAssetUploadBegin(const AssetUploadBeginData& data);
    Result<void> sendAssetUploadBeginResponse(const AssetUploadBeginResponseData& data);
    Result<void> sendAssetUploadChunk(const AssetUploadChunkData& data);
    Result<void> sendAssetUploadChunkResponse(const AssetUploadChunkResponseData& data);
    Result<void> sendAssetUploadComplete(const AssetUploadCompleteData& data);
    Result<void> sendAssetUploadCompleteResponse(const AssetUploadCompleteResponseData& data);
    Result<void> sendAssetUploadCancel(const std::array<uint8_t, 16>& uploadId);
    Result<void> sendAssetUploadCancelResponse(bool success, const std::string& errorMessage);

    // Scene management messages
    Result<void> sendCreateSceneRequest(const std::string& sceneName, bool transient);
    Result<void> sendCreateSceneResponse(bool success, uint64_t sceneId, const std::string& errorMessage);
    Result<void> sendDestroySceneRequest(uint64_t sceneId);
    Result<void> sendDestroySceneResponse(bool success, const std::string& errorMessage);
    Result<void> sendSetSceneEnabledRequest(uint64_t sceneId, bool enabled);
    Result<void> sendSetSceneEnabledResponse(bool success, const std::string& errorMessage);
    Result<void> sendAddEntityToSceneRequest(uint64_t entityId, uint64_t sceneId);
    Result<void> sendAddEntityToSceneResponse(bool success, const std::string& errorMessage);

    // Material system messages
    Result<void> sendCreateMaterialRequest(const MaterialAssetData& material, uint64_t requestId = 0);
    Result<void> sendCreateMaterialResponse(bool success, const std::array<uint8_t, 32>& materialId,
                                            const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendUpdateMaterialPropertyRequest(const std::array<uint8_t, 32>& materialId,
                                                   const std::string& propertyName, const PropertyValue& value,
                                                   uint64_t requestId = 0);
    Result<void> sendUpdateMaterialPropertyResponse(bool success, uint64_t newVersion, const std::string& errorMessage);
    Result<void> sendUpdateMaterialPropertiesBatchRequest(const std::array<uint8_t, 32>& materialId,
                                                          const std::vector<MaterialPropertyData>& properties,
                                                          uint64_t requestId = 0);
    Result<void> sendUpdateMaterialPropertiesBatchResponse(bool success, uint64_t newVersion,
                                                           const std::string& errorMessage);
    Result<void> sendMaterialPropertyUpdate(const std::array<uint8_t, 32>& materialId, const std::string& propertyName,
                                            const PropertyValue& value, uint64_t newVersion, uint64_t originSessionId);
    Result<void> sendMaterialSubscribeRequest(const std::array<uint8_t, 32>& materialId, uint64_t requestId = 0);
    Result<void> sendMaterialSubscribeResponse(bool success, const MaterialAssetData& material,
                                               const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendMaterialUnsubscribeRequest(const std::array<uint8_t, 32>& materialId, uint64_t requestId = 0);
    Result<void> sendMaterialUnsubscribeResponse(bool success, const std::string& errorMessage);
    Result<void> sendGetMaterialRequest(const std::array<uint8_t, 32>& materialId, uint64_t requestId = 0);
    Result<void> sendGetMaterialResponse(bool success, const MaterialAssetData& material,
                                         const std::string& errorMessage);
    Result<void> sendMaterialResolved(const std::array<uint8_t, 32>& materialId, const MaterialAssetData& material);

    // Mesh-material binding messages
    Result<void> sendMeshMaterialBindingRequest(uint64_t entityId,
                                                const std::vector<std::array<uint8_t, 32>>& materialIds,
                                                uint64_t requestId = 0);
    Result<void> sendMeshMaterialBindingResponse(bool success, const std::string& errorMessage, uint64_t requestId = 0);
    Result<void> sendMeshMaterialBindingUpdate(uint64_t entityId,
                                               const std::vector<std::array<uint8_t, 32>>& materialIds,
                                               uint64_t originSessionId);

    // Shader messages
    Result<void> sendGetShaderRequest(const std::array<uint8_t, 32>& shaderAssetId, uint64_t requestId = 0);
    Result<void> sendGetShaderResponse(const GetShaderResponseData& response, uint64_t requestId = 0);

    // Permission system messages
    Result<void> sendPermissionRequest(uint64_t requestId, uint64_t requestingSessionId, const std::string& appId,
                                       PermissionKey key, const std::string& reason);

    Result<void> sendHeadPosePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false);

    Result<void> sendIdentityPermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& identityHash = "");

    Result<void> sendUsernamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& username = "");

    Result<void> sendHostnamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId, bool granted,
                                                uint64_t grantToken = 0, bool isNewGrant = false,
                                                const std::string& hostname = "");

    Result<void> sendPermissionRevoked(uint64_t portalSessionId, PermissionKey key);

    Result<void> sendPermissionRequestCancelled(uint64_t requestId, PermissionKey key);

    // Spatial anchoring messages
    Result<void> sendRegisterLandmarkRequest(uint64_t requestId, const std::vector<uint8_t>& definitionMsgData);
    Result<void> sendRegisterLandmarkResponse(uint64_t requestId, bool success, uint64_t landmarkId,
                                              const std::string& errorMessage = "");
    Result<void> sendLandmarkObservation(uint64_t landmarkId, uint64_t observerSessionId, bool detected,
                                         const LandmarkPose& pose, float confidence, uint64_t timestamp);
    Result<void> sendLandmarkStateUpdate(uint64_t landmarkId, uint8_t state, const LandmarkPose& pose,
                                         uint16_t observerCount);
    Result<void> sendUnregisterLandmarkRequest(uint64_t requestId, uint64_t landmarkId);
    Result<void> sendUnregisterLandmarkResponse(uint64_t requestId, bool success, const std::string& errorMessage = "");
    Result<void> sendLandmarkSnapshot(const std::vector<uint8_t>& snapshotData);

    // Message callbacks
    void setEntityCreatedCallback(EntityCreatedCallback callback);
    void setEntityDestroyedCallback(EntityDestroyedCallback callback);
    void setPropertyUpdateCallback(PropertyUpdateCallback callback);
    void setSceneSnapshotCallback(SceneSnapshotCallback callback);
    void setHandshakeCallback(HandshakeCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setHeartbeatCallback(HeartbeatCallback callback);
    void setHeartbeatResponseCallback(HeartbeatResponseCallback callback);
    void setDisconnectCallback(DisconnectCallback callback);

    // Schema message callbacks
    void setRegisterSchemaResponseCallback(RegisterSchemaResponseCallback callback);
    void setQueryPublicSchemasResponseCallback(QueryPublicSchemasResponseCallback callback);
    void setPublishSchemaResponseCallback(PublishSchemaResponseCallback callback);
    void setUnpublishSchemaResponseCallback(UnpublishSchemaResponseCallback callback);
    void setSchemaNackCallback(SchemaNackCallback callback);
    void setSchemaAdvertisementCallback(SchemaAdvertisementCallback callback);

    // Asset message callbacks
    void setAssetAdvertiseCallback(AssetAdvertiseCallback callback);
    void setAssetAdvertiseResponseCallback(AssetAdvertiseResponseCallback callback);
    void setAssetWithdrawCallback(AssetWithdrawCallback callback);
    void setAssetWithdrawResponseCallback(AssetWithdrawResponseCallback callback);
    void setAssetWithdrawAllCallback(AssetWithdrawAllCallback callback);
    void setAssetWithdrawAllResponseCallback(AssetWithdrawAllResponseCallback callback);
    void setAssetResolveCallback(AssetResolveCallback callback);
    void setAssetResolveResponseCallback(AssetResolveResponseCallback callback);
    void setAssetResolveBatchCallback(AssetResolveBatchCallback callback);
    void setAssetResolveBatchResponseCallback(AssetResolveBatchResponseCallback callback);
    void setAssetProvideKeyCallback(AssetProvideKeyCallback callback);
    void setAssetProvideKeyResponseCallback(AssetProvideKeyResponseCallback callback);
    void setAssetUploadCallback(AssetUploadCallback callback);
    void setAssetUploadResponseCallback(AssetUploadResponseCallback callback);
    void setAssetFetchCallback(AssetFetchCallback callback);
    void setAssetFetchResponseCallback(AssetFetchResponseCallback callback);
    void setAssetFetchBeginCallback(AssetFetchBeginCallback callback);
    void setAssetFetchChunkCallback(AssetFetchChunkCallback callback);

    // Asset metadata callbacks
    void setAssetMetadataCallback(AssetMetadataCallback callback);
    void setAssetMetadataResponseCallback(AssetMetadataResponseCallback callback);

    // Chunked upload callbacks
    void setAssetUploadBeginCallback(AssetUploadBeginCallback callback);
    void setAssetUploadBeginResponseCallback(AssetUploadBeginResponseCallback callback);
    void setAssetUploadChunkCallback(AssetUploadChunkCallback callback);
    void setAssetUploadChunkResponseCallback(AssetUploadChunkResponseCallback callback);
    void setAssetUploadCompleteCallback(AssetUploadCompleteCallback callback);
    void setAssetUploadCompleteResponseCallback(AssetUploadCompleteResponseCallback callback);
    void setAssetUploadCancelCallback(AssetUploadCancelCallback callback);
    void setAssetUploadCancelResponseCallback(AssetUploadCancelResponseCallback callback);

    // Scene management callbacks
    void setCreateSceneCallback(CreateSceneCallback callback);
    void setCreateSceneResponseCallback(CreateSceneResponseCallback callback);
    void setDestroySceneCallback(DestroySceneCallback callback);
    void setDestroySceneResponseCallback(DestroySceneResponseCallback callback);
    void setSetSceneEnabledCallback(SetSceneEnabledCallback callback);
    void setSetSceneEnabledResponseCallback(SetSceneEnabledResponseCallback callback);
    void setAddEntityToSceneCallback(AddEntityToSceneCallback callback);
    void setAddEntityToSceneResponseCallback(AddEntityToSceneResponseCallback callback);

    // Material system callbacks
    void setCreateMaterialCallback(CreateMaterialCallback callback);
    void setCreateMaterialResponseCallback(CreateMaterialResponseCallback callback);
    void setUpdateMaterialPropertyCallback(UpdateMaterialPropertyCallback callback);
    void setUpdateMaterialPropertyResponseCallback(UpdateMaterialPropertyResponseCallback callback);
    void setUpdateMaterialPropertiesBatchCallback(UpdateMaterialPropertiesBatchCallback callback);
    void setUpdateMaterialPropertiesBatchResponseCallback(UpdateMaterialPropertiesBatchResponseCallback callback);
    void setMaterialPropertyUpdateCallback(MaterialPropertyUpdateCallback callback);
    void setMaterialSubscribeCallback(MaterialSubscribeCallback callback);
    void setMaterialSubscribeResponseCallback(MaterialSubscribeResponseCallback callback);
    void setMaterialUnsubscribeCallback(MaterialUnsubscribeCallback callback);
    void setMaterialUnsubscribeResponseCallback(MaterialUnsubscribeResponseCallback callback);
    void setGetMaterialCallback(GetMaterialCallback callback);
    void setGetMaterialResponseCallback(GetMaterialResponseCallback callback);
    void setMaterialResolvedCallback(MaterialResolvedCallback callback);

    // Mesh-material binding callbacks
    void setMeshMaterialBindingCallback(MeshMaterialBindingCallback callback);
    void setMeshMaterialBindingResponseCallback(MeshMaterialBindingResponseCallback callback);
    void setMeshMaterialBindingUpdateCallback(MeshMaterialBindingUpdateCallback callback);

    // Shader callbacks
    void setGetShaderCallback(GetShaderCallback callback);
    void setGetShaderResponseCallback(GetShaderResponseCallback callback);

    // Permission system callbacks
    void setPermissionRequestCallback(PermissionRequestCallback callback);
    void setHeadPosePermissionResponseCallback(HeadPosePermissionResponseCallback callback);
    void setIdentityPermissionResponseCallback(IdentityPermissionResponseCallback callback);
    void setUsernamePermissionResponseCallback(UsernamePermissionResponseCallback callback);
    void setHostnamePermissionResponseCallback(HostnamePermissionResponseCallback callback);
    void setPermissionRevokedCallback(PermissionRevokedCallback callback);
    void setPermissionRequestCancelledCallback(PermissionRequestCancelledCallback callback);

    // Spatial anchoring callbacks
    void setRegisterLandmarkRequestCallback(RegisterLandmarkRequestCallback callback);
    void setRegisterLandmarkResponseCallback(RegisterLandmarkResponseCallback callback);
    void setLandmarkObservationCallback(LandmarkObservationMsgCallback callback);
    void setLandmarkStateUpdateCallback(LandmarkStateUpdateMsgCallback callback);
    void setUnregisterLandmarkRequestCallback(UnregisterLandmarkRequestCallback callback);
    void setUnregisterLandmarkResponseCallback(UnregisterLandmarkResponseCallback callback);
    void setLandmarkSnapshotCallback(LandmarkSnapshotMsgCallback callback);

    /**
     * @brief Clears all callbacks to prevent invocation during/after destruction
     *
     * Called by SessionManager::destroySession before returning slot to free list.
     */
    void clearCallbacks();

    // Property registry access (always valid after construction)
    PropertyRegistry& getPropertyRegistry() {
        return *_propertyRegistry;
    }
    const PropertyRegistry& getPropertyRegistry() const {
        return *_propertyRegistry;
    }

    // Schema registry access (may be nullptr if not configured)
    ComponentSchemaRegistry* getSchemaRegistry() {
        return _schemaRegistry;
    }
    const ComponentSchemaRegistry* getSchemaRegistry() const {
        return _schemaRegistry;
    }

    // Statistics
    ConnectionStats getStats() const;

    // Network diagnostics
    uint64_t getDuplicatePacketCount() const {
        return _duplicatePacketsReceived.load(std::memory_order_relaxed);
    }
    uint64_t getPacketLossEventCount() const {
        return _packetLossEvents.load(std::memory_order_relaxed);
    }
    uint64_t getSequenceUpdateFailureCount() const {
        return _sequenceUpdateFailures.load(std::memory_order_relaxed);
    }
    uint64_t getUnknownSchemaDropCount() const {
        return _unknownSchemaDrops.load(std::memory_order_relaxed);
    }

    // Property update batching
    void setBatchingEnabled(bool enabled);
    bool isBatchingEnabled() const {
        return _batchingEnabled.load(std::memory_order_relaxed);
    }
    Result<void> flushPropertyUpdates();

    struct PropertyBatchStats
    {
        uint64_t totalBatchesSent{0};
        uint64_t totalUpdatesSent{0};
        uint64_t updatesDeduped{0};
        uint64_t averageBatchSize{0};
    };
    PropertyBatchStats getPropertyBatchStats() const;
    size_t getPendingPropertyUpdateCount() const;

private:
    static std::string generateSessionId();

    void onMessageReceived(const std::vector<uint8_t>& data);
    void onConnectionStateChanged(ConnectionState state);
    void handleReceivedMessage(const std::vector<uint8_t>& data);
    void handleUnknownSchema(ComponentTypeHash typeHash);

    NetworkConnection* _connection;  // Not owned, managed by ConnectionManager

    // Registry ownership model: external (non-owning) or internal (owned)
    PropertyRegistry* _propertyRegistry{nullptr};
    std::unique_ptr<PropertyRegistry> _ownedRegistry;  // used when no external provided

    // Schema registry: external (non-owning), optional
    ComponentSchemaRegistry* _schemaRegistry{nullptr};

    // NACK tracking
    SchemaNackTracker _nackTracker;

    // Unknown schema logging rate limiter
    struct LogRateLimiter
    {
        std::unordered_map<ComponentTypeHash, std::chrono::steady_clock::time_point> lastLogTimes;
        std::mutex mutex;

        bool shouldLog(ComponentTypeHash typeHash, std::chrono::milliseconds interval) {
            std::lock_guard<std::mutex> lock(mutex);
            auto now = std::chrono::steady_clock::now();
            auto it = lastLogTimes.find(typeHash);

            if (it == lastLogTimes.end()) {
                lastLogTimes[typeHash] = now;
                return true;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            if (elapsed >= interval) {
                it->second = now;
                return true;
            }
            return false;
        }
    };
    LogRateLimiter _logRateLimiter;

    std::string _sessionId;

    // ECS entity ID generator
    std::atomic<uint64_t> _nextEntityId{1};

    // Callbacks
    EntityCreatedCallback _entityCreatedCallback;
    EntityDestroyedCallback _entityDestroyedCallback;
    PropertyUpdateCallback _propertyUpdateCallback;
    SceneSnapshotCallback _sceneSnapshotCallback;
    HandshakeCallback _handshakeCallback;
    ErrorCallback _errorCallback;

    // Schema callbacks
    RegisterSchemaResponseCallback _registerSchemaResponseCallback;
    QueryPublicSchemasResponseCallback _queryPublicSchemasResponseCallback;
    PublishSchemaResponseCallback _publishSchemaResponseCallback;
    UnpublishSchemaResponseCallback _unpublishSchemaResponseCallback;
    SchemaNackCallback _schemaNackCallback;
    SchemaAdvertisementCallback _schemaAdvertisementCallback;
    HeartbeatCallback _heartbeatCallback;
    HeartbeatResponseCallback _heartbeatResponseCallback;
    DisconnectCallback _disconnectCallback;

    // Asset callbacks
    AssetAdvertiseCallback _assetAdvertiseCallback;
    AssetAdvertiseResponseCallback _assetAdvertiseResponseCallback;
    AssetWithdrawCallback _assetWithdrawCallback;
    AssetWithdrawResponseCallback _assetWithdrawResponseCallback;
    AssetWithdrawAllCallback _assetWithdrawAllCallback;
    AssetWithdrawAllResponseCallback _assetWithdrawAllResponseCallback;
    AssetResolveCallback _assetResolveCallback;
    AssetResolveResponseCallback _assetResolveResponseCallback;
    AssetResolveBatchCallback _assetResolveBatchCallback;
    AssetResolveBatchResponseCallback _assetResolveBatchResponseCallback;
    AssetProvideKeyCallback _assetProvideKeyCallback;
    AssetProvideKeyResponseCallback _assetProvideKeyResponseCallback;
    AssetUploadCallback _assetUploadCallback;
    AssetUploadResponseCallback _assetUploadResponseCallback;
    AssetFetchCallback _assetFetchCallback;
    AssetFetchResponseCallback _assetFetchResponseCallback;
    AssetFetchBeginCallback _assetFetchBeginCallback;
    AssetFetchChunkCallback _assetFetchChunkCallback;

    // Chunked upload callbacks
    AssetUploadBeginCallback _assetUploadBeginCallback;
    AssetUploadBeginResponseCallback _assetUploadBeginResponseCallback;
    AssetUploadChunkCallback _assetUploadChunkCallback;
    AssetUploadChunkResponseCallback _assetUploadChunkResponseCallback;
    AssetUploadCompleteCallback _assetUploadCompleteCallback;
    AssetUploadCompleteResponseCallback _assetUploadCompleteResponseCallback;
    AssetUploadCancelCallback _assetUploadCancelCallback;
    AssetUploadCancelResponseCallback _assetUploadCancelResponseCallback;

    // Scene management callbacks
    CreateSceneCallback _createSceneCallback;
    CreateSceneResponseCallback _createSceneResponseCallback;
    DestroySceneCallback _destroySceneCallback;
    DestroySceneResponseCallback _destroySceneResponseCallback;
    SetSceneEnabledCallback _setSceneEnabledCallback;
    SetSceneEnabledResponseCallback _setSceneEnabledResponseCallback;
    AddEntityToSceneCallback _addEntityToSceneCallback;
    AddEntityToSceneResponseCallback _addEntityToSceneResponseCallback;

    // Material system callbacks
    CreateMaterialCallback _createMaterialCallback;
    CreateMaterialResponseCallback _createMaterialResponseCallback;
    UpdateMaterialPropertyCallback _updateMaterialPropertyCallback;
    UpdateMaterialPropertyResponseCallback _updateMaterialPropertyResponseCallback;
    UpdateMaterialPropertiesBatchCallback _updateMaterialPropertiesBatchCallback;
    UpdateMaterialPropertiesBatchResponseCallback _updateMaterialPropertiesBatchResponseCallback;
    MaterialPropertyUpdateCallback _materialPropertyUpdateCallback;
    MaterialSubscribeCallback _materialSubscribeCallback;
    MaterialSubscribeResponseCallback _materialSubscribeResponseCallback;
    MaterialUnsubscribeCallback _materialUnsubscribeCallback;
    MaterialUnsubscribeResponseCallback _materialUnsubscribeResponseCallback;
    GetMaterialCallback _getMaterialCallback;
    GetMaterialResponseCallback _getMaterialResponseCallback;
    MaterialResolvedCallback _materialResolvedCallback;

    // Mesh-material binding callbacks
    MeshMaterialBindingCallback _meshMaterialBindingCallback;
    MeshMaterialBindingResponseCallback _meshMaterialBindingResponseCallback;
    MeshMaterialBindingUpdateCallback _meshMaterialBindingUpdateCallback;

    // Shader callbacks
    GetShaderCallback _getShaderCallback;
    GetShaderResponseCallback _getShaderResponseCallback;

    // Permission system callbacks
    PermissionRequestCallback _permissionRequestCallback;
    HeadPosePermissionResponseCallback _headPosePermissionResponseCallback;
    IdentityPermissionResponseCallback _identityPermissionResponseCallback;
    UsernamePermissionResponseCallback _usernamePermissionResponseCallback;
    HostnamePermissionResponseCallback _hostnamePermissionResponseCallback;
    PermissionRevokedCallback _permissionRevokedCallback;
    PermissionRequestCancelledCallback _permissionRequestCancelledCallback;

    // Spatial anchoring callbacks
    RegisterLandmarkRequestCallback _registerLandmarkRequestCallback;
    RegisterLandmarkResponseCallback _registerLandmarkResponseCallback;
    LandmarkObservationMsgCallback _landmarkObservationCallback;
    LandmarkStateUpdateMsgCallback _landmarkStateUpdateCallback;
    UnregisterLandmarkRequestCallback _unregisterLandmarkRequestCallback;
    UnregisterLandmarkResponseCallback _unregisterLandmarkResponseCallback;
    LandmarkSnapshotMsgCallback _landmarkSnapshotCallback;

    // Heartbeat tracking
    std::atomic<uint64_t> _lastHeartbeatReceivedMs{0};  // steady_clock ms since epoch

    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
    std::atomic<uint32_t> _nextSendSequence{0};
    std::atomic<uint32_t> _lastReceivedSequence{0};

    // Network diagnostics counters
    std::atomic<uint64_t> _duplicatePacketsReceived{0};
    std::atomic<uint64_t> _packetLossEvents{0};
    std::atomic<uint64_t> _sequenceUpdateFailures{0};  // CAS retry exhaustion
    std::atomic<uint64_t> _unknownSchemaDrops{0};      // Count of unknown schemas encountered

    // Handshake state
    std::atomic<bool> _handshakeComplete{false};
    std::string _clientType;
    std::string _clientId;

    // Shutdown coordination
    std::atomic<bool> _shuttingDown{false};
    std::atomic<uint32_t> _activeCallbacks{0};

    // Property update batching
    std::atomic<bool> _batchingEnabled{false};

    struct PendingPropertyUpdate
    {
        PropertyType type;
        PropertyValue value;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::unordered_map<PropertyHash, PendingPropertyUpdate> _pendingPropertyUpdates;
    mutable std::mutex _pendingUpdatesMutex;

    std::atomic<uint32_t> _batchSequenceNumber{0};

    // Batch statistics
    mutable std::mutex _batchStatsMutex;
    PropertyBatchStats _batchStats;

    mutable std::mutex _mutex;

    // ========================================================================
    // Async Message Queue (decouples receive thread from callback processing)
    // ========================================================================
    // Receive thread pushes messages to queue, worker thread processes them.
    // This prevents slow callbacks from blocking the receive path and allows
    // heartbeat detection to work correctly even when app is busy.
    std::deque<std::vector<uint8_t>> _messageQueue;
    std::mutex _messageQueueMutex;
    std::condition_variable _messageQueueCV;
    std::thread _messageWorkerThread;
    std::atomic<bool> _messageWorkerRunning{false};

    void messageWorkerLoop();
    void startMessageWorker();
    void stopMessageWorker();
};

}  // namespace EntropyEngine::Networking
