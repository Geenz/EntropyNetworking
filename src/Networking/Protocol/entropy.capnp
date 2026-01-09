@0xb5c7a9e2d4f1e8c3;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("EntropyEngine::Networking::Protocol");

# Entropy Network Protocol Schema
# Defines all message types for Canvas, Portal, and Paint communication

# ============================================================================
# Core Types
# ============================================================================

struct PropertyHash128 {
    high @0 :UInt64;
    low @1 :UInt64;
}

enum PropertyType {
    int32 @0;
    int64 @1;
    float32 @2;
    float64 @3;
    vec2 @4;
    vec3 @5;
    vec4 @6;
    quat @7;
    string @8;
    bool @9;
    bytes @10;

    # Array types
    int32Array @11;
    int64Array @12;
    float32Array @13;
    float64Array @14;
    vec2Array @15;
    vec3Array @16;
    vec4Array @17;
    quatArray @18;

    # Asset reference
    assetId @19;
}

struct Vec2 {
    x @0 :Float32;
    y @1 :Float32;
}

struct Vec3 {
    x @0 :Float32;
    y @1 :Float32;
    z @2 :Float32;
}

struct Vec4 {
    x @0 :Float32;
    y @1 :Float32;
    z @2 :Float32;
    w @3 :Float32;
}

struct Quat {
    x @0 :Float32;
    y @1 :Float32;
    z @2 :Float32;
    w @3 :Float32;
}

struct PropertyValue {
    union {
        int32 @0 :Int32;
        int64 @1 :Int64;
        float32 @2 :Float32;
        float64 @3 :Float64;
        vec2 @4 :Vec2;
        vec3 @5 :Vec3;
        vec4 @6 :Vec4;
        quat @7 :Quat;
        string @8 :Text;
        bool @9 :Bool;
        bytes @10 :Data;
        assetId @11 :Data;        # 32-byte SHA-256 asset identifier
    }
}

# ============================================================================
# High-Frequency Property Updates (Unreliable Channel)
# ============================================================================

struct PropertyUpdate {
    propertyHash @0 :PropertyHash128;
    expectedType @1 :PropertyType;
    value @2 :PropertyValue;
}

struct PropertyUpdateBatch {
    timestamp @0 :UInt64;            # Microseconds since Unix epoch (1970-01-01 00:00:00 UTC)
    sequence @1 :UInt32;             # Monotonic sequence number
    updates @2 :List(PropertyUpdate);
}

# ============================================================================
# Entity Lifecycle Messages (Reliable Channel)
# ============================================================================

struct PropertyRegistration {
    propertyHash @0 :PropertyHash128;
    entityId @1 :UInt64;
    componentType @2 :PropertyHash128;  # ComponentTypeHash from ComponentSchema
    propertyName @3 :Text;              # e.g., "position", "health"
    type @4 :PropertyType;
    registeredAt @5 :UInt64;            # Microseconds since Unix epoch (1970-01-01 00:00:00 UTC)
}

# ComponentGroup - Groups properties under a component
struct ComponentGroup {
    typeHash @0 :PropertyHash128;    # ComponentTypeHash
    componentName @1 :Text;          # Human-readable name (e.g., "Transform")
    properties @2 :List(PropertyRegistration);  # Properties within this component
}

struct EntityCreated {
    entityId @0 :UInt64;
    appId @1 :Text;
    typeName @2 :Text;
    parentId @3 :UInt64;             # 0 = root
    components @4 :List(ComponentGroup);  # Properties grouped by component
}

# ComponentAdded - Sent when a component is added to an existing entity
struct ComponentAdded {
    entityId @0 :UInt64;
    component @1 :ComponentGroup;
}

# ComponentRemoved - Sent when a component is removed from an entity
struct ComponentRemoved {
    entityId @0 :UInt64;
    typeHash @1 :PropertyHash128;    # Which component was removed
}

struct EntityDestroyed {
    entityId @0 :UInt64;
}

struct PropertySnapshot {
    propertyHash @0 :PropertyHash128;
    value @1 :PropertyValue;
}

# ============================================================================
# Property Registry Messages (Reliable Channel)
# ============================================================================

struct RegisterPropertiesRequest {
    properties @0 :List(PropertyRegistration);
}

struct RegisterPropertiesResponse {
    succeeded @0 :List(PropertyHash128);
    failed @1 :List(PropertyHash128);
    errors @2 :List(Text);           # Error message for each failed hash
}

