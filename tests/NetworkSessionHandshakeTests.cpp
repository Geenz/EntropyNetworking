/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Session/NetworkSession.h"
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"
#include "../src/Networking/Transport/ConnectionHandle.h"
#include <thread>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <mutex>

using namespace EntropyEngine::Networking;

/**
 * Tests for handshake protocol in NetworkSession
 * Uses cross-platform LocalServer for local IPC
 */
class NetworkSessionHandshakeTests : public ::testing::Test {
protected:
    void SetUp() override {
        endpoint = "/tmp/entropy_handshake_test_" + std::to_string(getTestCounter()) + ".sock";

        serverMgr = std::make_unique<ConnectionManager>(8);
        clientMgr = std::make_unique<ConnectionManager>(8);

        server = createLocalServer(serverMgr.get(), endpoint);
        auto listenRes = server->listen();
        ASSERT_TRUE(listenRes.success()) << "Server listen failed: " << listenRes.errorMessage;
    }

    void TearDown() override {
        // Clean up sessions
        clientSession.reset();
        serverSession.reset();

        // Clean up connections
        if (clientConnHandle.valid()) {
            clientConnHandle.close();
        }
        if (serverConnHandle.valid()) {
            serverConnHandle.close();
        }

        // Clean up server
        if (server) {
            server->close();
        }

        // Clean up managers
        clientMgr.reset();
        serverMgr.reset();
    }

    static uint64_t getTestCounter() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    bool connectSessions() {
        // Start server accept in background
        std::thread serverThread([this]() {
            serverConnHandle = server->accept();
        });

        // Connect client
        clientConnHandle = clientMgr->openLocalConnection(endpoint);
        if (!clientConnHandle.valid()) {
            if (serverThread.joinable()) serverThread.join();
            return false;
        }

        auto connectRes = clientConnHandle.connect();
        if (!connectRes.success()) {
            if (serverThread.joinable()) serverThread.join();
            return false;
        }

        // Wait for server to accept
        serverThread.join();
        if (!serverConnHandle.valid()) {
            return false;
        }

        // Wait for connections to establish
        for (int i = 0; i < 100 && (!clientConnHandle.isConnected() || !serverConnHandle.isConnected()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!clientConnHandle.isConnected() || !serverConnHandle.isConnected()) {
            return false;
        }

        // Get NetworkConnection pointers and create NetworkSession instances
        auto* serverConn = serverMgr->getConnectionPointer(serverConnHandle);
        auto* clientConn = clientMgr->getConnectionPointer(clientConnHandle);

        if (!serverConn || !clientConn) {
            return false;
        }

        serverSession = std::make_unique<NetworkSession>(serverConn);
        clientSession = std::make_unique<NetworkSession>(clientConn);

        return true;
    }

    std::string endpoint;
    std::unique_ptr<ConnectionManager> serverMgr;
    std::unique_ptr<ConnectionManager> clientMgr;
    std::unique_ptr<LocalServer> server;
    ConnectionHandle serverConnHandle;
    ConnectionHandle clientConnHandle;
    std::unique_ptr<NetworkSession> serverSession;
    std::unique_ptr<NetworkSession> clientSession;
};

// Test: Client initiates handshake, server responds automatically
TEST_F(NetworkSessionHandshakeTests, ClientHandshakeServerAutoResponds) {
    ASSERT_TRUE(connectSessions());

    // Initiate handshake from client
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshakeResult.success()) << handshakeResult.errorMessage;

    // Wait for handshake to complete (server auto-responds, client receives response)
    for (int i = 0; i < 100; ++i) {
        if (clientSession->isHandshakeComplete() && serverSession->isHandshakeComplete()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Both sides should have completed handshake
    EXPECT_TRUE(clientSession->isHandshakeComplete())
        << "Client handshake should complete after receiving HANDSHAKE_RESPONSE";
    EXPECT_TRUE(serverSession->isHandshakeComplete())
        << "Server handshake should complete after sending HANDSHAKE_RESPONSE";
}

// Test: Messages are blocked before handshake completes
TEST_F(NetworkSessionHandshakeTests, MessagesBlockedBeforeHandshake) {
    ASSERT_TRUE(connectSessions());

    // Try to send EntityCreated before handshake
    auto result = clientSession->sendEntityCreated(123, "TestApp", "TestEntity", 0);

    // Should fail with handshake error
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::HandshakeFailed);
}

// Test: Messages allowed after handshake completes
TEST_F(NetworkSessionHandshakeTests, MessagesAllowedAfterHandshake) {
    ASSERT_TRUE(connectSessions());

    std::atomic<bool> receivedEntity{false};
    std::atomic<uint64_t> receivedEntityId{0};

    // Set up server callback for EntityCreated
    serverSession->setEntityCreatedCallback(
        [&](uint64_t entityId, const std::string& appId, const std::string& typeName, uint64_t parentId) {
            receivedEntityId = entityId;
            receivedEntity = true;
        });

    // Perform handshake
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshakeResult.success()) << handshakeResult.errorMessage;

