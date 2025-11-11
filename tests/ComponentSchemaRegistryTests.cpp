/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Core/ComponentSchemaRegistry.h"
#include <thread>
#include <vector>
#include <atomic>

using namespace EntropyEngine::Networking;

TEST(ComponentSchemaRegistryTests, RegisterSchema_Success) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, false);
    ASSERT_TRUE(schemaResult.success());

    auto result = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(result.success());
    EXPECT_FALSE(result.value.isNull());
}

TEST(ComponentSchemaRegistryTests, RegisterSchema_Idempotent) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, false);
    ASSERT_TRUE(schemaResult.success());

    // Register first time
    auto result1 = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(result1.success());

    // Register second time with identical schema
    auto result2 = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(result2.success());
    EXPECT_EQ(result1.value, result2.value);
}

TEST(ComponentSchemaRegistryTests, GetSchema_Found) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, false);
    ASSERT_TRUE(schemaResult.success());

    auto registerResult = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(registerResult.success());

    auto found = registry.getSchema(registerResult.value);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->appId, "TestApp");
    EXPECT_EQ(found->componentName, "Transform");
}

TEST(ComponentSchemaRegistryTests, GetSchema_NotFound) {
    ComponentSchemaRegistry registry;

    PropertyHash nonExistentHash{0x1234567890ABCDEF, 0xFEDCBA0987654321};
    auto found = registry.getSchema(nonExistentHash);
    EXPECT_FALSE(found.has_value());
}

TEST(ComponentSchemaRegistryTests, PublicSchemas_DefaultPrivate) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    // Register private schema (default)
    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, false);
    ASSERT_TRUE(schemaResult.success());

    auto registerResult = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(registerResult.success());

    // Should not appear in public schemas
    auto publicSchemas = registry.getPublicSchemas();
    EXPECT_EQ(publicSchemas.size(), 0);

    // Should still be registered
    EXPECT_TRUE(registry.isRegistered(registerResult.value));
    EXPECT_FALSE(registry.isPublic(registerResult.value));
}

TEST(ComponentSchemaRegistryTests, PublishSchema_MakesDiscoverable) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    // Register private schema
    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, false);
    ASSERT_TRUE(schemaResult.success());

    auto registerResult = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(registerResult.success());

    // Publish it
    auto publishResult = registry.publishSchema(registerResult.value);
    EXPECT_TRUE(publishResult.success());

    // Should now appear in public schemas
    auto publicSchemas = registry.getPublicSchemas();
    EXPECT_EQ(publicSchemas.size(), 1);
    EXPECT_TRUE(registry.isPublic(registerResult.value));
}

TEST(ComponentSchemaRegistryTests, UnpublishSchema_RemovesFromDiscovery) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    // Register public schema
    auto schemaResult = ComponentSchema::create("TestApp", "Transform", 1, properties, 12, true);
    ASSERT_TRUE(schemaResult.success());

    auto registerResult = registry.registerSchema(schemaResult.value);
    ASSERT_TRUE(registerResult.success());

    EXPECT_TRUE(registry.isPublic(registerResult.value));

    // Unpublish it
    auto unpublishResult = registry.unpublishSchema(registerResult.value);
    EXPECT_TRUE(unpublishResult.success());

    // Should no longer appear in public schemas
    auto publicSchemas = registry.getPublicSchemas();
    EXPECT_EQ(publicSchemas.size(), 0);
    EXPECT_FALSE(registry.isPublic(registerResult.value));

    // Should still be registered
    EXPECT_TRUE(registry.isRegistered(registerResult.value));
}

TEST(ComponentSchemaRegistryTests, FindCompatibleSchemas_StructuralMatch) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    // Register two schemas with same structure but different apps (both public)
    auto schema1Result = ComponentSchema::create("App1", "Transform", 1, properties, 12, true);
    auto schema2Result = ComponentSchema::create("App2", "Position", 1, properties, 12, true);

    ASSERT_TRUE(schema1Result.success());
    ASSERT_TRUE(schema2Result.success());

    auto hash1Result = registry.registerSchema(schema1Result.value);
    auto hash2Result = registry.registerSchema(schema2Result.value);

    ASSERT_TRUE(hash1Result.success());
    ASSERT_TRUE(hash2Result.success());

    // Find compatible schemas
    auto compatible = registry.findCompatibleSchemas(hash1Result.value);

    // Should find schema2 as compatible
    EXPECT_EQ(compatible.size(), 1);
    EXPECT_EQ(compatible[0], hash2Result.value);
}

