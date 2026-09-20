/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file AssetId.h
 * @brief Content-addressed asset identifier for network protocol
 *
 * AssetId is a 256-bit (32-byte) SHA-256 hash that uniquely identifies
 * an asset by its content. This type is used in the network protocol
 * for asset references in property values.
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace EntropyEngine::Networking
{

/**
 * @brief Content-addressed asset identifier using SHA-256
 *
 * AssetId is a 256-bit (32-byte) hash that uniquely identifies an asset
 * by its content. Same content always produces the same AssetId, enabling:
 * - Global uniqueness without central authority
 * - Automatic deduplication of identical assets
 * - Integrity verification (asset matches its ID)
 *
 * Used for meshes, materials, textures, and any other asset type.
 */
struct AssetId
{
    std::array<uint8_t, 32> hash{};  ///< SHA-256 hash (256-bit)

    /**
     * @brief Check if this is a null/invalid asset ID (all zeros)
     */
    [[nodiscard]] bool isNull() const {
        for (auto byte : hash) {
            if (byte != 0) return false;
        }
        return true;
    }

    /**
     * @brief Check if this is a valid (non-null) asset ID
     */
    [[nodiscard]] bool isValid() const {
        return !isNull();
    }

    /**
     * @brief Create a null asset ID
     */
    static AssetId null() {
        return AssetId{};
    }

    /**
     * @brief Create an AssetId from raw hash bytes
     */
    static AssetId fromBytes(const uint8_t* data) {
        AssetId id;
        std::memcpy(id.hash.data(), data, 32);
        return id;
    }

    /**
     * @brief Create an AssetId from a hex string (64 characters)
     */
    static AssetId fromHex(const std::string& hex) {
        AssetId id;
        if (hex.size() != 64) return id;

        for (size_t i = 0; i < 32; ++i) {
            auto byteStr = hex.substr(i * 2, 2);
            id.hash[i] = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        }
        return id;
    }

    /**
     * @brief Convert to hex string representation
     */
    [[nodiscard]] std::string toHex() const {
        static const char* hexChars = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto byte : hash) {
            result.push_back(hexChars[(byte >> 4) & 0xF]);
            result.push_back(hexChars[byte & 0xF]);
        }
        return result;
    }

    /**
     * @brief Convert to shortened hex for logging (first 16 chars)
     */
    [[nodiscard]] std::string toShortHex() const {
        return toHex().substr(0, 16);
    }

    // Comparison operators
    bool operator==(const AssetId& other) const {
        return hash == other.hash;
    }

    bool operator!=(const AssetId& other) const {
        return hash != other.hash;
    }

    bool operator<(const AssetId& other) const {
        return hash < other.hash;
    }
};

}  // namespace EntropyEngine::Networking

// Hash support for std::unordered_map/set
namespace std
{
template <>
struct hash<EntropyEngine::Networking::AssetId>
{
    size_t operator()(const EntropyEngine::Networking::AssetId& id) const noexcept {
        // Use first 8 bytes as hash (sufficient for hash table distribution)
        size_t result = 0;
        std::memcpy(&result, id.hash.data(), sizeof(size_t));
        return result;
    }
};
}  // namespace std
