/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file NetworkTypes.h
 * @brief Core network type definitions for Entropy networking
 *
 * Defines fundamental types used throughout the networking system including
 * entity identifiers, vector types, and common type aliases.
 */

#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

namespace EntropyEngine
{
namespace Networking
{

/**
 * @brief Unique identifier for entities in the scene
 *
 * EntityId 0 is reserved to represent the root/no parent.
 */
using EntityId = uint64_t;

/**
 * @brief Application identifier
 *
 * Uniquely identifies an application in the Entropy ecosystem.
 */
using AppId = std::string;

/**
 * @brief Type name for entities
 *
 * Identifies the class/type of an entity within an application.
 */
using TypeName = std::string;

/**
 * @brief Request identifier for control messages
 *
 * Used to correlate requests and responses in the control plane.
 */
using RequestId = uint32_t;

/**
 * @brief 2D vector type (GLM)
 */
using Vec2 = glm::vec2;

/**
 * @brief 3D vector type (GLM)
 */
using Vec3 = glm::vec3;

/**
 * @brief 4D vector type (GLM)
 */
using Vec4 = glm::vec4;

/**
 * @brief Quaternion type for rotations (GLM)
 */
using Quat = glm::quat;

}  // namespace Networking
}  // namespace EntropyEngine
