/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ComponentSchemaSerializer.h"

#include <cstring>

#include "../Core/PropertyTypes.h"

namespace EntropyEngine
{
namespace Networking
{

namespace
{
// Helper to serialize PropertyValue to Cap'n Proto
// (Was here, now exposed in header)
}  // anonymous namespace

void serializePropertyValue(const PropertyValue& value, CapnpPropertyValue::Builder builder) {
    if (std::holds_alternative<int32_t>(value)) {
        builder.setInt32(std::get<int32_t>(value));
    } else if (std::holds_alternative<int64_t>(value)) {
        builder.setInt64(std::get<int64_t>(value));
    } else if (std::holds_alternative<float>(value)) {
        builder.setFloat32(std::get<float>(value));
    } else if (std::holds_alternative<double>(value)) {
        builder.setFloat64(std::get<double>(value));
    } else if (std::holds_alternative<Vec2>(value)) {
        auto vec = std::get<Vec2>(value);
        auto vecBuilder = builder.initVec2();
        vecBuilder.setX(vec.x);
        vecBuilder.setY(vec.y);
    } else if (std::holds_alternative<Vec3>(value)) {
        auto vec = std::get<Vec3>(value);
        auto vecBuilder = builder.initVec3();
        vecBuilder.setX(vec.x);
        vecBuilder.setY(vec.y);
        vecBuilder.setZ(vec.z);
    } else if (std::holds_alternative<Vec4>(value)) {
        auto vec = std::get<Vec4>(value);
        auto vecBuilder = builder.initVec4();
        vecBuilder.setX(vec.x);
        vecBuilder.setY(vec.y);
        vecBuilder.setZ(vec.z);
        vecBuilder.setW(vec.w);
    } else if (std::holds_alternative<Quat>(value)) {
        auto quat = std::get<Quat>(value);
        auto quatBuilder = builder.initQuat();
        quatBuilder.setX(quat.x);
        quatBuilder.setY(quat.y);
        quatBuilder.setZ(quat.z);
        quatBuilder.setW(quat.w);
    } else if (std::holds_alternative<std::string>(value)) {
        builder.setString(std::get<std::string>(value));
    } else if (std::holds_alternative<bool>(value)) {
        builder.setBool(std::get<bool>(value));
    } else if (std::holds_alternative<std::vector<uint8_t>>(value)) {
        const auto& bytes = std::get<std::vector<uint8_t>>(value);
        builder.setBytes(kj::ArrayPtr<const uint8_t>(bytes.data(), bytes.size()));
    } else if (std::holds_alternative<AssetId>(value)) {
        const auto& assetId = std::get<AssetId>(value);
        builder.setAssetId(kj::ArrayPtr<const uint8_t>(assetId.hash.data(), assetId.hash.size()));
    } else if (std::holds_alternative<std::vector<AssetId>>(value)) {
        const auto& assetIds = std::get<std::vector<AssetId>>(value);
        auto arr = builder.initAssetIdArray(assetIds.size());
        for (size_t i = 0; i < assetIds.size(); ++i) {
            arr.set(i, kj::ArrayPtr<const uint8_t>(assetIds[i].hash.data(), assetIds[i].hash.size()));
        }
    } else if (std::holds_alternative<Mat3>(value)) {
        auto mat = std::get<Mat3>(value);
        auto matBuilder = builder.initMat3();
        auto col0 = matBuilder.initCol0();
        col0.setX(mat[0].x);
        col0.setY(mat[0].y);
        col0.setZ(mat[0].z);
        auto col1 = matBuilder.initCol1();
        col1.setX(mat[1].x);
        col1.setY(mat[1].y);
        col1.setZ(mat[1].z);
        auto col2 = matBuilder.initCol2();
        col2.setX(mat[2].x);
        col2.setY(mat[2].y);
        col2.setZ(mat[2].z);
    } else if (std::holds_alternative<Mat4>(value)) {
        auto mat = std::get<Mat4>(value);
        auto matBuilder = builder.initMat4();
        auto col0 = matBuilder.initCol0();
        col0.setX(mat[0].x);
        col0.setY(mat[0].y);
        col0.setZ(mat[0].z);
        col0.setW(mat[0].w);
        auto col1 = matBuilder.initCol1();
        col1.setX(mat[1].x);
        col1.setY(mat[1].y);
        col1.setZ(mat[1].z);
        col1.setW(mat[1].w);
        auto col2 = matBuilder.initCol2();
        col2.setX(mat[2].x);
        col2.setY(mat[2].y);
        col2.setZ(mat[2].z);
        col2.setW(mat[2].w);
        auto col3 = matBuilder.initCol3();
        col3.setX(mat[3].x);
        col3.setY(mat[3].y);
        col3.setZ(mat[3].z);
        col3.setW(mat[3].w);
    }
    // Note: Array types not serialized for default values (not commonly used as defaults)
}

PropertyValue deserializePropertyValue(CapnpPropertyValue::Reader reader) {
    switch (reader.which()) {
        case CapnpPropertyValue::INT32:
            return reader.getInt32();
        case CapnpPropertyValue::INT64:
            return reader.getInt64();
        case CapnpPropertyValue::FLOAT32:
            return reader.getFloat32();
        case CapnpPropertyValue::FLOAT64:
            return reader.getFloat64();
        case CapnpPropertyValue::VEC2:
        {
            auto vec = reader.getVec2();
            return Vec2{vec.getX(), vec.getY()};
        }
        case CapnpPropertyValue::VEC3:
        {
            auto vec = reader.getVec3();
            return Vec3{vec.getX(), vec.getY(), vec.getZ()};
        }
        case CapnpPropertyValue::VEC4:
        {
            auto vec = reader.getVec4();
            return Vec4{vec.getX(), vec.getY(), vec.getZ(), vec.getW()};
        }
        case CapnpPropertyValue::QUAT:
        {
            auto quat = reader.getQuat();
            // glm::quat brace init order is {w, x, y, z} (not {x, y, z, w})
            return Quat{quat.getW(), quat.getX(), quat.getY(), quat.getZ()};
        }
        case CapnpPropertyValue::STRING:
            return std::string(reader.getString().cStr());
        case CapnpPropertyValue::BOOL:
            return reader.getBool();
        case CapnpPropertyValue::BYTES:
        {
            auto bytes = reader.getBytes();
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        }
        case CapnpPropertyValue::ASSET_ID:
        {
            auto assetIdBytes = reader.getAssetId();
            AssetId assetId;
            if (assetIdBytes.size() == 32) {
                std::memcpy(assetId.hash.data(), assetIdBytes.begin(), 32);
            }
            return assetId;
        }
        case CapnpPropertyValue::ASSET_ID_ARRAY:
        {
            auto arr = reader.getAssetIdArray();
            std::vector<AssetId> assetIds;
            assetIds.reserve(arr.size());
            for (auto item : arr) {
                AssetId id;
                if (item.size() == 32) {
                    std::memcpy(id.hash.data(), item.begin(), 32);
                }
                assetIds.push_back(id);
            }
            return assetIds;
        }
        case CapnpPropertyValue::MAT3:
        {
            auto mat = reader.getMat3();
            auto col0 = mat.getCol0();
            auto col1 = mat.getCol1();
            auto col2 = mat.getCol2();
            return Mat3{Vec3{col0.getX(), col0.getY(), col0.getZ()}, Vec3{col1.getX(), col1.getY(), col1.getZ()},
                        Vec3{col2.getX(), col2.getY(), col2.getZ()}};
        }
        case CapnpPropertyValue::MAT4:
        {
            auto mat = reader.getMat4();
            auto col0 = mat.getCol0();
            auto col1 = mat.getCol1();
            auto col2 = mat.getCol2();
            auto col3 = mat.getCol3();
            return Mat4{Vec4{col0.getX(), col0.getY(), col0.getZ(), col0.getW()},
                        Vec4{col1.getX(), col1.getY(), col1.getZ(), col1.getW()},
                        Vec4{col2.getX(), col2.getY(), col2.getZ(), col2.getW()},
                        Vec4{col3.getX(), col3.getY(), col3.getZ(), col3.getW()}};
        }
        default:
            return int32_t{0};
    }
}

void serializePropertyDefinition(const PropertyDefinition& definition,
                                 Protocol::PropertyDefinitionData::Builder builder) {
    builder.setName(definition.name);
    builder.setType(static_cast<Protocol::PropertyType>(toCapnpPropertyType(definition.type)));
    builder.setOffset(definition.offset);
    builder.setSize(definition.size);
    builder.setRequired(definition.required);

    if (definition.defaultValue.has_value()) {
        builder.setHasDefaultValue(true);
        auto defaultValueBuilder = builder.initDefaultValue();
        serializePropertyValue(definition.defaultValue.value(), defaultValueBuilder);
    } else {
        builder.setHasDefaultValue(false);
    }
}

PropertyDefinition deserializePropertyDefinition(Protocol::PropertyDefinitionData::Reader reader) {
    PropertyDefinition definition;
    definition.name = reader.getName();
    definition.type = fromCapnpPropertyType(static_cast<uint16_t>(reader.getType()));
    definition.offset = reader.getOffset();
    definition.size = reader.getSize();
    definition.required = reader.getRequired();

    if (reader.getHasDefaultValue()) {
        definition.defaultValue = deserializePropertyValue(reader.getDefaultValue());
    } else {
        definition.defaultValue = std::nullopt;
    }

    return definition;
}

void serializeComponentSchema(const ComponentSchema& schema, Protocol::ComponentSchemaData::Builder builder) {
    // Serialize typeHash
    auto typeHashBuilder = builder.initTypeHash();
    typeHashBuilder.setHigh(schema.typeHash.high);
    typeHashBuilder.setLow(schema.typeHash.low);

    // Serialize basic fields
    builder.setAppId(schema.appId);
    builder.setComponentName(schema.componentName);
    builder.setSchemaVersion(schema.schemaVersion);

    // Serialize structuralHash
    auto structuralHashBuilder = builder.initStructuralHash();
    structuralHashBuilder.setHigh(schema.structuralHash.high);
    structuralHashBuilder.setLow(schema.structuralHash.low);

    // Serialize properties
    auto propertiesBuilder = builder.initProperties(schema.properties.size());
    for (size_t i = 0; i < schema.properties.size(); ++i) {
        serializePropertyDefinition(schema.properties[i], propertiesBuilder[i]);
    }

    builder.setTotalSize(schema.totalSize);
    builder.setIsPublic(schema.isPublic);
}

Result<ComponentSchema> deserializeComponentSchema(Protocol::ComponentSchemaData::Reader reader) {
    // Deserialize basic fields
    ComponentSchema schema;

    auto typeHashReader = reader.getTypeHash();
    schema.typeHash = ComponentTypeHash{typeHashReader.getHigh(), typeHashReader.getLow()};

    schema.appId = reader.getAppId();
    schema.componentName = reader.getComponentName();
    schema.schemaVersion = reader.getSchemaVersion();

    auto structuralHashReader = reader.getStructuralHash();
    schema.structuralHash = PropertyHash{structuralHashReader.getHigh(), structuralHashReader.getLow()};

    // Deserialize properties
    auto propertiesReader = reader.getProperties();
    schema.properties.reserve(propertiesReader.size());
    for (auto propReader : propertiesReader) {
        schema.properties.push_back(deserializePropertyDefinition(propReader));
    }

    schema.totalSize = reader.getTotalSize();
    schema.isPublic = reader.getIsPublic();

    // Validate the deserialized schema
    auto computedStructuralHash = ComponentSchema::computeStructuralHash(schema.properties);
    if (computedStructuralHash != schema.structuralHash) {
        return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                            "Structural hash mismatch after deserialization");
    }

    auto computedTypeHash = ComponentSchema::computeTypeHash(schema.appId, schema.componentName, schema.schemaVersion,
                                                             schema.structuralHash);
    if (computedTypeHash != schema.typeHash) {
        return Result<ComponentSchema>::err(NetworkError::SchemaValidationFailed,
                                            "Type hash mismatch after deserialization");
    }

    return Result<ComponentSchema>::ok(std::move(schema));
}

}  // namespace Networking
}  // namespace EntropyEngine
