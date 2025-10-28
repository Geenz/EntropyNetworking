/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ComponentSchema.h"
#include <Logging/Logger.h>
#include <openssl/sha.h>
#include <vector>
#include <algorithm>
#include <sstream>
#include <format>

namespace EntropyEngine {
namespace Networking {

PropertyHash ComponentSchema::computeStructuralHash(
    const std::vector<PropertyDefinition>& properties)
{
    // Prepare input buffer: concatenate all property definitions
    // Format for each property: name || type (4 bytes) || offset (8 bytes) || size (8 bytes)
    std::vector<uint8_t> input;

    for (const auto& prop : properties) {
        // Add property name
        input.insert(input.end(), prop.name.begin(), prop.name.end());

        // Add type as 4-byte uint32 (big-endian)
        uint32_t typeValue = static_cast<uint32_t>(prop.type);
        for (int i = 3; i >= 0; --i) {
            input.push_back(static_cast<uint8_t>((typeValue >> (i * 8)) & 0xFF));
        }

        // Add offset as 8-byte uint64 (big-endian)
        uint64_t offsetValue = static_cast<uint64_t>(prop.offset);
        for (int i = 7; i >= 0; --i) {
            input.push_back(static_cast<uint8_t>((offsetValue >> (i * 8)) & 0xFF));
        }

        // Add size as 8-byte uint64 (big-endian)
        uint64_t sizeValue = static_cast<uint64_t>(prop.size);
        for (int i = 7; i >= 0; --i) {
            input.push_back(static_cast<uint8_t>((sizeValue >> (i * 8)) & 0xFF));
        }
    }

    // Compute SHA-256
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(input.data(), input.size(), hash);

    // Extract high 128 bits (first 16 bytes)
    uint64_t high = 0;
    uint64_t low = 0;

    // High 64 bits (bytes 0-7)
    for (int i = 0; i < 8; ++i) {
        high = (high << 8) | hash[i];
    }

    // Low 64 bits (bytes 8-15)
    for (int i = 8; i < 16; ++i) {
        low = (low << 8) | hash[i];
    }

    return PropertyHash{high, low};
}

ComponentTypeHash ComponentSchema::computeTypeHash(
    const std::string& appId,
    const std::string& componentName,
    uint32_t schemaVersion,
    const PropertyHash& structuralHash)
{
    // Prepare input buffer: appId || componentName || schemaVersion (4 bytes) || structuralHash (16 bytes)
    std::vector<uint8_t> input;
    input.reserve(appId.size() + componentName.size() + 4 + 16);

    // Add appId
    input.insert(input.end(), appId.begin(), appId.end());

    // Add componentName
    input.insert(input.end(), componentName.begin(), componentName.end());

    // Add schemaVersion as 4-byte uint32 (big-endian)
    for (int i = 3; i >= 0; --i) {
        input.push_back(static_cast<uint8_t>((schemaVersion >> (i * 8)) & 0xFF));
    }

    // Add structuralHash high (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        input.push_back(static_cast<uint8_t>((structuralHash.high >> (i * 8)) & 0xFF));
    }

    // Add structuralHash low (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        input.push_back(static_cast<uint8_t>((structuralHash.low >> (i * 8)) & 0xFF));
    }

    // Compute SHA-256
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(input.data(), input.size(), hash);

    // Extract high 128 bits (first 16 bytes)
    uint64_t high = 0;
    uint64_t low = 0;

    // High 64 bits (bytes 0-7)
    for (int i = 0; i < 8; ++i) {
        high = (high << 8) | hash[i];
    }

    // Low 64 bits (bytes 8-15)
    for (int i = 8; i < 16; ++i) {
        low = (low << 8) | hash[i];
    }

    return ComponentTypeHash{high, low};
}

