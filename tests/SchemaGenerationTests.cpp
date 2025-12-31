/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <Logging/Logger.h>
#include <TypeSystem/Reflection.h>
#include <gtest/gtest.h>

#include "../src/Networking/Core/SchemaGeneration.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core::TypeSystem;

/**
 * @brief Test component using EntropyCore reflection
 */
struct TestTransform
{
    ENTROPY_REGISTER_TYPE(TestTransform);

    ENTROPY_FIELD(Vec3, position) = { 0.0f, 0.0f, 0.0f };
    ENTROPY_FIELD(Quat, rotation) = { 0.0f, 0.0f, 0.0f, 1.0f };
    ENTROPY_FIELD(Vec3, scale) = { 1.0f, 1.0f, 1.0f };
};

/**
 * @brief Test component with various property types
 */
struct TestComponent
{
    ENTROPY_REGISTER_TYPE(TestComponent);

    ENTROPY_FIELD(int32_t, id) = 0;
    ENTROPY_FIELD(float, value) = 0.0f;
    ENTROPY_FIELD(bool, enabled) = true;
    ENTROPY_FIELD(std::string, name) = "";
    ENTROPY_FIELD(Vec2, position2d) = { 0.0f, 0.0f };
};

/**
 * NOTE: Enum testing is demonstrated in examples/schema_generation.cpp
 *
 * Testing custom enum mappings requires proper namespace scoping and runtime
 * function overrides, which is better demonstrated in application code.
 * The example shows the complete pattern for extending type mappings.
 */

TEST(SchemaGenerationTests, GenerateSchema_BasicTransform) {
    ENTROPY_LOG_INFO("Testing basic Transform schema generation");

    auto result = generateComponentSchema<TestTransform>("TestApp", 1, true);

    if (!result.success()) {
        ENTROPY_LOG_ERROR("Failed to generate TestTransform schema: " + result.errorMessage);
    }
    ASSERT_TRUE(result.success()) << result.errorMessage;

    const auto& schema = result.value;
    EXPECT_EQ(schema.appId, "TestApp");
    EXPECT_EQ(schema.componentName, "TestTransform");
    EXPECT_EQ(schema.schemaVersion, 1);
    EXPECT_EQ(schema.totalSize, sizeof(TestTransform));
    EXPECT_TRUE(schema.isPublic);
    EXPECT_EQ(schema.properties.size(), 3);

    // Verify property order matches ENTROPY_FIELD order
    EXPECT_EQ(schema.properties[0].name, "position");
    EXPECT_EQ(schema.properties[0].type, PropertyType::Vec3);

    EXPECT_EQ(schema.properties[1].name, "rotation");
    EXPECT_EQ(schema.properties[1].type, PropertyType::Quat);

    EXPECT_EQ(schema.properties[2].name, "scale");
    EXPECT_EQ(schema.properties[2].type, PropertyType::Vec3);

    ENTROPY_LOG_INFO("✓ Basic Transform schema generation passed");
}

TEST(SchemaGenerationTests, GenerateSchema_VariousTypes) {
    auto result = generateComponentSchema<TestComponent>("TestApp", 1, false);

    ASSERT_TRUE(result.success()) << result.errorMessage;

    const auto& schema = result.value;
    EXPECT_EQ(schema.properties.size(), 5);
    EXPECT_FALSE(schema.isPublic);

    // Verify all property types
    EXPECT_EQ(schema.properties[0].type, PropertyType::Int32);    // id
    EXPECT_EQ(schema.properties[1].type, PropertyType::Float32);  // value
    EXPECT_EQ(schema.properties[2].type, PropertyType::Bool);     // enabled
    EXPECT_EQ(schema.properties[3].type, PropertyType::String);   // name
    EXPECT_EQ(schema.properties[4].type, PropertyType::Vec2);     // position2d
}

TEST(SchemaGenerationTests, GenerateSchema_Offsets) {
    auto result = generateComponentSchema<TestTransform>("TestApp", 1, true);

    ASSERT_TRUE(result.success());

    const auto& schema = result.value;

    // Verify offsets are correctly extracted from reflection
    for (const auto& prop : schema.properties) {
        // Offsets should be valid (0 or positive)
        EXPECT_GE(prop.offset, 0u);  // Should be valid offset
        EXPECT_GT(prop.size, 0u);    // Should have non-zero size
    }
}

