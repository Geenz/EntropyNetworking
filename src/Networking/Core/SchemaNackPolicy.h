/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Global, thread-safe policy for schema NACK behavior
 *
 * Controls whether SchemaNack messages are sent when encountering unknown ComponentTypeHash
 * values in received messages (e.g., ENTITY_CREATED with unknown schemas).
 *
 * SchemaNack is OPTIONAL feedback:
 * - Default: disabled (no NACKs sent)
 * - Applications choose to enable based on their requirements
 * - Used for schema discovery and debugging unknown schema issues
 *
 * Behavior when unknown schema detected:
 * - ALWAYS: Increment unknownSchemaDrops metric (NetworkSession::getUnknownSchemaDropCount)
 * - ALWAYS: Emit rate-limited log with schema hash and error code
 * - IF ENABLED: Send SchemaNack message to peer (subject to per-schema rate limiting)
 *
 * All operations use atomics for lock-free access in hot paths.
 * Thread-safe: All fields are atomic and can be safely accessed from multiple threads.
 */
struct SchemaNackPolicy {
    /// @brief Enable/disable sending NACK messages (default: false)
    /// When false, unknown schemas are logged and counted but no NACKs are sent
    std::atomic<bool> enabled{false};

    /// @brief Minimum interval between NACKs for same schema in milliseconds (default: 1000ms)
    std::atomic<uint32_t> minIntervalMs{1000};

    /// @brief Maximum burst of NACKs allowed before rate limiting kicks in (default: 1)
    std::atomic<uint32_t> burst{1};

    /// @brief Interval for rate-limited logging of unknown schema events in milliseconds (default: 5000ms)
    std::atomic<uint32_t> logIntervalMs{5000};

    /**
     * @brief Get the global NACK policy instance
     *
     * Singleton accessor for the global policy. Thread-safe lazy initialization.
     *
     * @return Reference to the global policy
     */
    static SchemaNackPolicy& instance() {
        static SchemaNackPolicy policy;
        return policy;
    }

    /**
     * @brief Check if NACK sending is enabled
     * @return true if NACKs should be sent
     */
    bool isEnabled() const noexcept {
        return enabled.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the minimum interval between NACKs in milliseconds
     * @return Minimum interval in ms
     */
    uint32_t getMinIntervalMs() const noexcept {
        return minIntervalMs.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the burst allowance
     * @return Maximum burst count
     */
    uint32_t getBurst() const noexcept {
        return burst.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the logging interval in milliseconds
     * @return Log interval in ms
     */
    uint32_t getLogIntervalMs() const noexcept {
        return logIntervalMs.load(std::memory_order_relaxed);
    }

    /**
     * @brief Enable NACK sending
     */
    void enable() noexcept {
        enabled.store(true, std::memory_order_relaxed);
    }

    /**
     * @brief Disable NACK sending
     */
    void disable() noexcept {
        enabled.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Set the minimum interval between NACKs
     * @param intervalMs Interval in milliseconds
     */
    void setMinIntervalMs(uint32_t intervalMs) noexcept {
        minIntervalMs.store(intervalMs, std::memory_order_relaxed);
    }

    /**
     * @brief Set the burst allowance
     * @param burstCount Maximum burst count
     */
    void setBurst(uint32_t burstCount) noexcept {
        burst.store(burstCount, std::memory_order_relaxed);
    }

    /**
     * @brief Set the logging interval
     * @param intervalMs Interval in milliseconds
     */
    void setLogIntervalMs(uint32_t intervalMs) noexcept {
        logIntervalMs.store(intervalMs, std::memory_order_relaxed);
    }

private:
    // Private constructor for singleton
    SchemaNackPolicy() = default;
};

} // namespace Networking
} // namespace EntropyEngine
