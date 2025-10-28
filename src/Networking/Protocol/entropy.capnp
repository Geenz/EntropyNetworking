@0xb5c7a9e2d4f1e8c3;

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

struct EntityCreated {
    entityId @0 :UInt64;
    appId @1 :Text;
    typeName @2 :Text;
    parentId @3 :UInt64;             # 0 = root
    properties @4 :List(PropertyRegistration);
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
    }
}
