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
 * @brief Thread-safe property registry for network protocol
 *
 * Maintains mappings from property hashes to their metadata (name, type, entity).
 * Used by Canvas and Portals to validate property updates and provide debugging info.
 */

#pragma once

#include "PropertyHash.h"
#include "PropertyTypes.h"
#include "NetworkTypes.h"
#include "ErrorCodes.h"
#include <unordered_map>
#include <vector>
#include <optional>
#include <shared_mutex>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Metadata for a registered property
 *
 * Stored in the registry to enable type validation and debugging.
 */
struct PropertyMetadata {
    std::string propertyName;   ///< Field name (e.g., "position")
    PropertyType type;          ///< Property type
    EntityId entityId;          ///< Owner entity ID
    AppId appId;                ///< Application ID
    TypeName typeName;          ///< Entity type name (e.g., "Player")

    PropertyMetadata() = default;
    PropertyMetadata(
        std::string name,
        PropertyType t,
        EntityId entity,
        AppId app,
        TypeName type_name
    ) : propertyName(std::move(name))
      , type(t)
      , entityId(entity)
      , appId(std::move(app))
      , typeName(std::move(type_name))
    {}
};

/**
 * @brief Thread-safe registry mapping property hashes to metadata
 *
 * Properties are registered when entities are created. The registry enables:
 * - Type validation on property updates
 * - Debug information (reverse lookup hash → name)
 * - Entity cleanup (unregister all properties for an entity)
 *
 * Thread Safety: All methods are thread-safe using shared_mutex
 * (multiple readers, single writer).
 *
 * @code
 * PropertyRegistry registry;
 *
 * // Register property
 * auto hash = computePropertyHash(42, "app", "Player", "position");
 * PropertyMetadata meta{"position", PropertyType::Vec3, 42, "app", "Player"};
 * auto result = registry.registerProperty(hash, meta);
 *
 * // Lookup later
 * if (auto found = registry.lookupProperty(hash)) {
 *     // Validate type matches
 *     if (found->type == PropertyType::Vec3) {
 *         // Process update
 *     }
 * }
 *
 * // Cleanup when entity destroyed
 * registry.unregisterEntity(42);
 * @endcode
 */
class PropertyRegistry {
public:
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
     * Adds property metadata to the registry. Detects hash collisions:
     * if the same hash is already registered with different metadata,
     * returns HashCollision error.
     *
     * @param hash Property hash
     * @param metadata Property metadata
     * @return Result indicating success or error (HashCollision, RegistryFull)
     *
     * @threadsafety Thread-safe (write lock)
     */
    Result<void> registerProperty(PropertyHash128 hash, PropertyMetadata metadata);

    /**
     * @brief Lookup property metadata by hash
     *
     * @param hash Property hash
     * @return Optional metadata if found, nullopt otherwise
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::optional<PropertyMetadata> lookupProperty(PropertyHash128 hash) const;

    /**
     * @brief Unregister all properties for an entity
     *
     * Removes all property registrations associated with the given entity ID.
     * Used when an entity is destroyed to clean up registry.
     *
     * @param entityId Entity to unregister
     * @return Number of properties removed
     *
     * @threadsafety Thread-safe (write lock)
     */
    size_t unregisterEntity(EntityId entityId);

    /**
     * @brief Get all registered properties (for debugging)
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
     * @threadsafety Thread-safe (write lock)
     */
    void clear();

private:
    mutable std::shared_mutex _mutex;

    /// Map: PropertyHash128 → PropertyMetadata
    std::unordered_map<PropertyHash128, PropertyMetadata> _registry;

    /// Map: EntityId → vector of PropertyHash128 (for bulk unregister)
    std::unordered_map<EntityId, std::vector<PropertyHash128>> _entityProperties;
};

} // namespace Networking
} // namespace EntropyEngine
