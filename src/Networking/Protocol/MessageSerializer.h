/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file MessageSerializer.h
 * @brief Cap'n Proto message serialization utilities
 *
 * Provides functions for serializing and deserializing Cap'n Proto messages,
 * with optional zstd compression for large payloads like scene snapshots.
 */

#pragma once

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cstdint>
#include <vector>

#include "../Core/ErrorCodes.h"

namespace EntropyEngine
{
namespace Networking
{

/**
 * @brief Serialize a Cap'n Proto message to a byte vector
 *
 * Converts a Cap'n Proto message builder to a flat byte array suitable
 * for transmission over the network.
 *
 * @param builder The message builder containing the message
 * @return Result containing byte vector or error
 *
 * @code
 * capnp::MallocMessageBuilder builder;
 * auto msg = builder.initRoot<entropy::Message>();
 * // ... populate message ...
 *
 * auto result = serialize(builder);
 * if (result.success()) {
 *     // Send result.value over network
 * }
 * @endcode
 */
Result<std::vector<uint8_t>> serialize(capnp::MessageBuilder& builder);

/**
 * @brief Deserialize a byte vector to a Cap'n Proto message
 *
 * Parses a flat byte array into a Cap'n Proto message reader.
 * The returned reader is valid for the lifetime of the input buffer.
 *
 * @param buffer Serialized message bytes
 * @return Result containing message reader or error
 *
 * @code
 * std::vector<uint8_t> received = ...;
 * auto result = deserialize(received);
 * if (result.success()) {
 *     auto reader = result.value;
 *     // Access message via reader
 * }
 * @endcode
 */
Result<kj::Array<capnp::word>> deserialize(const std::vector<uint8_t>& buffer);

/**
 * @brief Compress data using zstd
 *
 * Compresses byte data for efficient transmission. Typically used for
 * scene snapshots which can be large.
 *
 * @param data Uncompressed data
 * @param compressionLevel Compression level (1-22, default 3)
 * @return Result containing compressed data or error
 *
 * @code
 * std::vector<uint8_t> largePayload = ...;
 * auto result = compress(largePayload);
 * if (result.success()) {
 *     // Compressed size: result.value.size()
 * }
 * @endcode
 */
Result<std::vector<uint8_t>> compress(const std::vector<uint8_t>& data, int compressionLevel = 3);

/**
 * @brief Decompress zstd-compressed data
 *
 * @param compressedData Compressed data
 * @return Result containing decompressed data or error
 *
 * @code
 * std::vector<uint8_t> compressed = ...;
 * auto result = decompress(compressed);
 * if (result.success()) {
 *     auto original = result.value;
 * }
 * @endcode
 */
Result<std::vector<uint8_t>> decompress(const std::vector<uint8_t>& compressedData);

}  // namespace Networking
}  // namespace EntropyEngine