struct UnregisterEntityRequest {
    entityId @0 :UInt64;
}

struct UnregisterEntityResponse {
    removedHashes @0 :List(PropertyHash128);
}

# ============================================================================
# Component Schema Messages (Reliable Channel)
# ============================================================================

struct PropertyDefinitionData {
    name @0 :Text;                   # Property name (e.g., "position", "health")
    type @1 :PropertyType;           # Property type
    offset @2 :UInt64;               # Byte offset within component struct
    size @3 :UInt64;                 # Size in bytes
    required @4 :Bool = true;        # Whether this property must be present (default: true)
    defaultValue @5 :PropertyValue;  # Optional default value (check hasDefaultValue)
    hasDefaultValue @6 :Bool = false; # Whether defaultValue is set
}

struct ComponentSchemaData {
    typeHash @0 :PropertyHash128;    # Unique component type identifier
    appId @1 :Text;                  # Originating application ID
    componentName @2 :Text;          # Human-readable component name
    schemaVersion @3 :UInt32;        # Schema version for evolution
    structuralHash @4 :PropertyHash128; # Hash of field layout
    properties @5 :List(PropertyDefinitionData);
    totalSize @6 :UInt64;            # Total component size in bytes
    isPublic @7 :Bool;               # Whether schema is published for discovery
}

struct RegisterSchemaRequest {
    schema @0 :ComponentSchemaData;
}

struct RegisterSchemaResponse {
    success @0 :Bool;
    typeHash @1 :PropertyHash128;    # ComponentTypeHash on success
    errorMessage @2 :Text;           # Error message on failure
}

struct QueryPublicSchemasRequest {
    # Empty - requests all public schemas
}

struct QueryPublicSchemasResponse {
    schemas @0 :List(ComponentSchemaData);
}

struct PublishSchemaRequest {
    typeHash @0 :PropertyHash128;    # ComponentTypeHash to publish
}

struct PublishSchemaResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

struct UnpublishSchemaRequest {
    typeHash @0 :PropertyHash128;    # ComponentTypeHash to unpublish
}

struct UnpublishSchemaResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

# SchemaNack - Optional negative acknowledgment for unknown schemas
#
# Sent when a peer receives a message referencing an unknown ComponentTypeHash.
# This is OPTIONAL feedback controlled by SchemaNackPolicy:
# - Only sent when policy is enabled (default: disabled)
# - Subject to per-schema rate limiting (default: 1000ms interval)
# - Triggered by unknown ComponentTypeHash in ENTITY_CREATED messages
# - Receiver should respond by advertising or registering the schema
#
# This is NOT a required acknowledgment - peers may silently drop unknown schemas.
# Applications control NACK behavior via SchemaNackPolicy for their use case.
struct SchemaNack {
    typeHash @0 :PropertyHash128;    # Unknown ComponentTypeHash that triggered the NACK
    reason @1 :Text;                 # Human-readable reason (e.g., "Schema not found in registry")
    timestamp @2 :UInt64;            # When the NACK occurred (microseconds since epoch)
}

# SchemaAdvertisement - Proactive schema notification
#
# Sent to inform peers about available schemas without requiring a request.
# Can be used in response to SchemaNack or proactively during connection setup.
struct SchemaAdvertisement {
    typeHash @0 :PropertyHash128;    # ComponentTypeHash being advertised
    appId @1 :Text;                  # Application ID
    componentName @2 :Text;          # Component name
    schemaVersion @3 :UInt32;        # Schema version
}

# ============================================================================
# Control Messages (Reliable Channel)
# ============================================================================

struct CreateNodeRequest {
    requestId @0 :UInt32;
    appId @1 :Text;
    typeName @2 :Text;
    parentId @3 :UInt64;
}

struct CreateNodeResponse {
    requestId @0 :UInt32;
    nodeId @1 :UInt64;
    success @2 :Bool;
    errorMessage @3 :Text;           # Empty if success
}

struct DestroyNodeRequest {
    requestId @0 :UInt32;
    nodeId @1 :UInt64;
}

struct DestroyNodeResponse {
    requestId @0 :UInt32;
    success @1 :Bool;
    errorMessage @2 :Text;
}

# ============================================================================
# Scene Synchronization (Reliable Channel)
# ============================================================================

struct SceneManifest {
    totalNodes @0 :UInt32;
    estimatedChunks @1 :UInt16;
    activeApps @2 :List(Text);
    compressed @3 :Bool;
}

