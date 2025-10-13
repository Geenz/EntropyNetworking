/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Core/PropertyTypes.h"

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
    Quat q{0.0f, 0.0f, 0.0f, 1.0f};

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
    Quat identity;  // Default constructor creates identity quaternion
    EXPECT_EQ(identity.x, 0.0f);
    EXPECT_EQ(identity.y, 0.0f);
    EXPECT_EQ(identity.z, 0.0f);
    EXPECT_EQ(identity.w, 1.0f);
}