    // Wait for handshake to complete
    for (int i = 0; i < 100 && !clientSession->isHandshakeComplete(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(clientSession->isHandshakeComplete());

    // Now try to send EntityCreated
    auto result = clientSession->sendEntityCreated(12345, "TestApp", "TestEntity", 0);
    EXPECT_TRUE(result.success()) << result.errorMessage;

    // Wait for server to receive message
    for (int i = 0; i < 100 && !receivedEntity.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(receivedEntity.load()) << "Server should receive EntityCreated message";
    EXPECT_EQ(receivedEntityId.load(), 12345u);
}

// Test: Bidirectional entity messages after handshake
TEST_F(NetworkSessionHandshakeTests, BidirectionalEntityMessagesAfterHandshake) {
    ASSERT_TRUE(connectSessions());

    std::atomic<bool> serverReceivedEntity{false};
    std::atomic<bool> clientReceivedEntity{false};
    std::atomic<uint64_t> serverEntityId{0};
    std::atomic<uint64_t> clientEntityId{0};

    // Set up callbacks
    serverSession->setEntityCreatedCallback(
        [&](uint64_t entityId, const std::string&, const std::string&, uint64_t) {
            serverEntityId = entityId;
            serverReceivedEntity = true;
        });

    clientSession->setEntityCreatedCallback(
        [&](uint64_t entityId, const std::string&, const std::string&, uint64_t) {
            clientEntityId = entityId;
            clientReceivedEntity = true;
        });

    // Perform handshake
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshakeResult.success());

    // Wait for handshake
    for (int i = 0; i < 100 && !clientSession->isHandshakeComplete(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(clientSession->isHandshakeComplete());
    ASSERT_TRUE(serverSession->isHandshakeComplete());

    // Send entity from client to server
    auto result1 = clientSession->sendEntityCreated(1001, "ClientApp", "ClientEntity", 0);
    EXPECT_TRUE(result1.success());

    // Send entity from server to client
    auto result2 = serverSession->sendEntityCreated(2002, "ServerApp", "ServerEntity", 0);
    EXPECT_TRUE(result2.success());

    // Wait for both messages to be received
    for (int i = 0; i < 100 && (!serverReceivedEntity.load() || !clientReceivedEntity.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(serverReceivedEntity.load());
    EXPECT_EQ(serverEntityId.load(), 1001u);

    EXPECT_TRUE(clientReceivedEntity.load());
    EXPECT_EQ(clientEntityId.load(), 2002u);
}

// Test: Handshake with protocol version and capabilities
TEST_F(NetworkSessionHandshakeTests, HandshakeNegotiatesCapabilities) {
    ASSERT_TRUE(connectSessions());

    // Perform handshake (client sends capabilities, server echoes them back)
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshakeResult.success());

    // Wait for handshake to complete
    for (int i = 0; i < 100 && !clientSession->isHandshakeComplete(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(clientSession->isHandshakeComplete());
    EXPECT_TRUE(serverSession->isHandshakeComplete());

    // In the current implementation, server echoes client capabilities
    // Both should agree on schema support being enabled
    // (This is implicit - actual capability values would need getters to verify)
}

// Test: Handshake timeout scenario (server never responds)
// Note: This test verifies client behavior when handshake isn't completed
TEST_F(NetworkSessionHandshakeTests, IncompleteHandshakeBlocksSending) {
    ASSERT_TRUE(connectSessions());

    // Don't perform handshake - just try to send

    auto result = clientSession->sendEntityCreated(999, "TestApp", "TestEntity", 0);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::HandshakeFailed);
}
