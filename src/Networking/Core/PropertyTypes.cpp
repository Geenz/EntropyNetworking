/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyTypes.h"

#include "src/Networking/Protocol/entropy.capnp.h"

namespace EntropyEngine
{
namespace Networking
{

bool validatePropertyType(const PropertyValue& value, PropertyType expectedType) {
    return getPropertyType(value) == expectedType;
}

PropertyType getPropertyType(const PropertyValue& value) {
    if (std::holds_alternative<int32_t>(value)) return PropertyType::Int32;
    if (std::holds_alternative<int64_t>(value)) return PropertyType::Int64;
    if (std::holds_alternative<float>(value)) return PropertyType::Float32;
    if (std::holds_alternative<double>(value)) return PropertyType::Float64;
    if (std::holds_alternative<Vec2>(value)) return PropertyType::Vec2;
    if (std::holds_alternative<Vec3>(value)) return PropertyType::Vec3;
    if (std::holds_alternative<Vec4>(value)) return PropertyType::Vec4;
    if (std::holds_alternative<Quat>(value)) return PropertyType::Quat;
    if (std::holds_alternative<std::string>(value)) return PropertyType::String;
    if (std::holds_alternative<bool>(value)) return PropertyType::Bool;
    if (std::holds_alternative<std::vector<uint8_t>>(value)) return PropertyType::Bytes;

    // Array types
    if (std::holds_alternative<std::vector<int32_t>>(value)) return PropertyType::Int32Array;
    if (std::holds_alternative<std::vector<int64_t>>(value)) return PropertyType::Int64Array;
    if (std::holds_alternative<std::vector<float>>(value)) return PropertyType::Float32Array;
    if (std::holds_alternative<std::vector<double>>(value)) return PropertyType::Float64Array;
    if (std::holds_alternative<std::vector<Vec2>>(value)) return PropertyType::Vec2Array;
    if (std::holds_alternative<std::vector<Vec3>>(value)) return PropertyType::Vec3Array;
    if (std::holds_alternative<std::vector<Vec4>>(value)) return PropertyType::Vec4Array;
    if (std::holds_alternative<std::vector<Quat>>(value)) return PropertyType::QuatArray;

    // Should never reach here
    return PropertyType::Int32;
}

const char* propertyTypeToString(PropertyType type) {
    switch (type) {
        case PropertyType::Int32:
            return "Int32";
        case PropertyType::Int64:
            return "Int64";
        case PropertyType::Float32:
            return "Float32";
        case PropertyType::Float64:
            return "Float64";
        case PropertyType::Vec2:
            return "Vec2";
        case PropertyType::Vec3:
            return "Vec3";
        case PropertyType::Vec4:
            return "Vec4";
        case PropertyType::Quat:
            return "Quat";
        case PropertyType::String:
            return "String";
        case PropertyType::Bool:
            return "Bool";
        case PropertyType::Bytes:
            return "Bytes";
        case PropertyType::Int32Array:
            return "Int32Array";
        case PropertyType::Int64Array:
            return "Int64Array";
        case PropertyType::Float32Array:
            return "Float32Array";
        case PropertyType::Float64Array:
            return "Float64Array";
        case PropertyType::Vec2Array:
            return "Vec2Array";
        case PropertyType::Vec3Array:
            return "Vec3Array";
        case PropertyType::Vec4Array:
            return "Vec4Array";
        case PropertyType::QuatArray:
            return "QuatArray";
        default:
            return "Unknown";
    }
}

uint16_t toCapnpPropertyType(PropertyType type) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Returns uint16_t (underlying type) to avoid capnp header dependency
    switch (type) {
        case PropertyType::Int32:
            return static_cast<uint16_t>(::PropertyType::INT32);
        case PropertyType::Int64:
            return static_cast<uint16_t>(::PropertyType::INT64);
        case PropertyType::Float32:
            return static_cast<uint16_t>(::PropertyType::FLOAT32);
        case PropertyType::Float64:
            return static_cast<uint16_t>(::PropertyType::FLOAT64);
        case PropertyType::Vec2:
            return static_cast<uint16_t>(::PropertyType::VEC2);
        case PropertyType::Vec3:
            return static_cast<uint16_t>(::PropertyType::VEC3);
        case PropertyType::Vec4:
            return static_cast<uint16_t>(::PropertyType::VEC4);
        case PropertyType::Quat:
            return static_cast<uint16_t>(::PropertyType::QUAT);
        case PropertyType::String:
            return static_cast<uint16_t>(::PropertyType::STRING);
        case PropertyType::Bool:
            return static_cast<uint16_t>(::PropertyType::BOOL);
        case PropertyType::Bytes:
            return static_cast<uint16_t>(::PropertyType::BYTES);
        case PropertyType::Int32Array:
            return static_cast<uint16_t>(::PropertyType::INT32_ARRAY);
        case PropertyType::Int64Array:
            return static_cast<uint16_t>(::PropertyType::INT64_ARRAY);
        case PropertyType::Float32Array:
            return static_cast<uint16_t>(::PropertyType::FLOAT32_ARRAY);
        case PropertyType::Float64Array:
            return static_cast<uint16_t>(::PropertyType::FLOAT64_ARRAY);
        case PropertyType::Vec2Array:
            return static_cast<uint16_t>(::PropertyType::VEC2_ARRAY);
        case PropertyType::Vec3Array:
            return static_cast<uint16_t>(::PropertyType::VEC3_ARRAY);
        case PropertyType::Vec4Array:
            return static_cast<uint16_t>(::PropertyType::VEC4_ARRAY);
        case PropertyType::QuatArray:
            return static_cast<uint16_t>(::PropertyType::QUAT_ARRAY);
        default:
            return static_cast<uint16_t>(::PropertyType::INT32);  // Fallback
    }
}

PropertyType fromCapnpPropertyType(uint16_t capnpType) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Accepts uint16_t to avoid capnp header dependency
    auto type = static_cast<::PropertyType>(capnpType);
    switch (type) {
        case ::PropertyType::INT32:
            return PropertyType::Int32;
        case ::PropertyType::INT64:
            return PropertyType::Int64;
        case ::PropertyType::FLOAT32:
            return PropertyType::Float32;
        case ::PropertyType::FLOAT64:
            return PropertyType::Float64;
        case ::PropertyType::VEC2:
            return PropertyType::Vec2;
        case ::PropertyType::VEC3:
            return PropertyType::Vec3;
        case ::PropertyType::VEC4:
            return PropertyType::Vec4;
        case ::PropertyType::QUAT:
            return PropertyType::Quat;
        case ::PropertyType::STRING:
            return PropertyType::String;
        case ::PropertyType::BOOL:
            return PropertyType::Bool;
        case ::PropertyType::BYTES:
            return PropertyType::Bytes;
        case ::PropertyType::INT32_ARRAY:
            return PropertyType::Int32Array;
        case ::PropertyType::INT64_ARRAY:
            return PropertyType::Int64Array;
        case ::PropertyType::FLOAT32_ARRAY:
            return PropertyType::Float32Array;
        case ::PropertyType::FLOAT64_ARRAY:
            return PropertyType::Float64Array;
        case ::PropertyType::VEC2_ARRAY:
            return PropertyType::Vec2Array;
        case ::PropertyType::VEC3_ARRAY:
            return PropertyType::Vec3Array;
        case ::PropertyType::VEC4_ARRAY:
            return PropertyType::Vec4Array;
        case ::PropertyType::QUAT_ARRAY:
            return PropertyType::QuatArray;
        default:
            return PropertyType::Int32;  // Fallback
    }
}

}  // namespace Networking
}  // namespace EntropyEngine
