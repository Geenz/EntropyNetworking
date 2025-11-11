/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "ComponentSchema.h"
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <optional>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Thread-safe registry for component schemas
 *
 * Provides opt-in schema discovery and compatibility validation.
 * Applications can:
 * - Register schemas (public or private)
 * - Query schemas by hash
 * - Find compatible schemas
 * - Validate compatibility
 *
 * Default behavior: Schemas are private unless explicitly published.
 *
 * Thread Safety:
 * - All operations are thread-safe using shared_mutex
 * - Multiple concurrent readers, single writer
 * - Pre-lock validation for performance
 *
 * Memory Efficiency:
 * - Single registry instance per server
 * - Structural hash indexing for fast compatibility queries
 * - Public/private schema separation
 */
class ComponentSchemaRegistry {
public:
    /// Callback invoked when a schema is published (made public)
    using SchemaPublishedCallback = std::function<void(ComponentTypeHash typeHash,
                                                        const ComponentSchema& schema)>;

    /// Callback invoked when a schema is unpublished (made private)
    using SchemaUnpublishedCallback = std::function<void(ComponentTypeHash typeHash)>;

    ComponentSchemaRegistry() = default;
    ~ComponentSchemaRegistry() = default;

    // Non-copyable, non-movable
    ComponentSchemaRegistry(const ComponentSchemaRegistry&) = delete;
    ComponentSchemaRegistry& operator=(const ComponentSchemaRegistry&) = delete;
    ComponentSchemaRegistry(ComponentSchemaRegistry&&) = delete;
    ComponentSchemaRegistry& operator=(ComponentSchemaRegistry&&) = delete;

    /**
     * @brief Register a component schema
     *
     * Schemas are private by default unless isPublic=true.
     * Re-registering the same schema (same typeHash) is idempotent.
     *
     * @param schema Component schema to register
     * @return Result with ComponentTypeHash on success
     *
     * @threadsafety Thread-safe (write lock)
     *
     * @code
     * ComponentSchemaRegistry registry;
     * auto schema = ComponentSchema::create("App", "Transform", 1, properties, 40, false);
     * if (schema.success()) {
     *     auto result = registry.registerSchema(schema.value());
     *     if (result.success()) {
     *         ENTROPY_LOG_INFO("Registered schema: {}", toString(result.value()));
     *     }
     * }
     * @endcode
     */
    Result<ComponentTypeHash> registerSchema(const ComponentSchema& schema);

    /**
     * @brief Lookup schema by type hash
     *
     * Returns schema if registered (public or private).
     *
     * @param typeHash Component type hash
     * @return Optional schema if found
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::optional<ComponentSchema> getSchema(ComponentTypeHash typeHash) const;

    /**
     * @brief Get all public schemas
     *
     * Returns only schemas marked as public.
     * Used for schema discovery by other applications.
     *
     * @return Vector of public schemas
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::vector<ComponentSchema> getPublicSchemas() const;

    /**
     * @brief Find schemas compatible with given type hash
     *
     * Returns all public schemas that are structurally compatible
     * (matching structural hash).
     *
     * @param typeHash Component type hash to match
     * @return Vector of compatible component type hashes
     *
     * @threadsafety Thread-safe (read lock)
     */
    std::vector<ComponentTypeHash> findCompatibleSchemas(
        ComponentTypeHash typeHash
    ) const;

    /**
     * @brief Check if two schemas are structurally compatible
     *
     * Fast check using structural hash comparison.
     *
     * @param a First component type hash
     * @param b Second component type hash
     * @return true if structurally compatible
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool areCompatible(ComponentTypeHash a, ComponentTypeHash b) const;

    /**
     * @brief Validate detailed compatibility
     *
     * Performs field-by-field validation.
     * More expensive than structural hash check.
     * Application decides what to do with the result.
     *
     * @param source Source schema
     * @param target Target schema
     * @return Result with detailed error messages if incompatible
     *
     * @threadsafety Thread-safe (read lock)
     */
    Result<void> validateDetailedCompatibility(
        ComponentTypeHash source,
        ComponentTypeHash target
    ) const;

    /**
     * @brief Check if schema is registered
     *
     * @param typeHash Component type hash
     * @return true if registered (public or private)
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool isRegistered(ComponentTypeHash typeHash) const;

    /**
     * @brief Check if schema is public
     *
     * @param typeHash Component type hash
     * @return true if schema is published for discovery
     *
     * @threadsafety Thread-safe (read lock)
     */
    bool isPublic(ComponentTypeHash typeHash) const;

    /**
     * @brief Publish a private schema
     *
     * Makes a previously private schema public for discovery.
     *
     * @param typeHash Component type hash
     * @return Result indicating success or error
     *
     * @threadsafety Thread-safe (write lock)
     */
    Result<void> publishSchema(ComponentTypeHash typeHash);

    /**
     * @brief Unpublish a public schema
     *
     * Makes a public schema private (no longer discoverable).
     *
     * @param typeHash Component type hash
     * @return Result indicating success or error
     *
     * @threadsafety Thread-safe (write lock)
     */
    Result<void> unpublishSchema(ComponentTypeHash typeHash);

    /**
     * @brief Get total schema count
     *
     * @return Number of registered schemas (public + private)
     *
     * @threadsafety Thread-safe (read lock)
     */
    size_t schemaCount() const;

    /**
     * @brief Get public schema count
     *
     * @return Number of public schemas
     *
     * @threadsafety Thread-safe (read lock)
     */
    size_t publicSchemaCount() const;

    /**
     * @brief Get consistent snapshot of registry stats
     *
     * Returns all stats under a single lock to ensure consistency.
     * Useful for concurrent readers that need consistent view of state.
     *
     * @param[out] totalCount Total number of schemas
     * @param[out] publicCount Number of public schemas
     * @param[out] publicSchemas Vector of public schemas
     *
     * @threadsafety Thread-safe (read lock)
     */
    void getStats(size_t& totalCount, size_t& publicCount, std::vector<ComponentSchema>& publicSchemas) const;

    /**
     * @brief Set callback for schema publish events
     *
     * Called when publishSchema() makes a private schema public.
     * Useful for broadcasting schema availability to connected clients.
     *
     * @param callback Function to call on publish (invoked under lock)
     *
     * @threadsafety Not thread-safe - set before publishing schemas
     */
    void setSchemaPublishedCallback(SchemaPublishedCallback callback);

    /**
     * @brief Set callback for schema unpublish events
     *
     * Called when unpublishSchema() makes a public schema private.
     * Useful for notifying clients that schema is no longer available.
     *
     * @param callback Function to call on unpublish (invoked under lock)
     *
     * @threadsafety Not thread-safe - set before unpublishing schemas
     */
    void setSchemaUnpublishedCallback(SchemaUnpublishedCallback callback);

private:
    mutable std::shared_mutex _mutex;

    /// Map: ComponentTypeHash → ComponentSchema
    std::unordered_map<ComponentTypeHash, ComponentSchema> _schemas;

    /// Index: StructuralHash → Set<ComponentTypeHash> (for compatibility lookup)
    std::unordered_multimap<PropertyHash, ComponentTypeHash> _structuralIndex;

    /// Set of public schema hashes (for discovery)
    std::unordered_set<ComponentTypeHash> _publicSchemas;

    /// Callbacks for schema lifecycle events
    SchemaPublishedCallback _schemaPublishedCallback;
    SchemaUnpublishedCallback _schemaUnpublishedCallback;
};

} // namespace Networking
} // namespace EntropyEngine