struct NodeState {
    entityId @0 :UInt64;
    appId @1 :Text;
    typeName @2 :Text;
    parentId @3 :UInt64;
    properties @4 :List(PropertySnapshot);
}

struct SceneSnapshotChunk {
    chunkIndex @0 :UInt16;
    totalChunks @1 :UInt16;
    compressed @2 :Bool;
    nodes @3 :List(NodeState);
}

# ============================================================================
# Connection Management
# ============================================================================

struct Handshake {
    protocolVersion @0 :UInt32;
    clientType @1 :Text;             # "portal", "paint", "canvas"
    clientId @2 :Text;

    # Capability flags (added for protocol evolution)
    supportsSchemaMetadata @3 :Bool = false;  # Supports required/defaultValue in PropertyDefinitionData
    supportsSchemaAck @4 :Bool = false;       # Supports acknowledgment of schema registration
    supportsSchemaAdvert @5 :Bool = false;    # Supports schema advertisement/discovery
}

struct HandshakeResponse {
    success @0 :Bool;
    serverId @1 :Text;
    errorMessage @2 :Text;

    # Server capabilities
    supportsSchemaMetadata @3 :Bool = false;
    supportsSchemaAck @4 :Bool = false;
    supportsSchemaAdvert @5 :Bool = false;
}

struct Heartbeat {
    timestamp @0 :UInt64;
}

struct HeartbeatResponse {
    timestamp @0 :UInt64;
    serverTime @1 :UInt64;
}

# ============================================================================
# Asset System Messages (Reliable Channel)
# ============================================================================

# Metadata for a single asset in the catalog
struct AssetEntry {
    id @0 :Data;                     # 32-byte AssetId (content-addressed lookup key)
    uri @1 :Text;                    # Location: file://, http://, https://
    contentType @2 :UInt8;           # ContentType enum
    sizeBytes @3 :UInt64;            # Asset size for progress/allocation
    encrypted @4 :Bool;              # True if AES-256-GCM encrypted
    plaintextHash @5 :Data;          # 32-byte verification hash (if encrypted)
    appId @6 :Text;                  # Owning app (empty = canvas-owned)
    persistent @7 :Bool;             # Survives app disconnect
}

# App registers assets in the catalog
struct AssetAdvertiseRequest {
    appId @0 :Text;
    entries @1 :List(AssetEntry);
    requestId @2 :UInt64;            # For response correlation
}

struct AssetAdvertiseResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
    requestId @2 :UInt64;            # Echo back for correlation
}

# App removes specific assets from catalog
struct AssetWithdrawRequest {
    assetIds @0 :List(Data);         # List of 32-byte AssetIds
    requestId @1 :UInt64;            # For response correlation
}

struct AssetWithdrawResponse {
    success @0 :Bool;
    removedCount @1 :UInt32;
    errorMessage @2 :Text;
    requestId @3 :UInt64;            # Echo back for correlation
}

# App removes all its transient assets
struct AssetWithdrawAllRequest {
    appId @0 :Text;
    requestId @1 :UInt64;            # For response correlation
}

struct AssetWithdrawAllResponse {
    success @0 :Bool;
    removedCount @1 :UInt32;
    errorMessage @2 :Text;
    requestId @3 :UInt64;            # Echo back for correlation
}

# Portal requests asset resolution
struct AssetResolveRequest {
    assetId @0 :Data;                # 32-byte AssetId
    requestId @1 :UInt64;            # For response correlation
}

struct AssetResolveResponse {
    found @0 :Bool;
    entry @1 :AssetEntry;
    hasKey @2 :Bool;
    key @3 :Data;                    # 32-byte AES key (if hasKey)
    deliveryMethod @4 :UInt8;        # DeliveryMethod enum
    requestId @5 :UInt64;            # Echo back for correlation
}

# Portal requests batch resolution
struct AssetResolveBatchRequest {
    assetIds @0 :List(Data);         # List of 32-byte AssetIds
    requestId @1 :UInt64;            # For response correlation
}

struct AssetResolveBatchResponse {
    responses @0 :List(AssetResolveResponse);
    requestId @1 :UInt64;            # Echo back for correlation
}

# App provides decryption key for encrypted asset
struct AssetProvideKeyRequest {
    assetId @0 :Data;                # 32-byte AssetId
    key @1 :Data;                    # 32-byte AES-256 key
    requestId @2 :UInt64;            # For response correlation
}

