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

namespace EntropyEngine {
namespace Networking {

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

    // Should never reach here
    return PropertyType::Int32;
}

const char* propertyTypeToString(PropertyType type) {
    switch (type) {
        case PropertyType::Int32: return "Int32";
        case PropertyType::Int64: return "Int64";
        case PropertyType::Float32: return "Float32";
        case PropertyType::Float64: return "Float64";
        case PropertyType::Vec2: return "Vec2";
        case PropertyType::Vec3: return "Vec3";
        case PropertyType::Vec4: return "Vec4";
        case PropertyType::Quat: return "Quat";
        case PropertyType::String: return "String";
        case PropertyType::Bool: return "Bool";
        case PropertyType::Bytes: return "Bytes";
        default: return "Unknown";
    }
}

uint16_t toCapnpPropertyType(PropertyType type) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Returns uint16_t (underlying type) to avoid capnp header dependency
    switch (type) {
        case PropertyType::Int32: return static_cast<uint16_t>(::PropertyType::INT32);
        case PropertyType::Int64: return static_cast<uint16_t>(::PropertyType::INT64);
        case PropertyType::Float32: return static_cast<uint16_t>(::PropertyType::FLOAT32);
        case PropertyType::Float64: return static_cast<uint16_t>(::PropertyType::FLOAT64);
        case PropertyType::Vec2: return static_cast<uint16_t>(::PropertyType::VEC2);
        case PropertyType::Vec3: return static_cast<uint16_t>(::PropertyType::VEC3);
        case PropertyType::Vec4: return static_cast<uint16_t>(::PropertyType::VEC4);
        case PropertyType::Quat: return static_cast<uint16_t>(::PropertyType::QUAT);
        case PropertyType::String: return static_cast<uint16_t>(::PropertyType::STRING);
        case PropertyType::Bool: return static_cast<uint16_t>(::PropertyType::BOOL);
        case PropertyType::Bytes: return static_cast<uint16_t>(::PropertyType::BYTES);
        default: return static_cast<uint16_t>(::PropertyType::INT32);  // Fallback
    }
}

PropertyType fromCapnpPropertyType(uint16_t capnpType) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Accepts uint16_t to avoid capnp header dependency
    auto type = static_cast<::PropertyType>(capnpType);
    switch (type) {
        case ::PropertyType::INT32: return PropertyType::Int32;
        case ::PropertyType::INT64: return PropertyType::Int64;
        case ::PropertyType::FLOAT32: return PropertyType::Float32;
        case ::PropertyType::FLOAT64: return PropertyType::Float64;
        case ::PropertyType::VEC2: return PropertyType::Vec2;
        case ::PropertyType::VEC3: return PropertyType::Vec3;
        case ::PropertyType::VEC4: return PropertyType::Vec4;
        case ::PropertyType::QUAT: return PropertyType::Quat;
        case ::PropertyType::STRING: return PropertyType::String;
        case ::PropertyType::BOOL: return PropertyType::Bool;
        case ::PropertyType::BYTES: return PropertyType::Bytes;
        default: return PropertyType::Int32;  // Fallback
    }
}

} // namespace Networking
} // namespace EntropyEngine
