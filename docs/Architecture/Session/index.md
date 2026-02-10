---
title: Overview
sidebar:
  order: 1
---

# Session Layer Overview

The Session Layer is the intelligent core of EntropyNetworking. It sits above the Transport and Protocol layers, managing the lifecycle of an application's connection to the larger Entropy ecosystem.

## Key Components

### SessionManager
The `SessionManager` orchestrates the creation and destruction of sessions.
*   **Factory**: `createSession(connection)`
*   **Callback Router**: Registers protocol-level callbacks (e.g., `setEntityCreatedCallback`) and routes messages to the appropriate `NetworkSession`.
*   **Lifecycle**: Manages the `ConnectionHandle` and ensures clean session teardown.

### NetworkSession
A `NetworkSession` represents a stateful connection to a peer application (e.g., Portal, Paint, or Server).
*   **Entity Replication**: Handles `EntityCreated`, `PropertyUpdate`, etc. to keep the local ECS world in sync with the remote peer.
*   **Asset Management**: Coordinates `AssetResolve`, `AssetFetch`, and `AssetUpload` requests.
*   **RPCs**: Provides a mechanism for Remote Procedure Calls (e.g., `createScene`, `destroyNode`).
*   **Handshake**: Performs capability negotiation on connection establishment.
