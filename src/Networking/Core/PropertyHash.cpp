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
#include <vector>
#include <cstring>

namespace EntropyEngine {
namespace Networking {

PropertyHash128 computePropertyHash(
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    const std::string& fieldName)
{
    // Prepare input buffer: entityId (8 bytes, big-endian) + appId + typeName + fieldName
    std::vector<uint8_t> input;

    // Add entityId as big-endian uint64
    for (int i = 7; i >= 0; --i) {
        input.push_back(static_cast<uint8_t>((entityId >> (i * 8)) & 0xFF));
    }

    // Add appId
    input.insert(input.end(), appId.begin(), appId.end());

    // Add typeName
    input.insert(input.end(), typeName.begin(), typeName.end());

    // Add fieldName
    input.insert(input.end(), fieldName.begin(), fieldName.end());

    // Compute SHA-256
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(input.data(), input.size(), hash);

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

    return PropertyHash128{high, low};
}

} // namespace Networking
} // namespace EntropyEngine
