/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file NetworkSessionPermissionTests.cpp
 * @brief End-to-end tests for permission protocol messages over real IPC transport.
 *
 * Tests verify that PermissionRequest, PermissionResponse, and PermissionRevoked
 * messages serialize, transmit, and deserialize correctly through the full
 * NetworkSession → Cap'n Proto → IPC → NetworkSession pipeline.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../src/Networking/Core/PermissionTypes.h"
#include "../src/Networking/Session/NetworkSession.h"
#include "../src/Networking/Transport/ConnectionHandle.h"
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"

using namespace EntropyEngine::Networking;

class NetworkSessionPermissionTests : public ::testing::Test
{
protected:
    void SetUp() override {
        endpoint = "/tmp/entropy_permission_test_" + std::to_string(getTestCounter()) + ".sock";

        serverMgr = std::make_unique<ConnectionManager>(8);
        clientMgr = std::make_unique<ConnectionManager>(8);

        server = createLocalServer(serverMgr.get(), endpoint);
        auto listenRes = server->listen();
        ASSERT_TRUE(listenRes.success()) << "Server listen failed: " << listenRes.errorMessage;
    }

    void TearDown() override {
        if (clientSession) clientSession->disconnect();
        if (serverSession) serverSession->disconnect();

        clientSession.reset();
        serverSession.reset();

        if (clientConnHandle.valid()) clientConnHandle.close();
        if (serverConnHandle.valid()) serverConnHandle.close();

        if (server) server->close();

        clientMgr.reset();
        serverMgr.reset();
    }

