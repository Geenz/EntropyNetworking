/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include <chrono>

#include "../src/Networking/Core/ComponentSchema.h"
#include "../src/Networking/Core/PropertyRegistry.h"

using namespace EntropyEngine::Networking;

static uint64_t getCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Helper function to create a ComponentTypeHash for testing
static ComponentTypeHash createTestComponentType(const std::string& appId, const std::string& componentName) {
    // Create a minimal valid schema with one dummy property
    std::vector<PropertyDefinition> props = {{"dummy", PropertyType::Int32, 0, 4}};
    auto schemaResult = ComponentSchema::create(appId, componentName, 1, props, 4, false);
    if (schemaResult.success()) {
        return schemaResult.value.typeHash;
    }
    // Fallback to a simple hash if schema creation fails (shouldn't happen)
    return ComponentTypeHash{0, 0};
}

TEST(PropertyRegistryTests, RegisterAndLookup) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");
    PropertyMetadata meta{hash, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};

    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.success());

    auto found = registry.lookup(hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->propertyName, "position");
    EXPECT_EQ(found->componentType, typeHash);
    EXPECT_EQ(found->type, PropertyType::Vec3);
    EXPECT_EQ(found->entityId, 42);
}

TEST(PropertyRegistryTests, LookupNonExistent) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Type");
    auto hash = computePropertyHash(999, typeHash, "field");
    auto found = registry.lookup(hash);

    EXPECT_FALSE(found.has_value());
}

TEST(PropertyRegistryTests, HashCollisionDetection) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");

    // Register first property
    PropertyMetadata meta1{hash, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Attempt to register same hash with different metadata (should fail)
    PropertyMetadata meta2{
        hash, 42, typeHash, "velocity", PropertyType::Vec3, getCurrentTimestamp()};  // Different propertyName
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::HashCollision);
}

TEST(PropertyRegistryTests, IdempotentRegistration) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");
    auto timestamp1 = getCurrentTimestamp();
    PropertyMetadata meta1{hash, 42, typeHash, "position", PropertyType::Vec3, timestamp1};

    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Re-registering exact same property with new timestamp should succeed (idempotent)
    auto timestamp2 = getCurrentTimestamp();
    PropertyMetadata meta2{hash, 42, typeHash, "position", PropertyType::Vec3, timestamp2};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.success());

    // Timestamp should be updated to timestamp2
    auto found = registry.lookup(hash);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->registeredAt, timestamp2);
}

TEST(PropertyRegistryTests, UnregisterEntity) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(42, typeHash, "velocity");
    auto hash3 = computePropertyHash(43, typeHash, "position");

    PropertyMetadata meta1{hash1, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, typeHash, "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta3{hash3, 43, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};

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

    auto typeHash1 = createTestComponentType("TestApp", "Player");
    auto typeHash2 = createTestComponentType("TestApp", "Enemy");
    auto hash1 = computePropertyHash(42, typeHash1, "position");
    auto hash2 = computePropertyHash(42, typeHash1, "velocity");
    auto hash3 = computePropertyHash(43, typeHash2, "position");

    PropertyMetadata meta1{hash1, 42, typeHash1, "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, typeHash1, "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta3{hash3, 43, typeHash2, "position", PropertyType::Vec3, getCurrentTimestamp()};

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

    auto typeHash = createTestComponentType("TestApp", "Transform");
    auto hash = computePropertyHash(42, typeHash, "position");
    PropertyMetadata meta{hash, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};

    registry.registerProperty(meta);

    // Validate correct type
    EXPECT_TRUE(registry.validateType(hash, PropertyType::Vec3));

    // Validate wrong type
    EXPECT_FALSE(registry.validateType(hash, PropertyType::Float32));

    // Validate unknown hash
    auto unknownTypeHash = createTestComponentType("TestApp", "Unknown");
    auto unknownHash = computePropertyHash(99, unknownTypeHash, "field");
    EXPECT_FALSE(registry.validateType(unknownHash, PropertyType::Vec3));
}

TEST(PropertyRegistryTests, ValidatePropertyValue) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Transform");
    auto hash = computePropertyHash(42, typeHash, "position");
    PropertyMetadata meta{hash, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};

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
    auto unknownTypeHash = createTestComponentType("TestApp", "Unknown");
    auto unknownHash = computePropertyHash(99, unknownTypeHash, "field");
    auto result3 = registry.validatePropertyValue(unknownHash, correctValue);
    EXPECT_TRUE(result3.failed());
    EXPECT_EQ(result3.error, NetworkError::UnknownProperty);
}

TEST(PropertyRegistryTests, InvalidMetadata) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");

    // Empty property name
    PropertyMetadata meta{hash, 42, typeHash, "", PropertyType::Vec3, getCurrentTimestamp()};
    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::InvalidParameter);
}

TEST(PropertyRegistryTests, UnregisterSingleProperty) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash1 = computePropertyHash(42, typeHash, "position");
    auto hash2 = computePropertyHash(42, typeHash, "velocity");

    PropertyMetadata meta1{hash1, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};
    PropertyMetadata meta2{hash2, 42, typeHash, "velocity", PropertyType::Vec3, getCurrentTimestamp()};

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
    auto unknownTypeHash = createTestComponentType("TestApp", "Unknown");
    auto unknownHash = computePropertyHash(99, unknownTypeHash, "field");
    bool removed2 = registry.unregisterProperty(unknownHash);
    EXPECT_FALSE(removed2);
}

