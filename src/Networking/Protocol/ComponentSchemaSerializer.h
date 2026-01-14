/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "../Core/ComponentSchema.h"
#include "../Core/ErrorCodes.h"
#include "Networking/Protocol/entropy.capnp.h"

namespace EntropyEngine
{
namespace Networking
{

/**
 * @brief Serialization helpers for ComponentSchema ↔ Cap'n Proto
 *
 * Provides functions to convert between C++ ComponentSchema structures
 * and Cap'n Proto ComponentSchemaData messages.
 */

// Type alias for Cap'n Proto generated PropertyValue to avoid collision
// with EntropyEngine::Networking::PropertyValue
using CapnpPropertyValue = Protocol::PropertyValue;

/**
 * @brief Deserialize a PropertyValue from Cap'n Proto
 *
 * @param reader Cap'n Proto reader for PropertyValue
 * @return PropertyValue The deserialized C++ variant
 */
PropertyValue deserializePropertyValue(CapnpPropertyValue::Reader reader);

/**
 * @brief Serialize a PropertyValue to Cap'n Proto
 *
 * @param value The C++ PropertyValue to serialize
 * @param builder Cap'n Proto builder for PropertyValue
 */
void serializePropertyValue(const PropertyValue& value, CapnpPropertyValue::Builder builder);

/**
 * @brief Serialize a PropertyDefinition to Cap'n Proto
 *
 * @param definition The C++ PropertyDefinition to serialize
 * @param builder Cap'n Proto builder for PropertyDefinitionData
 */
void serializePropertyDefinition(const PropertyDefinition& definition,
                                 Protocol::PropertyDefinitionData::Builder builder);

/**
 * @brief Deserialize a PropertyDefinition from Cap'n Proto
 *
 * @param reader Cap'n Proto reader for PropertyDefinitionData
 * @return PropertyDefinition The deserialized C++ structure
 */
PropertyDefinition deserializePropertyDefinition(Protocol::PropertyDefinitionData::Reader reader);

/**
 * @brief Serialize a ComponentSchema to Cap'n Proto
 *
 * @param schema The C++ ComponentSchema to serialize
 * @param builder Cap'n Proto builder for ComponentSchemaData
 */
void serializeComponentSchema(const ComponentSchema& schema, Protocol::ComponentSchemaData::Builder builder);

/**
 * @brief Deserialize a ComponentSchema from Cap'n Proto
 *
 * @param reader Cap'n Proto reader for ComponentSchemaData
 * @return Result<ComponentSchema> The deserialized schema or error
 */
Result<ComponentSchema> deserializeComponentSchema(Protocol::ComponentSchemaData::Reader reader);

}  // namespace Networking
}  // namespace EntropyEngine
