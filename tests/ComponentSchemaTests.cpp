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
    std::vector<PropertyDefinition> properties = {{"position", PropertyType::Vec3, 0, 12},
                                                  {"rotation", PropertyType::Quat, 12, 16},
                                                  {"scale", PropertyType::Vec3, 28, 12}};

    auto result = ComponentSchema::create("TestApp", "Transform", 1, properties, 40, false);

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

    auto result = ComponentSchema::create("TestApp", "Empty", 1, properties, 0, false);

    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::InvalidParameter);
}

TEST(ComponentSchemaTests, ComputeStructuralHash_Deterministic) {
    std::vector<PropertyDefinition> properties = {{"position", PropertyType::Vec3, 0, 12},
                                                  {"velocity", PropertyType::Vec3, 12, 12}};

    auto hash1 = ComponentSchema::computeStructuralHash(properties);
    auto hash2 = ComponentSchema::computeStructuralHash(properties);

    EXPECT_EQ(hash1, hash2);
}

TEST(ComponentSchemaTests, ComputeStructuralHash_DifferentOrder) {
    std::vector<PropertyDefinition> properties1 = {{"position", PropertyType::Vec3, 0, 12},
                                                   {"velocity", PropertyType::Vec3, 12, 12}};

    std::vector<PropertyDefinition> properties2 = {{"velocity", PropertyType::Vec3, 12, 12},
                                                   {"position", PropertyType::Vec3, 0, 12}};

    auto hash1 = ComponentSchema::computeStructuralHash(properties1);
    auto hash2 = ComponentSchema::computeStructuralHash(properties2);

    // With canonical string hashing, properties are sorted by name
    // So different input orders produce the SAME hash (order-independent)
    EXPECT_EQ(hash1, hash2);
}

TEST(ComponentSchemaTests, ComputeTypeHash_UniquePerVersion) {
    std::vector<PropertyDefinition> properties = {{"field", PropertyType::Int32, 0, 4}};

    auto structuralHash = ComponentSchema::computeStructuralHash(properties);

    auto hash1 = ComponentSchema::computeTypeHash("App", "Component", 1, structuralHash);
    auto hash2 = ComponentSchema::computeTypeHash("App", "Component", 2, structuralHash);

    // Different versions should produce different hashes
    EXPECT_NE(hash1, hash2);
}

TEST(ComponentSchemaTests, StructuralCompatibility_Identical) {
    std::vector<PropertyDefinition> properties = {{"position", PropertyType::Vec3, 0, 12}};

    auto result1 = ComponentSchema::create("App1", "Transform", 1, properties, 12, false);
    auto result2 = ComponentSchema::create("App2", "Transform", 1, properties, 12, false);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Same structure should be compatible
    EXPECT_TRUE(result1.value.isStructurallyCompatible(result2.value));
}

