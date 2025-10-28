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

using namespace EntropyEngine::Networking;

TEST(ComponentSchemaTests, Create_ValidSchema) {
    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12},
        {"rotation", PropertyType::Quat, 12, 16},
        {"scale", PropertyType::Vec3, 28, 12}
    };

    auto result = ComponentSchema::create(
        "TestApp",
        "Transform",
        1,
        properties,
        40,
        false
    );

    ASSERT_TRUE(result.success());
    EXPECT_EQ(result.value.appId, "TestApp");
    EXPECT_EQ(result.value.componentName, "Transform");
    EXPECT_EQ(result.value.schemaVersion, 1);
    EXPECT_EQ(result.value.totalSize, 40);
    EXPECT_EQ(result.value.properties.size(), 3);
    EXPECT_FALSE(result.value.isPublic);
    EXPECT_FALSE(result.value.typeHash.isNull());
    EXPECT_FALSE(result.value.structuralHash.isNull());
}

TEST(ComponentSchemaTests, Create_EmptyProperties) {
    std::vector<PropertyDefinition> properties;

    auto result = ComponentSchema::create(
        "TestApp",
        "Empty",
        1,
        properties,
        0,
        false
    );

    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::InvalidParameter);
}

TEST(ComponentSchemaTests, ComputeStructuralHash_Deterministic) {
    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}
    };

    auto hash1 = ComponentSchema::computeStructuralHash(properties);
    auto hash2 = ComponentSchema::computeStructuralHash(properties);

    EXPECT_EQ(hash1, hash2);
}

TEST(ComponentSchemaTests, ComputeStructuralHash_DifferentOrder) {
    std::vector<PropertyDefinition> properties1 = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}
    };

    std::vector<PropertyDefinition> properties2 = {
        {"velocity", PropertyType::Vec3, 12, 12},
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto hash1 = ComponentSchema::computeStructuralHash(properties1);
    auto hash2 = ComponentSchema::computeStructuralHash(properties2);

    // Different order should produce different hash
    EXPECT_NE(hash1, hash2);
}

TEST(ComponentSchemaTests, ComputeTypeHash_UniquePerVersion) {
    std::vector<PropertyDefinition> properties = {
        {"field", PropertyType::Int32, 0, 4}
    };

    auto structuralHash = ComponentSchema::computeStructuralHash(properties);

    auto hash1 = ComponentSchema::computeTypeHash("App", "Component", 1, structuralHash);
    auto hash2 = ComponentSchema::computeTypeHash("App", "Component", 2, structuralHash);

    // Different versions should produce different hashes
    EXPECT_NE(hash1, hash2);
}

TEST(ComponentSchemaTests, StructuralCompatibility_Identical) {
    std::vector<PropertyDefinition> properties = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto result1 = ComponentSchema::create("App1", "Transform", 1, properties, 12, false);
    auto result2 = ComponentSchema::create("App2", "Transform", 1, properties, 12, false);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Same structure should be compatible
    EXPECT_TRUE(result1.value.isStructurallyCompatible(result2.value));
}

TEST(ComponentSchemaTests, StructuralCompatibility_Different) {
    std::vector<PropertyDefinition> properties1 = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> properties2 = {
        {"position", PropertyType::Vec4, 0, 16}  // Different type
    };

    auto result1 = ComponentSchema::create("App", "Transform", 1, properties1, 12, false);
    auto result2 = ComponentSchema::create("App", "Transform", 1, properties2, 16, false);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Different structure should not be compatible
    EXPECT_FALSE(result1.value.isStructurallyCompatible(result2.value));
}

TEST(ComponentSchemaTests, CanReadFrom_Subset) {
    // Source has more fields than target
    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    auto source = ComponentSchema::create("App", "Physics", 1, sourceProps, 24, false);
    auto target = ComponentSchema::create("App", "Transform", 1, targetProps, 12, false);

    ASSERT_TRUE(source.success());
    ASSERT_TRUE(target.success());

    // Target can read from source (subset)
    auto result = target.value.canReadFrom(source.value);
    EXPECT_TRUE(result.success());
}

TEST(ComponentSchemaTests, CanReadFrom_Superset) {
    // Target has more fields than source
    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec3, 0, 12},
        {"velocity", PropertyType::Vec3, 12, 12}
    };

    auto source = ComponentSchema::create("App", "Transform", 1, sourceProps, 12, false);
    auto target = ComponentSchema::create("App", "Physics", 1, targetProps, 24, false);

    ASSERT_TRUE(source.success());
    ASSERT_TRUE(target.success());

    // Target cannot read from source (missing velocity field)
    auto result = target.value.canReadFrom(source.value);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::SchemaIncompatible);
}

TEST(ComponentSchemaTests, CanReadFrom_FieldTypeMismatch) {
    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec4, 0, 16}  // Different type
    };

    auto source = ComponentSchema::create("App", "Transform", 1, sourceProps, 12, false);
    auto target = ComponentSchema::create("App", "Transform", 1, targetProps, 16, false);

    ASSERT_TRUE(source.success());
    ASSERT_TRUE(target.success());

    // Type mismatch should fail
    auto result = target.value.canReadFrom(source.value);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::SchemaIncompatible);
    EXPECT_NE(result.errorMessage.find("type mismatch"), std::string::npos);
}

TEST(ComponentSchemaTests, CanReadFrom_FieldOffsetMismatch) {
    std::vector<PropertyDefinition> sourceProps = {
        {"position", PropertyType::Vec3, 0, 12}
    };

    std::vector<PropertyDefinition> targetProps = {
        {"position", PropertyType::Vec3, 4, 12}  // Different offset
    };

    auto source = ComponentSchema::create("App", "Transform", 1, sourceProps, 12, false);
    auto target = ComponentSchema::create("App", "Transform", 1, targetProps, 16, false);

    ASSERT_TRUE(source.success());
    ASSERT_TRUE(target.success());

    // Offset mismatch should fail
    auto result = target.value.canReadFrom(source.value);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::SchemaIncompatible);
    EXPECT_NE(result.errorMessage.find("offset mismatch"), std::string::npos);
}