TEST(PropertyRegistryTests, Size) {
    PropertyRegistry registry;

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());

    auto typeHash = createTestComponentType("TestApp", "Type");
    auto hash1 = computePropertyHash(1, typeHash, "field1");
    auto hash2 = computePropertyHash(2, typeHash, "field2");

    registry.registerProperty(
        PropertyMetadata{hash1, 1, typeHash, "field1", PropertyType::Int32, getCurrentTimestamp()});
    EXPECT_EQ(registry.size(), 1);
    EXPECT_FALSE(registry.empty());

    registry.registerProperty(
        PropertyMetadata{hash2, 2, typeHash, "field2", PropertyType::Int32, getCurrentTimestamp()});
    EXPECT_EQ(registry.size(), 2);
}

TEST(PropertyRegistryTests, Clear) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Type");
    auto hash1 = computePropertyHash(1, typeHash, "field1");
    auto hash2 = computePropertyHash(2, typeHash, "field2");

    registry.registerProperty(
        PropertyMetadata{hash1, 1, typeHash, "field1", PropertyType::Int32, getCurrentTimestamp()});
    registry.registerProperty(
        PropertyMetadata{hash2, 2, typeHash, "field2", PropertyType::Int32, getCurrentTimestamp()});

    EXPECT_EQ(registry.size(), 2);

    registry.clear();

    EXPECT_EQ(registry.size(), 0);
    EXPECT_TRUE(registry.empty());
    EXPECT_FALSE(registry.lookup(hash1).has_value());
}

TEST(PropertyRegistryTests, GetAllProperties) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Type");
    auto hash1 = computePropertyHash(1, typeHash, "field1");
    auto hash2 = computePropertyHash(2, typeHash, "field2");

    registry.registerProperty(
        PropertyMetadata{hash1, 1, typeHash, "field1", PropertyType::Int32, getCurrentTimestamp()});
    registry.registerProperty(
        PropertyMetadata{hash2, 2, typeHash, "field2", PropertyType::Float32, getCurrentTimestamp()});

    auto all = registry.getAllProperties();

    EXPECT_EQ(all.size(), 2);
}

TEST(PropertyRegistryTests, IsRegistered) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");
    PropertyMetadata meta{hash, 42, typeHash, "position", PropertyType::Vec3, getCurrentTimestamp()};

    EXPECT_FALSE(registry.isRegistered(hash));

    registry.registerProperty(meta);

    EXPECT_TRUE(registry.isRegistered(hash));
}

TEST(PropertyRegistryTests, InvalidPropertyType) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash, "position");

    // Invalid enum value
    PropertyMetadata meta{hash, 42, typeHash, "position", static_cast<PropertyType>(999), getCurrentTimestamp()};
    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::InvalidParameter);
}

TEST(PropertyRegistryTests, NameLengthLimits) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");

    // Test property name too long
    std::string longPropertyName(PropertyRegistry::MAX_NAME_LENGTH + 1, 'B');
    auto hash1 = computePropertyHash(42, typeHash, longPropertyName);
    PropertyMetadata meta1{hash1, 42, typeHash, longPropertyName, PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.failed());
    EXPECT_EQ(result1.error, NetworkError::InvalidParameter);

    // Test valid max length
    std::string maxPropertyName(PropertyRegistry::MAX_NAME_LENGTH, 'D');
    auto hash2 = computePropertyHash(42, typeHash, maxPropertyName);
    PropertyMetadata meta2{hash2, 42, typeHash, maxPropertyName, PropertyType::Vec3, getCurrentTimestamp()};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.success());
}

TEST(PropertyRegistryTests, EntityPropertyLimit) {
    PropertyRegistry registry;

    auto typeHash = createTestComponentType("TestApp", "Player");

    // Register properties up to the limit
    for (size_t i = 0; i < PropertyRegistry::MAX_PROPERTIES_PER_ENTITY; ++i) {
        auto hash = computePropertyHash(42, typeHash, "prop" + std::to_string(i));
        PropertyMetadata meta{
            hash, 42, typeHash, "prop" + std::to_string(i), PropertyType::Int32, getCurrentTimestamp()};
        auto result = registry.registerProperty(meta);
        EXPECT_TRUE(result.success());
    }

    // Try to register one more - should fail
    auto hash = computePropertyHash(42, typeHash, "overflow");
    PropertyMetadata meta{hash, 42, typeHash, "overflow", PropertyType::Int32, getCurrentTimestamp()};
    auto result = registry.registerProperty(meta);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::ResourceLimitExceeded);
}

TEST(PropertyRegistryTests, DetailedCollisionDiagnostics) {
    PropertyRegistry registry;

    auto typeHash1 = createTestComponentType("TestApp", "Player");
    auto hash = computePropertyHash(42, typeHash1, "position");

    // Register first property
    PropertyMetadata meta1{hash, 42, typeHash1, "position", PropertyType::Vec3, getCurrentTimestamp()};
    auto result1 = registry.registerProperty(meta1);
    EXPECT_TRUE(result1.success());

    // Try to register with same hash but different metadata
    auto typeHash2 = createTestComponentType("TestApp", "Enemy");
    PropertyMetadata meta2{hash, 99, typeHash2, "velocity", PropertyType::Vec3, getCurrentTimestamp()};
    auto result2 = registry.registerProperty(meta2);
    EXPECT_TRUE(result2.failed());
    EXPECT_EQ(result2.error, NetworkError::HashCollision);

    // Error message should contain details of both properties
    EXPECT_TRUE(result2.errorMessage.find("42") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find(toString(typeHash1)) != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("position") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("99") != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find(toString(typeHash2)) != std::string::npos);
    EXPECT_TRUE(result2.errorMessage.find("velocity") != std::string::npos);
}
