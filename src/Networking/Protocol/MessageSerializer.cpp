/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "MessageSerializer.h"
#include <zstd.h>
#include <kj/array.h>

namespace EntropyEngine {
namespace Networking {

Result<std::vector<uint8_t>> serialize(capnp::MessageBuilder& builder) {
    try {
        // Serialize to flat array
        kj::Array<capnp::word> words = capnp::messageToFlatArray(builder);

        // Convert to byte vector
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(words.begin());
        size_t byteSize = words.size() * sizeof(capnp::word);

        std::vector<uint8_t> result(bytes, bytes + byteSize);
        return Result<std::vector<uint8_t>>::ok(std::move(result));

    } catch (const std::exception& e) {
        return Result<std::vector<uint8_t>>::err(
            NetworkError::SerializationFailed,
            std::string("Serialization failed: ") + e.what()
        );
    }
}

Result<kj::Array<capnp::word>> deserialize(const std::vector<uint8_t>& buffer) {
    try {
        // Ensure buffer size is multiple of word size
        if (buffer.size() % sizeof(capnp::word) != 0) {
            return Result<kj::Array<capnp::word>>::err(
                NetworkError::DeserializationFailed,
                "Buffer size not aligned to word boundary"
            );
        }

        // Convert byte buffer to word array
        size_t wordCount = buffer.size() / sizeof(capnp::word);
        auto words = kj::heapArray<capnp::word>(wordCount);

        std::memcpy(words.begin(), buffer.data(), buffer.size());

        return Result<kj::Array<capnp::word>>::ok(kj::mv(words));

    } catch (const std::exception& e) {
        return Result<kj::Array<capnp::word>>::err(
            NetworkError::DeserializationFailed,
            std::string("Deserialization failed: ") + e.what()
        );
    }
}

Result<std::vector<uint8_t>> compress(const std::vector<uint8_t>& data, int compressionLevel) {
    try {
        // Get maximum compressed size
        size_t maxCompressedSize = ZSTD_compressBound(data.size());
        std::vector<uint8_t> compressed(maxCompressedSize);

        // Compress
        size_t compressedSize = ZSTD_compress(
            compressed.data(),
            compressed.size(),
            data.data(),
            data.size(),
            compressionLevel
        );

        // Check for errors
        if (ZSTD_isError(compressedSize)) {
            return Result<std::vector<uint8_t>>::err(
                NetworkError::CompressionFailed,
                std::string("Compression failed: ") + ZSTD_getErrorName(compressedSize)
            );
        }

        // Resize to actual compressed size
        compressed.resize(compressedSize);
        return Result<std::vector<uint8_t>>::ok(std::move(compressed));

    } catch (const std::exception& e) {
        return Result<std::vector<uint8_t>>::err(
            NetworkError::CompressionFailed,
            std::string("Compression failed: ") + e.what()
        );
    }
}

Result<std::vector<uint8_t>> decompress(const std::vector<uint8_t>& compressedData) {
    try {
        // Get decompressed size
        unsigned long long decompressedSize = ZSTD_getFrameContentSize(
            compressedData.data(),
            compressedData.size()
        );

        if (decompressedSize == ZSTD_CONTENTSIZE_ERROR) {
            return Result<std::vector<uint8_t>>::err(
                NetworkError::DecompressionFailed,
                "Not compressed by zstd"
            );
        }

        if (decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            return Result<std::vector<uint8_t>>::err(
                NetworkError::DecompressionFailed,
                "Original size unknown"
            );
        }

        // Allocate buffer for decompressed data
        std::vector<uint8_t> decompressed(decompressedSize);

        // Decompress
        size_t actualSize = ZSTD_decompress(
            decompressed.data(),
            decompressed.size(),
            compressedData.data(),
            compressedData.size()
        );

        // Check for errors
        if (ZSTD_isError(actualSize)) {
            return Result<std::vector<uint8_t>>::err(
                NetworkError::DecompressionFailed,
                std::string("Decompression failed: ") + ZSTD_getErrorName(actualSize)
            );
        }

        return Result<std::vector<uint8_t>>::ok(std::move(decompressed));

    } catch (const std::exception& e) {
        return Result<std::vector<uint8_t>>::err(
            NetworkError::DecompressionFailed,
            std::string("Decompression failed: ") + e.what()
        );
    }
}

} // namespace Networking
} // namespace EntropyEngine
