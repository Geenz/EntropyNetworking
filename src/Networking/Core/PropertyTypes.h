/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file PropertyTypes.h
 * @brief Property type system for Entropy networking
 *
 * Defines property types and values used in the network protocol.
 */

#pragma once

#include "NetworkTypes.h"
#include <variant>
#include <vector>
#include <string>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Property type enumeration
 *
 * Matches the PropertyType enum in entropy.capnp
 */
enum class PropertyType {
    Int32,
    Int64,
    Float32,
    Float64,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    String,
    Bool,
    Bytes
};

/**
 * @brief Type-safe variant for property values
 *
 * Holds any of the supported property types in a type-safe manner.
 */
using PropertyValue = std::variant<
    int32_t,
    int64_t,
    float,
    double,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    std::string,
    bool,
    std::vector<uint8_t>
>;

/**
 * @brief Validate that a property value matches the expected type
 *
 * @param value The property value variant
 * @param expectedType The expected type
 * @return true if types match, false otherwise
 *
 * @code
 * PropertyValue val = 42;
 * if (validatePropertyType(val, PropertyType::Int32)) {
 *     // Safe to use as int32
 * }
 * @endcode
 */
bool validatePropertyType(const PropertyValue& value, PropertyType expectedType);

/**
 * @brief Get the PropertyType for a PropertyValue variant
 *
 * @param value The property value
 * @return The PropertyType corresponding to the active variant
 *
 * @code
 * PropertyValue val = Vec3{1.0f, 2.0f, 3.0f};
 * PropertyType type = getPropertyType(val); // Returns PropertyType::Vec3
 * @endcode
 */
PropertyType getPropertyType(const PropertyValue& value);

/**
 * @brief Convert PropertyType to human-readable string
 *
 * @param type The property type
 * @return String representation
 */
const char* propertyTypeToString(PropertyType type);

} // namespace Networking
} // namespace EntropyEngine
