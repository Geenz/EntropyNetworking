/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file SchemaGeneration.h
 * @brief Auto-generation of ComponentSchema from EntropyCore reflection metadata
 *
 * Provides utilities to automatically generate ComponentSchema definitions from
 * types registered with EntropyCore's ENTROPY_REGISTER_TYPE and ENTROPY_FIELD macros.
 * Eliminates manual PropertyDefinition boilerplate.
 */

#pragma once

#include <TypeSystem/Reflection.h>

#include <format>
#include <type_traits>

#include "ComponentSchema.h"
#include "ErrorCodes.h"
#include "NetworkTypes.h"
#include "PropertyTypes.h"

namespace EntropyEngine
{
namespace Networking
{

using namespace EntropyEngine::Core::TypeSystem;

/**
 * @brief Compile-time mapping from C++ types to PropertyType enum
 *
 * Template specialization pattern for type-safe PropertyType mapping.
 * Primary template intentionally fails for unmapped types with static_assert.
 *
 * Applications can extend this by providing specializations for their own types:
 * @code
 * template<> struct TypeToPropertyType<MyCustomType> {
 *     static constexpr PropertyType value = PropertyType::Int32;
 * };
 * @endcode
 */
template <typename T>
struct TypeToPropertyType
{
    static_assert(!std::is_same_v<std::type_identity_t<T>, T>, "Type not mapped to PropertyType - add specialization");
};

// Fundamental types
template <>
struct TypeToPropertyType<int32_t>
{
    static constexpr PropertyType value = PropertyType::Int32;
};

template <>
struct TypeToPropertyType<int64_t>
{
    static constexpr PropertyType value = PropertyType::Int64;
};

template <>
struct TypeToPropertyType<uint64_t>
{
    static constexpr PropertyType value = PropertyType::Int64;  // Map unsigned to signed for network
};

template <>
struct TypeToPropertyType<float>
{
    static constexpr PropertyType value = PropertyType::Float32;
};

template <>
struct TypeToPropertyType<double>
{
    static constexpr PropertyType value = PropertyType::Float64;
};

template <>
struct TypeToPropertyType<bool>
{
    static constexpr PropertyType value = PropertyType::Bool;
};

template <>
struct TypeToPropertyType<std::string>
{
    static constexpr PropertyType value = PropertyType::String;
};

// EntropyNetworking vector types (from NetworkTypes.h)
template <>
struct TypeToPropertyType<Vec2>
{
    static constexpr PropertyType value = PropertyType::Vec2;
};

template <>
struct TypeToPropertyType<Vec3>
{
    static constexpr PropertyType value = PropertyType::Vec3;
};

template <>
struct TypeToPropertyType<Vec4>
{
    static constexpr PropertyType value = PropertyType::Vec4;
};

template <>
struct TypeToPropertyType<Quat>
{
    static constexpr PropertyType value = PropertyType::Quat;
};

// Array types
template <>
struct TypeToPropertyType<std::vector<uint8_t>>
{
    static constexpr PropertyType value = PropertyType::Bytes;
};

template <>
struct TypeToPropertyType<std::vector<int32_t>>
{
    static constexpr PropertyType value = PropertyType::Int32Array;
};

template <>
struct TypeToPropertyType<std::vector<int64_t>>
{
    static constexpr PropertyType value = PropertyType::Int64Array;
};

template <>
struct TypeToPropertyType<std::vector<uint64_t>>
{
    static constexpr PropertyType value = PropertyType::Int64Array;
};

template <>
struct TypeToPropertyType<std::vector<float>>
{
    static constexpr PropertyType value = PropertyType::Float32Array;
};

template <>
struct TypeToPropertyType<std::vector<Vec3>>
{
    static constexpr PropertyType value = PropertyType::Vec3Array;
};

// Generic enum support (all enums map to Int32)
template <typename T>
    requires std::is_enum_v<T>
struct TypeToPropertyType<T>
{
    static constexpr PropertyType value = PropertyType::Int32;
};

/**
 * @brief Runtime mapping from TypeID to PropertyType
 *
 * Dispatches TypeID comparison to known types. Required because TypeID
 * is runtime hash-based (not compile-time constexpr).
 *
 * Applications can extend this by adding their own type checks:
 * @code
 * inline std::optional<PropertyType> mapTypeIdToPropertyType(TypeID typeId) {
 *     // Check application types first
 *     if (typeId == createTypeId<MyCustomType>()) return PropertyType::Int32;
 *
 *     // Fall back to EntropyNetworking defaults
 *     return EntropyEngine::Networking::mapTypeIdToPropertyTypeDefault(typeId);
 * }
 * @endcode
 *
 * @param typeId TypeID from FieldInfo
 * @return PropertyType if mapped, nullopt otherwise
 */
inline std::optional<PropertyType> mapTypeIdToPropertyType(TypeID typeId) {
    // Fundamental types
    if (typeId == createTypeId<int32_t>()) return PropertyType::Int32;
    if (typeId == createTypeId<int64_t>()) return PropertyType::Int64;
    if (typeId == createTypeId<uint64_t>()) return PropertyType::Int64;
    if (typeId == createTypeId<float>()) return PropertyType::Float32;
    if (typeId == createTypeId<double>()) return PropertyType::Float64;
    if (typeId == createTypeId<bool>()) return PropertyType::Bool;
    if (typeId == createTypeId<std::string>()) return PropertyType::String;

    // EntropyNetworking vector types
    if (typeId == createTypeId<Vec2>()) return PropertyType::Vec2;
    if (typeId == createTypeId<Vec3>()) return PropertyType::Vec3;
    if (typeId == createTypeId<Vec4>()) return PropertyType::Vec4;
    if (typeId == createTypeId<Quat>()) return PropertyType::Quat;

    // Array types
    if (typeId == createTypeId<std::vector<uint8_t>>()) return PropertyType::Bytes;
    if (typeId == createTypeId<std::vector<int32_t>>()) return PropertyType::Int32Array;
    if (typeId == createTypeId<std::vector<int64_t>>()) return PropertyType::Int64Array;
    if (typeId == createTypeId<std::vector<uint64_t>>()) return PropertyType::Int64Array;
    if (typeId == createTypeId<std::vector<float>>()) return PropertyType::Float32Array;
    if (typeId == createTypeId<std::vector<Vec3>>()) return PropertyType::Vec3Array;

    return std::nullopt;
}

/**
 * @brief Runtime field size lookup
 *
 * Workaround for FieldInfo not containing size information.
 *
 * TODO: Submit PR to EntropyCore to add size_t to FieldInfo,
 * eliminating need for this dispatch table.
 *
 * Applications can extend this by adding their own type checks:
 * @code
 * inline size_t getFieldSize(TypeID typeId) {
 *     // Check application types first
 *     if (typeId == createTypeId<MyCustomType>()) return sizeof(MyCustomType);
 *
 *     // Fall back to EntropyNetworking defaults
 *     return EntropyEngine::Networking::getFieldSizeDefault(typeId);
 * }
 * @endcode
 *
 * @param typeId TypeID from FieldInfo
 * @return Field size in bytes, or 0 if unknown
 */
inline size_t getFieldSize(TypeID typeId) {
    // Fundamental types
    if (typeId == createTypeId<int32_t>()) return sizeof(int32_t);
    if (typeId == createTypeId<int64_t>()) return sizeof(int64_t);
    if (typeId == createTypeId<uint64_t>()) return sizeof(uint64_t);
    if (typeId == createTypeId<float>()) return sizeof(float);
    if (typeId == createTypeId<double>()) return sizeof(double);
    if (typeId == createTypeId<bool>()) return sizeof(bool);
    if (typeId == createTypeId<std::string>()) return sizeof(std::string);

    // EntropyNetworking vector types
    if (typeId == createTypeId<Vec2>()) return sizeof(Vec2);
    if (typeId == createTypeId<Vec3>()) return sizeof(Vec3);
    if (typeId == createTypeId<Vec4>()) return sizeof(Vec4);
    if (typeId == createTypeId<Quat>()) return sizeof(Quat);

    // Array types (container size, not element count)
    if (typeId == createTypeId<std::vector<uint8_t>>()) return sizeof(std::vector<uint8_t>);
    if (typeId == createTypeId<std::vector<int32_t>>()) return sizeof(std::vector<int32_t>);
    if (typeId == createTypeId<std::vector<int64_t>>()) return sizeof(std::vector<int64_t>);
    if (typeId == createTypeId<std::vector<uint64_t>>()) return sizeof(std::vector<uint64_t>);
    if (typeId == createTypeId<std::vector<float>>()) return sizeof(std::vector<float>);
    if (typeId == createTypeId<std::vector<Vec3>>()) return sizeof(std::vector<Vec3>);

    return 0;
}

/**
 * @brief Auto-generate ComponentSchema from TypeInfo reflection
 *
 * Eliminates manual PropertyDefinition boilerplate by extracting field
 * information from ENTROPY_FIELD macro registrations.
 *
 * Requirements:
 * - Component must use ENTROPY_REGISTER_TYPE macro
 * - All reflected fields must use ENTROPY_FIELD macro
 * - Field types must be mapped in TypeToPropertyType specializations or mapTypeIdToPropertyType()
 *
 * Example:
 * @code
 * struct Transform {
 *     ENTROPY_REGISTER_TYPE(Transform);
 *     ENTROPY_FIELD(Vec3, position) = {0.0f, 0.0f, 0.0f};
 *     ENTROPY_FIELD(Quat, rotation) = {0.0f, 0.0f, 0.0f, 1.0f};
 *     ENTROPY_FIELD(Vec3, scale) = {1.0f, 1.0f, 1.0f};
 * };
 *
 * auto result = generateComponentSchema<Transform>("MyApp", 1, true);
 * if (result.success()) {
 *     ComponentSchema schema = result.value;
 *     // Register with ComponentSchemaRegistry...
 * }
 * @endcode
 *
 * @tparam T Component type with reflection (must have ENTROPY_REGISTER_TYPE)
 * @param appId Application identifier (e.g., "CanvasEngine")
 * @param schemaVersion Schema version number (increment on breaking changes)
 * @param isPublic Whether to publish for discovery (default: true)
 * @return Result with ComponentSchema on success, error on failure
 */
template <typename T>
Result<ComponentSchema> generateComponentSchema(const std::string& appId, uint32_t schemaVersion,
                                                bool isPublic = true) {
    // 1. Get TypeInfo for component
    const auto* typeInfo = TypeInfo::get<T>();
    if (!typeInfo) {
        return Result<ComponentSchema>::err(
            NetworkError::InvalidParameter,
            std::format("Type '{}' not registered with ENTROPY_REGISTER_TYPE", typeid(T).name()));
    }

    // 2. Extract component name from TypeInfo
    std::string componentName{typeInfo->getName()};

    // 3. Build PropertyDefinition list from FieldInfo
    std::vector<PropertyDefinition> properties;
    properties.reserve(typeInfo->getFields().size());

    for (const auto& field : typeInfo->getFields()) {
        // Map TypeID → PropertyType
        std::optional<PropertyType> propType = mapTypeIdToPropertyType(field.type);

        if (!propType.has_value()) {
            return Result<ComponentSchema>::err(NetworkError::InvalidParameter,
                                                std::format("Field '{}' has unmapped type (TypeID: {}, hash: {})",
                                                            field.name, field.type.prettyName(), field.type.id));
        }

        // Get field size
        size_t fieldSize = getFieldSize(field.type);
        if (fieldSize == 0) {
            return Result<ComponentSchema>::err(
                NetworkError::InvalidParameter,
                std::format("Field '{}' has unknown size (TypeID: {})", field.name, field.type.prettyName()));
        }

        PropertyDefinition def{.name = std::string(field.name),
                               .type = propType.value(),
                               .offset = field.offset,
                               .size = fieldSize,
                               .required = true,
                               .defaultValue = std::nullopt};

        properties.push_back(std::move(def));
    }

    // 4. Create ComponentSchema with validation
    return ComponentSchema::create(appId, componentName, schemaVersion, properties, sizeof(T), isPublic);
}

}  // namespace Networking
}  // namespace EntropyEngine