TEST(ComponentSchemaTests, StructuralCompatibility_Different) {
    std::vector<PropertyDefinition> properties1 = {{"position", PropertyType::Vec3, 0, 12}};

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
    std::vector<PropertyDefinition> sourceProps = {{"position", PropertyType::Vec3, 0, 12},
                                                   {"velocity", PropertyType::Vec3, 12, 12}};

    std::vector<PropertyDefinition> targetProps = {{"position", PropertyType::Vec3, 0, 12}};

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
    std::vector<PropertyDefinition> sourceProps = {{"position", PropertyType::Vec3, 0, 12}};

    std::vector<PropertyDefinition> targetProps = {{"position", PropertyType::Vec3, 0, 12},
                                                   {"velocity", PropertyType::Vec3, 12, 12}};

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
    std::vector<PropertyDefinition> sourceProps = {{"position", PropertyType::Vec3, 0, 12}};

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
    std::vector<PropertyDefinition> sourceProps = {{"position", PropertyType::Vec3, 0, 12}};

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

// ============================================================================
// Test Vectors - Canonical String Hashing
// ============================================================================
// These tests document the exact canonical string format and hash values
// for cross-language validation and reference implementations.

TEST(ComponentSchemaTests, TestVector_SimpleTransform) {
    // Test Vector 1: Simple Transform component with position and rotation
    std::vector<PropertyDefinition> props = {{"position", PropertyType::Vec3, 0, 12},
                                             {"rotation", PropertyType::Quat, 12, 16}};

    auto result = ComponentSchema::create("TestApp", "Transform", 1, props, 28, false);

    ASSERT_TRUE(result.success());

    // Verify canonical string format
    std::string canonical = result.value.toCanonicalString();
    EXPECT_EQ(canonical, "TestApp.Transform@1{position:Vec3:0:12,rotation:Quat:12:16}");

    // Document the computed hashes (these are reference values)
    EXPECT_FALSE(result.value.structuralHash.isNull());
    EXPECT_FALSE(result.value.typeHash.isNull());
}

TEST(ComponentSchemaTests, TestVector_Physics) {
    // Test Vector 2: Physics component with multiple properties
    std::vector<PropertyDefinition> props = {{"mass", PropertyType::Float32, 0, 4},
                                             {"velocity", PropertyType::Vec3, 4, 12},
                                             {"acceleration", PropertyType::Vec3, 16, 12}};

    auto result = ComponentSchema::create("PhysicsEngine", "RigidBody", 2, props, 28,
                                          true  // Public schema
    );

    ASSERT_TRUE(result.success());

    // Verify canonical string (properties sorted alphabetically)
    std::string canonical = result.value.toCanonicalString();
    EXPECT_EQ(canonical, "PhysicsEngine.RigidBody@2{acceleration:Vec3:16:12,mass:Float32:0:4,velocity:Vec3:4:12}");

    EXPECT_TRUE(result.value.isPublic);
}

TEST(ComponentSchemaTests, TestVector_SingleProperty) {
    // Test Vector 3: Minimal component with single property
    std::vector<PropertyDefinition> props = {{"health", PropertyType::Int32, 0, 4}};

    auto result = ComponentSchema::create("GameEngine", "Health", 1, props, 4, false);

    ASSERT_TRUE(result.success());

    std::string canonical = result.value.toCanonicalString();
    EXPECT_EQ(canonical, "GameEngine.Health@1{health:Int32:0:4}");
}

TEST(ComponentSchemaTests, TestVector_ComplexSchema) {
    // Test Vector 4: Complex schema with many property types
    std::vector<PropertyDefinition> props = {
        {"id", PropertyType::Int64, 0, 8},        {"name", PropertyType::String, 8, 64},
        {"position", PropertyType::Vec3, 72, 12}, {"rotation", PropertyType::Quat, 84, 16},
        {"scale", PropertyType::Vec3, 100, 12},   {"visible", PropertyType::Bool, 112, 1},
        {"layer", PropertyType::Int32, 116, 4}};

    auto result = ComponentSchema::create("RenderEngine", "Drawable", 3, props, 120, true);

    ASSERT_TRUE(result.success());

    // Properties should be sorted alphabetically in canonical form
    std::string canonical = result.value.toCanonicalString();
    EXPECT_EQ(canonical,
              "RenderEngine.Drawable@3{id:Int64:0:8,layer:Int32:116:4,name:String:8:64,"
              "position:Vec3:72:12,rotation:Quat:84:16,scale:Vec3:100:12,visible:Bool:112:1}");
}

TEST(ComponentSchemaTests, TestVector_PropertyOrdering) {
    // Test Vector 5: Verify that input order doesn't affect canonical form
    std::vector<PropertyDefinition> props1 = {{"z_last", PropertyType::Float32, 0, 4},
                                              {"a_first", PropertyType::Float32, 4, 4},
                                              {"m_middle", PropertyType::Float32, 8, 4}};

    std::vector<PropertyDefinition> props2 = {{"a_first", PropertyType::Float32, 4, 4},
                                              {"m_middle", PropertyType::Float32, 8, 4},
                                              {"z_last", PropertyType::Float32, 0, 4}};

    auto result1 = ComponentSchema::create("App", "Test", 1, props1, 12, false);
    auto result2 = ComponentSchema::create("App", "Test", 1, props2, 12, false);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Both should produce identical canonical strings (alphabetically sorted)
    EXPECT_EQ(result1.value.toCanonicalString(), result2.value.toCanonicalString());
    EXPECT_EQ(result1.value.toCanonicalString(),
              "App.Test@1{a_first:Float32:4:4,m_middle:Float32:8:4,z_last:Float32:0:4}");

    // And identical hashes
    EXPECT_EQ(result1.value.structuralHash, result2.value.structuralHash);
    EXPECT_EQ(result1.value.typeHash, result2.value.typeHash);
}

TEST(ComponentSchemaTests, TestVector_ASCIIIdentifiers) {
    // Test Vector 6: Various valid ASCII identifier formats
    std::vector<PropertyDefinition> props = {{"_private", PropertyType::Int32, 0, 4},
                                             {"snake_case", PropertyType::Int32, 4, 4},
                                             {"camelCase", PropertyType::Int32, 8, 4},
                                             {"PascalCase", PropertyType::Int32, 12, 4},
                                             {"with123numbers", PropertyType::Int32, 16, 4}};

    auto result = ComponentSchema::create("MyApp_v2", "Test_Component", 1, props, 20, false);

    ASSERT_TRUE(result.success());

    std::string canonical = result.value.toCanonicalString();
    EXPECT_EQ(canonical,
              "MyApp_v2.Test_Component@1{PascalCase:Int32:12:4,_private:Int32:0:4,"
              "camelCase:Int32:8:4,snake_case:Int32:4:4,with123numbers:Int32:16:4}");
}

TEST(ComponentSchemaTests, TestVector_VersionDifferentiation) {
    // Test Vector 7: Same structure, different versions produce different type hashes
    std::vector<PropertyDefinition> props = {{"value", PropertyType::Float32, 0, 4}};

    auto v1 = ComponentSchema::create("App", "Component", 1, props, 4, false);
    auto v2 = ComponentSchema::create("App", "Component", 2, props, 4, false);
    auto v3 = ComponentSchema::create("App", "Component", 3, props, 4, false);

    ASSERT_TRUE(v1.success());
    ASSERT_TRUE(v2.success());
    ASSERT_TRUE(v3.success());

    // Same structural hash (identical structure)
    EXPECT_EQ(v1.value.structuralHash, v2.value.structuralHash);
    EXPECT_EQ(v2.value.structuralHash, v3.value.structuralHash);

    // Different type hashes (different versions)
    EXPECT_NE(v1.value.typeHash, v2.value.typeHash);
    EXPECT_NE(v2.value.typeHash, v3.value.typeHash);
    EXPECT_NE(v1.value.typeHash, v3.value.typeHash);

    // Canonical strings differ by version number
    EXPECT_EQ(v1.value.toCanonicalString(), "App.Component@1{value:Float32:0:4}");
    EXPECT_EQ(v2.value.toCanonicalString(), "App.Component@2{value:Float32:0:4}");
    EXPECT_EQ(v3.value.toCanonicalString(), "App.Component@3{value:Float32:0:4}");
}

// ============================================================================
// Property Metadata Tests
// ============================================================================
// Tests for required flag and defaultValue functionality

TEST(ComponentSchemaTests, PropertyMetadata_DefaultRequired) {
    // Properties are required by default
    std::vector<PropertyDefinition> props = {{"health", PropertyType::Int32, 0, 4}};

    auto result = ComponentSchema::create("App", "Health", 1, props, 4, false);
    ASSERT_TRUE(result.success());

    EXPECT_TRUE(result.value.properties[0].required);
    EXPECT_FALSE(result.value.properties[0].defaultValue.has_value());
}

TEST(ComponentSchemaTests, PropertyMetadata_OptionalWithDefault) {
    // Optional property with default value
    std::vector<PropertyDefinition> props = {{"score", PropertyType::Int32, 0, 4, false, int32_t{0}}};

    auto result = ComponentSchema::create("App", "Player", 1, props, 4, false);
    ASSERT_TRUE(result.success());

    EXPECT_FALSE(result.value.properties[0].required);
    ASSERT_TRUE(result.value.properties[0].defaultValue.has_value());

    // Verify the default value
    auto& defaultVal = result.value.properties[0].defaultValue.value();
    EXPECT_TRUE(std::holds_alternative<int32_t>(defaultVal));
    EXPECT_EQ(std::get<int32_t>(defaultVal), 0);
}

TEST(ComponentSchemaTests, PropertyMetadata_DefaultValueTypeMismatch) {
    // Default value type doesn't match property type - should fail
    std::vector<PropertyDefinition> props = {
        {"health", PropertyType::Int32, 0, 4, true, float{100.0f}}  // Wrong type
    };

    auto result = ComponentSchema::create("App", "Health", 1, props, 4, false);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::SchemaValidationFailed);
    EXPECT_NE(result.errorMessage.find("defaultValue type mismatch"), std::string::npos);
}

