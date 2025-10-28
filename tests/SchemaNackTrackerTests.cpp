/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Core/SchemaNackTracker.h"
#include <thread>
#include <chrono>

using namespace EntropyEngine::Networking;

TEST(SchemaNackTrackerTests, InitialStateShouldSendNack) {
    SchemaNackTracker tracker;

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    // First encounter - should send NACK
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
}

TEST(SchemaNackTrackerTests, RecordNackSentUpdatesMetrics) {
    SchemaNackTracker tracker;

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    EXPECT_EQ(tracker.getTotalNacksSent(), 0);
    EXPECT_EQ(tracker.getUniqueSchemas(), 0);

    tracker.recordNackSent(hash1);

    EXPECT_EQ(tracker.getTotalNacksSent(), 1);
    EXPECT_EQ(tracker.getUniqueSchemas(), 1);
}

TEST(SchemaNackTrackerTests, RateLimitingSameSchema) {
    SchemaNackTracker::Config config;
    config.minInterval = std::chrono::milliseconds(100);
    SchemaNackTracker tracker(config);

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    // First NACK - should send
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
    tracker.recordNackSent(hash1);

    // Immediately after - should be rate limited
    EXPECT_FALSE(tracker.shouldSendNack(hash1));

    // Still within interval - should be rate limited
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(tracker.shouldSendNack(hash1));

    // After interval - should send again
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
}

TEST(SchemaNackTrackerTests, DifferentSchemasIndependent) {
    SchemaNackTracker tracker;

    ComponentTypeHash hash1{0x1111111111111111, 0x1111111111111111};
    ComponentTypeHash hash2{0x2222222222222222, 0x2222222222222222};

    // Send NACK for hash1
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
    tracker.recordNackSent(hash1);

    // hash2 should still be sendable (independent)
    EXPECT_TRUE(tracker.shouldSendNack(hash2));
    tracker.recordNackSent(hash2);

    // But hash1 should be rate limited
    EXPECT_FALSE(tracker.shouldSendNack(hash1));

    EXPECT_EQ(tracker.getTotalNacksSent(), 2);
    EXPECT_EQ(tracker.getUniqueSchemas(), 2);
}

TEST(SchemaNackTrackerTests, MultipleNacksForSameSchema) {
    SchemaNackTracker::Config config;
    config.minInterval = std::chrono::milliseconds(50);
    SchemaNackTracker tracker(config);

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    // Send 3 NACKs with waits
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(tracker.shouldSendNack(hash1));
        tracker.recordNackSent(hash1);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    EXPECT_EQ(tracker.getTotalNacksSent(), 3);
    EXPECT_EQ(tracker.getUniqueSchemas(), 1);
}

TEST(SchemaNackTrackerTests, ClearResetsState) {
    SchemaNackTracker tracker;

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    tracker.recordNackSent(hash1);
    EXPECT_EQ(tracker.getTotalNacksSent(), 1);
    EXPECT_EQ(tracker.getUniqueSchemas(), 1);

    tracker.clear();

    EXPECT_EQ(tracker.getTotalNacksSent(), 0);
    EXPECT_EQ(tracker.getUniqueSchemas(), 0);

    // After clear, should send NACK again
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
}

TEST(SchemaNackTrackerTests, MaxTrackedSchemasLimit) {
    SchemaNackTracker::Config config;
    config.maxTrackedSchemas = 10;
    SchemaNackTracker tracker(config);

    // Register more schemas than the limit
    for (uint64_t i = 0; i < 15; ++i) {
        ComponentTypeHash hash{i, i};
        tracker.recordNackSent(hash);
    }

    // Should have pruned down to reasonable size
    EXPECT_LE(tracker.getUniqueSchemas(), config.maxTrackedSchemas);
    EXPECT_EQ(tracker.getTotalNacksSent(), 15);
}

TEST(SchemaNackTrackerTests, DefaultConfigValues) {
    SchemaNackTracker tracker;

    ComponentTypeHash hash1{0x1111111111111111, 0x1111111111111111};

    // With default 1000ms interval
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
    tracker.recordNackSent(hash1);

    // Immediately after should be rate limited
    EXPECT_FALSE(tracker.shouldSendNack(hash1));

    // After 100ms still rate limited (default is 1000ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(tracker.shouldSendNack(hash1));
}

TEST(SchemaNackTrackerTests, ZeroIntervalAlwaysSends) {
    SchemaNackTracker::Config config;
    config.minInterval = std::chrono::milliseconds(0);
    SchemaNackTracker tracker(config);

    ComponentTypeHash hash1{0x1234567890abcdef, 0xfedcba0987654321};

    // Even with 0 interval, should track properly
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
    tracker.recordNackSent(hash1);

    // With 0 interval, should immediately allow another NACK
    EXPECT_TRUE(tracker.shouldSendNack(hash1));
}