Result<ComponentSchema> ComponentSchema::create(
    const std::string& appId,
    const std::string& componentName,
    uint32_t schemaVersion,
    const std::vector<PropertyDefinition>& properties,
    size_t totalSize,
    bool isPublic)
{
    // Validate app ID
    if (appId.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: appId is empty");
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            "appId cannot be empty"
        );
    }

    // Validate component name
    if (componentName.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: componentName is empty");
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            "componentName cannot be empty"
        );
    }

    // Validate properties list
    if (properties.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: properties list is empty");
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            "properties list cannot be empty"
        );
    }

    // Validate each property
    for (const auto& prop : properties) {
        // Check for empty property name
        if (prop.name.empty()) {
            ENTROPY_LOG_ERROR("Component schema validation failed: property has empty name");
            return Result<ComponentSchema>::err(
                NetworkError::SchemaValidationFailed,
                "Property name cannot be empty"
            );
        }

        // Check for valid property type (Bytes=10, QuatArray=18)
        if (static_cast<int>(prop.type) < 0 || static_cast<int>(prop.type) > 18) {
            std::string errorMsg = std::format("Property '{}' has invalid type: {}",
                prop.name, static_cast<int>(prop.type));
            ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
            return Result<ComponentSchema>::err(
                NetworkError::SchemaValidationFailed,
                "Property '" + prop.name + "' has invalid type"
            );
        }

        // Check that property fits within totalSize
        if (prop.offset + prop.size > totalSize) {
            std::string errorMsg = std::format(
                "Property '{}' extends beyond totalSize: offset={}, size={}, totalSize={}",
                prop.name, prop.offset, prop.size, totalSize);
            ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
            return Result<ComponentSchema>::err(
                NetworkError::SchemaValidationFailed,
                "Property '" + prop.name + "' extends beyond totalSize"
            );
        }
    }

    // Check for overlapping fields
    for (size_t i = 0; i < properties.size(); ++i) {
        for (size_t j = i + 1; j < properties.size(); ++j) {
            const auto& prop1 = properties[i];
            const auto& prop2 = properties[j];

            // Check if ranges overlap
            bool overlaps = (prop1.offset < prop2.offset + prop2.size) &&
                          (prop2.offset < prop1.offset + prop1.size);

            if (overlaps) {
                std::string errorMsg = std::format(
                    "Properties '{}' and '{}' have overlapping memory ranges",
                    prop1.name, prop2.name);
                ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
                return Result<ComponentSchema>::err(
                    NetworkError::SchemaValidationFailed,
                    "Properties '" + prop1.name + "' and '" + prop2.name + "' overlap"
                );
            }
        }
    }

    // Compute structural hash
    auto structuralHash = computeStructuralHash(properties);

    // Compute type hash
    auto typeHash = computeTypeHash(appId, componentName, schemaVersion, structuralHash);

    // Create schema
    ComponentSchema schema;
    schema.typeHash = typeHash;
    schema.appId = appId;
    schema.componentName = componentName;
    schema.schemaVersion = schemaVersion;
    schema.structuralHash = structuralHash;
    schema.properties = properties;
    schema.totalSize = totalSize;
    schema.isPublic = isPublic;

    std::string infoMsg = std::format(
        "Created schema for {}.{} v{} (public: {})",
        appId, componentName, schemaVersion, isPublic);
    ENTROPY_LOG_INFO_CAT("ComponentSchema", infoMsg);

    return Result<ComponentSchema>::ok(schema);
}

Result<void> ComponentSchema::canReadFrom(const ComponentSchema& other) const {
    // Check if all our properties exist in the other schema with matching types and offsets
    for (const auto& ourProp : properties) {
        // Find matching property in other schema
        auto it = std::find_if(other.properties.begin(), other.properties.end(),
            [&ourProp](const PropertyDefinition& otherProp) {
                return otherProp.name == ourProp.name;
            });

        if (it == other.properties.end()) {
            std::string errorMsg = "Property '" + ourProp.name + "' not found in source schema";
            std::string logMsg = std::format("Compatibility check failed: {}", errorMsg);
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(
                NetworkError::SchemaIncompatible,
                errorMsg
            );
        }

        const auto& otherProp = *it;

        // Check type match
        if (ourProp.type != otherProp.type) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' type mismatch: "
                << "expected " << static_cast<int>(ourProp.type)
                << ", got " << static_cast<int>(otherProp.type);
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(
                NetworkError::SchemaIncompatible,
                oss.str()
            );
        }

        // Check offset match
        if (ourProp.offset != otherProp.offset) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' offset mismatch: "
                << "expected " << ourProp.offset
                << ", got " << otherProp.offset;
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(
                NetworkError::SchemaIncompatible,
                oss.str()
            );
        }

        // Check size match
        if (ourProp.size != otherProp.size) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' size mismatch: "
                << "expected " << ourProp.size
                << ", got " << otherProp.size;
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(
                NetworkError::SchemaIncompatible,
                oss.str()
            );
        }
    }

    // All our properties exist in other schema with matching types/offsets/sizes
    return Result<void>::ok();
}

} // namespace Networking
} // namespace EntropyEngine
