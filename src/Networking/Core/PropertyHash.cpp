/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "PropertyHash.h"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace EntropyEngine
{
namespace Networking
{

PropertyHash computePropertyHash(uint64_t entityId, ComponentTypeHash componentType, const std::string& propertyName) {
    // Build canonical string: {entityId}:{componentTypeHex}:{propertyName}
    // Format: "12345:1234567890abcdeffedcba0987654321:health"
    std::ostringstream oss;
    oss << entityId << ":" << std::hex << std::setfill('0') << std::setw(16) << componentType.high << std::setw(16)
        << componentType.low << ":" << propertyName;

    std::string canonical = oss.str();

    // Hash the UTF-8 bytes
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), hash);

    // Extract high 128 bits (first 16 bytes)
    uint64_t high = 0;
    uint64_t low = 0;

    // High 64 bits (bytes 0-7)
    for (int i = 0; i < 8; ++i) {
        high = (high << 8) | hash[i];
    }

    // Low 64 bits (bytes 8-15)
    for (int i = 8; i < 16; ++i) {
        low = (low << 8) | hash[i];
    }

    return PropertyHash{high, low};
}

}  // namespace Networking
}  // namespace EntropyEngine