TEST(ComponentSchemaRegistryTests, FindCompatibleSchemas_NoMatches) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties1 = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> properties2 = {
        {"velocity", PropertyType::Vec3, 0, 12}  // Different field name
    };

    auto schema1Result = ComponentSchema::create("App1", "Transform", 1, properties1, 12, true);
    auto schema2Result = ComponentSchema::create("App2", "Physics", 1, properties2, 12, true);

    ASSERT_TRUE(schema1Result.success());
    ASSERT_TRUE(schema2Result.success());

    auto hash1Result = registry.registerSchema(schema1Result.value);
    auto hash2Result = registry.registerSchema(schema2Result.value);

    ASSERT_TRUE(hash1Result.success());
    ASSERT_TRUE(hash2Result.success());

    // Find compatible schemas - should be empty
    auto compatible = registry.findCompatibleSchemas(hash1Result.value);
    EXPECT_EQ(compatible.size(), 0);
}

TEST(ComponentSchemaRegistryTests, AreCompatible_True) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto schema1Result = ComponentSchema::create("App1", "Transform", 1, properties, 12, false);
    auto schema2Result = ComponentSchema::create("App2", "Transform", 1, properties, 12, false);

    ASSERT_TRUE(schema1Result.success());
    ASSERT_TRUE(schema2Result.success());

    auto hash1Result = registry.registerSchema(schema1Result.value);
    auto hash2Result = registry.registerSchema(schema2Result.value);

    ASSERT_TRUE(hash1Result.success());
    ASSERT_TRUE(hash2Result.success());

    EXPECT_TRUE(registry.areCompatible(hash1Result.value, hash2Result.value));
}

TEST(ComponentSchemaRegistryTests, AreCompatible_False) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties1 = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> properties2 = {
        {"position", PropertyType::Vec4, 0, 16}  // Different type
    };

    auto schema1Result = ComponentSchema::create("App1", "Transform", 1, properties1, 12, false);
    auto schema2Result = ComponentSchema::create("App2", "Transform", 1, properties2, 16, false);

    ASSERT_TRUE(schema1Result.success());
    ASSERT_TRUE(schema2Result.success());

    auto hash1Result = registry.registerSchema(schema1Result.value);
    auto hash2Result = registry.registerSchema(schema2Result.value);

    ASSERT_TRUE(hash1Result.success());
    ASSERT_TRUE(hash2Result.success());

    EXPECT_FALSE(registry.areCompatible(hash1Result.value, hash2Result.value));
}

TEST(ComponentSchemaRegistryTests, ValidateDetailedCompatibility_Compatible) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec3, 0, 12}  // Subset
    };

    auto sourceResult = ComponentSchema::create("App", "Physics", 1, sourceProps, 24, false);
    auto targetResult = ComponentSchema::create("App", "Transform", 1, targetProps, 12, false);

    ASSERT_TRUE(sourceResult.success());
    ASSERT_TRUE(targetResult.success());

    auto sourceHashResult = registry.registerSchema(sourceResult.value);
    auto targetHashResult = registry.registerSchema(targetResult.value);

    ASSERT_TRUE(sourceHashResult.success());
    ASSERT_TRUE(targetHashResult.success());

    auto result = registry.validateDetailedCompatibility(
        sourceHashResult.value,
        targetHashResult.value
    );

    EXPECT_TRUE(result.success());
}

TEST(ComponentSchemaRegistryTests, ValidateDetailedCompatibility_Incompatible) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}  // Source missing this
    };

    auto sourceResult = ComponentSchema::create("App", "Transform", 1, sourceProps, 12, false);
    auto targetResult = ComponentSchema::create("App", "Physics", 1, targetProps, 24, false);

    ASSERT_TRUE(sourceResult.success());
    ASSERT_TRUE(targetResult.success());

    auto sourceHashResult = registry.registerSchema(sourceResult.value);
    auto targetHashResult = registry.registerSchema(targetResult.value);

    ASSERT_TRUE(sourceHashResult.success());
    ASSERT_TRUE(targetHashResult.success());

    auto result = registry.validateDetailedCompatibility(
        sourceHashResult.value,
        targetHashResult.value
    );

    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::SchemaIncompatible);
}

