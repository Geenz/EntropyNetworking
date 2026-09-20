/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file PropertyRegistry.h
 * @brief Thread-safe global property registry for Canvas server
 *
 * The PropertyRegistry is the single source of truth for all properties across
 * all entities in the Canvas ecosystem. It serves as the global type registry,
 * validating property types and preventing malicious updates.
 *
 * Key responsibilities:
 * - Register property instances with their types
 * - Validate property value types on every update (SECURITY CRITICAL)
 * - Track which properties belong to which entities
 * - Enable efficient entity cleanup on destruction
 */

#pragma once

#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ErrorCodes.h"
#include "NetworkTypes.h"
#include "PropertyHash.h"
#include "PropertyTypes.h"

namespace EntropyEngine
{
namespace Networking
{

/**
 * @brief Metadata for a registered property instance
 *
 * Each property instance on each entity gets its own unique hash and metadata.
 * The hash is computed once and stored - never recomputed.
 */
struct PropertyMetadata
{
    PropertyHash hash;                ///< Unique 128-bit identifier (computed once)
    uint64_t entityId;                ///< Owner entity ID
    ComponentTypeHash componentType;  ///< Component type hash from ComponentSchema
    std::string propertyName;         ///< Property name (e.g., "position", "health")
    PropertyType type;                ///< Property type (e.g., Vec3, Float32)
    uint64_t registeredAt;            ///< Timestamp when registered (microseconds since epoch)

    PropertyMetadata() = default;
    PropertyMetadata(PropertyHash h, uint64_t entity, ComponentTypeHash component, std::string prop, PropertyType t,
                     uint64_t timestamp)
        : hash(h),
          entityId(entity),
          componentType(component),
          propertyName(std::move(prop)),
          type(t),
          registeredAt(timestamp) {}

    /**
     * @brief Check if metadata matches another instance (for idempotent registration)
     *
     * Compares all fields except registeredAt.
     */
    bool matches(const PropertyMetadata& other) const {
        return hash == other.hash && entityId == other.entityId && componentType == other.componentType &&
               propertyName == other.propertyName && type == other.type;
    }
};

/**
 * @brief Thread-safe global property registry
 *
 * The PropertyRegistry is Canvas's single source of truth for all properties.
 * One instance on the server tracks ALL properties across ALL entities.
 *
 * Registration is idempotent: re-registering the same property (same hash + metadata)
 * updates the timestamp and returns success. This allows clients to re-register
 * on reconnection without errors.
 *
 * SECURITY: Always call validatePropertyValue() before applying property updates.
 * This prevents bad actors from crashing the server by sending wrong types.
 *
 * Thread Safety: All methods are thread-safe using shared_mutex
 * (multiple readers, single writer).
 *
 * @code
 * PropertyRegistry registry;
 *
 * // Register property once when entity created
 * auto typeHash = transformSchema.typeHash;  // From ComponentSchema
 * auto hash = computePropertyHash(42, typeHash, "position");
 * PropertyMetadata meta{hash, 42, typeHash, "position", PropertyType::Vec3, now};
 * auto result = registry.registerProperty(meta);
 *
 * // SECURITY: Validate on every update
 * PropertyValue incomingValue = getFromNetwork();
 * auto validation = registry.validatePropertyValue(hash, incomingValue);
 * if (validation.success()) {
 *     applyUpdate(hash, incomingValue);  // Safe
 * } else {
 *     LOG_ERROR("Rejected malicious update: " + validation.errorMessage);
 * }
 *
 * // Cleanup when entity destroyed
 * auto removedHashes = registry.unregisterEntity(42);
 * @endcode
 */
class PropertyRegistry
{
public:
    // Resource limits (configurable)
    static constexpr size_t MAX_PROPERTIES_PER_ENTITY = 1000;
    static constexpr size_t MAX_TOTAL_PROPERTIES = 1'000'000;
    static constexpr size_t MAX_NAME_LENGTH = 256;

    PropertyRegistry() = default;
    ~PropertyRegistry() = default;

    // Non-copyable, non-movable (due to mutex)
    PropertyRegistry(const PropertyRegistry&) = delete;
    PropertyRegistry& operator=(const PropertyRegistry&) = delete;
    PropertyRegistry(PropertyRegistry&&) = delete;
    PropertyRegistry& operator=(PropertyRegistry&&) = delete;

