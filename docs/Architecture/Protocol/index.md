---
title: Overview
sidebar:
  order: 1
---

# Protocol Layer Overview

The Protocol Layer defines the format of data transmitted between Entropy applications. It handles message serialization, schema validation, and compatibility negotiation.

## Wire Format: Cap'n Proto

EntropyNetworking uses [Cap'n Proto](https://capnproto.org/) for its wire format. Cap'n Proto is a "zero-copy" serialization system, meaning that the in-memory representation of data is identical to the wire format. This eliminates encode/decode steps, significantly reducing CPU usage and latency.

*   **Schema Definition**: Defined in `src/Networking/Protocol/entropy.capnp`.
*   **Message Types**:
    *   **Control**: Handshake, Heartbeat, CreateNode.
    *   **Replication**: EntityCreated, PropertyUpdate, ComponentAdded.
    *   **Asset**: AssetAdvertise, AssetFetch, AssetResolve.

## Channels

The protocol utilizes two distinct transmission channels provided by the Transport Layer:

### 1. Reliable Channel (Ordered, Guaranteed)
*   **Used for**: `EntityCreated`, `AssetUpload`, `Handshake`, `RpcCalls`.
*   **Behavior**: Messages arrive in order; lost messages are retransmitted.

### 2. Unreliable Channel (Unordered, Best Effort)
*   **Used for**: `PropertyUpdate` (e.g., position/rotation updates).
*   **Behavior**: Messages may be dropped or arrive out of order. This is preferred for high-frequency data where the latest value is the only one that matters.
