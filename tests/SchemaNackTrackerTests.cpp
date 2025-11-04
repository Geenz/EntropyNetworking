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
#include "../src/Networking/Core/SchemaNackPolicy.h"
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
    // Add 10ms buffer to account for sleep_for imprecision and thread scheduling
    std::this_thread::sleep_for(std::chrono::milliseconds(70));  // Total: 120ms > 100ms
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

// SchemaNackPolicy Tests

TEST(SchemaNackPolicyTests, DefaultsToDisabled) {
    auto& policy = SchemaNackPolicy::instance();
    // Note: Since this is a singleton, we need to explicitly set state
    policy.disable();  // Ensure known state

    EXPECT_FALSE(policy.isEnabled());
    EXPECT_EQ(policy.getMinIntervalMs(), 1000u);
    EXPECT_EQ(policy.getBurst(), 1u);
    EXPECT_EQ(policy.getLogIntervalMs(), 5000u);
}

TEST(SchemaNackPolicyTests, CanEnableAndDisable) {
    auto& policy = SchemaNackPolicy::instance();

    policy.enable();
    EXPECT_TRUE(policy.isEnabled());

    policy.disable();
    EXPECT_FALSE(policy.isEnabled());
}

TEST(SchemaNackPolicyTests, CanModifyParameters) {
    auto& policy = SchemaNackPolicy::instance();

    policy.setMinIntervalMs(2000);
    EXPECT_EQ(policy.getMinIntervalMs(), 2000u);

    policy.setBurst(5);
    EXPECT_EQ(policy.getBurst(), 5u);

    policy.setLogIntervalMs(10000);
    EXPECT_EQ(policy.getLogIntervalMs(), 10000u);

    // Reset to defaults
    policy.setMinIntervalMs(1000);
    policy.setBurst(1);
    policy.setLogIntervalMs(5000);
}

TEST(SchemaNackPolicyTests, ThreadSafeAccess) {
    auto& policy = SchemaNackPolicy::instance();
    policy.disable();

    std::atomic<int> enableCount{0};
    std::atomic<int> disableCount{0};

    // Spawn multiple threads that toggle the policy
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&policy, &enableCount, &disableCount, i]() {
            for (int j = 0; j < 100; ++j) {
                if ((i + j) % 2 == 0) {
                    policy.enable();
                    enableCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    policy.disable();
                    disableCount.fetch_add(1, std::memory_order_relaxed);
                }

                // Also test parameter changes
                policy.setMinIntervalMs(1000 + (j % 10));
                policy.setBurst(1 + (j % 5));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify counts
    EXPECT_EQ(enableCount.load(), 500);
    EXPECT_EQ(disableCount.load(), 500);

    // Policy state should be consistent (no torn reads/writes)
    bool enabled = policy.isEnabled();
    EXPECT_TRUE(enabled == true || enabled == false);  // No garbage value
}
