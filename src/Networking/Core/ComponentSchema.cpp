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

#include <algorithm>
#include <format>
#include <iomanip>
#include <sstream>
#include <vector>

namespace EntropyEngine
{
namespace Networking
{

namespace
{
/**
 * @brief Validate that a string is a valid ASCII identifier
 *
 * Valid identifiers:
 * - Contain only [a-zA-Z0-9_.]
 * - Start with letter or underscore
 * - Non-empty
 * - Support reverse domain notation (e.g., "com.entropy.canvas")
 */
bool isAsciiIdentifier(const std::string& str) {
    if (str.empty()) {
        return false;
    }

    // First character must be letter or underscore
    char first = str[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) {
        return false;
    }

    // All characters must be alphanumeric, underscore, or dot
    for (char c : str) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return false;
        }
    }

    return true;
}
}  // anonymous namespace

PropertyHash ComponentSchema::computeStructuralHash(const std::vector<PropertyDefinition>& properties) {
    // Sort properties by name (ASCII lexicographic order)
    auto sortedProps = properties;
    std::sort(sortedProps.begin(), sortedProps.end(),
              [](const PropertyDefinition& a, const PropertyDefinition& b) { return a.name < b.name; });

    // Build canonical string for properties: prop1:type1:offset1:size1,prop2:...
    std::ostringstream oss;
    for (size_t i = 0; i < sortedProps.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }

        const auto& prop = sortedProps[i];
        oss << prop.name << ":" << propertyTypeToString(prop.type) << ":" << prop.offset << ":" << prop.size;
    }

    std::string canonical = oss.str();

    // Hash the UTF-8 bytes
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), hash);

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

ComponentTypeHash ComponentSchema::computeTypeHash(const std::string& appId, const std::string& componentName,
                                                   uint32_t schemaVersion, const PropertyHash& structuralHash) {
    // Build canonical string: {appId}.{componentName}@{version}{structuralHashHex}
    std::ostringstream oss;
    oss << appId << "." << componentName << "@" << schemaVersion << "{" << std::hex << std::setfill('0')
        << std::setw(16) << structuralHash.high << std::setw(16) << structuralHash.low << "}";

    std::string canonical = oss.str();

    // Hash the UTF-8 bytes
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), hash);

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

