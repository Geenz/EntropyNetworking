/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyRegistry.h"

namespace EntropyEngine {
namespace Networking {

Result<void> PropertyRegistry::registerProperty(PropertyHash128 hash, PropertyMetadata metadata) {
    std::unique_lock<std::shared_mutex> lock(_mutex);

    // Check if hash already exists
    auto it = _registry.find(hash);
    if (it != _registry.end()) {
        // Hash collision: same hash, different metadata
        const auto& existing = it->second;

        // Check if it's the exact same property (re-registration)
        if (existing.propertyName == metadata.propertyName &&
            existing.type == metadata.type &&
            existing.entityId == metadata.entityId &&
            existing.appId == metadata.appId &&
            existing.typeName == metadata.typeName) {
            // Exact match - not an error, just a no-op
            return Result<void>::ok();
        }

        // Different metadata = collision
        return Result<void>::err(
            NetworkError::HashCollision,
            "Property hash collision detected"
        );
    }

    // Register new property
    _registry[hash] = metadata;

    // Track by entity ID for bulk unregister
    _entityProperties[metadata.entityId].push_back(hash);

    return Result<void>::ok();
}

std::optional<PropertyMetadata> PropertyRegistry::lookupProperty(PropertyHash128 hash) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    auto it = _registry.find(hash);
    if (it != _registry.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t PropertyRegistry::unregisterEntity(EntityId entityId) {
    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto it = _entityProperties.find(entityId);
    if (it == _entityProperties.end()) {
        return 0; // Entity not found
    }

    size_t removedCount = 0;

    // Remove all properties for this entity
    for (const auto& hash : it->second) {
        _registry.erase(hash);
        removedCount++;
    }

    // Remove entity tracking
    _entityProperties.erase(it);

    return removedCount;
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
