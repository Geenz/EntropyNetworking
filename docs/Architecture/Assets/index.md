---
title: Overview
sidebar:
  order: 1
---

# Asset System Overview

The Asset System in EntropyNetworking enables the seamless distribution of content (Textures, Meshes, Shaders) across the network. It is built on a content-addressable storage model.

## Core Concepts

### AssetId
Every asset is identified by a 32-byte SHA-256 hash of its content, known as the `AssetId`.
*   **Deduplication**: Identical files always have the same ID, preventing redundant transfers.
*   **Verification**: The ID acts as a checksum to verify data integrity upon receipt.
*   **Immutable**: Assets are never "modified" in place; a change in content results in a new ID.

### Storage Backend
The networking layer integrates with `EntropyCore`'s `VirtualFileSystem` to store and retrieve asset data.
*   **Cache**: Downloaded assets are cached locally on disk.

## Metadata
Assets can carry type-specific metadata that is available before the full asset body is downloaded.
*   **ShaderMetadata**: Parameters, Keywords, Render Queue.
*   **TextureMetadata**: Width, Height, Format, MipLevels.
