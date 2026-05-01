---
title: Assets
sidebar:
  order: 3
---

# Assets

The `NetworkSession` provides a comprehensive API for managing application content across the network.

## Uploading Assets (Sender)

When an application creates a new asset (e.g., imports a texture or compiles a shader), it must upload it to the network.

```cpp
// 1. Prepare data
std::vector<uint8_t> textureData = LoadFile("my_texture.png");

// 2. Upload (returns immediately, async process)
// Note: contentType is a uint8_t. Applications typically define an enum for this.
// Example: 1 = Texture_PNG
session.sendAssetUpload(
    "Canvas",
    textureData,
    1,   // ContentType (Application specific)
    true // persistent?
);
```

## Resolving Assets (Receiver)

When an entity references an asset by ID, the receiving application must resolve it.

```cpp
// 1. You receive an EntityCreated message with a Material component
// 2. The Material references a specific AssetId
// 3. Request the asset
session.sendAssetResolve(assetId);

// 4. Handle the response (callback)
sessionManager.setAssetResolveResponseCallback(sessionHandle, [](const AssetResolveResponse& response) {
    if (response.found) {
        // Asset is available! Download body if needed.
        if (response.entry.sizeBytes < 1024 * 1024) {
             // Small asset, data might be inline
        } else {
             // Large asset, fetch body separately
             session.sendAssetFetch(response.entry.id);
        }
    }
});
```

## Metadata

Assets carry metadata that helps the receiver decide *how* to process the data before downloading the full body.

*   **ShaderMetadata**: Parameters, Keywords.
*   **TextureMetadata**: Dimensions, Format.

This allows the UI (e.g., Inspector) to show properties of an asset without downloading 50MB of texture data.
