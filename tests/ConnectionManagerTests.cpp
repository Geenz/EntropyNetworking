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
    ConnectionManager manager;

    auto result = manager.createUnixSocketConnection("/tmp/test.sock");
    EXPECT_TRUE(result.success());
    EXPECT_NE(result.value, 0);
}

TEST(ConnectionManagerTests, GetConnection) {
    ConnectionManager manager;

    auto createResult = manager.createUnixSocketConnection("/tmp/test.sock");
    ASSERT_TRUE(createResult.success());

    ConnectionId id = createResult.value;
    auto* connection = manager.getConnection(id);

    ASSERT_NE(connection, nullptr);
    EXPECT_EQ(connection->getType(), ConnectionType::UnixSocket);
    EXPECT_EQ(connection->getState(), ConnectionState::Disconnected);

    // Release our reference
    connection->release();
}

TEST(ConnectionManagerTests, GetNonExistentConnection) {
    ConnectionManager manager;

    auto* connection = manager.getConnection(9999);
    EXPECT_EQ(connection, nullptr);
}

TEST(ConnectionManagerTests, RemoveConnection) {
    ConnectionManager manager;

    auto createResult = manager.createUnixSocketConnection("/tmp/test.sock");
    ASSERT_TRUE(createResult.success());

    ConnectionId id = createResult.value;
    EXPECT_EQ(manager.getConnectionCount(), 1);

    auto removeResult = manager.removeConnection(id);
    EXPECT_TRUE(removeResult.success());
    EXPECT_EQ(manager.getConnectionCount(), 0);

    // Getting removed connection should return nullptr
    auto* connection = manager.getConnection(id);
    EXPECT_EQ(connection, nullptr);
}

TEST(ConnectionManagerTests, RemoveNonExistentConnection) {
    ConnectionManager manager;

    auto result = manager.removeConnection(9999);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::EntityNotFound);
}

TEST(ConnectionManagerTests, MultipleConnections) {
    ConnectionManager manager;

    auto result1 = manager.createUnixSocketConnection("/tmp/test1.sock");
    auto result2 = manager.createUnixSocketConnection("/tmp/test2.sock");
    auto result3 = manager.createUnixSocketConnection("/tmp/test3.sock");

    ASSERT_TRUE(result1.success());
    ASSERT_TRUE(result2.success());
    ASSERT_TRUE(result3.success());

    EXPECT_EQ(manager.getConnectionCount(), 3);

    // All IDs should be unique
    EXPECT_NE(result1.value, result2.value);
    EXPECT_NE(result2.value, result3.value);
    EXPECT_NE(result1.value, result3.value);

    auto ids = manager.getAllConnectionIds();
    EXPECT_EQ(ids.size(), 3);
}

TEST(ConnectionManagerTests, DisconnectAll) {
    ConnectionManager manager;

    manager.createUnixSocketConnection("/tmp/test1.sock");
    manager.createUnixSocketConnection("/tmp/test2.sock");
    manager.createUnixSocketConnection("/tmp/test3.sock");

    EXPECT_EQ(manager.getConnectionCount(), 3);

    manager.disconnectAll();

    EXPECT_EQ(manager.getConnectionCount(), 0);
}

TEST(ConnectionManagerTests, RefCounting) {
    ConnectionManager manager;

    auto createResult = manager.createUnixSocketConnection("/tmp/test.sock");
    ASSERT_TRUE(createResult.success());

    ConnectionId id = createResult.value;

    // Get connection and retain
    auto* connection = manager.getConnection(id);
    ASSERT_NE(connection, nullptr);

    // Connection should still exist after removal from manager
    // because we hold a reference
    manager.removeConnection(id);

    // Connection object should still be valid
    EXPECT_EQ(connection->getType(), ConnectionType::UnixSocket);

    // Release our reference
    connection->release();
}

TEST(ConnectionManagerTests, ConnectionStats) {
    ConnectionManager manager;

    auto createResult = manager.createUnixSocketConnection("/tmp/test.sock");
    ASSERT_TRUE(createResult.success());

    auto* connection = manager.getConnection(createResult.value);
    ASSERT_NE(connection, nullptr);

    auto stats = connection->getStats();
    EXPECT_EQ(stats.bytesSent, 0);
    EXPECT_EQ(stats.bytesReceived, 0);
    EXPECT_EQ(stats.messagesSent, 0);
    EXPECT_EQ(stats.messagesReceived, 0);

    connection->release();
}
