/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "PropertyHash.h"
#include <chrono>
#include <unordered_map>
#include <mutex>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Tracks schema NACKs with rate limiting and metrics
 *
 * Simple tracker to prevent NACK spam when encountering unknown schemas.
 * Uses basic time-based rate limiting per schema hash.
 */
class SchemaNackTracker {
public:
    /**
     * @brief Configuration for NACK rate limiting
     */
    struct Config {
        std::chrono::milliseconds minInterval;  ///< Minimum interval between NACKs for same schema
        size_t maxTrackedSchemas;               ///< Maximum number of schemas to track

        Config()
            : minInterval(1000)
            , maxTrackedSchemas(1000)
        {
        }
    };

    explicit SchemaNackTracker(const Config& config = Config());

    /**
     * @brief Check if a NACK should be sent for a schema
     *
     * Applies rate limiting - returns true if enough time has passed since last NACK.
     *
     * @param typeHash The ComponentTypeHash that is unknown
     * @return true if NACK should be sent, false if rate limited
     */
    bool shouldSendNack(const ComponentTypeHash& typeHash);

    /**
     * @brief Record that a NACK was sent
     *
     * Updates the last NACK timestamp for the schema.
     *
     * @param typeHash The ComponentTypeHash that was NACKed
     */
    void recordNackSent(const ComponentTypeHash& typeHash);

    /**
     * @brief Get total number of NACKs sent
     */
    size_t getTotalNacksSent() const;

    /**
     * @brief Get number of unique schemas NACKed
     */
    size_t getUniqueSchemas() const;

    /**
     * @brief Clear all tracked state
     */
    void clear();

private:
    struct NackRecord {
        std::chrono::steady_clock::time_point lastNackTime;
        size_t count{0};
    };

    Config _config;
    mutable std::mutex _mutex;
    std::unordered_map<ComponentTypeHash, NackRecord> _nackRecords;
    size_t _totalNacksSent{0};

    void pruneOldRecords();
};

} // namespace Networking
} // namespace EntropyEngine
