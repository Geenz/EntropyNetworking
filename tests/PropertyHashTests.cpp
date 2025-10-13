/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Core/PropertyHash.h"

using namespace EntropyEngine::Networking;

TEST(PropertyHashTests, Determinism) {
    // Same inputs should produce same hash
    auto hash1 = computePropertyHash(42, "com.example.app", "Player", "position");
    auto hash2 = computePropertyHash(42, "com.example.app", "Player", "position");

    EXPECT_EQ(hash1, hash2);
}

TEST(PropertyHashTests, DifferentEntityIds) {
    // Different entity IDs should produce different hashes
    auto hash1 = computePropertyHash(42, "com.example.app", "Player", "position");
    auto hash2 = computePropertyHash(43, "com.example.app", "Player", "position");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, DifferentAppIds) {
    // Different app IDs should produce different hashes
    auto hash1 = computePropertyHash(42, "com.example.app1", "Player", "position");
    auto hash2 = computePropertyHash(42, "com.example.app2", "Player", "position");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, DifferentTypeNames) {
    // Different type names should produce different hashes
    auto hash1 = computePropertyHash(42, "com.example.app", "Player", "position");
    auto hash2 = computePropertyHash(42, "com.example.app", "Enemy", "position");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, DifferentFieldNames) {
    // Different field names should produce different hashes
    auto hash1 = computePropertyHash(42, "com.example.app", "Player", "position");
    auto hash2 = computePropertyHash(42, "com.example.app", "Player", "velocity");

    EXPECT_NE(hash1, hash2);
}

TEST(PropertyHashTests, NotNull) {
    // Valid hash should not be null
    auto hash = computePropertyHash(42, "com.example.app", "Player", "position");

    EXPECT_FALSE(hash.isNull());
}

TEST(PropertyHashTests, NullHash) {
    // Default-constructed hash should be null
    PropertyHash128 hash;

    EXPECT_TRUE(hash.isNull());
    EXPECT_EQ(hash.high, 0);
    EXPECT_EQ(hash.low, 0);
}

TEST(PropertyHashTests, HashMapUsage) {
    // PropertyHash128 should be usable in std::unordered_map
    std::unordered_map<PropertyHash128, std::string> map;

    auto hash1 = computePropertyHash(42, "app", "Player", "position");
    auto hash2 = computePropertyHash(43, "app", "Player", "position");

    map[hash1] = "value1";
    map[hash2] = "value2";

    EXPECT_EQ(map[hash1], "value1");
    EXPECT_EQ(map[hash2], "value2");
    EXPECT_EQ(map.size(), 2);
}

TEST(PropertyHashTests, Comparison) {
    // Test comparison operators
    auto hash1 = computePropertyHash(1, "app", "Type", "field");
    auto hash2 = computePropertyHash(2, "app", "Type", "field");
    auto hash3 = computePropertyHash(1, "app", "Type", "field");

    EXPECT_EQ(hash1, hash3);
    EXPECT_NE(hash1, hash2);
    EXPECT_TRUE(hash1 < hash2 || hash2 < hash1);  // At least one should be less
}
