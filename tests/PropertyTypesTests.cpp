/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <capnp/message.h>
#include <gtest/gtest.h>

#include "../src/Networking/Core/PropertyTypes.h"
#include "../src/Networking/Protocol/ComponentSchemaSerializer.h"

using namespace EntropyEngine::Networking;

TEST(PropertyTypesTests, GetPropertyTypeInt32) {
    PropertyValue val = static_cast<int32_t>(42);
    EXPECT_EQ(getPropertyType(val), PropertyType::Int32);
}

TEST(PropertyTypesTests, GetPropertyTypeInt64) {
    PropertyValue val = static_cast<int64_t>(42);
    EXPECT_EQ(getPropertyType(val), PropertyType::Int64);
}

TEST(PropertyTypesTests, GetPropertyTypeFloat32) {
    PropertyValue val = 3.14f;
    EXPECT_EQ(getPropertyType(val), PropertyType::Float32);
}

TEST(PropertyTypesTests, GetPropertyTypeFloat64) {
    PropertyValue val = 3.14;
    EXPECT_EQ(getPropertyType(val), PropertyType::Float64);
}

TEST(PropertyTypesTests, GetPropertyTypeVec3) {
    PropertyValue val = Vec3{1.0f, 2.0f, 3.0f};
    EXPECT_EQ(getPropertyType(val), PropertyType::Vec3);
}

TEST(PropertyTypesTests, GetPropertyTypeString) {
    PropertyValue val = std::string("hello");
    EXPECT_EQ(getPropertyType(val), PropertyType::String);
}

TEST(PropertyTypesTests, GetPropertyTypeBool) {
    PropertyValue val = true;
    EXPECT_EQ(getPropertyType(val), PropertyType::Bool);
}

TEST(PropertyTypesTests, GetPropertyTypeBytes) {
    PropertyValue val = std::vector<uint8_t>{1, 2, 3};
    EXPECT_EQ(getPropertyType(val), PropertyType::Bytes);
}

TEST(PropertyTypesTests, ValidatePropertyTypeCorrect) {
    PropertyValue val = static_cast<int32_t>(42);
    EXPECT_TRUE(validatePropertyType(val, PropertyType::Int32));
}

TEST(PropertyTypesTests, ValidatePropertyTypeIncorrect) {
    PropertyValue val = static_cast<int32_t>(42);
    EXPECT_FALSE(validatePropertyType(val, PropertyType::Float32));
}

TEST(PropertyTypesTests, PropertyTypeToString) {
    EXPECT_STREQ(propertyTypeToString(PropertyType::Int32), "Int32");
    EXPECT_STREQ(propertyTypeToString(PropertyType::Float32), "Float32");
    EXPECT_STREQ(propertyTypeToString(PropertyType::Vec3), "Vec3");
    EXPECT_STREQ(propertyTypeToString(PropertyType::String), "String");
}

TEST(PropertyTypesTests, VectorTypes) {
    Vec2 v2{1.0f, 2.0f};
    Vec3 v3{1.0f, 2.0f, 3.0f};
    Vec4 v4{1.0f, 2.0f, 3.0f, 4.0f};
    Quat q{1.0f, 0.0f, 0.0f, 0.0f};  // GLM constructor is (w, x, y, z)

    PropertyValue val2 = v2;
    PropertyValue val3 = v3;
    PropertyValue val4 = v4;
    PropertyValue valQ = q;

    EXPECT_EQ(getPropertyType(val2), PropertyType::Vec2);
    EXPECT_EQ(getPropertyType(val3), PropertyType::Vec3);
    EXPECT_EQ(getPropertyType(val4), PropertyType::Vec4);
    EXPECT_EQ(getPropertyType(valQ), PropertyType::Quat);
}

TEST(PropertyTypesTests, VectorEquality) {
    Vec3 v1{1.0f, 2.0f, 3.0f};
    Vec3 v2{1.0f, 2.0f, 3.0f};
    Vec3 v3{1.0f, 2.0f, 4.0f};

    EXPECT_EQ(v1, v2);
    EXPECT_NE(v1, v3);
}

TEST(PropertyTypesTests, QuaternionIdentity) {
    Quat identity{1.0f, 0.0f, 0.0f, 0.0f};  // Identity quaternion - GLM constructor is (w, x, y, z)
    EXPECT_EQ(identity.x, 0.0f);
    EXPECT_EQ(identity.y, 0.0f);
    EXPECT_EQ(identity.z, 0.0f);
    EXPECT_EQ(identity.w, 1.0f);
}

TEST(PropertyTypesTests, GetPropertyTypeMat3) {
    Mat3 m = Mat3(1.0f);  // Identity matrix
    PropertyValue val = m;
    EXPECT_EQ(getPropertyType(val), PropertyType::Mat3);
}

TEST(PropertyTypesTests, GetPropertyTypeMat4) {
    Mat4 m = Mat4(1.0f);  // Identity matrix
    PropertyValue val = m;
    EXPECT_EQ(getPropertyType(val), PropertyType::Mat4);
}

