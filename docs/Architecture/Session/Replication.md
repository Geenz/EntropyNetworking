---
title: Replication Model
sidebar:
  order: 2
---

# Replication Model

Entropy uses a **State Replication** model rather than pure event streaming.

## Authority
Only the "Owner" of an entity can modify its component structure (add/remove components) or destroy it.

*   **Eventual Consistency**: The system guarantees that all peers will eventually converge on the same state, even if intermediate updates are dropped (via Unreliable channels).

```mermaid
sequenceDiagram
    participant Owner
    participant Network
    participant Peer

    Note over Owner: Create Entity (ID: 100)
    Owner->>Network: EntityCreated (Reliable)
    Network->>Peer: EntityCreated
    Note over Peer: Entity 100 Spawned

    loop High-Frequency Update
        Owner->>Network: PropertyUpdate (Pos=Vec3, Unreliable)
        opt Dropped
            Network-xPeer: Packet Lost
        end
        Owner->>Network: PropertyUpdate (Pos=Vec3, Unreliable)
        Network->>Peer: PropertyUpdate
        Note over Peer: Pos Updated
    end
```

## Asset Integration
The Session Layer integrates tightly with the Asset System.
*   **Lazy Loading**: Assets (textures, meshes) are only fetched when referenced by a replicated entity.
*   **Content Addressing**: Assets are identified by their SHA-256 hash (`AssetId`), allowing for efficient caching and avoiding duplication.
