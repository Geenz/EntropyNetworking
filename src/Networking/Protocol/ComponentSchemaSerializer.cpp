/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ComponentSchemaSerializer.h"

#include "../Core/PropertyTypes.h"

namespace EntropyEngine
{
namespace Networking
{

namespace
{
// Helper to serialize PropertyValue to Cap'n Proto
void serializePropertyValue(const PropertyValue& value, ::PropertyValue::Builder builder) {
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
    }
    // Note: Array types not serialized for default values (not commonly used as defaults)
}

// Helper to deserialize PropertyValue from Cap'n Proto
PropertyValue deserializePropertyValue(::PropertyValue::Reader reader) {
    switch (reader.which()) {
        case ::PropertyValue::INT32:
            return reader.getInt32();
        case ::PropertyValue::INT64:
            return reader.getInt64();
        case ::PropertyValue::FLOAT32:
            return reader.getFloat32();
        case ::PropertyValue::FLOAT64:
            return reader.getFloat64();
        case ::PropertyValue::VEC2:
        {
            auto vec = reader.getVec2();
            return Vec2{vec.getX(), vec.getY()};
        }
        case ::PropertyValue::VEC3:
        {
            auto vec = reader.getVec3();
            return Vec3{vec.getX(), vec.getY(), vec.getZ()};
        }
        case ::PropertyValue::VEC4:
        {
            auto vec = reader.getVec4();
            return Vec4{vec.getX(), vec.getY(), vec.getZ(), vec.getW()};
        }
        case ::PropertyValue::QUAT:
        {
            auto quat = reader.getQuat();
            return Quat{quat.getX(), quat.getY(), quat.getZ(), quat.getW()};
        }
        case ::PropertyValue::STRING:
            return std::string(reader.getString().cStr());
        case ::PropertyValue::BOOL:
            return reader.getBool();
        case ::PropertyValue::BYTES:
        {
            auto bytes = reader.getBytes();
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        }
        default:
            // For unsupported types, return int32(0) as placeholder
            return int32_t{0};
    }
}
}  // anonymous namespace

void serializePropertyDefinition(const PropertyDefinition& definition, PropertyDefinitionData::Builder builder) {
    builder.setName(definition.name);
    builder.setType(static_cast<::PropertyType>(toCapnpPropertyType(definition.type)));
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

PropertyDefinition deserializePropertyDefinition(PropertyDefinitionData::Reader reader) {
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

void serializeComponentSchema(const ComponentSchema& schema, ComponentSchemaData::Builder builder) {
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

Result<ComponentSchema> deserializeComponentSchema(ComponentSchemaData::Reader reader) {
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
