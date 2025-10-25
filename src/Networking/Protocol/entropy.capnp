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
    timestamp @0 :UInt64;            # Microseconds since epoch
    sequence @1 :UInt32;             # Monotonic sequence number
    updates @2 :List(PropertyUpdate);
}

# ============================================================================
# Entity Lifecycle Messages (Reliable Channel)
# ============================================================================

struct PropertyRegistration {
    propertyHash @0 :PropertyHash128;
    entityId @1 :UInt64;
    componentType @2 :Text;          # e.g., "Transform", "Player"
    propertyName @3 :Text;           # e.g., "position", "health"
    type @4 :PropertyType;
    registeredAt @5 :UInt64;         # Microseconds since epoch
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
}

struct HandshakeResponse {
    success @0 :Bool;
    serverId @1 :Text;
    errorMessage @2 :Text;
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
    }
}
