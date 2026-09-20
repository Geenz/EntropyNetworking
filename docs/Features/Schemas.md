---
title: Component Schemas
sidebar:
  order: 2
---

# Component Schemas

Component Schemas are the blueprint for all replicated data in Entropy. They define the structure, types, and memory layout of components, ensuring that different applications can safely exchange data.

## Anatomy of a Schema

A `ComponentSchema` consists of:
*   **Name**: Human-readable identifier (e.g., "Transform").
*   **Version**: Monotonic integer for schema evolution.
*   **Properties**: Ordered list of fields (name, type, offset).
*   **Structural Hash**: Unique identifier derived from the structure.

## Defining a Schema

Schemas are defined using a builder pattern.

```cpp
std::vector<PropertyDefinition> properties;
properties.push_back({"position", PropertyType::Vec3, offsetof(Transform, position), 12});
properties.push_back({"rotation", PropertyType::Quat, offsetof(Transform, rotation), 16});
properties.push_back({"scale", PropertyType::Vec3, offsetof(Transform, scale), 12});

auto result = ComponentSchema::create(
    "Canvas",           // App ID
    "Transform",        // Component Name
    1,                  // Version
    properties,         // Properties List
    sizeof(Transform),  // Total Size
    true                // Is Public?
);

if (result.success()) {
    auto schema = result.value();
}
```

## Registration & Discovery

To use a schema, it must be registered with the `ComponentSchemaRegistry`.

```cpp
auto result = registry.registerSchema(schema);
if (result.success()) {
    // Schema is now available for use
}
```

### Public vs Private
*   **Private (Default)**: Only known to the local application. Incoming messages using this schema will be processed if the peer knows it, but it won't be advertised.
*   **Public**: The schema is broadcast to all connected peers. This is useful for "Discovery" workflows where a tool (like Inspector) wants to know all available component types.

## Versioning & Compatibility

When a schema changes (e.g., adding a field), you must increment the version.
*   **Backward Compatibility**: The system checks structural hashes. If the structure changes, the hash changes, and the new schema is treated as a distinct type.
*   **Migration**: Applications can register converters to translate data between old and new schema versions.
