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
#include <chrono>

using namespace EntropyEngine::Networking;

static uint64_t getCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

TEST(PropertyRegistryTests, RegisterAndLookup) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");
    PropertyMetadata meta{hash, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};

    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.success());

    auto found = registry.lookup(hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->propertyName, "position");
    EXPECT_EQ(found->componentType, "Player");
    EXPECT_EQ(found->type, PropertyType::Vec3);
    EXPECT_EQ(found->entityId, 42);
}

TEST(PropertyRegistryTests, LookupNonExistent) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(999, "Type", "field");
    auto found = registry.lookup(hash);

    EXPECT_FALSE(found.has_value());
}

TEST(PropertyRegistryTests, HashCollisionDetection) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");

    // Register first property
    PropertyMetadata meta1{hash, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Attempt to register same hash with different metadata (should fail)
    PropertyMetadata meta2{hash, 42, "Player", "velocity", PropertyType::Vec3, getCurrentTimestamp()};  // Different propertyName
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::HashCollision);
}

TEST(PropertyRegistryTests, IdempotentRegistration) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");
    auto timestamp1 = getCurrentTimestamp();
    PropertyMetadata meta1{hash, 42, "Player", "position", PropertyType::Vec3, timestamp1};

    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Re-registering exact same property with new timestamp should succeed (idempotent)
    auto timestamp2 = getCurrentTimestamp();
    PropertyMetadata meta2{hash, 42, "Player", "position", PropertyType::Vec3, timestamp2};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.success());

    // Timestamp should be updated to timestamp2
    auto found = registry.lookup(hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->registeredAt, timestamp2);
}

TEST(PropertyRegistryTests, UnregisterEntity) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(42, "Player", "position");
    auto hash2 = computePropertyHash(42, "Player", "velocity");
    auto hash3 = computePropertyHash(43, "Player", "position");

    PropertyMetadata meta1{hash1, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, "Player", "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta3{hash3, 43, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta1);
    registry.registerProperty(meta2);
    registry.registerProperty(meta3);

    EXPECT_EQ(registry.size(), 3);

    // Unregister entity 42
    auto removed = registry.unregisterEntity(42);
    EXPECT_EQ(removed.size(), 2);
    EXPECT_EQ(registry.size(), 1);

    // Entity 42's properties should be gone
    EXPECT_FALSE(registry.lookup(hash1).has_value());
    EXPECT_FALSE(registry.lookup(hash2).has_value());

    // Entity 43's property should still exist
    EXPECT_TRUE(registry.lookup(hash3).has_value());
}

TEST(PropertyRegistryTests, GetEntityProperties) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(42, "Player", "position");
    auto hash2 = computePropertyHash(42, "Player", "velocity");
    auto hash3 = computePropertyHash(43, "Enemy", "position");

    PropertyMetadata meta1{hash1, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, "Player", "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta3{hash3, 43, "Enemy", "position", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta1);
    registry.registerProperty(meta2);
    registry.registerProperty(meta3);

    // Get properties for entity 42
    auto props42 = registry.getEntityProperties(42);
    EXPECT_EQ(props42.size(), 2);

    // Get properties for entity 43
    auto props43 = registry.getEntityProperties(43);
    EXPECT_EQ(props43.size(), 1);

    // Get properties for non-existent entity
    auto props99 = registry.getEntityProperties(99);
    EXPECT_EQ(props99.size(), 0);
}

TEST(PropertyRegistryTests, ValidateType) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Transform", "position");
    PropertyMetadata meta{hash, 42, "Transform", "position", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta);

    // Validate correct type
    EXPECT_TRUE(registry.validateType(hash, PropertyType::Vec3));

    // Validate wrong type
    EXPECT_FALSE(registry.validateType(hash, PropertyType::Float32));

    // Validate unknown hash
    auto unknownHash = computePropertyHash(99, "Unknown", "field");
    EXPECT_FALSE(registry.validateType(unknownHash, PropertyType::Vec3));
}

TEST(PropertyRegistryTests, ValidatePropertyValue) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Transform", "position");
    PropertyMetadata meta{hash, 42, "Transform", "position", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta);

    // Validate correct type
    PropertyValue correctValue = Vec3{1.0f, 2.0f, 3.0f};
    auto result1 = registry.validatePropertyValue(hash, correctValue);
    EXPECT_TRUE(result1.success());

    // Validate wrong type (SECURITY test)
    PropertyValue wrongValue = 42.0f;  // Float32 instead of Vec3
    auto result2 = registry.validatePropertyValue(hash, wrongValue);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::TypeMismatch);

    // Validate unknown hash
    auto unknownHash = computePropertyHash(99, "Unknown", "field");
    auto result3 = registry.validatePropertyValue(unknownHash, correctValue);
    EXPECT_TRUE(result3.failed());
    EXPECT_EQ(result3.error, NetworkError::UnknownProperty);
}

TEST(PropertyRegistryTests, InvalidMetadata) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");

    // Empty component type
    PropertyMetadata meta1{hash, 42, "", "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.failed());
    EXPECT_EQ(result1.error, NetworkError::InvalidParameter);

    // Empty property name
    PropertyMetadata meta2{hash, 42, "Player", "", PropertyType::Vec3, getCurrentTimestamp()};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::InvalidParameter);
}