Result<ComponentSchema> ComponentSchema::create(const std::string& appId, const std::string& componentName,
                                                uint32_t schemaVersion,
                                                const std::vector<PropertyDefinition>& properties, size_t totalSize,
                                                bool isPublic) {
    // Validate app ID
    if (appId.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: appId is empty");
        return Result<ComponentSchema>::err(NetworkError::InvalidParameter, "appId cannot be empty");
    }

    // Validate appId is ASCII identifier
    if (!isAsciiIdentifier(appId)) {
        std::string errorMsg =
            std::format("Component schema validation failed: appId '{}' must be ASCII identifier [a-zA-Z0-9_.]", appId);
        ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            "appId must be ASCII identifier [a-zA-Z0-9_.], starting with letter or underscore");
    }

    // Validate component name
    if (componentName.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: componentName is empty");
        return Result<ComponentSchema>::err(NetworkError::InvalidParameter, "componentName cannot be empty");
    }

    // Validate componentName is ASCII identifier
    if (!isAsciiIdentifier(componentName)) {
        std::string errorMsg =
            std::format("Component schema validation failed: componentName '{}' must be ASCII identifier [a-zA-Z0-9_.]",
                        componentName);
        ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            "componentName must be ASCII identifier [a-zA-Z0-9_.], starting with letter or underscore");
    }

    // Validate properties list
    if (properties.empty()) {
        ENTROPY_LOG_ERROR("Component schema validation failed: properties list is empty");
        return Result<ComponentSchema>::err(NetworkError::InvalidParameter, "properties list cannot be empty");
    }

    // Validate each property
    for (const auto& prop : properties) {
        // Check for empty property name
        if (prop.name.empty()) {
            ENTROPY_LOG_ERROR("Component schema validation failed: property has empty name");
            return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed, "Property name cannot be empty");
        }

        // Validate property name is ASCII identifier
        if (!isAsciiIdentifier(prop.name)) {
            std::string errorMsg = std::format(
                "Component schema validation failed: property name '{}' must be ASCII identifier [a-zA-Z0-9_.]",
                prop.name);
            ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
            return Result<ComponentSchema>::err(
                NetworkError::SchemaValidationFailed,
                "Property name '" + prop.name +
                    "' must be ASCII identifier [a-zA-Z0-9_.], starting with letter or underscore");
        }

        // Check for valid property type (Bytes=10, QuatArray=18)
        if (static_cast<int>(prop.type) < 0 || static_cast<int>(prop.type) > 18) {
            std::string errorMsg =
                std::format("Property '{}' has invalid type: {}", prop.name, static_cast<int>(prop.type));
            ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
            return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                                "Property '" + prop.name + "' has invalid type");
        }

        // Validate defaultValue type matches property type if provided
        if (prop.defaultValue.has_value()) {
            if (!validatePropertyType(prop.defaultValue.value(), prop.type)) {
                std::string errorMsg = std::format("Property '{}' has defaultValue with mismatched type (expected {})",
                                                   prop.name, propertyTypeToString(prop.type));
                ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
                return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                                    "Property '" + prop.name + "' has defaultValue type mismatch");
            }
        }

        // Check that property fits within totalSize
        if (prop.offset + prop.size > totalSize) {
            std::string errorMsg =
                std::format("Property '{}' extends beyond totalSize: offset={}, size={}, totalSize={}", prop.name,
                            prop.offset, prop.size, totalSize);
            ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
            return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                                "Property '" + prop.name + "' extends beyond totalSize");
        }
    }

    // Check for overlapping fields
    for (size_t i = 0; i < properties.size(); ++i) {
        for (size_t j = i + 1; j < properties.size(); ++j) {
            const auto& prop1 = properties[i];
            const auto& prop2 = properties[j];

            // Check if ranges overlap
            bool overlaps = (prop1.offset < prop2.offset + prop2.size) && (prop2.offset < prop1.offset + prop1.size);

            if (overlaps) {
                std::string errorMsg =
                    std::format("Properties '{}' and '{}' have overlapping memory ranges", prop1.name, prop2.name);
                ENTROPY_LOG_ERROR_CAT("ComponentSchema", errorMsg);
                return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                                    "Properties '" + prop1.name + "' and '" + prop2.name + "' overlap");
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

    std::string infoMsg =
        std::format("Created schema for {}.{} v{} (public: {})", appId, componentName, schemaVersion, isPublic);
    ENTROPY_LOG_INFO_CAT("ComponentSchema", infoMsg);

    return Result<ComponentSchema>::ok(schema);
}

Result<void> ComponentSchema::canReadFrom(const ComponentSchema& other) const {
    // Check if all our properties exist in the other schema with matching types and offsets
    for (const auto& ourProp : properties) {
        // Find matching property in other schema
        auto it =
            std::find_if(other.properties.begin(), other.properties.end(),
                         [&ourProp](const PropertyDefinition& otherProp) { return otherProp.name == ourProp.name; });

        if (it == other.properties.end()) {
            std::string errorMsg = "Property '" + ourProp.name + "' not found in source schema";
            std::string logMsg = std::format("Compatibility check failed: {}", errorMsg);
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(NetworkError::SchemaIncompatible, errorMsg);
        }

        const auto& otherProp = *it;

        // Check type match
        if (ourProp.type != otherProp.type) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' type mismatch: "
                << "expected " << static_cast<int>(ourProp.type) << ", got " << static_cast<int>(otherProp.type);
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(NetworkError::SchemaIncompatible, oss.str());
        }

        // Check offset match
        if (ourProp.offset != otherProp.offset) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' offset mismatch: "
                << "expected " << ourProp.offset << ", got " << otherProp.offset;
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(NetworkError::SchemaIncompatible, oss.str());
        }

        // Check size match
        if (ourProp.size != otherProp.size) {
            std::ostringstream oss;
            oss << "Property '" << ourProp.name << "' size mismatch: "
                << "expected " << ourProp.size << ", got " << otherProp.size;
            std::string logMsg = std::format("Compatibility check failed: {}", oss.str());
            ENTROPY_LOG_DEBUG_CAT("ComponentSchema", logMsg);
            return Result<void>::err(NetworkError::SchemaIncompatible, oss.str());
        }
    }

    // All our properties exist in other schema with matching types/offsets/sizes
    return Result<void>::ok();
}

std::string ComponentSchema::toCanonicalString() const {
    // Sort properties by name (ASCII lexicographic order)
    auto sortedProps = properties;
    std::sort(sortedProps.begin(), sortedProps.end(),
              [](const PropertyDefinition& a, const PropertyDefinition& b) { return a.name < b.name; });

    // Build canonical string: {appId}.{componentName}@{version}{prop:type:offset:size,...}
    std::ostringstream oss;
    oss << appId << "." << componentName << "@" << schemaVersion << "{";

    for (size_t i = 0; i < sortedProps.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }

        const auto& prop = sortedProps[i];
        oss << prop.name << ":" << propertyTypeToString(prop.type) << ":" << prop.offset << ":" << prop.size;
    }

    oss << "}";
    return oss.str();
}

}  // namespace Networking
}  // namespace EntropyEngine
