// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../Core/PropertyHash.h"
#include "../Core/PropertyTypes.h"
#include "../Core/ErrorCodes.h"
#include <EntropyCore.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

namespace EntropyEngine::Networking {

// Forward declaration
class NetworkSession;

/**
 * @brief Batches high-frequency property updates for efficient network transmission
 *
 * Design:
 * - Global batch rate (configurable, default 16ms for 60Hz)
 * - Accumulates property updates in a map (automatic deduplication)
 * - Uses WorkContract for scheduling batch processing
 * - Sends batches on unreliable channel
 * - Dynamic rate adjustment on backpressure (drop old batches, reduce rate)
 * - Property hashes are provided by caller (never computed internally)
 *
 * Usage:
 * @code
 * auto batcher = std::make_shared<BatchManager>(session.get());
 *
 * // Caller computes hash from entity metadata
 * auto hash = computePropertyHash(entityId, appId, typeName, "transform.position");
 * batcher->updateProperty(hash, PropertyType::Vec3, position);
 *
 * // BatchManager automatically sends batches every 16ms via WorkContract
 * @endcode
 */
class BatchManager {
public:
    /**
     * @brief Construct a batch manager for a network session
     * @param session The session to send batches through
     * @param batchIntervalMs Batch interval in milliseconds (default 16ms = 60Hz)
     *
     * Note: The application is responsible for calling processBatch() periodically,
     * typically by scheduling work contracts in a loop.
     */
    explicit BatchManager(NetworkSession* session, uint32_t batchIntervalMs = 16);
    ~BatchManager();

    /**
     * @brief Update a property value (will be batched)
     *
     * Accumulates the update. If the same property is updated multiple times
     * before the next batch, only the latest value is kept.
     *
     * @param hash Property hash (computed by caller)
     * @param type Property type
     * @param value Property value
     */
    void updateProperty(PropertyHash128 hash, PropertyType type, const PropertyValue& value);

    /**
     * @brief Process pending updates and send a batch
     *
     * This should be called periodically by the application (e.g., via work contracts).
     * The application is responsible for scheduling this at the desired batch interval.
     */
    void processBatch();

    /**
     * @brief Force an immediate flush of pending updates
     *
     * Alias for processBatch(). Useful for critical updates that can't wait.
     */
    void flush();

    /**
     * @brief Set the batch interval
     * @param intervalMs New interval in milliseconds
     */
    void setBatchInterval(uint32_t intervalMs);

    /**
     * @brief Get current batch interval
     */
    uint32_t getBatchInterval() const { return _batchIntervalMs; }

    /**
     * @brief Get number of pending updates waiting to be batched
     */
    size_t getPendingCount() const;

    /**
     * @brief Get statistics
     */
    struct Stats {
        uint64_t totalBatchesSent{0};
        uint64_t totalUpdatesSent{0};
        uint64_t batchesDropped{0};      // Due to backpressure
        uint64_t updatesDeduped{0};       // Same property updated multiple times
        uint32_t averageBatchSize{0};
        uint32_t currentBatchInterval{0};
    };
    Stats getStats() const;

private:
    struct PendingUpdate {
        PropertyType type;
        PropertyValue value;
        std::chrono::steady_clock::time_point timestamp;
    };

    void adjustBatchRate();

    NetworkSession* _session; // Not owned

    // Pending updates (keyed by property hash)
    std::unordered_map<PropertyHash128, PendingUpdate> _pendingUpdates;
    mutable std::mutex _mutex;

    // Batch scheduling
    uint32_t _batchIntervalMs;
    std::atomic<uint32_t> _dynamicIntervalMs; // Adjusted for backpressure

    // Sequencing
    std::atomic<uint32_t> _sequenceNumber{0};

    // Statistics
    mutable std::mutex _statsMutex;
    Stats _stats;

    // Backpressure tracking
    std::atomic<uint32_t> _pendingBatchCount{0}; // Batches waiting to send
    static constexpr uint32_t MAX_PENDING_BATCHES = 3;
};

} // namespace EntropyEngine::Networking
