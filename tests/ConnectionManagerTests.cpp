/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/NetworkConnection.h"

using namespace EntropyEngine::Networking;

TEST(ConnectionManagerTests, CreateConnection) {
    ConnectionManager manager(64);

    auto handle = manager.openLocalConnection("/tmp/test.sock");
    EXPECT_TRUE(handle.valid());
}

TEST(ConnectionManagerTests, GetConnection) {
    ConnectionManager manager(64);

    auto handle = manager.openLocalConnection("/tmp/test.sock");
    ASSERT_TRUE(handle.valid());

    EXPECT_EQ(handle.getType(), ConnectionType::Local);
    EXPECT_EQ(handle.getState(), ConnectionState::Disconnected);
}

TEST(ConnectionManagerTests, InvalidHandle) {
    ConnectionManager manager(64);

    ConnectionHandle invalidHandle;
    EXPECT_FALSE(invalidHandle.valid());
}

TEST(ConnectionManagerTests, MultipleConnections) {
    ConnectionManager manager(64);

    auto handle1 = manager.openLocalConnection("/tmp/test1.sock");
    auto handle2 = manager.openLocalConnection("/tmp/test2.sock");
    auto handle3 = manager.openLocalConnection("/tmp/test3.sock");

    ASSERT_TRUE(handle1.valid());
    ASSERT_TRUE(handle2.valid());
    ASSERT_TRUE(handle3.valid());

    EXPECT_EQ(manager.activeCount(), 3);

    // All handles should be independent
    EXPECT_NE(handle1.handleIndex(), handle2.handleIndex());
    EXPECT_NE(handle2.handleIndex(), handle3.handleIndex());
    EXPECT_NE(handle1.handleIndex(), handle3.handleIndex());
}

TEST(ConnectionManagerTests, HandleCopy) {
    ConnectionManager manager(64);

    auto handle1 = manager.openLocalConnection("/tmp/test.sock");
    ASSERT_TRUE(handle1.valid());

    // Copy handle
    auto handle2 = handle1;
    EXPECT_TRUE(handle2.valid());
    EXPECT_EQ(handle2.handleIndex(), handle1.handleIndex());
    EXPECT_EQ(handle2.handleGeneration(), handle1.handleGeneration());
}

TEST(ConnectionManagerTests, ConnectionStats) {
    ConnectionManager manager(64);

    auto handle = manager.openLocalConnection("/tmp/test.sock");
    ASSERT_TRUE(handle.valid());

    auto stats = handle.getStats();
    EXPECT_EQ(stats.bytesSent, 0);
    EXPECT_EQ(stats.bytesReceived, 0);
    EXPECT_EQ(stats.messagesSent, 0);
    EXPECT_EQ(stats.messagesReceived, 0);
}

TEST(ConnectionManagerTests, CapacityLimit) {
    ConnectionManager manager(2);  // Small capacity

    auto handle1 = manager.openLocalConnection("/tmp/test1.sock");
    auto handle2 = manager.openLocalConnection("/tmp/test2.sock");
    auto handle3 = manager.openLocalConnection("/tmp/test3.sock");

    EXPECT_TRUE(handle1.valid());
    EXPECT_TRUE(handle2.valid());
    EXPECT_FALSE(handle3.valid());  // Should fail - capacity reached

    EXPECT_EQ(manager.activeCount(), 2);
}
