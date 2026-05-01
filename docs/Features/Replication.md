---
title: Entity Replication
sidebar:
  order: 1
---

# Entity Replication

Entity Replication is the process of synchronizing ECS entities between a host application (e.g., Canvas) and a remote peer (e.g., Portal or Paint).

## Key Concepts

*   **Owner Authoritative**: Only the session that created an entity can modify its component structure (add/remove components) or destroy it.
*   **Property Sync**: Any peer with write access can update properties on replicated entities.
*   **Eventual Consistency**: State converges over time; intermediate updates may be skipped for performance.
*   **Math Types**: Uses [GLM](https://github.com/g-truc/glm) types (`glm::vec3`, `glm::quat`, etc.) aliased as `Vec3`, `Quat` in the `EntropyEngine::Networking` namespace.

## Creating Replicated Entities

The `EntityBuilder` provides a fluent API for constructing entities and syncing them to the network.

```cpp
// 1. Start building an entity (e.g., a "Mesh")
auto builder = session.createEntity("Mesh", "MyApp");

// 2. Attach components and set initial values
builder.attach(transformSchema)
       .set("position", Vec3{0, 0, 0})
       .set("scale", Vec3{1, 1, 1});

builder.attach(meshRendererSchema)
       .set("mesh", myMeshAssetId)
       .set("material", myMaterialAssetId);

// 3. Sync to network (sends EntityCreated message)
builder.sync();
```

## Updating Properties

Once an entity is created, you can update its properties. Using property hashes is preferred for performance as it avoids string lookups.

```cpp
// 1. Pre-compute hash (static or cached)
static auto positionHash = computePropertyHash(
    entityId,
    transformSchema.typeHash,
    "position"
);

// 2. Send update using hash
session.sendPropertyUpdate(
    positionHash,
    PropertyType::Vec3,
    Vec3{10, 5, 0}
);
```

## Receiving Updates

To react to changes from the network, register callbacks with the `SessionManager`.

```cpp
sessionManager.setPropertyUpdateCallback(sessionHandle, [](const PropertyUpdate& update) {
    if (update.propertyName == "position") {
        // Update local transform
    }
});
```
