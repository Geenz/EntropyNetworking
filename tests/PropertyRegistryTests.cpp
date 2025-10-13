/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Core/PropertyRegistry.h"

using namespace EntropyEngine::Networking;

TEST(PropertyRegistryTests, RegisterAndLookup) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "app", "Player", "position");
    PropertyMetadata meta{"position", PropertyType::Vec3, 42, "app", "Player"};

    auto result = registry.registerProperty(hash, meta);
    EXPECT_TRUE(result.success());

    auto found = registry.lookupProperty(hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->propertyName, "position");
    EXPECT_EQ(found->type, PropertyType::Vec3);
    EXPECT_EQ(found->entityId, 42);
}

TEST(PropertyRegistryTests, LookupNonExistent) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(999, "app", "Type", "field");
    auto found = registry.lookupProperty(hash);

    EXPECT_FALSE(found.has_value());
}

TEST(PropertyRegistryTests, HashCollisionDetection) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "app", "Player", "position");

    PropertyMetadata meta1{"position", PropertyType::Vec3, 42, "app", "Player"};
    PropertyMetadata meta2{"velocity", PropertyType::Vec3, 42, "app", "Player"};  // Different metadata

    auto result1 = registry.registerProperty(hash, meta1);
    EXPECT_TRUE(result1.success());

    // Attempt to register same hash with different metadata
    auto result2 = registry.registerProperty(hash, meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::HashCollision);
}

TEST(PropertyRegistryTests, ReregisterSameProperty) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "app", "Player", "position");
    PropertyMetadata meta{"position", PropertyType::Vec3, 42, "app", "Player"};

    auto result1 = registry.registerProperty(hash, meta);
    EXPECT_TRUE(result1.success());

    // Re-registering exact same property should succeed (no-op)
    auto result2 = registry.registerProperty(hash, meta);
    EXPECT_TRUE(result2.success());
}

TEST(PropertyRegistryTests, UnregisterEntity) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(42, "app", "Player", "position");
    auto hash2 = computePropertyHash(42, "app", "Player", "velocity");
    auto hash3 = computePropertyHash(43, "app", "Player", "position");

    PropertyMetadata meta1{"position", PropertyType::Vec3, 42, "app", "Player"};
    PropertyMetadata meta2{"velocity", PropertyType::Vec3, 42, "app", "Player"};
    PropertyMetadata meta3{"position", PropertyType::Vec3, 43, "app", "Player"};

    registry.registerProperty(hash1, meta1);
    registry.registerProperty(hash2, meta2);
    registry.registerProperty(hash3, meta3);

    EXPECT_EQ(registry.size(), 3);

    // Unregister entity 42
    size_t removed = registry.unregisterEntity(42);
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(registry.size(), 1);

    // Entity 42's properties should be gone
    EXPECT_FALSE(registry.lookupProperty(hash1).has_value());
    EXPECT_FALSE(registry.lookupProperty(hash2).has_value());

    // Entity 43's property should still exist
    EXPECT_TRUE(registry.lookupProperty(hash3).has_value());
}

TEST(PropertyRegistryTests, Size) {
    PropertyRegistry registry;

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());

    auto hash1 = computePropertyHash(1, "app", "Type", "field1");
    auto hash2 = computePropertyHash(2, "app", "Type", "field2");

    registry.registerProperty(hash1, PropertyMetadata{"field1", PropertyType::Int32, 1, "app", "Type"});
    EXPECT_EQ(registry.size(), 1);
    EXPECT_FALSE(registry.empty());

    registry.registerProperty(hash2, PropertyMetadata{"field2", PropertyType::Int32, 2, "app", "Type"});
    EXPECT_EQ(registry.size(), 2);
}

TEST(PropertyRegistryTests, Clear) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(1, "app", "Type", "field1");
    auto hash2 = computePropertyHash(2, "app", "Type", "field2");

    registry.registerProperty(hash1, PropertyMetadata{"field1", PropertyType::Int32, 1, "app", "Type"});
    registry.registerProperty(hash2, PropertyMetadata{"field2", PropertyType::Int32, 2, "app", "Type"});

    EXPECT_EQ(registry.size(), 2);

    registry.clear();

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());
    EXPECT_FALSE(registry.lookupProperty(hash1).has_value());
}

TEST(PropertyRegistryTests, GetAllProperties) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(1, "app", "Type", "field1");
    auto hash2 = computePropertyHash(2, "app", "Type", "field2");

    registry.registerProperty(hash1, PropertyMetadata{"field1", PropertyType::Int32, 1, "app", "Type"});
    registry.registerProperty(hash2, PropertyMetadata{"field2", PropertyType::Float32, 2, "app", "Type"});

    auto all = registry.getAllProperties();

    EXPECT_EQ(all.size(), 2);
}
