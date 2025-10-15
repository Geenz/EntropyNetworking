/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "BatchManager.h"
#include "SessionHandle.h"
#include "SessionManager.h"
#include "src/Networking/Protocol/entropy.capnp.h"
#include <capnp/message.h>

namespace EntropyEngine::Networking {

BatchManager::BatchManager(SessionHandle session, uint32_t batchIntervalMs)
    : _session(session)
    , _batchIntervalMs(batchIntervalMs)
    , _dynamicIntervalMs(batchIntervalMs)
{
}

BatchManager::~BatchManager() {
    // Flush any remaining pending updates
    flush();
}

void BatchManager::updateProperty(PropertyHash128 hash, PropertyType type, const PropertyValue& value) {
    std::lock_guard<std::mutex> lock(_mutex);

    // Check if this property already has a pending update (deduplication)
    auto it = _pendingUpdates.find(hash);
    if (it != _pendingUpdates.end()) {
        // Update exists, just replace the value
        it->second.value = value;
        it->second.timestamp = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> statsLock(_statsMutex);
        _stats.updatesDeduped++;
    } else {
        // New update
        _pendingUpdates[hash] = PendingUpdate{
            type,
            value,
            std::chrono::steady_clock::now()
        };
    }
}

void BatchManager::flush() {
    processBatch();
}

void BatchManager::setBatchInterval(uint32_t intervalMs) {
    _batchIntervalMs = intervalMs;
    _dynamicIntervalMs = intervalMs;
    // The contract will pick up the new interval on its next reschedule
}

size_t BatchManager::getPendingCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _pendingUpdates.size();
}

BatchManager::Stats BatchManager::getStats() const {
    std::lock_guard<std::mutex> lock(_statsMutex);
    return _stats;
}

void BatchManager::processBatch() {
    // Move pending updates out of the accumulator
    std::unordered_map<PropertyHash128, PendingUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pendingUpdates.empty()) {
            return; // Nothing to send
        }
        updates = std::move(_pendingUpdates);
        _pendingUpdates.clear();
    }

    // Check backpressure
    if (_pendingBatchCount >= MAX_PENDING_BATCHES) {
        // Too many batches pending, drop this one
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.batchesDropped++;
        adjustBatchRate(); // Slow down
        return;
    }

    _pendingBatchCount++;

    try {
        // Build Cap'n Proto PropertyUpdateBatch
        ::capnp::MallocMessageBuilder builder;
        auto message = builder.initRoot<Message>();
        auto batch = message.initPropertyUpdateBatch();

        // Set timestamp and sequence
        batch.setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
        batch.setSequence(_sequenceNumber++);

        // Add all updates
        auto updatesList = batch.initUpdates(updates.size());
        size_t index = 0;
        for (const auto& [hash, pending] : updates) {
            auto update = updatesList[index++];

            // Set property hash
            update.getPropertyHash().setHigh(hash.high);
            update.getPropertyHash().setLow(hash.low);

            // Set type
            update.setExpectedType(static_cast<::PropertyType>(pending.type));

            // Set value based on type
            auto value = update.initValue();
            std::visit([&value](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, int32_t>) {
                    value.setInt32(val);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    value.setInt64(val);
                } else if constexpr (std::is_same_v<T, float>) {
                    value.setFloat32(val);
                } else if constexpr (std::is_same_v<T, double>) {
                    value.setFloat64(val);
                } else if constexpr (std::is_same_v<T, Vec2>) {
                    auto vec = value.initVec2();
                    vec.setX(val.x);
                    vec.setY(val.y);
                } else if constexpr (std::is_same_v<T, Vec3>) {
                    auto vec = value.initVec3();
                    vec.setX(val.x);
                    vec.setY(val.y);
                    vec.setZ(val.z);
                } else if constexpr (std::is_same_v<T, Vec4>) {
                    auto vec = value.initVec4();
                    vec.setX(val.x);
                    vec.setY(val.y);
                    vec.setZ(val.z);
                    vec.setW(val.w);
                } else if constexpr (std::is_same_v<T, Quat>) {
                    auto quat = value.initQuat();
                    quat.setX(val.x);
                    quat.setY(val.y);
                    quat.setZ(val.z);
                    quat.setW(val.w);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    value.setString(val);
                } else if constexpr (std::is_same_v<T, bool>) {
                    value.setBool(val);
                } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                    value.setBytes(kj::arrayPtr(val.data(), val.size()));
                }
            }, pending.value);
        }

        // Serialize
        auto serialized = serialize(builder);
        if (serialized.failed()) {
            _pendingBatchCount--;
            return;
        }

        // Send on unreliable channel (via session handle)
        auto result = _session.sendPropertyUpdateBatch(serialized.value);

        _pendingBatchCount--;

        // Update statistics
        std::lock_guard<std::mutex> lock(_statsMutex);
        if (result.success()) {
            _stats.totalBatchesSent++;
            _stats.totalUpdatesSent += updates.size();
            _stats.averageBatchSize = (uint32_t)(
                _stats.totalUpdatesSent / std::max(_stats.totalBatchesSent, 1UL)
            );
        }

        // Restore batch rate if things are flowing smoothly
        if (_pendingBatchCount == 0 && _dynamicIntervalMs > _batchIntervalMs) {
            _dynamicIntervalMs = std::max(_batchIntervalMs, _dynamicIntervalMs - 1);
        }

    } catch (const std::exception& e) {
        _pendingBatchCount--;
        // Failed to create batch
    }
}


void BatchManager::adjustBatchRate() {
    // Increase batch interval (slow down) when under backpressure
    uint32_t current = _dynamicIntervalMs.load();
    uint32_t newInterval = std::min(current * 2, 100u); // Cap at 100ms (10Hz)
    _dynamicIntervalMs = newInterval;

    std::lock_guard<std::mutex> lock(_statsMutex);
    _stats.currentBatchInterval = newInterval;
}

} // namespace EntropyEngine::Networking
