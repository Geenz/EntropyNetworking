/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyRegistry.h"
#include <mutex>

namespace EntropyEngine {
namespace Networking {

Result<void> PropertyRegistry::registerProperty(PropertyMetadata metadata) {
    // Validate enum before acquiring lock
    auto isValidType = [](PropertyType t) {
        switch (t) {
            case PropertyType::Int32: case PropertyType::Int64:
            case PropertyType::Float32: case PropertyType::Float64:
            case PropertyType::Vec2: case PropertyType::Vec3: case PropertyType::Vec4:
            case PropertyType::Quat: case PropertyType::String:
            case PropertyType::Bool: case PropertyType::Bytes:
                return true;
            default: return false;
        }
    };

    if (!isValidType(metadata.type)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid property type");
    }

    // Validate metadata before acquiring lock
    if (metadata.componentType.empty() || metadata.componentType.size() > MAX_NAME_LENGTH) {
        return Result<void>::err(
            NetworkError::InvalidParameter,
            "Invalid componentType length"
        );
    }

    if (metadata.propertyName.empty() || metadata.propertyName.size() > MAX_NAME_LENGTH) {
        return Result<void>::err(
            NetworkError::InvalidParameter,
            "Invalid propertyName length"
        );
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);

    // Check if hash already exists
    auto it = _registry.find(metadata.hash);
    if (it != _registry.end()) {
        const auto& existing = it->second;

        // Check if it's the same property (idempotent re-registration)
        if (existing.matches(metadata)) {
            // Same property - update timestamp and return success
            _registry[metadata.hash].registeredAt = metadata.registeredAt;
            return Result<void>::ok();
        }

        // Different metadata = collision
        return Result<void>::err(
            NetworkError::HashCollision,
            "Property hash collision for (" + std::to_string(metadata.hash.high) + ":" + std::to_string(metadata.hash.low) + ")\n"
            "Existing: entity=" + std::to_string(existing.entityId) +
                ", component=" + existing.componentType +
                ", property=" + existing.propertyName +
                ", type=" + propertyTypeToString(existing.type) + "\n"
            "Incoming: entity=" + std::to_string(metadata.entityId) +
                ", component=" + metadata.componentType +
                ", property=" + metadata.propertyName +
                ", type=" + propertyTypeToString(metadata.type)
        );
    }

    // Check resource limits before inserting
    auto entIt = _entityProperties.find(metadata.entityId);
    if (entIt != _entityProperties.end() && entIt->second.size() >= MAX_PROPERTIES_PER_ENTITY) {
        return Result<void>::err(NetworkError::ResourceLimitExceeded, "Entity property limit exceeded");
    }
    if (_registry.size() >= MAX_TOTAL_PROPERTIES) {
        return Result<void>::err(NetworkError::ResourceLimitExceeded, "Global property limit exceeded");
    }

    // Register new property: emplace returns iterator to inserted element
    // Success is guaranteed here since we verified hash doesn't exist and hold exclusive lock
    auto [insertedIt, _] = _registry.emplace(metadata.hash, std::move(metadata));

    // Track by entity ID for bulk unregister (access from emplaced element)
    _entityProperties[insertedIt->second.entityId].insert(insertedIt->first);

    return Result<void>::ok();
}

bool PropertyRegistry::isRegistered(PropertyHash hash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _registry.find(hash) != _registry.end();
}

std::optional<PropertyMetadata> PropertyRegistry::lookup(PropertyHash hash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto it = _registry.find(hash);
    if (it != _registry.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool PropertyRegistry::validateType(PropertyHash hash, PropertyType expectedType) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto it = _registry.find(hash);
    if (it == _registry.end()) {
        return false; // Property not registered
    }

    return it->second.type == expectedType;
}

Result<void> PropertyRegistry::validatePropertyValue(
    PropertyHash hash,
    const PropertyValue& value) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Check if property is registered
    auto it = _registry.find(hash);
    if (it == _registry.end()) {
        return Result<void>::err(
            NetworkError::UnknownProperty,
            "Property hash not registered"
        );
    }

    const auto& metadata = it->second;

    // Get the actual type from the variant
    PropertyType actualType = getPropertyType(value);

    // Validate type matches
    if (actualType != metadata.type) {
        return Result<void>::err(
            NetworkError::TypeMismatch,
            std::string("Property type mismatch: expected ") +
            propertyTypeToString(metadata.type) + ", got " +
            propertyTypeToString(actualType)
        );
    }

    return Result<void>::ok();
}

std::vector<PropertyHash> PropertyRegistry::getEntityProperties(uint64_t entityId) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto it = _entityProperties.find(entityId);
    if (it == _entityProperties.end()) {
        return {}; // Entity not found
    }

    // Convert set to vector
    return std::vector<PropertyHash>(it->second.begin(), it->second.end());
}

std::vector<PropertyHash> PropertyRegistry::unregisterEntity(uint64_t entityId) {
    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto it = _entityProperties.find(entityId);
    if (it == _entityProperties.end()) {
        return {}; // Entity not found
    }

    // Collect hashes to return
    std::vector<PropertyHash> removedHashes(it->second.begin(), it->second.end());

    // Remove all properties for this entity from registry
    for (const auto& hash : it->second) {
        _registry.erase(hash);
    }

    // Remove entity tracking
    _entityProperties.erase(it);

    return removedHashes;
}

bool PropertyRegistry::unregisterProperty(PropertyHash hash) {
    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto it = _registry.find(hash);
    if (it == _registry.end()) {
        return false; // Property not found
    }

    // Get entity ID before erasing from registry
    uint64_t entityId = it->second.entityId;

    // Remove from registry
    _registry.erase(it);

    // Remove from entity tracking
    auto entityIt = _entityProperties.find(entityId);
    if (entityIt != _entityProperties.end()) {
        entityIt->second.erase(hash);

        // If entity has no more properties, remove entity entry
        if (entityIt->second.empty()) {
            _entityProperties.erase(entityIt);
        }
    }

    return true;
}

std::vector<PropertyMetadata> PropertyRegistry::getAllProperties() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    std::vector<PropertyMetadata> result;
    result.reserve(_registry.size());

    for (const auto& [hash, metadata] : _registry) {
        result.push_back(metadata);
    }

    return result;
}

size_t PropertyRegistry::size() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _registry.size();
}

bool PropertyRegistry::empty() const {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _registry.empty();
}

void PropertyRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(_mutex);
    _registry.clear();
    _entityProperties.clear();
}

} // namespace Networking
} // namespace EntropyEngine