TEST(PropertyRegistryTests, UnregisterSingleProperty) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(42, "Player", "position");
    auto hash2 = computePropertyHash(42, "Player", "velocity");

    PropertyMetadata meta1{hash1, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, "Player", "velocity", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta1);
    registry.registerProperty(meta2);

    EXPECT_EQ(registry.size(), 2);

    // Unregister single property
    bool removed = registry.unregisterProperty(hash1);
    EXPECT_TRUE(removed);
    EXPECT_EQ(registry.size(), 1);

    // First property should be gone
    EXPECT_FALSE(registry.lookup(hash1).has_value());

    // Second property should still exist
    EXPECT_TRUE(registry.lookup(hash2).has_value());

    // Unregister non-existent property
    auto unknownHash = computePropertyHash(99, "Unknown", "field");
    bool removed2 = registry.unregisterProperty(unknownHash);
    EXPECT_FALSE(removed2);
}

TEST(PropertyRegistryTests, Size) {
    PropertyRegistry registry;

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());

    auto hash1 = computePropertyHash(1, "Type", "field1");
    auto hash2 = computePropertyHash(2, "Type", "field2");

    registry.registerProperty(PropertyMetadata{hash1, 1, "Type", "field1", PropertyType::Int32, getCurrentTimestamp()});
    EXPECT_EQ(registry.size(), 1);
    EXPECT_FALSE(registry.empty());

    registry.registerProperty(PropertyMetadata{hash2, 2, "Type", "field2", PropertyType::Int32, getCurrentTimestamp()});
    EXPECT_EQ(registry.size(), 2);
}

TEST(PropertyRegistryTests, Clear) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(1, "Type", "field1");
    auto hash2 = computePropertyHash(2, "Type", "field2");

    registry.registerProperty(PropertyMetadata{hash1, 1, "Type", "field1", PropertyType::Int32, getCurrentTimestamp()});
    registry.registerProperty(PropertyMetadata{hash2, 2, "Type", "field2", PropertyType::Int32, getCurrentTimestamp()});

    EXPECT_EQ(registry.size(), 2);

    registry.clear();

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());
    EXPECT_FALSE(registry.lookup(hash1).has_value());
}

TEST(PropertyRegistryTests, GetAllProperties) {
    PropertyRegistry registry;

    auto hash1 = computePropertyHash(1, "Type", "field1");
    auto hash2 = computePropertyHash(2, "Type", "field2");

    registry.registerProperty(PropertyMetadata{hash1, 1, "Type", "field1", PropertyType::Int32, getCurrentTimestamp()});
    registry.registerProperty(PropertyMetadata{hash2, 2, "Type", "field2", PropertyType::Float32, getCurrentTimestamp()});

    auto all = registry.getAllProperties();

    EXPECT_EQ(all.size(), 2);
}

TEST(PropertyRegistryTests, IsRegistered) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");
    PropertyMetadata meta{hash, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};

    EXPECT_FALSE(registry.isRegistered(hash));

    registry.registerProperty(meta);

    EXPECT_TRUE(registry.isRegistered(hash));
}

TEST(PropertyRegistryTests, InvalidPropertyType) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");

    // Invalid enum value
    PropertyMetadata meta{hash, 42, "Player", "position", static_cast<PropertyType>(999), getCurrentTimestamp()};
    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::InvalidParameter);
}

TEST(PropertyRegistryTests, NameLengthLimits) {
    PropertyRegistry registry;

    // Test component type too long
    std::string longComponentType(PropertyRegistry::MAX_NAME_LENGTH + 1, 'A');
    auto hash1 = computePropertyHash(42, longComponentType, "position");
    PropertyMetadata meta1{hash1, 42, longComponentType, "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.failed());
    EXPECT_EQ(result1.error, NetworkError::InvalidParameter);

    // Test property name too long
    std::string longPropertyName(PropertyRegistry::MAX_NAME_LENGTH + 1, 'B');
    auto hash2 = computePropertyHash(42, "Player", longPropertyName);
    PropertyMetadata meta2{hash2, 42, "Player", longPropertyName, PropertyType::Vec3, getCurrentTimestamp()};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::InvalidParameter);

    // Test valid max length
    std::string maxComponentType(PropertyRegistry::MAX_NAME_LENGTH, 'C');
    std::string maxPropertyName(PropertyRegistry::MAX_NAME_LENGTH, 'D');
    auto hash3 = computePropertyHash(42, maxComponentType, maxPropertyName);
    PropertyMetadata meta3{hash3, 42, maxComponentType, maxPropertyName, PropertyType::Vec3, getCurrentTimestamp()};
    auto result3 = registry.registerProperty(meta3);
    EXPECT_TRUE(result3.success());
}

TEST(PropertyRegistryTests, EntityPropertyLimit) {
    PropertyRegistry registry;

    // Register properties up to the limit
    for (size_t i = 0; i < PropertyRegistry::MAX_PROPERTIES_PER_ENTITY; ++i) {
        auto hash = computePropertyHash(42, "Player", "prop" + std::to_string(i));
        PropertyMetadata meta{hash, 42, "Player", "prop" + std::to_string(i), PropertyType::Int32, getCurrentTimestamp()};
        auto result = registry.registerProperty(meta);
        EXPECT_TRUE(result.success());
    }

    // Try to register one more - should fail
    auto hash = computePropertyHash(42, "Player", "overflow");
    PropertyMetadata meta{hash, 42, "Player", "overflow", PropertyType::Int32, getCurrentTimestamp()};
    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::ResourceLimitExceeded);
}

TEST(PropertyRegistryTests, DetailedCollisionDiagnostics) {
    PropertyRegistry registry;

    auto hash = computePropertyHash(42, "Player", "position");

    // Register first property
    PropertyMetadata meta1{hash, 42, "Player", "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Try to register with same hash but different metadata
    PropertyMetadata meta2{hash, 99, "Enemy", "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::HashCollision);

    // Error message should contain details of both properties
    EXPECT_TRUE(result2.errorMessage.find("42") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("Player") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("position") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("99") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("Enemy") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("velocity") != std::string::npos);
}