struct AssetProvideKeyResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
    requestId @2 :UInt64;            # Echo back for correlation
}

# App uploads asset data to canvas storage
struct AssetUploadRequest {
    appId @0 :Text;
    data @1 :Data;                   # Asset content
    contentType @2 :UInt8;           # ContentType enum
    persistent @3 :Bool;
    requestId @4 :UInt64;            # For response correlation
}

struct AssetUploadResponse {
    success @0 :Bool;
    assetId @1 :Data;                # 32-byte AssetId of stored content
    uri @2 :Text;                    # Generated URI for the asset
    errorMessage @3 :Text;
    requestId @4 :UInt64;            # Echo back for correlation
}

# Portal fetches asset data over WebRTC data channel
struct AssetFetchRequest {
    assetId @0 :Data;                # 32-byte AssetId
    requestId @1 :UInt64;            # For response correlation
}

struct AssetFetchResponse {
    found @0 :Bool;
    data @1 :Data;                   # Asset content
    errorMessage @2 :Text;
    requestId @3 :UInt64;            # Echo back for correlation
}

# ============================================================================
# Chunked Asset Upload (for large assets)
# ============================================================================

# Begin a chunked upload - returns upload ID for subsequent chunks
struct AssetUploadBeginRequest {
    appId @0 :Text;
    totalSize @1 :UInt64;            # Total size in bytes for allocation/progress
    contentType @2 :UInt8;           # ContentType enum
    persistent @3 :Bool;
    chunkSize @4 :UInt32;            # Preferred chunk size (server may adjust)
    encrypted @5 :Bool;              # Will the final asset be encrypted?
    plaintextHash @6 :Data;          # 32-byte hash for verification (if encrypted)
    requestId @7 :UInt64;            # For response correlation
}

struct AssetUploadBeginResponse {
    success @0 :Bool;
    uploadId @1 :Data;               # 16-byte UUID for this upload session
    chunkSize @2 :UInt32;            # Actual chunk size to use
    errorMessage @3 :Text;
    requestId @4 :UInt64;            # Echo back for correlation
}

# Send a chunk of data
struct AssetUploadChunkRequest {
    uploadId @0 :Data;               # 16-byte upload session ID
    offset @1 :UInt64;               # Byte offset in the complete asset
    data @2 :Data;                   # Chunk data
    sequence @3 :UInt32;             # Chunk sequence number (for ordering)
}

struct AssetUploadChunkResponse {
    success @0 :Bool;
    uploadId @1 :Data;               # Echo back for correlation
    bytesReceived @2 :UInt64;        # Total bytes received so far
    errorMessage @3 :Text;
}

# Complete the chunked upload - triggers assembly and hash verification
struct AssetUploadCompleteRequest {
    uploadId @0 :Data;               # 16-byte upload session ID
    totalChunks @1 :UInt32;          # Expected total chunks for verification
}

struct AssetUploadCompleteResponse {
    success @0 :Bool;
    assetId @1 :Data;                # 32-byte AssetId of assembled content
    uri @2 :Text;                    # Generated URI for the asset
    bytesStored @3 :UInt64;          # Actual bytes stored
    errorMessage @4 :Text;
    uploadId @5 :Data;               # 16-byte upload session ID for correlation
}

# Cancel an in-progress chunked upload
struct AssetUploadCancelRequest {
    uploadId @0 :Data;               # 16-byte upload session ID
}

struct AssetUploadCancelResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

# ============================================================================
# Scene Management
# ============================================================================

# Create a new scene (optionally transient - deleted when session disconnects)
struct CreateSceneRequest {
    sceneName @0 :Text;           # Human-readable scene name
    transient @1 :Bool;           # If true, scene is deleted when owning session disconnects
}

struct CreateSceneResponse {
    success @0 :Bool;
    sceneId @1 :UInt64;           # Unique scene identifier
    errorMessage @2 :Text;
}

# Destroy a scene and all its entities
struct DestroySceneRequest {
    sceneId @0 :UInt64;
}

struct DestroySceneResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

# Enable or disable a scene (disabled scenes don't render/process but can receive updates)
struct SetSceneEnabledRequest {
    sceneId @0 :UInt64;
    enabled @1 :Bool;
}

struct SetSceneEnabledResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

# Add an entity to a scene
struct AddEntityToSceneRequest {
    entityId @0 :UInt64;
    sceneId @1 :UInt64;
}

struct AddEntityToSceneResponse {
    success @0 :Bool;
    errorMessage @1 :Text;
}

