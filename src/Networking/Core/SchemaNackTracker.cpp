/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "SchemaNackTracker.h"

#include <algorithm>

namespace EntropyEngine
{
namespace Networking
{

SchemaNackTracker::SchemaNackTracker(const Config& config) : _config(config) {}

bool SchemaNackTracker::shouldSendNack(const ComponentTypeHash& typeHash) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _nackRecords.find(typeHash);
    if (it == _nackRecords.end()) {
        // First time seeing this schema - should send NACK
        return true;
    }

    // Check if enough time has passed since last NACK
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.lastNackTime);

    return elapsed >= _config.minInterval;
}

void SchemaNackTracker::recordNackSent(const ComponentTypeHash& typeHash) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto now = std::chrono::steady_clock::now();
    auto it = _nackRecords.find(typeHash);
    if (it != _nackRecords.end()) {
        it->second.lastNackTime = now;
        it->second.count++;
    } else {
        NackRecord record;
        record.lastNackTime = now;
        record.count = 1;
        _nackRecords.emplace(typeHash, record);
    }

    _totalNacksSent++;

    // Prune old records if we exceed the limit
    if (_nackRecords.size() > _config.maxTrackedSchemas) {
        pruneOldRecords();
    }
}

size_t SchemaNackTracker::getTotalNacksSent() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _totalNacksSent;
}

size_t SchemaNackTracker::getUniqueSchemas() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _nackRecords.size();
}

void SchemaNackTracker::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _nackRecords.clear();
    _totalNacksSent = 0;
}

void SchemaNackTracker::pruneOldRecords() {
    // Remove oldest 25% of records when limit is exceeded
    if (_nackRecords.empty()) {
        return;
    }

    size_t targetSize = _config.maxTrackedSchemas * 3 / 4;

    // Find oldest records
    std::vector<std::pair<ComponentTypeHash, std::chrono::steady_clock::time_point>> records;
    records.reserve(_nackRecords.size());

    for (const auto& [hash, record] : _nackRecords) {
        records.emplace_back(hash, record.lastNackTime);
    }

    // Sort by timestamp (oldest first)
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    // Remove oldest entries
    size_t toRemove = records.size() - targetSize;
    for (size_t i = 0; i < toRemove; ++i) {
        _nackRecords.erase(records[i].first);
    }
}

}  // namespace Networking
}  // namespace EntropyEngine
