/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file PermissionTypes.h
 * @brief Generic permission key types for the permission request/response framework
 *
 * Permission keys are integer-indexed pairs: category (uint16) + permission (uint16).
 * Specific category/permission values are defined by consumers (e.g., Portal defines
 * kBodyMovement = {0, 0}), not the framework.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace EntropyEngine::Networking
{

/**
 * @brief Generic permission key — two integer indices identifying a permission.
 *
 * The framework is ID-agnostic. Consumers define the meaning of category/permission
 * values (e.g., Body(0)/Movement(0), User(1)/Identity(0)).
 */
struct PermissionKey
{
    uint16_t category = 0;
    uint16_t permission = 0;

    bool operator==(const PermissionKey& other) const {
        return category == other.category && permission == other.permission;
    }

    bool operator!=(const PermissionKey& other) const {
        return !(*this == other);
    }

    /** Pack into uint32 for fast hashing/comparison. */
    uint32_t packed() const {
        return (uint32_t(category) << 16) | permission;
    }

    struct Hash
    {
        size_t operator()(const PermissionKey& k) const {
            return std::hash<uint32_t>{}(k.packed());
        }
    };
};

}  // namespace EntropyEngine::Networking