    static uint64_t getTestCounter() {
        static std::atomic<uint64_t> counter{100};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    bool connectAndHandshake() {
        std::thread serverThread([this]() { serverConnHandle = server->accept(); });

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

        serverThread.join();
        if (!serverConnHandle.valid()) return false;

        // Wait for connections to establish
        for (int i = 0; i < 100 && (!clientConnHandle.isConnected() || !serverConnHandle.isConnected()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!clientConnHandle.isConnected() || !serverConnHandle.isConnected()) return false;

        auto* serverConn = serverMgr->getConnectionPointer(serverConnHandle);
        auto* clientConn = clientMgr->getConnectionPointer(clientConnHandle);
        if (!serverConn || !clientConn) return false;

        serverSession = std::make_unique<NetworkSession>(serverConn);
        clientSession = std::make_unique<NetworkSession>(clientConn);

        serverSession->setupCallbacks();
        clientSession->setupCallbacks();

        // Perform handshake
        auto hsResult = clientSession->performHandshake("sdk", "test-client");
        if (!hsResult.success()) return false;

        for (int i = 0; i < 100 && !serverSession->isHandshakeComplete(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return serverSession->isHandshakeComplete();
    }

    // Wait for an atomic flag to become true
    bool waitFor(const std::atomic<bool>& flag, int maxMs = 2000) {
        for (int i = 0; i < maxMs / 10; ++i) {
            if (flag.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load(std::memory_order_acquire);
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

// =============================================================================
// PermissionRequest: Client → Server
// =============================================================================

TEST_F(NetworkSessionPermissionTests, PermissionRequestRoundTrip) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxRequestId = 0;
    uint64_t rxRequestingSessionId = 0;
    std::string rxAppId;
    PermissionKey rxKey{};
    std::string rxReason;

    serverSession->setPermissionRequestCallback([&](uint64_t requestId, uint64_t requestingSessionId,
                                                    const std::string& appId, PermissionKey key,
                                                    const std::string& reason) {
        rxRequestId = requestId;
        rxRequestingSessionId = requestingSessionId;
        rxAppId = appId;
        rxKey = key;
        rxReason = reason;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = clientSession->sendPermissionRequest(42, 100, "SculptTool", {0, 1}, "To show your avatar");
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for PermissionRequest";

    EXPECT_EQ(rxRequestId, 42u);
    EXPECT_EQ(rxRequestingSessionId, 100u);
    EXPECT_EQ(rxAppId, "SculptTool");
    EXPECT_EQ(rxKey.category, 0u);
    EXPECT_EQ(rxKey.permission, 1u);
    EXPECT_EQ(rxReason, "To show your avatar");
}

// =============================================================================
// PermissionResponse: Server → Client
// =============================================================================

TEST_F(NetworkSessionPermissionTests, HeadPosePermissionResponseGranted) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxRequestId = 0;
    uint64_t rxRespondingSessionId = 0;
    bool rxGranted = false;
    uint64_t rxGrantToken = 0;
    bool rxIsNewGrant = false;

    clientSession->setHeadPosePermissionResponseCallback(
        [&](uint64_t requestId, uint64_t respondingSessionId, bool granted, uint64_t grantToken, bool isNewGrant) {
            rxRequestId = requestId;
            rxRespondingSessionId = respondingSessionId;
            rxGranted = granted;
            rxGrantToken = grantToken;
            rxIsNewGrant = isNewGrant;
            received.store(true, std::memory_order_release);
        });

    auto sendResult = serverSession->sendHeadPosePermissionResponse(42, 200, true, 9999, true);
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for HeadPosePermissionResponse";

    EXPECT_EQ(rxRequestId, 42u);
    EXPECT_EQ(rxRespondingSessionId, 200u);
    EXPECT_TRUE(rxGranted);
    EXPECT_EQ(rxGrantToken, 9999u);
    EXPECT_TRUE(rxIsNewGrant);
}

TEST_F(NetworkSessionPermissionTests, IdentityPermissionResponseGranted) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxRequestId = 0;
    uint64_t rxRespondingSessionId = 0;
    bool rxGranted = false;
    uint64_t rxGrantToken = 0;
    bool rxIsNewGrant = false;
    std::string rxIdentityHash;

    clientSession->setIdentityPermissionResponseCallback([&](uint64_t requestId, uint64_t respondingSessionId,
                                                             bool granted, uint64_t grantToken, bool isNewGrant,
                                                             const std::string& identityHash) {
        rxRequestId = requestId;
        rxRespondingSessionId = respondingSessionId;
        rxGranted = granted;
        rxGrantToken = grantToken;
        rxIsNewGrant = isNewGrant;
        rxIdentityHash = identityHash;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = serverSession->sendIdentityPermissionResponse(43, 200, true, 7777, true, "abc123hash");
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for IdentityPermissionResponse";

    EXPECT_EQ(rxRequestId, 43u);
    EXPECT_EQ(rxRespondingSessionId, 200u);
    EXPECT_TRUE(rxGranted);
    EXPECT_EQ(rxGrantToken, 7777u);
    EXPECT_TRUE(rxIsNewGrant);
    EXPECT_EQ(rxIdentityHash, "abc123hash");
}

TEST_F(NetworkSessionPermissionTests, UsernamePermissionResponseGranted) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxRequestId = 0;
    bool rxGranted = false;
    uint64_t rxGrantToken = 0;
    bool rxIsNewGrant = false;
    std::string rxUsername;

    clientSession->setUsernamePermissionResponseCallback([&](uint64_t requestId, uint64_t respondingSessionId,
                                                             bool granted, uint64_t grantToken, bool isNewGrant,
                                                             const std::string& username) {
        rxRequestId = requestId;
        rxGranted = granted;
        rxGrantToken = grantToken;
        rxIsNewGrant = isNewGrant;
        rxUsername = username;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = serverSession->sendUsernamePermissionResponse(44, 200, true, 8888, true, "testuser");
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for UsernamePermissionResponse";

    EXPECT_EQ(rxRequestId, 44u);
    EXPECT_TRUE(rxGranted);
    EXPECT_EQ(rxGrantToken, 8888u);
    EXPECT_TRUE(rxIsNewGrant);
    EXPECT_EQ(rxUsername, "testuser");
}

TEST_F(NetworkSessionPermissionTests, HostnamePermissionResponseGranted) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxRequestId = 0;
    bool rxGranted = false;
    uint64_t rxGrantToken = 0;
    bool rxIsNewGrant = false;
    std::string rxHostname;

    clientSession->setHostnamePermissionResponseCallback([&](uint64_t requestId, uint64_t respondingSessionId,
                                                             bool granted, uint64_t grantToken, bool isNewGrant,
                                                             const std::string& hostname) {
        rxRequestId = requestId;
        rxGranted = granted;
        rxGrantToken = grantToken;
        rxIsNewGrant = isNewGrant;
        rxHostname = hostname;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = serverSession->sendHostnamePermissionResponse(45, 200, true, 6666, true, "testhost.local");
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for HostnamePermissionResponse";

    EXPECT_EQ(rxRequestId, 45u);
    EXPECT_TRUE(rxGranted);
    EXPECT_EQ(rxGrantToken, 6666u);
    EXPECT_TRUE(rxIsNewGrant);
    EXPECT_EQ(rxHostname, "testhost.local");
}

TEST_F(NetworkSessionPermissionTests, HeadPosePermissionResponseDenied) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    bool rxGranted = true;  // Start as true to verify it gets set to false

    clientSession->setHeadPosePermissionResponseCallback([&](uint64_t, uint64_t, bool granted, uint64_t, bool) {
        rxGranted = granted;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = serverSession->sendHeadPosePermissionResponse(42, 200, false);
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for HeadPosePermissionResponse";
    EXPECT_FALSE(rxGranted);
}

// =============================================================================
// PermissionRevoked: Server → Client
// =============================================================================

TEST_F(NetworkSessionPermissionTests, PermissionRevokedRoundTrip) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<bool> received{false};
    uint64_t rxPortalSessionId = 0;
    PermissionKey rxKey{};

    clientSession->setPermissionRevokedCallback([&](uint64_t portalSessionId, PermissionKey key) {
        rxPortalSessionId = portalSessionId;
        rxKey = key;
        received.store(true, std::memory_order_release);
    });

    auto sendResult = serverSession->sendPermissionRevoked(200, {0, 0});
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    ASSERT_TRUE(waitFor(received)) << "Timed out waiting for PermissionRevoked";

    EXPECT_EQ(rxPortalSessionId, 200u);
    EXPECT_EQ(rxKey.category, 0u);
    EXPECT_EQ(rxKey.permission, 0u);
}

// =============================================================================
// Bidirectional: Request → Response complete flow
// =============================================================================

TEST_F(NetworkSessionPermissionTests, RequestResponseFullFlow) {
    ASSERT_TRUE(connectAndHandshake());

    // Server receives the request and sends back a HeadPose response
    std::atomic<bool> serverReceivedRequest{false};
    serverSession->setPermissionRequestCallback([&](uint64_t requestId, uint64_t requestingSessionId,
                                                    const std::string& appId, PermissionKey key,
                                                    const std::string& reason) {
        // Server (acting as relay) sends HeadPose response back
        serverSession->sendHeadPosePermissionResponse(requestId, 300, true, 12345, true);
        serverReceivedRequest.store(true, std::memory_order_release);
    });

    // Client receives the HeadPose response
    std::atomic<bool> clientReceivedResponse{false};
    uint64_t rxRequestId = 0;
    bool rxGranted = false;
    uint64_t rxGrantToken = 0;
    bool rxIsNewGrant = false;

    clientSession->setHeadPosePermissionResponseCallback(
        [&](uint64_t requestId, uint64_t respondingSessionId, bool granted, uint64_t grantToken, bool isNewGrant) {
            rxRequestId = requestId;
            rxGranted = granted;
            rxGrantToken = grantToken;
            rxIsNewGrant = isNewGrant;
            clientReceivedResponse.store(true, std::memory_order_release);
        });

    // Client sends permission request (category 0 = Body, permission 0 = HeadPose)
    auto sendResult = clientSession->sendPermissionRequest(1, 100, "TestApp", {0, 0}, "Need body tracking");
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    // Wait for the full round trip
    ASSERT_TRUE(waitFor(serverReceivedRequest)) << "Server didn't receive request";
    ASSERT_TRUE(waitFor(clientReceivedResponse)) << "Client didn't receive response";

    EXPECT_EQ(rxRequestId, 1u);
    EXPECT_TRUE(rxGranted);
    EXPECT_EQ(rxGrantToken, 12345u);
    EXPECT_TRUE(rxIsNewGrant);
}

// =============================================================================
// PermissionKey Struct Tests
// =============================================================================

TEST_F(NetworkSessionPermissionTests, PermissionKeyEquality) {
    PermissionKey a{0, 0};
    PermissionKey b{0, 0};
    PermissionKey c{1, 0};
    PermissionKey d{0, 1};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_NE(c, d);
}

TEST_F(NetworkSessionPermissionTests, PermissionKeyPacked) {
    PermissionKey a{0, 0};
    EXPECT_EQ(a.packed(), 0u);

    PermissionKey b{1, 0};
    EXPECT_EQ(b.packed(), 0x00010000u);

    PermissionKey c{0, 1};
    EXPECT_EQ(c.packed(), 1u);

    PermissionKey d{0xFFFF, 0xFFFF};
    EXPECT_EQ(d.packed(), 0xFFFFFFFFu);
}

TEST_F(NetworkSessionPermissionTests, PermissionKeyHash) {
    PermissionKey::Hash hasher;
    PermissionKey a{0, 0};
    PermissionKey b{1, 0};

    // Different keys should have different hashes (not guaranteed but highly likely)
    EXPECT_NE(hasher(a), hasher(b));

    // Same keys should have same hashes
    PermissionKey c{0, 0};
    EXPECT_EQ(hasher(a), hasher(c));
}

// =============================================================================
// Multiple Messages in Sequence
// =============================================================================

TEST_F(NetworkSessionPermissionTests, MultiplePermissionRequestsInSequence) {
    ASSERT_TRUE(connectAndHandshake());

    std::atomic<int> receivedCount{0};

    serverSession->setPermissionRequestCallback(
        [&](uint64_t, uint64_t, const std::string&, PermissionKey, const std::string&) {
            receivedCount.fetch_add(1, std::memory_order_release);
        });

    // Send 3 requests
    ASSERT_TRUE(clientSession->sendPermissionRequest(1, 100, "App1", {0, 0}, "reason1").success());
    ASSERT_TRUE(clientSession->sendPermissionRequest(2, 101, "App2", {1, 0}, "reason2").success());
    ASSERT_TRUE(clientSession->sendPermissionRequest(3, 102, "App3", {0, 1}, "reason3").success());

    // Wait for all to arrive
    for (int i = 0; i < 200 && receivedCount.load(std::memory_order_acquire) < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(receivedCount.load(std::memory_order_acquire), 3);
}
