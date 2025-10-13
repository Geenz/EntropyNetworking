/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyTypes.h"

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

} // namespace Networking
} // namespace EntropyEngine