TEST(ComponentSchemaTests, PropertyMetadata_MultipleDefaults) {
    // Multiple properties with different default values
    std::vector<PropertyDefinition> props = {{"health", PropertyType::Int32, 0, 4, true, int32_t{100}},
                                             {"speed", PropertyType::Float32, 4, 4, false, float{5.0f}},
                                             {"name", PropertyType::String, 8, 64, false, std::string{"Player"}}};

    auto result = ComponentSchema::create("App", "Character", 1, props, 72, false);
    ASSERT_TRUE(result.success());

    // Verify health (required with default)
    EXPECT_TRUE(result.value.properties[0].required);
    ASSERT_TRUE(result.value.properties[0].defaultValue.has_value());
    EXPECT_EQ(std::get<int32_t>(result.value.properties[0].defaultValue.value()), 100);

    // Verify speed (optional with default)
    EXPECT_FALSE(result.value.properties[1].required);
    ASSERT_TRUE(result.value.properties[1].defaultValue.has_value());
    EXPECT_FLOAT_EQ(std::get<float>(result.value.properties[1].defaultValue.value()), 5.0f);

    // Verify name (optional with default)
    EXPECT_FALSE(result.value.properties[2].required);
    ASSERT_TRUE(result.value.properties[2].defaultValue.has_value());
    EXPECT_EQ(std::get<std::string>(result.value.properties[2].defaultValue.value()), "Player");
}