TEST(ComponentSchemaRegistryTests, ThreadSafety_ConcurrentReadWrite) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> baseProperties = {
        {"field", PropertyType::Int32, 0, 4}
    };

    // Pre-register some schemas
    for (int i = 0; i < 10; ++i) {
        auto schemaResult = ComponentSchema::create(
            "App",
            "Component" + std::to_string(i),
            1,
            baseProperties,
            4,
            true
        );
        ASSERT_TRUE(schemaResult.success());
        auto result = registry.registerSchema(schemaResult.value);
        ASSERT_TRUE(result.success());
    }

    std::atomic<bool> stopFlag{false};
    std::atomic<int> errorCount{0};

    // Writer threads
    auto writerFunc = [&]() {
        for (int i = 10; i < 20 && !stopFlag; ++i) {
            auto schemaResult = ComponentSchema::create(
                "App",
                "Component" + std::to_string(i),
                1,
                baseProperties,
                4,
                i % 2 == 0
            );

            if (schemaResult.success()) {
                auto result = registry.registerSchema(schemaResult.value);
                if (result.failed()) {
                    errorCount++;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // Reader threads
    auto readerFunc = [&]() {
        for (int i = 0; i < 100 && !stopFlag; ++i) {
            // Read operations atomically to get consistent snapshot
            size_t count, publicCount;
            std::vector<ComponentSchema> publicSchemas;
            registry.getStats(count, publicCount, publicSchemas);

            // Simple sanity checks on consistent snapshot
            if (count < 10) errorCount++;
            if (publicCount > count) errorCount++;
            if (publicSchemas.size() != publicCount) errorCount++;

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    // Launch threads
    std::vector<std::thread> threads;
    threads.emplace_back(writerFunc);
    threads.emplace_back(writerFunc);
    threads.emplace_back(readerFunc);
    threads.emplace_back(readerFunc);
    threads.emplace_back(readerFunc);
    threads.emplace_back(readerFunc);

    // Wait for completion
    for (auto& t : threads) {
        t.join();
    }

    // Verify no errors occurred
    EXPECT_EQ(errorCount.load(), 0);

    // Verify final state is consistent
    auto finalCount = registry.schemaCount();
    auto finalPublicCount = registry.publicSchemaCount();
    EXPECT_GE(finalCount, 10);
    EXPECT_LE(finalPublicCount, finalCount);
}

TEST(ComponentSchemaRegistryTests, SchemaCount) {
    ComponentSchemaRegistry registry;

    EXPECT_EQ(registry.schemaCount(), 0);
    EXPECT_EQ(registry.publicSchemaCount(), 0);

    std::vector<PropertyDefinition> properties = {
        {"field", PropertyType::Int32, 0, 4}
    };

    // Register 5 private schemas
    for (int i = 0; i < 5; ++i) {
        auto schemaResult = ComponentSchema::create(
            "App",
            "Component" + std::to_string(i),
            1,
            properties,
            4,
            false
        );
        ASSERT_TRUE(schemaResult.success());
        auto result = registry.registerSchema(schemaResult.value);
        ASSERT_TRUE(result.success());
    }

    EXPECT_EQ(registry.schemaCount(), 5);
    EXPECT_EQ(registry.publicSchemaCount(), 0);

    // Register 3 public schemas
    for (int i = 5; i < 8; ++i) {
        auto schemaResult = ComponentSchema::create(
            "App",
            "Component" + std::to_string(i),
            1,
            properties,
            4,
            true
        );
        ASSERT_TRUE(schemaResult.success());
        auto result = registry.registerSchema(schemaResult.value);
        ASSERT_TRUE(result.success());
    }

    EXPECT_EQ(registry.schemaCount(), 8);
    EXPECT_EQ(registry.publicSchemaCount(), 3);
}

TEST(ComponentSchemaRegistryTests, PublicSchemaCount) {
    ComponentSchemaRegistry registry;

    std::vector<PropertyDefinition> properties = {
        {"field", PropertyType::Int32, 0, 4}
    };

    // Register private schema
    auto schema1Result = ComponentSchema::create("App", "Component1", 1, properties, 4, false);
    ASSERT_TRUE(schema1Result.success());
    auto hash1Result = registry.registerSchema(schema1Result.value);
    ASSERT_TRUE(hash1Result.success());

    EXPECT_EQ(registry.publicSchemaCount(), 0);

    // Publish it
    registry.publishSchema(hash1Result.value);
    EXPECT_EQ(registry.publicSchemaCount(), 1);

    // Unpublish it
    registry.unpublishSchema(hash1Result.value);
    EXPECT_EQ(registry.publicSchemaCount(), 0);
}