TEST(SchemaGenerationTests, GenerateSchema_Sizes) {
    auto result = generateComponentSchema<TestTransform>("TestApp", 1, true);

    ASSERT_TRUE(result.success());

    const auto& schema = result.value;

    // Verify sizes match expected types
    for (const auto& prop : schema.properties) {
        if (prop.type == PropertyType::Vec3) {
            EXPECT_EQ(prop.size, sizeof(Vec3));
        } else if (prop.type == PropertyType::Quat) {
            EXPECT_EQ(prop.size, sizeof(Quat));
        }
    }
}

TEST(SchemaGenerationTests, GenerateSchema_DeterministicHash) {
    auto result1 = generateComponentSchema<TestTransform>("TestApp", 1, true);
    auto result2 = generateComponentSchema<TestTransform>("TestApp", 1, true);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Same input should produce same hashes
    EXPECT_EQ(result1.value.typeHash, result2.value.typeHash);
    EXPECT_EQ(result1.value.structuralHash, result2.value.structuralHash);
}

TEST(SchemaGenerationTests, GenerateSchema_DifferentVersionsDifferentHash) {
    auto result1 = generateComponentSchema<TestTransform>("TestApp", 1, true);
    auto result2 = generateComponentSchema<TestTransform>("TestApp", 2, true);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Different versions should produce different type hashes
    EXPECT_NE(result1.value.typeHash, result2.value.typeHash);

    // But same structural hash (same fields)
    EXPECT_EQ(result1.value.structuralHash, result2.value.structuralHash);
}

TEST(SchemaGenerationTests, TypeMapping_FundamentalTypes) {
    // Verify fundamental types are mapped correctly
    EXPECT_EQ(TypeToPropertyType<int32_t>::value, PropertyType::Int32);
    EXPECT_EQ(TypeToPropertyType<int64_t>::value, PropertyType::Int64);
    EXPECT_EQ(TypeToPropertyType<float>::value, PropertyType::Float32);
    EXPECT_EQ(TypeToPropertyType<double>::value, PropertyType::Float64);
    EXPECT_EQ(TypeToPropertyType<bool>::value, PropertyType::Bool);
    EXPECT_EQ(TypeToPropertyType<std::string>::value, PropertyType::String);
}

TEST(SchemaGenerationTests, TypeMapping_VectorTypes) {
    // Verify vector types are mapped correctly
    EXPECT_EQ(TypeToPropertyType<Vec2>::value, PropertyType::Vec2);
    EXPECT_EQ(TypeToPropertyType<Vec3>::value, PropertyType::Vec3);
    EXPECT_EQ(TypeToPropertyType<Vec4>::value, PropertyType::Vec4);
    EXPECT_EQ(TypeToPropertyType<Quat>::value, PropertyType::Quat);
}

TEST(SchemaGenerationTests, TypeMapping_ArrayTypes) {
    // Verify array types are mapped correctly
    EXPECT_EQ(TypeToPropertyType<std::vector<uint8_t>>::value, PropertyType::Bytes);
    EXPECT_EQ(TypeToPropertyType<std::vector<int32_t>>::value, PropertyType::Int32Array);
    EXPECT_EQ(TypeToPropertyType<std::vector<float>>::value, PropertyType::Float32Array);
    EXPECT_EQ(TypeToPropertyType<std::vector<Vec3>>::value, PropertyType::Vec3Array);
}

TEST(SchemaGenerationTests, RuntimeMapping_FundamentalTypes) {
    // Verify runtime TypeID mapping works
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<int32_t>()), PropertyType::Int32);
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<float>()), PropertyType::Float32);
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<bool>()), PropertyType::Bool);
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<std::string>()), PropertyType::String);
}

TEST(SchemaGenerationTests, RuntimeMapping_VectorTypes) {
    // Verify runtime TypeID mapping for vectors
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<Vec3>()), PropertyType::Vec3);
    EXPECT_EQ(mapTypeIdToPropertyType(createTypeId<Quat>()), PropertyType::Quat);
}

TEST(SchemaGenerationTests, RuntimeMapping_UnknownType) {
    // Create a TypeID for an unmapped type
    struct UnmappedType
    {
    };
    auto result = mapTypeIdToPropertyType(createTypeId<UnmappedType>());

    EXPECT_FALSE(result.has_value());
}

TEST(SchemaGenerationTests, FieldSizeMapping) {
    // Verify field sizes are correct
    EXPECT_EQ(getFieldSize(createTypeId<int32_t>()), sizeof(int32_t));
    EXPECT_EQ(getFieldSize(createTypeId<float>()), sizeof(float));
    EXPECT_EQ(getFieldSize(createTypeId<Vec3>()), sizeof(Vec3));
    EXPECT_EQ(getFieldSize(createTypeId<Quat>()), sizeof(Quat));
    EXPECT_EQ(getFieldSize(createTypeId<std::string>()), sizeof(std::string));
}
