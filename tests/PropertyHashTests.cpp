/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include "../src/Networking/Core/ComponentSchema.h"
#include "../src/Networking/Core/PropertyHash.h"

using namespace EntropyEngine::Networking;

// Helper function to create a ComponentTypeHash for testing
static ComponentTypeHash createTestComponentType(const std::string& appId, const std::string& componentName) {
    // Create a minimal valid schema with one dummy property
    std::vector<PropertyDefinition> props = {{"dummy", PropertyType::Int32, 0, 4}};
    auto schemaResult = ComponentSchema::create(appId, componentName, 1, props, 4, false);
    if (schemaResult.success()) {
        return schemaResult.value.typeHash;
    }
    return ComponentTypeHash{0, 0};
}

TEST(PropertyHashTests, Determinism) {
    // Same inputs should produce same hash
    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(42, typeHash, "position");

    EXPECT_EQ(hash1, hash2);
}

TEST(PropertyHashTests, DifferentEntityIds) {
    // Different entity IDs should produce different hashes
    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(43, typeHash, "position");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, DifferentComponentTypes) {
    // Different component types should produce different hashes
    auto typeHash1 = createTestComponentType("TestApp", "Player");
    auto typeHash2 = createTestComponentType("TestApp", "Enemy");
    auto hash1 = computePropertyHash(42, typeHash1, "position");
    auto hash2 = computePropertyHash(42, typeHash2, "position");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, DifferentPropertyNames) {
    // Different property names should produce different hashes
    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(42, typeHash, "velocity");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, NotNull) {
    // Valid hash should not be null
    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");

    EXPECT_FALSE(hash.isNull());
}

TEST(PropertyHashTests, NullHash) {
    // Default-constructed hash should be null
    PropertyHash hash;

    EXPECT_TRUE(hash.isNull());
    EXPECT_EQ(hash.high, 0);
    EXPECT_EQ(hash.low, 0);
}

TEST(PropertyHashTests, HashMapUsage) {
    // PropertyHash should be usable in std::unordered_map
    std::unordered_map<PropertyHash, std::string> map;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(43, typeHash, "position");

    map[hash1] = "value1";
    map[hash2] = "value2";

    EXPECT_EQ(map[hash1], "value1");
    EXPECT_EQ(map[hash2], "value2");
    EXPECT_EQ(map.size(), 2);
}

TEST(PropertyHashTests, Comparison) {
    // Test comparison operators
    auto typeHash = createTestComponentType("TestApp", "Type");
    auto hash1 = computePropertyHash(1, typeHash, "field");
    auto hash2 = computePropertyHash(2, typeHash, "field");
    auto hash3 = computePropertyHash(1, typeHash, "field");

    EXPECT_EQ(hash1, hash3);
    EXPECT_NE(hash1, hash2);
    EXPECT_TRUE(hash1 < hash2 || hash2 < hash1);  // At least one should be less
}