TEST(PropertyTypesTests, Mat3Identity) {
    Mat3 identity{1.0f};  // Identity matrix
    EXPECT_EQ(identity[0][0], 1.0f);
    EXPECT_EQ(identity[1][1], 1.0f);
    EXPECT_EQ(identity[2][2], 1.0f);
    EXPECT_EQ(identity[0][1], 0.0f);
    EXPECT_EQ(identity[1][0], 0.0f);
}

TEST(PropertyTypesTests, Mat4Identity) {
    Mat4 identity{1.0f};  // Identity matrix
    EXPECT_EQ(identity[0][0], 1.0f);
    EXPECT_EQ(identity[1][1], 1.0f);
    EXPECT_EQ(identity[2][2], 1.0f);
    EXPECT_EQ(identity[3][3], 1.0f);
    EXPECT_EQ(identity[0][1], 0.0f);
    EXPECT_EQ(identity[3][0], 0.0f);
}

TEST(PropertyTypesTests, Mat3Equality) {
    Mat3 m1{1.0f};
    Mat3 m2{1.0f};
    Mat3 m3{2.0f};  // Scaled identity

    EXPECT_EQ(m1, m2);
    EXPECT_NE(m1, m3);
}

TEST(PropertyTypesTests, Mat4Equality) {
    Mat4 m1{1.0f};
    Mat4 m2{1.0f};
    Mat4 m3{2.0f};  // Scaled identity

    EXPECT_EQ(m1, m2);
    EXPECT_NE(m1, m3);
}

TEST(PropertyTypesTests, PropertyTypeToString_MatrixTypes) {
    EXPECT_STREQ(propertyTypeToString(PropertyType::Mat3), "Mat3");
    EXPECT_STREQ(propertyTypeToString(PropertyType::Mat4), "Mat4");
}

// Round-trip tests: verify all PropertyType enums can be converted to Cap'n Proto and back
TEST(PropertyTypesTests, RoundTrip_ScalarTypes) {
    // Test all scalar types
    std::vector<PropertyType> scalarTypes = {PropertyType::Int32,   PropertyType::Int64, PropertyType::Float32,
                                             PropertyType::Float64, PropertyType::Vec2,  PropertyType::Vec3,
                                             PropertyType::Vec4,    PropertyType::Quat,  PropertyType::String,
                                             PropertyType::Bool,    PropertyType::Bytes};

    for (auto type : scalarTypes) {
        uint16_t capnpType = toCapnpPropertyType(type);
        PropertyType roundTrip = fromCapnpPropertyType(capnpType);
        EXPECT_EQ(type, roundTrip) << "Round-trip failed for " << propertyTypeToString(type);
    }
}

TEST(PropertyTypesTests, RoundTrip_ArrayTypes) {
    // Test all array types
    std::vector<PropertyType> arrayTypes = {
        PropertyType::Int32Array, PropertyType::Int64Array, PropertyType::Float32Array, PropertyType::Float64Array,
        PropertyType::Vec2Array,  PropertyType::Vec3Array,  PropertyType::Vec4Array,    PropertyType::QuatArray};

    for (auto type : arrayTypes) {
        uint16_t capnpType = toCapnpPropertyType(type);
        PropertyType roundTrip = fromCapnpPropertyType(capnpType);
        EXPECT_EQ(type, roundTrip) << "Round-trip failed for " << propertyTypeToString(type);
    }
}

TEST(PropertyTypesTests, RoundTrip_MatrixTypes) {
    // Test matrix types
    std::vector<PropertyType> matrixTypes = {PropertyType::Mat3, PropertyType::Mat4};

    for (auto type : matrixTypes) {
        uint16_t capnpType = toCapnpPropertyType(type);
        PropertyType roundTrip = fromCapnpPropertyType(capnpType);
        EXPECT_EQ(type, roundTrip) << "Round-trip failed for " << propertyTypeToString(type);
    }
}

TEST(PropertyTypesTests, RoundTrip_AllTypes) {
    // Comprehensive test of all types
    std::vector<PropertyType> allTypes = {
        PropertyType::Int32,      PropertyType::Int64,        PropertyType::Float32,      PropertyType::Float64,
        PropertyType::Vec2,       PropertyType::Vec3,         PropertyType::Vec4,         PropertyType::Quat,
        PropertyType::String,     PropertyType::Bool,         PropertyType::Bytes,        PropertyType::Int32Array,
        PropertyType::Int64Array, PropertyType::Float32Array, PropertyType::Float64Array, PropertyType::Vec2Array,
        PropertyType::Vec3Array,  PropertyType::Vec4Array,    PropertyType::QuatArray,    PropertyType::Mat3,
        PropertyType::Mat4};

    for (auto type : allTypes) {
        // Convert to Cap'n Proto
        uint16_t capnpType = toCapnpPropertyType(type);

        // Convert back to C++
        PropertyType roundTrip = fromCapnpPropertyType(capnpType);

        // Verify round-trip preserves type
        EXPECT_EQ(type, roundTrip) << "Round-trip failed for " << propertyTypeToString(type);

        // Verify toString doesn't crash
        const char* name = propertyTypeToString(type);
        EXPECT_NE(name, nullptr);
        EXPECT_GT(strlen(name), 0u);
    }
}

