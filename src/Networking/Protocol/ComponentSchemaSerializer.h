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
#include "src/Networking/Protocol/entropy.capnp.h"

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

/**
 * @brief Serialize a PropertyDefinition to Cap'n Proto
 *
 * @param definition The C++ PropertyDefinition to serialize
 * @param builder Cap'n Proto builder for PropertyDefinitionData
 */
void serializePropertyDefinition(const PropertyDefinition& definition, PropertyDefinitionData::Builder builder);

/**
 * @brief Deserialize a PropertyDefinition from Cap'n Proto
 *
 * @param reader Cap'n Proto reader for PropertyDefinitionData
 * @return PropertyDefinition The deserialized C++ structure
 */
PropertyDefinition deserializePropertyDefinition(PropertyDefinitionData::Reader reader);

/**
 * @brief Serialize a ComponentSchema to Cap'n Proto
 *
 * @param schema The C++ ComponentSchema to serialize
 * @param builder Cap'n Proto builder for ComponentSchemaData
 */
void serializeComponentSchema(const ComponentSchema& schema, ComponentSchemaData::Builder builder);

/**
 * @brief Deserialize a ComponentSchema from Cap'n Proto
 *
 * @param reader Cap'n Proto reader for ComponentSchemaData
 * @return Result<ComponentSchema> The deserialized schema or error
 */
Result<ComponentSchema> deserializeComponentSchema(ComponentSchemaData::Reader reader);

}  // namespace Networking
}  // namespace EntropyEngine