# ============================================================================
# Top-Level Message Envelope
# ============================================================================

struct Message {
    union {
        # High-frequency updates
        propertyUpdateBatch @0 :PropertyUpdateBatch;

        # Entity lifecycle
        entityCreated @1 :EntityCreated;
        entityDestroyed @2 :EntityDestroyed;

        # Control messages
        createNodeRequest @3 :CreateNodeRequest;
        createNodeResponse @4 :CreateNodeResponse;
        destroyNodeRequest @5 :DestroyNodeRequest;
        destroyNodeResponse @6 :DestroyNodeResponse;

        # Scene sync
        sceneManifest @7 :SceneManifest;
        sceneSnapshotChunk @8 :SceneSnapshotChunk;

        # Connection
        handshake @9 :Handshake;
        handshakeResponse @10 :HandshakeResponse;
        heartbeat @11 :Heartbeat;
        heartbeatResponse @12 :HeartbeatResponse;

        # Property registry
        registerPropertiesRequest @13 :RegisterPropertiesRequest;
        registerPropertiesResponse @14 :RegisterPropertiesResponse;
        unregisterEntityRequest @15 :UnregisterEntityRequest;
        unregisterEntityResponse @16 :UnregisterEntityResponse;

        # Component schema registry
        registerSchemaRequest @17 :RegisterSchemaRequest;
        registerSchemaResponse @18 :RegisterSchemaResponse;
        queryPublicSchemasRequest @19 :QueryPublicSchemasRequest;
        queryPublicSchemasResponse @20 :QueryPublicSchemasResponse;
        publishSchemaRequest @21 :PublishSchemaRequest;
        publishSchemaResponse @22 :PublishSchemaResponse;
        unpublishSchemaRequest @23 :UnpublishSchemaRequest;
        unpublishSchemaResponse @24 :UnpublishSchemaResponse;
        schemaNack @25 :SchemaNack;
        schemaAdvertisement @26 :SchemaAdvertisement;

        # Asset system
        assetAdvertiseRequest @27 :AssetAdvertiseRequest;
        assetAdvertiseResponse @28 :AssetAdvertiseResponse;
        assetWithdrawRequest @29 :AssetWithdrawRequest;
        assetWithdrawResponse @30 :AssetWithdrawResponse;
        assetWithdrawAllRequest @31 :AssetWithdrawAllRequest;
        assetWithdrawAllResponse @32 :AssetWithdrawAllResponse;
        assetResolveRequest @33 :AssetResolveRequest;
        assetResolveResponse @34 :AssetResolveResponse;
        assetResolveBatchRequest @35 :AssetResolveBatchRequest;
        assetResolveBatchResponse @36 :AssetResolveBatchResponse;
        assetProvideKeyRequest @37 :AssetProvideKeyRequest;
        assetProvideKeyResponse @38 :AssetProvideKeyResponse;
        assetUploadRequest @39 :AssetUploadRequest;
        assetUploadResponse @40 :AssetUploadResponse;
        assetFetchRequest @41 :AssetFetchRequest;
        assetFetchResponse @42 :AssetFetchResponse;

        # Chunked upload
        assetUploadBeginRequest @43 :AssetUploadBeginRequest;
        assetUploadBeginResponse @44 :AssetUploadBeginResponse;
        assetUploadChunkRequest @45 :AssetUploadChunkRequest;
        assetUploadChunkResponse @46 :AssetUploadChunkResponse;
        assetUploadCompleteRequest @47 :AssetUploadCompleteRequest;
        assetUploadCompleteResponse @48 :AssetUploadCompleteResponse;
        assetUploadCancelRequest @49 :AssetUploadCancelRequest;
        assetUploadCancelResponse @50 :AssetUploadCancelResponse;

        # Scene management
        createSceneRequest @51 :CreateSceneRequest;
        createSceneResponse @52 :CreateSceneResponse;
        destroySceneRequest @53 :DestroySceneRequest;
        destroySceneResponse @54 :DestroySceneResponse;
        setSceneEnabledRequest @55 :SetSceneEnabledRequest;
        setSceneEnabledResponse @56 :SetSceneEnabledResponse;
        addEntityToSceneRequest @57 :AddEntityToSceneRequest;
        addEntityToSceneResponse @58 :AddEntityToSceneResponse;

        # Component lifecycle
        componentAdded @59 :ComponentAdded;
        componentRemoved @60 :ComponentRemoved;
    }
}