TEST(PropertyTypesTests, CapnpMapping_UnknownValue_FailsClosed) {
    // Test that unknown Cap'n Proto values fail to a known type (fail closed)
    // This verifies we don't have undefined behavior for unknown enum values
    uint16_t invalidValue = 9999;
    PropertyType result = fromCapnpPropertyType(invalidValue);

    // Should fall back to Int32 (safe default)
    EXPECT_EQ(result, PropertyType::Int32);
}

// Serialization round-trip tests using Cap'n Proto
TEST(PropertyTypesTests, SerializationRoundTrip_Mat3) {
    // Create a non-trivial Mat3
    Mat3 original{1.0f};
    original[0][1] = 2.0f;
    original[1][2] = 3.0f;
    original[2][0] = 4.0f;

    PropertyValue originalVal = original;

    // Serialize to Cap'n Proto
    ::capnp::MallocMessageBuilder builder;
    auto propBuilder = builder.initRoot<Protocol::PropertyValue>();
    serializePropertyValue(originalVal, propBuilder);

    // Deserialize from Cap'n Proto
    auto reader = builder.getRoot<Protocol::PropertyValue>();
    PropertyValue roundTrip = deserializePropertyValue(reader);

    // Verify round-trip
    ASSERT_TRUE(std::holds_alternative<Mat3>(roundTrip));
    Mat3 result = std::get<Mat3>(roundTrip);

    // Compare all matrix elements
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            EXPECT_FLOAT_EQ(result[col][row], original[col][row]) << "Mismatch at [" << col << "][" << row << "]";
        }
    }
}

TEST(PropertyTypesTests, SerializationRoundTrip_Mat4) {
    // Create a non-trivial Mat4 (like a translation matrix)
    Mat4 original{1.0f};
    original[3][0] = 10.0f;  // Translation X
    original[3][1] = 20.0f;  // Translation Y
    original[3][2] = 30.0f;  // Translation Z
    original[0][1] = 0.5f;   // Some rotation component

    PropertyValue originalVal = original;

    // Serialize to Cap'n Proto
    ::capnp::MallocMessageBuilder builder;
    auto propBuilder = builder.initRoot<Protocol::PropertyValue>();
    serializePropertyValue(originalVal, propBuilder);

    // Deserialize from Cap'n Proto
    auto reader = builder.getRoot<Protocol::PropertyValue>();
    PropertyValue roundTrip = deserializePropertyValue(reader);

    // Verify round-trip
    ASSERT_TRUE(std::holds_alternative<Mat4>(roundTrip));
    Mat4 result = std::get<Mat4>(roundTrip);

    // Compare all matrix elements
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            EXPECT_FLOAT_EQ(result[col][row], original[col][row]) << "Mismatch at [" << col << "][" << row << "]";
        }
    }
}

TEST(PropertyTypesTests, SerializationRoundTrip_Mat3Identity) {
    Mat3 identity{1.0f};  // Identity matrix
    PropertyValue originalVal = identity;

    ::capnp::MallocMessageBuilder builder;
    auto propBuilder = builder.initRoot<Protocol::PropertyValue>();
    serializePropertyValue(originalVal, propBuilder);

    auto reader = builder.getRoot<Protocol::PropertyValue>();
    PropertyValue roundTrip = deserializePropertyValue(reader);

    ASSERT_TRUE(std::holds_alternative<Mat3>(roundTrip));
    Mat3 result = std::get<Mat3>(roundTrip);

    // Verify identity matrix
    EXPECT_FLOAT_EQ(result[0][0], 1.0f);
    EXPECT_FLOAT_EQ(result[1][1], 1.0f);
    EXPECT_FLOAT_EQ(result[2][2], 1.0f);
    EXPECT_FLOAT_EQ(result[0][1], 0.0f);
    EXPECT_FLOAT_EQ(result[1][0], 0.0f);
}

TEST(PropertyTypesTests, SerializationRoundTrip_Mat4Identity) {
    Mat4 identity{1.0f};  // Identity matrix
    PropertyValue originalVal = identity;

    ::capnp::MallocMessageBuilder builder;
    auto propBuilder = builder.initRoot<Protocol::PropertyValue>();
    serializePropertyValue(originalVal, propBuilder);

    auto reader = builder.getRoot<Protocol::PropertyValue>();
    PropertyValue roundTrip = deserializePropertyValue(reader);

    ASSERT_TRUE(std::holds_alternative<Mat4>(roundTrip));
    Mat4 result = std::get<Mat4>(roundTrip);

    // Verify identity matrix
    EXPECT_FLOAT_EQ(result[0][0], 1.0f);
    EXPECT_FLOAT_EQ(result[1][1], 1.0f);
    EXPECT_FLOAT_EQ(result[2][2], 1.0f);
    EXPECT_FLOAT_EQ(result[3][3], 1.0f);
    EXPECT_FLOAT_EQ(result[0][1], 0.0f);
    EXPECT_FLOAT_EQ(result[3][0], 0.0f);
}
