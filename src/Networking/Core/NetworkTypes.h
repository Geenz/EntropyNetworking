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
#include <string>

namespace EntropyEngine {
namespace Networking {

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
 * @brief 2D vector type
 */
struct Vec2 {
    float x{0.0f};
    float y{0.0f};

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    bool operator==(const Vec2& other) const {
        return x == other.x && y == other.y;
    }
};

/**
 * @brief 3D vector type
 */
struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    bool operator==(const Vec3& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

/**
 * @brief 4D vector type
 */
struct Vec4 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{0.0f};

    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    bool operator==(const Vec4& other) const {
        return x == other.x && y == other.y && z == other.z && w == other.w;
    }
};

/**
 * @brief Quaternion type for rotations
 */
struct Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};  // Identity quaternion

    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    bool operator==(const Quat& other) const {
        return x == other.x && y == other.y && z == other.z && w == other.w;
    }
};

} // namespace Networking
} // namespace EntropyEngine
