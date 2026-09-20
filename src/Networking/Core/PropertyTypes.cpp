/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyTypes.h"

#include "Networking/Protocol/entropy.capnp.h"

namespace EntropyEngine
{
namespace Networking
{

// Type alias for Cap'n Proto generated PropertyType to avoid collision
// with EntropyEngine::Networking::PropertyType
using CapnpPropertyType = Protocol::PropertyType;

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

    // Asset reference
    if (std::holds_alternative<AssetId>(value)) return PropertyType::AssetId;
    if (std::holds_alternative<std::vector<AssetId>>(value)) return PropertyType::AssetIdArray;

    // Matrix types
    if (std::holds_alternative<Mat3>(value)) return PropertyType::Mat3;
    if (std::holds_alternative<Mat4>(value)) return PropertyType::Mat4;

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
        case PropertyType::AssetId:
            return "AssetId";
        case PropertyType::AssetIdArray:
            return "AssetIdArray";
        case PropertyType::Mat3:
            return "Mat3";
        case PropertyType::Mat4:
            return "Mat4";
        case PropertyType::Texture1D:
            return "Texture1D";
        case PropertyType::Texture2D:
            return "Texture2D";
        case PropertyType::Texture3D:
            return "Texture3D";
        case PropertyType::TextureCube:
            return "TextureCube";
        case PropertyType::Texture2DArray:
            return "Texture2DArray";
        case PropertyType::TextureCubeArray:
            return "TextureCubeArray";
        case PropertyType::Sampler:
            return "Sampler";
        default:
            return "Unknown";
    }
}

size_t getPropertySize(PropertyType type) {
    switch (type) {
        case PropertyType::Bool:
            return sizeof(bool);
        case PropertyType::Int32:
            return sizeof(int32_t);
        case PropertyType::Int64:
            return sizeof(int64_t);
        case PropertyType::Float32:
            return sizeof(float);
        case PropertyType::Float64:
            return sizeof(double);
        case PropertyType::Vec2:
            return sizeof(Vec2);
        case PropertyType::Vec3:
            return sizeof(Vec3);
        case PropertyType::Vec4:
            return sizeof(Vec4);
        case PropertyType::Quat:
            return sizeof(Quat);
        case PropertyType::AssetId:
            return sizeof(EntropyEngine::Networking::AssetId);
        case PropertyType::String:
            return sizeof(std::string);
        case PropertyType::Bytes:
            return sizeof(std::vector<uint8_t>);
        case PropertyType::Int32Array:
            return sizeof(std::vector<int32_t>);
        case PropertyType::Int64Array:
            return sizeof(std::vector<int64_t>);
        case PropertyType::Float32Array:
            return sizeof(std::vector<float>);
        case PropertyType::Float64Array:
            return sizeof(std::vector<double>);
        case PropertyType::Vec2Array:
            return sizeof(std::vector<Vec2>);
        case PropertyType::Vec3Array:
            return sizeof(std::vector<Vec3>);
        case PropertyType::Vec4Array:
            return sizeof(std::vector<Vec4>);
        case PropertyType::QuatArray:
            return sizeof(std::vector<Quat>);
        case PropertyType::AssetIdArray:
            return sizeof(std::vector<AssetId>);
        case PropertyType::Mat3:
            return sizeof(Mat3);
        case PropertyType::Mat4:
            return sizeof(Mat4);
        // Texture and sampler types use AssetId as their value
        case PropertyType::Texture1D:
        case PropertyType::Texture2D:
        case PropertyType::Texture3D:
        case PropertyType::TextureCube:
        case PropertyType::Texture2DArray:
        case PropertyType::TextureCubeArray:
        case PropertyType::Sampler:
            return sizeof(EntropyEngine::Networking::AssetId);
    }
    return 0;
}

