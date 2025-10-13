/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief 128-bit property hash for per-instance property identification
 *
 * Uniquely identifies a property on a specific entity instance. Computed using
 * SHA-256(entityId || appId || typeName || fieldName) truncated to 128 bits.
 *
 * The hash provides per-instance uniqueness and namespace isolation at ecosystem scale.
 */
struct PropertyHash128 {
    uint64_t high{0};   ///< High 64 bits of hash
    uint64_t low{0};    ///< Low 64 bits of hash

    PropertyHash128() = default;
    PropertyHash128(uint64_t h, uint64_t l) : high(h), low(l) {}

    bool operator==(const PropertyHash128& other) const {
        return high == other.high && low == other.low;
    }

    bool operator!=(const PropertyHash128& other) const {
        return !(*this == other);
    }

    bool operator<(const PropertyHash128& other) const {
        if (high != other.high) {
            return high < other.high;
        }
        return low < other.low;
    }

    /**
     * @brief Check if hash is null/uninitialized
     * @return true if both components are zero
     */
    bool isNull() const {
        return high == 0 && low == 0;
    }
};

/**
 * @brief Compute property hash from components
 *
 * Computes a 128-bit hash using SHA-256. The hash is deterministic and provides
 * per-instance property identification with ecosystem-scale collision resistance.
 *
 * Hash construction: SHA-256(entityId || appId || typeName || fieldName)
 * - entityId: 8 bytes (big-endian)
 * - appId: UTF-8 string
 * - typeName: UTF-8 string
 * - fieldName: UTF-8 string
 *
 * Result is truncated to 128 bits (high 128 bits of SHA-256 output).
 *
 * @param entityId Entity instance identifier
 * @param appId Application identifier
 * @param typeName Entity type name
 * @param fieldName Property field name
 * @return 128-bit property hash
 *
 * @code
 * auto hash = computePropertyHash(42, "com.example.app", "Player", "position");
 * // hash uniquely identifies Player.position property on entity 42
 * @endcode
 */
PropertyHash128 computePropertyHash(
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    const std::string& fieldName
);

} // namespace Networking
} // namespace EntropyEngine

// Hash function for std::unordered_map support
namespace std {
    template<>
    struct hash<EntropyEngine::Networking::PropertyHash128> {
        size_t operator()(const EntropyEngine::Networking::PropertyHash128& h) const noexcept {
            // Combine high and low using XOR and rotation
            return static_cast<size_t>(h.high ^ (h.low << 1));
        }
    };
}
