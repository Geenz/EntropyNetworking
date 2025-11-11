/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ComponentSchemaRegistry.h"
#include <Logging/Logger.h>
#include <algorithm>
#include <format>

namespace EntropyEngine {
namespace Networking {

Result<ComponentTypeHash> ComponentSchemaRegistry::registerSchema(const ComponentSchema& schema) {
    // Pre-lock validation
    if (schema.typeHash.isNull()) {
        ENTROPY_LOG_ERROR("Cannot register schema with null type hash");
        return Result<ComponentTypeHash>::err(
            NetworkError::InvalidParameter,
            "Schema type hash is null"
        );
    }

    if (schema.structuralHash.isNull()) {
        ENTROPY_LOG_ERROR("Cannot register schema with null structural hash");
        return Result<ComponentTypeHash>::err(
            NetworkError::InvalidParameter,
            "Schema structural hash is null"
        );
    }

    // Acquire write lock
    std::unique_lock<std::shared_mutex> lock(_mutex);

    // Check if schema already exists
    auto it = _schemas.find(schema.typeHash);
    if (it != _schemas.end()) {
        // Schema already registered - check if it's identical (idempotent)
        const auto& existing = it->second;

        if (existing.structuralHash == schema.structuralHash &&
            existing.appId == schema.appId &&
            existing.componentName == schema.componentName &&
            existing.schemaVersion == schema.schemaVersion) {
            // Identical schema - idempotent registration
            ENTROPY_LOG_DEBUG_CAT("ComponentSchemaRegistry",
                std::format("Schema {}.{} v{} already registered (idempotent)",
                    schema.appId, schema.componentName, schema.schemaVersion));
            return Result<ComponentTypeHash>::ok(schema.typeHash);
        } else {
            // Different schema with same type hash - conflict
            std::string errorMsg = std::format(
                "Schema conflict: type hash {} already registered with different content",
                toString(schema.typeHash));
            ENTROPY_LOG_ERROR_CAT("ComponentSchemaRegistry", errorMsg);
            return Result<ComponentTypeHash>::err(
                NetworkError::SchemaAlreadyExists,
                errorMsg
            );
        }
    }

    // Register new schema
    _schemas[schema.typeHash] = schema;

    // Add to structural index
    _structuralIndex.emplace(schema.structuralHash, schema.typeHash);

    // Add to public schemas if needed
    if (schema.isPublic) {
        _publicSchemas.insert(schema.typeHash);
    }

    std::string logMsg = std::format(
        "Registered schema {}.{} v{} (public: {}, properties: {})",
        schema.appId, schema.componentName, schema.schemaVersion,
        schema.isPublic, schema.properties.size());
    ENTROPY_LOG_INFO_CAT("ComponentSchemaRegistry", logMsg);

    return Result<ComponentTypeHash>::ok(schema.typeHash);
}

std::optional<ComponentSchema> ComponentSchemaRegistry::getSchema(ComponentTypeHash typeHash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto it = _schemas.find(typeHash);
    if (it != _schemas.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<ComponentSchema> ComponentSchemaRegistry::getPublicSchemas() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::vector<ComponentSchema> publicSchemas;
    publicSchemas.reserve(_publicSchemas.size());

    for (const auto& typeHash : _publicSchemas) {
        auto it = _schemas.find(typeHash);
        if (it != _schemas.end()) {
            publicSchemas.push_back(it->second);
        }
    }

    return publicSchemas;
}

std::vector<ComponentTypeHash> ComponentSchemaRegistry::findCompatibleSchemas(
    ComponentTypeHash typeHash) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Find the schema
    auto schemaIt = _schemas.find(typeHash);
    if (schemaIt == _schemas.end()) {
        return {};
    }

    const auto& schema = schemaIt->second;

    // Find all schemas with matching structural hash
    std::vector<ComponentTypeHash> compatible;

    auto range = _structuralIndex.equal_range(schema.structuralHash);
    for (auto it = range.first; it != range.second; ++it) {
        const auto& candidateHash = it->second;

        // Skip self
        if (candidateHash == typeHash) {
            continue;
        }

        // Only return public schemas
        if (_publicSchemas.find(candidateHash) != _publicSchemas.end()) {
            compatible.push_back(candidateHash);
        }
    }

    return compatible;
}

bool ComponentSchemaRegistry::areCompatible(ComponentTypeHash a, ComponentTypeHash b) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto aIt = _schemas.find(a);
    auto bIt = _schemas.find(b);

    if (aIt == _schemas.end() || bIt == _schemas.end()) {
        return false;
    }

    return aIt->second.isStructurallyCompatible(bIt->second);
}

Result<void> ComponentSchemaRegistry::validateDetailedCompatibility(
    ComponentTypeHash source,
    ComponentTypeHash target) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Find source schema
    auto sourceIt = _schemas.find(source);
    if (sourceIt == _schemas.end()) {
        std::string errorMsg = std::format("Source schema {} not found", toString(source));
        return Result<void>::err(NetworkError::SchemaNotFound, errorMsg);
    }