    /**
     * @brief Register a property in the registry
     *
     * Registers a property instance with its metadata. This is idempotent:
     * re-registering the same property (same hash + metadata) updates the
     * timestamp and returns success.
     *
     * Returns error if:
     * - Hash collision (same hash, different metadata)
     * - Invalid metadata (empty componentType or propertyName)
     * - Invalid type (PropertyType enum out of range)
     *
     * @param metadata Property metadata (includes hash)
     * @return Result indicating success or error
     *
     * @threadsafety Thread-safe (write lock)
     */
    Result<void> registerProperty(PropertyMetadata metadata);

    /**
     * @brief Check if a property is registered
     *
     * @param hash Property hash
     * @return true if registered, false otherwise
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool isRegistered(PropertyHash hash) const;

    /**
     * @brief Lookup property metadata by hash
     *
     * @param hash Property hash
     * @return Optional metadata if found, nullopt otherwise
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::optional<PropertyMetadata> lookup(PropertyHash hash) const;

    /**
     * @brief Validate property type (hash → type check)
     *
     * Checks that the registered property has the expected type.
     *
     * @param hash Property hash
     * @param expectedType Expected type
     * @return true if property exists and type matches, false otherwise
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool validateType(PropertyHash hash, PropertyType expectedType) const;

    /**
     * @brief Validate property value type (SECURITY CRITICAL)
     *
     * Validates that a property value's type matches the registered type.
     * ALWAYS call this before applying property updates to prevent bad actors
     * from crashing the server with type confusion attacks.
     *
     * Returns error if:
     * - Property not registered (UnknownProperty)
     * - Type mismatch (TypeMismatch)
     *
     * @param hash Property hash
     * @param value Property value to validate
     * @return Result indicating success or error
     *
     * @threadsafety Thread-safe (read lock)
     *
     * @code
     * // SECURE: Validate before applying
     * auto validation = registry.validatePropertyValue(hash, value);
     * if (validation.success()) {
     *     applyUpdate(hash, value);
     * } else {
     *     LOG_ERROR("Rejected: " + validation.errorMessage);
     * }
     *
     * // INSECURE: NEVER do this!
     * // applyUpdate(hash, value);  // ← Can crash from type mismatch!
     * @endcode
     */
    Result<void> validatePropertyValue(PropertyHash hash, const PropertyValue& value) const;

    /**
     * @brief Get all property hashes for an entity
     *
     * Returns a vector of all property hashes registered for the given entity.
     * Useful for debugging and entity inspection.
     *
     * @param entityId Entity ID
     * @return Vector of property hashes (empty if entity not found)
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::vector<PropertyHash> getEntityProperties(uint64_t entityId) const;

    /**
     * @brief Unregister all properties for an entity
     *
     * Removes all property registrations for the given entity.
     * Used when an entity is destroyed to clean up the registry.
     *
     * Returns a vector of all removed property hashes. Callers can use this
     * to remove corresponding data from ECS, network buffers, etc.
     *
     * @param entityId Entity to unregister
     * @return Vector of removed property hashes
     *
     * @threadsafety Thread-safe (write lock)
     */
    std::vector<PropertyHash> unregisterEntity(uint64_t entityId);

    /**
     * @brief Unregister a single property
     *
     * Removes a single property registration.
     *
     * @param hash Property hash to unregister
     * @return true if property was removed, false if not found
     *
     * @threadsafety Thread-safe (write lock)
     */
    bool unregisterProperty(PropertyHash hash);

    /**
     * @brief Get all registered properties (for debugging)
     *
     * Returns a vector of all property metadata in the registry.
     * Warning: This copies the entire registry - expensive for large registries.
     *
     * @return Vector of all property metadata
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::vector<PropertyMetadata> getAllProperties() const;

    /**
     * @brief Get total number of registered properties
     *
     * @return Property count
     *
     * @threadsafety Thread-safe (read lock)
     */
    size_t size() const;

    /**
     * @brief Check if registry is empty
     *
     * @return true if no properties registered
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool empty() const;

    /**
     * @brief Clear all registrations
     *
     * Removes all properties from the registry.
     *
     * @threadsafety Thread-safe (write lock)
     */
    void clear();

private:
    mutable std::shared_mutex _mutex;

    /// Map: PropertyHash → PropertyMetadata
    std::unordered_map<PropertyHash, PropertyMetadata> _registry;

    /// Map: EntityId → set of PropertyHash (for entity cleanup)
    std::unordered_map<uint64_t, std::unordered_set<PropertyHash>> _entityProperties;
};

}  // namespace Networking
}  // namespace EntropyEngine