uint16_t toCapnpPropertyType(PropertyType type) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Returns uint16_t (underlying type) to avoid capnp header dependency
    switch (type) {
        case PropertyType::Int32:
            return static_cast<uint16_t>(CapnpPropertyType::INT32);
        case PropertyType::Int64:
            return static_cast<uint16_t>(CapnpPropertyType::INT64);
        case PropertyType::Float32:
            return static_cast<uint16_t>(CapnpPropertyType::FLOAT32);
        case PropertyType::Float64:
            return static_cast<uint16_t>(CapnpPropertyType::FLOAT64);
        case PropertyType::Vec2:
            return static_cast<uint16_t>(CapnpPropertyType::VEC2);
        case PropertyType::Vec3:
            return static_cast<uint16_t>(CapnpPropertyType::VEC3);
        case PropertyType::Vec4:
            return static_cast<uint16_t>(CapnpPropertyType::VEC4);
        case PropertyType::Quat:
            return static_cast<uint16_t>(CapnpPropertyType::QUAT);
        case PropertyType::String:
            return static_cast<uint16_t>(CapnpPropertyType::STRING);
        case PropertyType::Bool:
            return static_cast<uint16_t>(CapnpPropertyType::BOOL);
        case PropertyType::Bytes:
            return static_cast<uint16_t>(CapnpPropertyType::BYTES);
        case PropertyType::Int32Array:
            return static_cast<uint16_t>(CapnpPropertyType::INT32_ARRAY);
        case PropertyType::Int64Array:
            return static_cast<uint16_t>(CapnpPropertyType::INT64_ARRAY);
        case PropertyType::Float32Array:
            return static_cast<uint16_t>(CapnpPropertyType::FLOAT32_ARRAY);
        case PropertyType::Float64Array:
            return static_cast<uint16_t>(CapnpPropertyType::FLOAT64_ARRAY);
        case PropertyType::Vec2Array:
            return static_cast<uint16_t>(CapnpPropertyType::VEC2_ARRAY);
        case PropertyType::Vec3Array:
            return static_cast<uint16_t>(CapnpPropertyType::VEC3_ARRAY);
        case PropertyType::Vec4Array:
            return static_cast<uint16_t>(CapnpPropertyType::VEC4_ARRAY);
        case PropertyType::QuatArray:
            return static_cast<uint16_t>(CapnpPropertyType::QUAT_ARRAY);
        case PropertyType::AssetId:
            return static_cast<uint16_t>(CapnpPropertyType::ASSET_ID);
        case PropertyType::AssetIdArray:
            return static_cast<uint16_t>(CapnpPropertyType::ASSET_ID_ARRAY);
        case PropertyType::Mat3:
            return static_cast<uint16_t>(CapnpPropertyType::MAT3);
        case PropertyType::Mat4:
            return static_cast<uint16_t>(CapnpPropertyType::MAT4);
        case PropertyType::Texture1D:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE1_D);
        case PropertyType::Texture2D:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE2_D);
        case PropertyType::Texture3D:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE3_D);
        case PropertyType::TextureCube:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE_CUBE);
        case PropertyType::Texture2DArray:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE2_D_ARRAY);
        case PropertyType::TextureCubeArray:
            return static_cast<uint16_t>(CapnpPropertyType::TEXTURE_CUBE_ARRAY);
        case PropertyType::Sampler:
            return static_cast<uint16_t>(CapnpPropertyType::SAMPLER);
        default:
            return static_cast<uint16_t>(CapnpPropertyType::INT32);  // Fallback
    }
}

PropertyType fromCapnpPropertyType(uint16_t capnpType) {
    // Explicit mapping ensures safety even if enum ordinals change
    // Accepts uint16_t to avoid capnp header dependency
    auto type = static_cast<CapnpPropertyType>(capnpType);
    switch (type) {
        case CapnpPropertyType::INT32:
            return PropertyType::Int32;
        case CapnpPropertyType::INT64:
            return PropertyType::Int64;
        case CapnpPropertyType::FLOAT32:
            return PropertyType::Float32;
        case CapnpPropertyType::FLOAT64:
            return PropertyType::Float64;
        case CapnpPropertyType::VEC2:
            return PropertyType::Vec2;
        case CapnpPropertyType::VEC3:
            return PropertyType::Vec3;
        case CapnpPropertyType::VEC4:
            return PropertyType::Vec4;
        case CapnpPropertyType::QUAT:
            return PropertyType::Quat;
        case CapnpPropertyType::STRING:
            return PropertyType::String;
        case CapnpPropertyType::BOOL:
            return PropertyType::Bool;
        case CapnpPropertyType::BYTES:
            return PropertyType::Bytes;
        case CapnpPropertyType::INT32_ARRAY:
            return PropertyType::Int32Array;
        case CapnpPropertyType::INT64_ARRAY:
            return PropertyType::Int64Array;
        case CapnpPropertyType::FLOAT32_ARRAY:
            return PropertyType::Float32Array;
        case CapnpPropertyType::FLOAT64_ARRAY:
            return PropertyType::Float64Array;
        case CapnpPropertyType::VEC2_ARRAY:
            return PropertyType::Vec2Array;
        case CapnpPropertyType::VEC3_ARRAY:
            return PropertyType::Vec3Array;
        case CapnpPropertyType::VEC4_ARRAY:
            return PropertyType::Vec4Array;
        case CapnpPropertyType::QUAT_ARRAY:
            return PropertyType::QuatArray;
        case CapnpPropertyType::ASSET_ID:
            return PropertyType::AssetId;
        case CapnpPropertyType::ASSET_ID_ARRAY:
            return PropertyType::AssetIdArray;
        case CapnpPropertyType::MAT3:
            return PropertyType::Mat3;
        case CapnpPropertyType::MAT4:
            return PropertyType::Mat4;
        case CapnpPropertyType::TEXTURE1_D:
            return PropertyType::Texture1D;
        case CapnpPropertyType::TEXTURE2_D:
            return PropertyType::Texture2D;
        case CapnpPropertyType::TEXTURE3_D:
            return PropertyType::Texture3D;
        case CapnpPropertyType::TEXTURE_CUBE:
            return PropertyType::TextureCube;
        case CapnpPropertyType::TEXTURE2_D_ARRAY:
            return PropertyType::Texture2DArray;
        case CapnpPropertyType::TEXTURE_CUBE_ARRAY:
            return PropertyType::TextureCubeArray;
        case CapnpPropertyType::SAMPLER:
            return PropertyType::Sampler;
        default:
            return PropertyType::Int32;  // Fallback
    }
}

}  // namespace Networking
}  // namespace EntropyEngine