    // Find target schema
    auto targetIt = _schemas.find(target);
    if (targetIt == _schemas.end()) {
        std::string errorMsg = std::format("Target schema {} not found", toString(target));
        return Result<void>::err(NetworkError::SchemaNotFound, errorMsg);
    }

    // Validate that target can read from source
    return targetIt->second.canReadFrom(sourceIt->second);
}

bool ComponentSchemaRegistry::isRegistered(ComponentTypeHash typeHash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _schemas.find(typeHash) != _schemas.end();
}

bool ComponentSchemaRegistry::isPublic(ComponentTypeHash typeHash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _publicSchemas.find(typeHash) != _publicSchemas.end();
}

Result<void> ComponentSchemaRegistry::publishSchema(ComponentTypeHash typeHash) {
    // Copy callback and schema for invocation outside lock
    SchemaPublishedCallback callback;
    std::optional<ComponentSchema> schema;

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);

        // Check if schema exists
        auto it = _schemas.find(typeHash);
        if (it == _schemas.end()) {
            std::string errorMsg = std::format("Schema {} not found", toString(typeHash));
            ENTROPY_LOG_ERROR_CAT("ComponentSchemaRegistry", errorMsg);
            return Result<void>::err(NetworkError::SchemaNotFound, errorMsg);
        }

        // Check if already public
        if (_publicSchemas.find(typeHash) != _publicSchemas.end()) {
            // Already public - idempotent
            return Result<void>::ok();
        }

        // Mark as public
        _publicSchemas.insert(typeHash);

        // Update schema's isPublic flag
        it->second.isPublic = true;

        std::string logMsg = std::format(
            "Published schema {}.{} v{}",
            it->second.appId, it->second.componentName, it->second.schemaVersion);
        ENTROPY_LOG_INFO_CAT("ComponentSchemaRegistry", logMsg);

        // Copy callback and schema for invocation outside lock
        callback = _schemaPublishedCallback;
        schema = it->second;
    }

    // Invoke callback outside lock to avoid potential deadlock
    if (callback && schema) {
        callback(typeHash, *schema);
    }

    return Result<void>::ok();
}

Result<void> ComponentSchemaRegistry::unpublishSchema(ComponentTypeHash typeHash) {
    // Copy callback for invocation outside lock
    SchemaUnpublishedCallback callback;
    bool wasPublic = false;

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);

        // Check if schema exists
        auto it = _schemas.find(typeHash);
        if (it == _schemas.end()) {
            std::string errorMsg = std::format("Schema {} not found", toString(typeHash));
            ENTROPY_LOG_ERROR_CAT("ComponentSchemaRegistry", errorMsg);
            return Result<void>::err(NetworkError::SchemaNotFound, errorMsg);
        }

        // Check if already private
        if (_publicSchemas.find(typeHash) == _publicSchemas.end()) {
            // Already private - idempotent
            return Result<void>::ok();
        }

        // Mark as private
        _publicSchemas.erase(typeHash);
        wasPublic = true;

        // Update schema's isPublic flag
        it->second.isPublic = false;

        std::string logMsg = std::format(
            "Unpublished schema {}.{} v{}",
            it->second.appId, it->second.componentName, it->second.schemaVersion);
        ENTROPY_LOG_INFO_CAT("ComponentSchemaRegistry", logMsg);

        // Copy callback for invocation outside lock
        callback = _schemaUnpublishedCallback;
    }

    // Invoke callback outside lock to avoid potential deadlock
    if (callback && wasPublic) {
        callback(typeHash);
    }

    return Result<void>::ok();
}

size_t ComponentSchemaRegistry::schemaCount() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _schemas.size();
}

size_t ComponentSchemaRegistry::publicSchemaCount() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _publicSchemas.size();
}

void ComponentSchemaRegistry::getStats(size_t& totalCount, size_t& publicCount,
                                       std::vector<ComponentSchema>& publicSchemas) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Get counts
    totalCount = _schemas.size();
    publicCount = _publicSchemas.size();

    // Get public schemas
    publicSchemas.clear();
    publicSchemas.reserve(publicCount);

    for (const auto& typeHash : _publicSchemas) {
        auto it = _schemas.find(typeHash);
        if (it != _schemas.end()) {
            publicSchemas.push_back(it->second);
        }
    }
}

void ComponentSchemaRegistry::setSchemaPublishedCallback(SchemaPublishedCallback callback) {
    _schemaPublishedCallback = std::move(callback);
}

void ComponentSchemaRegistry::setSchemaUnpublishedCallback(SchemaUnpublishedCallback callback) {
    _schemaUnpublishedCallback = std::move(callback);
}

} // namespace Networking
} // namespace EntropyEngine
