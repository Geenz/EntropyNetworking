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
#include <sstream>
#include <iomanip>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief 128-bit property hash for per-instance property identification
 *
 * Uniquely identifies a property on a specific entity instance. Computed using
 * SHA-256(entityId || componentType || propertyName) truncated to 128 bits.
 *
 * The hash is computed ONCE on property creation and provides per-instance
 * uniqueness at ecosystem scale. Each property on each entity has a unique hash.
 *
 * Example: Entity 42's Transform.position has a different hash than
 * Entity 99's Transform.position.
 */
struct PropertyHash {
    uint64_t high{0};   ///< High 64 bits of hash
    uint64_t low{0};    ///< Low 64 bits of hash

    PropertyHash() = default;
    PropertyHash(uint64_t h, uint64_t l) : high(h), low(l) {}

    bool operator==(const PropertyHash& other) const {
        return high == other.high && low == other.low;
    }

    bool operator!=(const PropertyHash& other) const {
        return !(*this == other);
    }

    bool operator<(const PropertyHash& other) const {
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
 * Hash construction: SHA-256(entityId || componentType || propertyName)
 * - entityId: 8 bytes (big-endian uint64)
 * - componentType: UTF-8 string (e.g., "Transform", "Player")
 * - propertyName: UTF-8 string (e.g., "position", "health")
 *
 * Result is truncated to 128 bits (high 128 bits of SHA-256 output).
 *
 * The hash should be computed ONCE on property registration and stored/reused.
 * Never recompute the hash - it is a stable identifier for the property instance.
 *
 * @param entityId Entity instance identifier
 * @param componentType Component type name (e.g., "Transform")
 * @param propertyName Property name within the component (e.g., "position")
 * @return 128-bit property hash
 *
 * @code
 * // Compute hash once when property is created
 * auto hash = computePropertyHash(42, "Transform", "position");
 * // hash uniquely identifies Transform.position property on entity 42
 *
 * // Different entity = different hash
 * auto hash2 = computePropertyHash(99, "Transform", "position");
 * // hash != hash2 (different entity IDs)
 * @endcode
 */
PropertyHash computePropertyHash(
    uint64_t entityId,
    const std::string& componentType,
    const std::string& propertyName
);

/**
 * @brief Convert PropertyHash to string for logging/diagnostics
 * @param hash PropertyHash to convert
 * @return String representation in format "high:low" (hex)
 */
inline std::string toString(const PropertyHash& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash.high
        << ':' << std::setw(16) << hash.low;
    return oss.str();
}

} // namespace Networking
} // namespace EntropyEngine

// Hash function for std::unordered_map support
namespace std {
    template<>
    struct hash<EntropyEngine::Networking::PropertyHash> {
        /**
         * @brief SplitMix64 hash mixing function
         *
         * High-quality 64-bit hash finalizer from the SplitMix64 PRNG algorithm.
         * Used to distribute combined hash bits uniformly across the hash space.
         *
         * The constants and bit operations provide excellent avalanche properties,
         * ensuring small changes in input produce large changes in output.
         *
         * @param x Input value to mix
         * @return Mixed 64-bit hash value with good distribution
         * @see https://xorshift.di.unimi.it/splitmix64.c
         */
        static inline uint64_t splitmix64(uint64_t x) {
            x += 0x9e3779b97f4a7c15ull;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
            return x ^ (x >> 31);
        }
        size_t operator()(const EntropyEngine::Networking::PropertyHash& h) const noexcept {
            // Cache h.high for readability (multiple uses in expression)
            uint64_t high = h.high;

            // Combine high and low using boost::hash_combine-inspired formula
            // Uses golden ratio constant (0x9e3779b97f4a7c15) for good distribution
            // Bit shifts (<<6, >>2) and XOR provide avalanche properties:
            // small input changes cascade to large output changes
            uint64_t combined = high ^ (h.low + 0x9e3779b97f4a7c15ull + (high << 6) + (high >> 2));

            return static_cast<size_t>(splitmix64(combined));
        }
    };
}
