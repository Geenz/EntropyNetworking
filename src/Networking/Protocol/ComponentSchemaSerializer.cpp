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

namespace EntropyEngine {
namespace Networking {

void serializePropertyDefinition(
    const PropertyDefinition& definition,
    PropertyDefinitionData::Builder builder)
{
    builder.setName(definition.name);
    builder.setType(static_cast<::PropertyType>(toCapnpPropertyType(definition.type)));
    builder.setOffset(definition.offset);
    builder.setSize(definition.size);
}

PropertyDefinition deserializePropertyDefinition(
    PropertyDefinitionData::Reader reader)
{
    PropertyDefinition definition;
    definition.name = reader.getName();
    definition.type = fromCapnpPropertyType(static_cast<uint16_t>(reader.getType()));
    definition.offset = reader.getOffset();
    definition.size = reader.getSize();
    return definition;
}

void serializeComponentSchema(
    const ComponentSchema& schema,
    ComponentSchemaData::Builder builder)
{
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

Result<ComponentSchema> deserializeComponentSchema(
    ComponentSchemaData::Reader reader)
{
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
        return Result<ComponentSchema>::err(
            NetworkError::SchemaValidationFailed,
            "Structural hash mismatch after deserialization"
        );
    }

    auto computedTypeHash = ComponentSchema::computeTypeHash(
        schema.appId,
        schema.componentName,
        schema.schemaVersion,
        schema.structuralHash
    );
    if (computedTypeHash != schema.typeHash) {
        return Result<ComponentSchema>::err(
            NetworkError::SchemaValidationFailed,
            "Type hash mismatch after deserialization"
        );
    }

    return Result<ComponentSchema>::ok(std::move(schema));
}

} // namespace Networking
} // namespace EntropyEngine
