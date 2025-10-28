/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "PropertyHash.h"
#include "PropertyTypes.h"
#include "ErrorCodes.h"
#include <string>
#include <vector>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Property definition within a component schema
 *
 * Describes a single property/field within a component type, including
 * its name, type, memory layout offset, and size.
 */
struct PropertyDefinition {
    std::string name;           ///< Property name (e.g., "position", "health")
    PropertyType type;          ///< Property type from PropertyTypes.h
    size_t offset;              ///< Byte offset within component struct
    size_t size;                ///< Size in bytes

    /**
     * @brief Equality comparison for property definitions
     *
     * Two definitions are equal if all fields match exactly.
     */
    bool operator==(const PropertyDefinition& other) const {
        return name == other.name &&
               type == other.type &&
               offset == other.offset &&
               size == other.size;
    }

    bool operator!=(const PropertyDefinition& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Component schema definition
 *
 * Describes the complete structure of a component type including all properties.
 * Used for validation and cross-application compatibility checking.
 *
 * Schema Evolution:
 * - Application decides compatibility policy
 * - Structural hash provides fast equality check
 * - canReadFrom() provides subset compatibility checking
 * - Applications control how to handle version differences
 *
 * Privacy:
 * - Schemas are private by default
 * - Applications opt-in to publishing via isPublic flag
 * - Only public schemas are discoverable by other applications
 */
struct ComponentSchema {
    ComponentTypeHash typeHash;         ///< Unique component type identifier
    std::string appId;                  ///< Originating application ID
    std::string componentName;          ///< Human-readable component name
    uint32_t schemaVersion;             ///< Schema version for evolution
    PropertyHash structuralHash;        ///< Hash of field layout
    std::vector<PropertyDefinition> properties;
    size_t totalSize;                   ///< Total component size in bytes
    bool isPublic;                      ///< Whether schema is published for discovery

    /**
     * @brief Check structural compatibility with another schema
     *
     * Two schemas are structurally compatible if they have identical
     * field layouts (same fields, types, and offsets).
     *
     * This is a fast O(1) check using structural hash comparison.
     *
     * @param other Schema to compare against
     * @return true if structurally compatible
     */
    bool isStructurallyCompatible(const ComponentSchema& other) const {
        return structuralHash == other.structuralHash;
    }

    /**
     * @brief Check if this schema can read from another schema
     *
     * Returns true if all our properties exist in the other schema
     * with matching types and offsets (subset compatibility).
     *
     * This is a more expensive O(n*m) check that validates each field.
     * Application decides what to do with the result.
     *
     * @param other Schema to check compatibility with
     * @return Result with error details if incompatible
     */
    Result<void> canReadFrom(const ComponentSchema& other) const;

    /**
     * @brief Build canonical string representation for hashing
     *
     * Format: "{appId}.{componentName}@{version}{prop1:type1:offset1:size1,...}"
     * - Properties sorted by name (ASCII lexicographic order)
     * - All identifiers must be ASCII [a-zA-Z0-9_]
     * - Used as input for hash computation
     *
     * Example: "MyApp.Transform@1{position:Vec3:0:12,rotation:Quat:12:16}"
     *
     * @return Canonical string representation
     */
    std::string toCanonicalString() const;

    /**
     * @brief Compute structural hash from properties
     *
     * Uses SHA-256 of concatenated field definitions:
     * - For each property: name || type || offset || size
     * - Concatenated in order
     * - Hashed to 128 bits
     *
     * Field order matters - reordering fields produces different hash.
     *
     * @param properties List of property definitions
     * @return 128-bit structural hash
     */
    static PropertyHash computeStructuralHash(
        const std::vector<PropertyDefinition>& properties
    );

    /**
     * @brief Compute component type hash
     *
     * Hash = SHA-256(appId || componentName || schemaVersion || structuralHash)
     * - appId: Application identifier (UTF-8)
     * - componentName: Component type name (UTF-8)
     * - schemaVersion: 4-byte uint32 (big-endian)
     * - structuralHash: 16 bytes (PropertyHash)
     *
     * This provides a globally unique identifier for the component type.
     *
     * @param appId Application identifier
     * @param componentName Component type name
     * @param schemaVersion Schema version number
     * @param structuralHash Structural hash from computeStructuralHash
     * @return 128-bit component type hash
     */
    static ComponentTypeHash computeTypeHash(
        const std::string& appId,
        const std::string& componentName,
        uint32_t schemaVersion,
        const PropertyHash& structuralHash
    );

    /**
     * @brief Create and validate a component schema
     *
     * Validates field definitions and computes hashes automatically.
     * Checks for:
     * - Empty app ID or component name
     * - Empty properties list
     * - Invalid property types
     * - Overlapping field offsets
     * - Fields extending beyond totalSize
     *
     * @param appId Application identifier
     * @param componentName Component type name
     * @param schemaVersion Version number
     * @param properties List of property definitions
     * @param totalSize Total struct size in bytes
     * @param isPublic Whether to publish for discovery (default: false)
     * @return Result with ComponentSchema on success, error on validation failure
     *
     * @code
     * std::vector<PropertyDefinition> transformProps = {
     *     {"position", PropertyType::Vec3, 0, 12},
     *     {"rotation", PropertyType::Quat, 12, 16},
     *     {"scale", PropertyType::Vec3, 28, 12}
     * };
     *
     * auto result = ComponentSchema::create(
     *     "CanvasEngine",
     *     "Transform",
     *     1,
     *     transformProps,
     *     40,
     *     true  // Public
     * );
     *
     * if (result.success()) {
     *     // Use result.value
     * }
     * @endcode
     */
    static Result<ComponentSchema> create(
        const std::string& appId,
        const std::string& componentName,
        uint32_t schemaVersion,
        const std::vector<PropertyDefinition>& properties,
        size_t totalSize,
        bool isPublic = false
    );
};

} // namespace Networking
} // namespace EntropyEngine
