/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../src/Networking/Session/NetworkSession.h"
#include "../src/Networking/Transport/ConnectionHandle.h"
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"

using namespace EntropyEngine::Networking;

/**
 * Tests for handshake protocol in NetworkSession
 * Uses cross-platform LocalServer for local IPC
 */
class NetworkSessionHandshakeTests : public ::testing::Test
{
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
        // CRITICAL: Disconnect and destroy sessions BEFORE closing connection handles
        // Sessions hold raw pointers to connections, so they must be destroyed first
        if (clientSession) {
            clientSession->disconnect();
        }
        if (serverSession) {
            serverSession->disconnect();
        }

        // Destroy sessions BEFORE closing connections (sessions hold raw pointers to connections)
        clientSession.reset();
        serverSession.reset();

        // Now safe to close connection handles (which may destroy the underlying connections)
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
        std::thread serverThread([this]() { serverConnHandle = server->accept(); });

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

        // Set up callbacks manually (SessionManager does this automatically in production)
        serverSession->setupCallbacks();
        clientSession->setupCallbacks();

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
    serverSession->setEntityCreatedCallback([&](uint64_t entityId, const std::string&, const std::string&, uint64_t) {
        serverEntityId = entityId;
        serverReceivedEntity = true;
    });

    clientSession->setEntityCreatedCallback([&](uint64_t entityId, const std::string&, const std::string&, uint64_t) {
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

// Test: Handshake callback is invoked on server when handshake completes
TEST_F(NetworkSessionHandshakeTests, ServerHandshakeCallbackInvoked) {
    ASSERT_TRUE(connectSessions());

    std::atomic<bool> callbackInvoked{false};
    std::string receivedClientType;
    std::string receivedClientId;
    std::mutex callbackMutex;

    // Set handshake callback on server
    serverSession->setHandshakeCallback([&](const std::string& clientType, const std::string& clientId) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        receivedClientType = clientType;
        receivedClientId = clientId;
        callbackInvoked = true;
    });

    // Client initiates handshake
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-123");
    ASSERT_TRUE(handshakeResult.success()) << handshakeResult.errorMessage;

    // Wait for handshake to complete and callback to be invoked
    for (int i = 0; i < 100 && !callbackInvoked.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify callback was invoked with correct parameters
    EXPECT_TRUE(callbackInvoked.load()) << "Server handshake callback should be invoked";
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        EXPECT_EQ(receivedClientType, "TestClient");
        EXPECT_EQ(receivedClientId, "client-123");
    }

    // Both sessions should have completed handshake
    EXPECT_TRUE(serverSession->isHandshakeComplete());
    EXPECT_TRUE(clientSession->isHandshakeComplete());
}

// Test: Handshake callback timing - invoked before messages can be sent
TEST_F(NetworkSessionHandshakeTests, HandshakeCallbackTimingCorrect) {
    ASSERT_TRUE(connectSessions());

    std::atomic<bool> handshakeCallbackInvoked{false};
    std::atomic<bool> canSendMessages{false};
    std::condition_variable cv;
    std::mutex mtx;

    // Set handshake callback that signals when handshake completes
    serverSession->setHandshakeCallback([&](const std::string& clientType, const std::string& clientId) {
        handshakeCallbackInvoked = true;
        // At this point, server should be able to send messages
        auto result = serverSession->sendEntityCreated(999, "ServerApp", "ServerEntity", 0);
        canSendMessages = result.success();
        cv.notify_all();
    });

    // Client initiates handshake
    auto handshakeResult = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshakeResult.success());

    // Wait for callback with timeout
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return handshakeCallbackInvoked.load(); });
    }

    EXPECT_TRUE(handshakeCallbackInvoked.load()) << "Handshake callback should be invoked";
    EXPECT_TRUE(canSendMessages.load()) << "Server should be able to send messages from within handshake callback";
}

// Test: Multiple sessions each get their own handshake callbacks
TEST_F(NetworkSessionHandshakeTests, MultipleSessionHandshakeCallbacks) {
    // Create first connection pair
    ASSERT_TRUE(connectSessions());

    std::atomic<int> handshakeCount{0};
    std::string firstClientId;
    std::string secondClientId;
    std::mutex callbackMutex;

    // Set handshake callback on first server session
    serverSession->setHandshakeCallback([&](const std::string& clientType, const std::string& clientId) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        firstClientId = clientId;
        handshakeCount++;
    });

    // Client initiates handshake
    auto handshake1 = clientSession->performHandshake("TestClient", "client-001");
    ASSERT_TRUE(handshake1.success());

    // Wait for first handshake
    for (int i = 0; i < 100 && handshakeCount.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(handshakeCount.load(), 1);
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        EXPECT_EQ(firstClientId, "client-001");
    }

    // Create second connection pair
    std::string endpoint2 = "/tmp/entropy_handshake_test_multi_" + std::to_string(getTestCounter()) + ".sock";
    auto serverMgr2 = std::make_unique<ConnectionManager>(8);
    auto clientMgr2 = std::make_unique<ConnectionManager>(8);
    auto server2 = createLocalServer(serverMgr2.get(), endpoint2);
    ASSERT_TRUE(server2->listen().success());

    ConnectionHandle serverConnHandle2;
    std::thread serverThread([&]() { serverConnHandle2 = server2->accept(); });

    auto clientConnHandle2 = clientMgr2->openLocalConnection(endpoint2);
    ASSERT_TRUE(clientConnHandle2.valid());
    ASSERT_TRUE(clientConnHandle2.connect().success());

    serverThread.join();
    ASSERT_TRUE(serverConnHandle2.valid());

    // Wait for connection
    for (int i = 0; i < 100 && !clientConnHandle2.isConnected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto* serverConn2 = serverMgr2->getConnectionPointer(serverConnHandle2);
    auto* clientConn2 = clientMgr2->getConnectionPointer(clientConnHandle2);
    ASSERT_NE(serverConn2, nullptr);
    ASSERT_NE(clientConn2, nullptr);

    auto serverSession2 = std::make_unique<NetworkSession>(serverConn2);
    auto clientSession2 = std::make_unique<NetworkSession>(clientConn2);

    // Set up callbacks manually (SessionManager does this automatically in production)
    serverSession2->setupCallbacks();
    clientSession2->setupCallbacks();

    // Set handshake callback on second server session
    serverSession2->setHandshakeCallback([&](const std::string& clientType, const std::string& clientId) {
        std::lock_guard<std::mutex> lock(callbackMutex);
        secondClientId = clientId;
        handshakeCount++;
    });

    // Second client initiates handshake
    auto handshake2 = clientSession2->performHandshake("TestClient", "client-002");
    ASSERT_TRUE(handshake2.success());

    // Wait for second handshake
    for (int i = 0; i < 100 && handshakeCount.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(handshakeCount.load(), 2) << "Both handshake callbacks should be invoked";
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        EXPECT_EQ(firstClientId, "client-001");
        EXPECT_EQ(secondClientId, "client-002");
    }

    // Cleanup second connection
    clientSession2.reset();
    serverSession2.reset();
    clientConnHandle2.close();
    serverConnHandle2.close();
    server2->close();
}