TEST(ComponentSchemaTests, PropertyMetadata_Vec3Default) {
    // Vector type with default value
    Vec3 defaultPos{0.0f, 0.0f, 0.0f};
    std::vector<PropertyDefinition> props = {{"position", PropertyType::Vec3, 0, 12, false, defaultPos}};

    auto result = ComponentSchema::create("App", "Transform", 1, props, 12, false);
    ASSERT_TRUE(result.success());

    EXPECT_FALSE(result.value.properties[0].required);
    ASSERT_TRUE(result.value.properties[0].defaultValue.has_value());

    auto& defaultVal = result.value.properties[0].defaultValue.value();
    EXPECT_TRUE(std::holds_alternative<Vec3>(defaultVal));
    auto& vec = std::get<Vec3>(defaultVal);
    EXPECT_FLOAT_EQ(vec.x, 0.0f);
    EXPECT_FLOAT_EQ(vec.y, 0.0f);
    EXPECT_FLOAT_EQ(vec.z, 0.0f);
}

TEST(ComponentSchemaTests, PropertyMetadata_DoesNotAffectStructuralHash) {
    // Metadata (required, defaultValue) should NOT affect structural hash
    std::vector<PropertyDefinition> props1 = {
        {"health", PropertyType::Int32, 0, 4, true}  // Required, no default
    };

    std::vector<PropertyDefinition> props2 = {
        {"health", PropertyType::Int32, 0, 4, false, int32_t{100}}  // Optional with default
    };

    auto result1 = ComponentSchema::create("App", "A", 1, props1, 4, false);
    auto result2 = ComponentSchema::create("App", "B", 1, props2, 4, false);

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());

    // Structural hashes should be identical (metadata not included)
    EXPECT_EQ(result1.value.structuralHash, result2.value.structuralHash);

    // Type hashes should be different (different app/component names)
    EXPECT_NE(result1.value.typeHash, result2.value.typeHash);
}

TEST(ComponentSchemaTests, PropertyMetadata_OptionalWithoutDefault) {
    // Optional property without default is allowed
    std::vector<PropertyDefinition> props = {
        {"nickname", PropertyType::String, 0, 64, false}  // Optional, no default
    };

    auto result = ComponentSchema::create("App", "Player", 1, props, 64, false);
    ASSERT_TRUE(result.success());

    EXPECT_FALSE(result.value.properties[0].required);
    EXPECT_FALSE(result.value.properties[0].defaultValue.has_value());
}

TEST(ComponentSchemaTests, PropertyMetadata_ArrayTypeDefault) {
    // Array type with default value
    std::vector<int32_t> defaultArray{1, 2, 3};
    std::vector<PropertyDefinition> props = {{"values", PropertyType::Int32Array, 0, 24, false, defaultArray}};

    auto result = ComponentSchema::create("App", "Data", 1, props, 24, false);
    ASSERT_TRUE(result.success());

    ASSERT_TRUE(result.value.properties[0].defaultValue.has_value());
    auto& defaultVal = result.value.properties[0].defaultValue.value();
    EXPECT_TRUE(std::holds_alternative<std::vector<int32_t>>(defaultVal));
    auto& array = std::get<std::vector<int32_t>>(defaultVal);
    EXPECT_EQ(array.size(), 3);
    EXPECT_EQ(array[0], 1);
    EXPECT_EQ(array[1], 2);
    EXPECT_EQ(array[2], 3);
}

TEST(ComponentSchemaTests, PropertyMetadata_BoolDefault) {
    // Boolean property with default
    std::vector<PropertyDefinition> props = {{"enabled", PropertyType::Bool, 0, 1, false, true}};

    auto result = ComponentSchema::create("App", "Feature", 1, props, 1, false);
    ASSERT_TRUE(result.success());

    ASSERT_TRUE(result.value.properties[0].defaultValue.has_value());
    EXPECT_TRUE(std::get<bool>(result.value.properties[0].defaultValue.value()));
}
