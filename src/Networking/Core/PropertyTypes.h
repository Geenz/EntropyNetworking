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

#include <string>
#include <variant>
#include <vector>

#include "NetworkTypes.h"

namespace EntropyEngine
{
namespace Networking
{

/**
 * @brief Property type enumeration
 *
 * Matches the PropertyType enum in entropy.capnp
 */
enum class PropertyType
{
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
    Bytes,

    // Array types
    Int32Array,
    Int64Array,
    Float32Array,
    Float64Array,
    Vec2Array,
    Vec3Array,
    Vec4Array,
    QuatArray
};

/**
 * @brief Type-safe variant for property values
 *
 * Holds any of the supported property types in a type-safe manner.
 * Includes scalar types and array types.
 */
using PropertyValue = std::variant<int32_t, int64_t, float, double, Vec2, Vec3, Vec4, Quat, std::string, bool,
                                   std::vector<uint8_t>,  // Bytes

                                   // Array types
                                   std::vector<int32_t>, std::vector<int64_t>, std::vector<float>, std::vector<double>,
                                   std::vector<Vec2>, std::vector<Vec3>, std::vector<Vec4>, std::vector<Quat> >;

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

/**
 * @brief Convert C++ PropertyType to Cap'n Proto PropertyType enum
 *
 * Provides explicit, safe conversion between C++ and Cap'n Proto enum types
 * rather than relying on implicit static_cast which assumes matching ordinals.
 *
 * Note: Return type is uint16_t to avoid including capnp headers here.
 * Cast result to ::PropertyType when using with Cap'n Proto builders.
 *
 * @param type The C++ PropertyType
 * @return The corresponding Cap'n Proto PropertyType ordinal
 */
uint16_t toCapnpPropertyType(PropertyType type);

/**
 * @brief Convert Cap'n Proto PropertyType to C++ PropertyType enum
 *
 * Provides explicit, safe conversion between Cap'n Proto and C++ enum types.
 *
 * @param capnpType The Cap'n Proto PropertyType ordinal
 * @return The corresponding C++ PropertyType
 */
PropertyType fromCapnpPropertyType(uint16_t capnpType);

}  // namespace Networking
}  // namespace EntropyEngine
