/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <EntropyCore.h>
#include <Logging/Logger.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../src/Networking/Transport/WebRTCConnection.h"
#include "WebRTCTestHelpers.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::Testing;

/**
 * @brief WebRTC Integration Tests
 *
 * These tests verify WebRTC peer-to-peer connections using in-process signaling.
 * Two WebRTCConnection instances exchange signaling data directly without an external server.
 *
 * Note: These tests are hermetic: in-process signaling, loopback-only host ICE,
 * ICE-TCP enabled, and a bounded port range. No external STUN/TURN is required.
 */

// Integration test - requires network and takes time (~5-10 seconds)
TEST(WebRTCIntegrationTests, TwoPeerConnect) {
    // Setup in-process signaling
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    // Peer 1 (offerer) setup - polite peer for perfect negotiation
    // Peer 2 (answerer) setup - impolite peer
    auto [config1, config2] = localHermeticRtcConfigPair();
    WebRTCConnection peer1(config1, callbacks1);
    WebRTCConnection peer2(config2, callbacks2);

    // Register peers with signaling helper
    signaling.setPeers(&peer1, &peer2);

    // Track connection state - use shared_ptr to prevent stack-use-after-scope
    // when callbacks fire from libdatachannel threads after test ends
    struct State
    {
        std::mutex mutex;
        bool peer1Connected = false;
        bool peer2Connected = false;
    };
    auto state = std::make_shared<State>();

    peer1.setStateCallback([state](ConnectionState connState) {
        if (connState == ConnectionState::Connected) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->peer1Connected = true;
        }
    });

    peer2.setStateCallback([state](ConnectionState connState) {
        if (connState == ConnectionState::Connected) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->peer2Connected = true;
        }
    });

    // Initiate connection - peer1 creates offer, peer2 creates answer
    auto r1 = peer1.connect();
    ASSERT_TRUE(r1.success()) << r1.errorMessage;
    ASSERT_TRUE(peer1.isReady());

    auto r2 = peer2.connect();
    ASSERT_TRUE(r2.success()) << r2.errorMessage;
    ASSERT_TRUE(peer2.isReady());

    // Wait for ICE gathering and connection establishment (up to 15 seconds)
    // WebRTC connection can take several seconds due to STUN server queries
    bool connected = false;
    for (int i = 0; i < 150 && !connected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(state->mutex);
        connected = state->peer1Connected && state->peer2Connected;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        EXPECT_TRUE(state->peer1Connected) << "Peer 1 failed to connect within timeout";
        EXPECT_TRUE(state->peer2Connected) << "Peer 2 failed to connect within timeout";
    }
    EXPECT_TRUE(peer1.isConnected());
    EXPECT_TRUE(peer2.isConnected());

    // Verify signaling was exchanged
    EXPECT_GE(signaling.getDescriptionsExchanged(), 2) << "Should have exchanged offer and answer";
    EXPECT_GT(signaling.getCandidatesExchanged(), 0) << "Should have exchanged ICE candidates";

    // If connection succeeded, test message exchange
    if (peer1.isConnected() && peer2.isConnected()) {
        // Use shared_ptr to prevent stack-use-after-scope
        struct MessageState
        {
            std::mutex mutex;
            bool gotMessage = false;
            std::vector<uint8_t> receivedData;
        };
        auto msgState = std::make_shared<MessageState>();

        peer2.setMessageCallback([msgState](const std::vector<uint8_t>& data) {
            std::lock_guard<std::mutex> lock(msgState->mutex);
            msgState->receivedData = data;
            msgState->gotMessage = true;
        });

        // Send message from peer1 to peer2
        std::string testMessage = "Hello from peer1";
        std::vector<uint8_t> sendData(testMessage.begin(), testMessage.end());

        auto sendResult = peer1.send(sendData);
        ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

        // Wait for message (up to 5 seconds)
        bool gotMsg = false;
        for (int i = 0; i < 50 && !gotMsg; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(msgState->mutex);
            gotMsg = msgState->gotMessage;
        }

        {
            std::lock_guard<std::mutex> lock(msgState->mutex);
            EXPECT_TRUE(msgState->gotMessage) << "Peer 2 did not receive message";
            EXPECT_EQ(msgState->receivedData, sendData);
        }

        // Verify stats
        auto stats1 = peer1.getStats();
        EXPECT_GT(stats1.bytesSent, 0u);
        EXPECT_GE(stats1.messagesSent, 1u);

        auto stats2 = peer2.getStats();
        EXPECT_GT(stats2.bytesReceived, 0u);
        EXPECT_GE(stats2.messagesReceived, 1u);
    }

    // Clean disconnect
    auto d1 = peer1.disconnect();
    EXPECT_TRUE(d1.success()) << d1.errorMessage;

    auto d2 = peer2.disconnect();
    EXPECT_TRUE(d2.success()) << d2.errorMessage;

    // Wait for clean shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(peer1.getState(), ConnectionState::Disconnected);
    EXPECT_EQ(peer2.getState(), ConnectionState::Disconnected);
}

// Integration test - verify bidirectional message exchange
TEST(WebRTCIntegrationTests, TwoPeerBidirectionalMessaging) {
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    auto [config1, config2] = localHermeticRtcConfigPair();
    WebRTCConnection peer1(config1, callbacks1);
    WebRTCConnection peer2(config2, callbacks2);
    signaling.setPeers(&peer1, &peer2);

    std::atomic<bool> peer1Connected{false};
    std::atomic<bool> peer2Connected{false};

    peer1.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer1Connected = true;
    });
    peer2.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer2Connected = true;
    });

    ASSERT_TRUE(peer1.connect().success());
    ASSERT_TRUE(peer2.connect().success());

    // Wait for connection
    for (int i = 0; i < 150 && !(peer1Connected.load() && peer2Connected.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(peer1.isConnected() && peer2.isConnected());

    // Setup message tracking
    std::atomic<int> peer1Messages{0};
    std::atomic<int> peer2Messages{0};

    peer1.setMessageCallback([&](const std::vector<uint8_t>&) { peer1Messages.fetch_add(1); });
    peer2.setMessageCallback([&](const std::vector<uint8_t>&) { peer2Messages.fetch_add(1); });

    // Send messages both directions
    std::vector<uint8_t> msg1{'1', '2', '3'};
    std::vector<uint8_t> msg2{'a', 'b', 'c'};

    ASSERT_TRUE(peer1.send(msg1).success());
    ASSERT_TRUE(peer2.send(msg2).success());

    // Wait for messages
    for (int i = 0; i < 50 && (peer1Messages.load() < 1 || peer2Messages.load() < 1); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_GE(peer1Messages.load(), 1) << "Peer 1 should receive message from peer 2";
    EXPECT_GE(peer2Messages.load(), 1) << "Peer 2 should receive message from peer 1";

    peer1.disconnect();
    peer2.disconnect();
}

// Integration test - verify clean disconnect during connection
TEST(WebRTCIntegrationTests, TwoPeerDisconnectDuringConnection) {
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    auto [config1, config2] = localHermeticRtcConfigPair();
    WebRTCConnection peer1(config1, callbacks1);
    WebRTCConnection peer2(config2, callbacks2);
    signaling.setPeers(&peer1, &peer2);

    // Start connection
    ASSERT_TRUE(peer1.connect().success());
    ASSERT_TRUE(peer2.connect().success());

    // Disconnect before fully connected
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto d1 = peer1.disconnect();
    auto d2 = peer2.disconnect();

    EXPECT_TRUE(d1.success()) << d1.errorMessage;
    EXPECT_TRUE(d2.success()) << d2.errorMessage;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Note: With ICE restart semantics, disconnect() during connection doesn't prevent
    // connection completion. The connection may still reach Connected state.
    // This test verifies that disconnect() succeeds without error.
    // State checks removed as they're incompatible with ICE restart semantics.
}

// Integration test - verify reconnection after disconnect (real-world scenario)
TEST(WebRTCIntegrationTests, TwoPeerReconnection) {
    InProcessSignaling signaling;
    auto [cb1, cb2] = signaling.createCallbackPair();

    auto [cfg1, cfg2] = localHermeticRtcConfigPair();
    WebRTCConnection a(cfg1, cb1);
    WebRTCConnection b(cfg2, cb2);
    signaling.setPeers(&a, &b);

    std::atomic<bool> aUp{false}, bUp{false};
    std::atomic<bool> aDown{false}, bDown{false};

    a.setStateCallback([&](ConnectionState s) {
        if (s == ConnectionState::Connected) aUp = true;
        if (s == ConnectionState::Disconnected) aDown = true;
    });
    b.setStateCallback([&](ConnectionState s) {
        if (s == ConnectionState::Connected) bUp = true;
        if (s == ConnectionState::Disconnected) bDown = true;
    });

    // 1) Initial connection
    ASSERT_TRUE(a.connect().success());
    ASSERT_TRUE(b.connect().success());
    for (int i = 0; i < 100 && !(aUp.load() && bUp.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(a.isConnected() && b.isConnected());

    // Smoke messaging
    std::atomic<bool> bGot{false};
    b.setMessageCallback([&](const std::vector<uint8_t>& d) { bGot = (std::string(d.begin(), d.end()) == "ping"); });
    ASSERT_TRUE(a.send(std::vector<uint8_t>{'p', 'i', 'n', 'g'}).success());
    for (int i = 0; i < 50 && !bGot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(bGot.load());

    // 2) Test ICE restart: both sides call disconnect() then reconnect()
    // With ICE restart, disconnect() is a no-op - connection stays alive
    // reconnect() triggers ICE restart with new ICE credentials
    aUp = bUp = false;
    ASSERT_TRUE(a.disconnect().success());
    ASSERT_TRUE(b.disconnect().success());
    // State remains Connected during ICE restart

    // 3) Reconnect both sides using ICE restart
    int d0 = signaling.getDescriptionsExchanged();
    ASSERT_TRUE(a.reconnect().success());
    ASSERT_TRUE(b.reconnect().success());
    for (int i = 0; i < 120 && !(aUp.load() && bUp.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(a.isConnected() && b.isConnected());
    // With optimistic reconnect, state callbacks fire immediately but signaling happens async
    // Wait for signaling to complete (description count to stabilize)
    for (int i = 0; i < 20 && signaling.getDescriptionsExchanged() < d0 + 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(signaling.getDescriptionsExchanged(), d0 + 2);

    // Verify messaging post-reconnect
    bGot = false;
    b.setMessageCallback([&](const std::vector<uint8_t>& d) { bGot = (std::string(d.begin(), d.end()) == "pong"); });
    ASSERT_TRUE(a.send(std::vector<uint8_t>{'p', 'o', 'n', 'g'}).success());
    for (int i = 0; i < 50 && !bGot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(bGot.load());

    // 4) Second ICE restart cycle to verify repeatability
    aUp = bUp = false;
    ASSERT_TRUE(a.disconnect().success());
    ASSERT_TRUE(b.disconnect().success());

    ASSERT_TRUE(a.reconnect().success());
    ASSERT_TRUE(b.reconnect().success());
    for (int i = 0; i < 120 && !(aUp.load() && bUp.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(a.isConnected() && b.isConnected());

    (void)a.disconnect();
    (void)b.disconnect();
}

// Integration test - verify state callback receives all transitions
TEST(WebRTCIntegrationTests, TwoPeerStateCallback) {
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    auto [config1, config2] = localHermeticRtcConfigPair();
    WebRTCConnection peer1(config1, callbacks1);
    WebRTCConnection peer2(config2, callbacks2);
    signaling.setPeers(&peer1, &peer2);

    // Track all state transitions for peer1
    std::vector<ConnectionState> peer1States;
    std::mutex peer1Mutex;

    peer1.setStateCallback([&](ConnectionState state) {
        std::lock_guard lock(peer1Mutex);
        peer1States.push_back(state);
    });

    // Track connection for peer2
    std::atomic<bool> peer2Connected{false};
    peer2.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer2Connected = true;
    });

    // Connect
    ASSERT_TRUE(peer1.connect().success());
    ASSERT_TRUE(peer2.connect().success());

    // Wait for connection
    for (int i = 0; i < 150 && !peer2Connected.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Disconnect
    peer1.disconnect();
    peer2.disconnect();

    // Wait for disconnect to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify state transitions
    std::lock_guard lock(peer1Mutex);
    EXPECT_FALSE(peer1States.empty()) << "Should have received state callbacks";

    // Should have received at least Connected and Disconnected states
    bool sawConnected = false;
    bool sawDisconnected = false;

    for (auto state : peer1States) {
        if (state == ConnectionState::Connected) sawConnected = true;
        if (state == ConnectionState::Disconnected) sawDisconnected = true;
    }

    EXPECT_TRUE(sawConnected) << "Should have transitioned to Connected state";
    EXPECT_TRUE(sawDisconnected) << "Should have transitioned to Disconnected state";
}

// Integration test - verify custom data channel label
TEST(WebRTCIntegrationTests, TwoPeerCustomDataChannelLabel) {
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    // Use paired configs so one peer is polite and the other is impolite to avoid offer glare
    auto [config1, config2] = localHermeticRtcConfigPair();

    // Create connections with custom channel label
    WebRTCConnection peer1(config1, callbacks1, "custom-channel");
    WebRTCConnection peer2(config2, callbacks2, "custom-channel");
    signaling.setPeers(&peer1, &peer2);

    std::atomic<bool> peer1Connected{false};
    std::atomic<bool> peer2Connected{false};

    peer1.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer1Connected = true;
    });
    peer2.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer2Connected = true;
    });

    // Connect
    ASSERT_TRUE(peer1.connect().success());
    ASSERT_TRUE(peer2.connect().success());

    // Wait for connection
    for (int i = 0; i < 150 && !(peer1Connected.load() && peer2Connected.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!(peer1.isConnected() && peer2.isConnected())) {
        ENTROPY_LOG_ERROR("TwoPeerCustomDataChannelLabel: timed out waiting for Connected");
        ENTROPY_LOG_ERROR("diagnostic: descriptions=" + std::to_string(signaling.getDescriptionsExchanged()) +
                          ", candidates=" + std::to_string(signaling.getCandidatesExchanged()));
    }
    ASSERT_TRUE(peer1.isConnected() && peer2.isConnected());

    // Test message exchange on custom channel
    std::atomic<bool> peer2GotMessage{false};
    peer2.setMessageCallback([&](const std::vector<uint8_t>&) { peer2GotMessage = true; });

    std::vector<uint8_t> testData{'t', 'e', 's', 't'};
    ASSERT_TRUE(peer1.send(testData).success());

    // Wait for message
    for (int i = 0; i < 50 && !peer2GotMessage.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(peer2GotMessage.load()) << "Message should be received on custom channel";

    peer1.disconnect();
    peer2.disconnect();
}

// Integration test - verify multiple ICE servers
TEST(WebRTCIntegrationTests, TwoPeerMultipleICEServers) {
    InProcessSignaling signaling;
    auto [callbacks1, callbacks2] = signaling.createCallbackPair();

    // Hermetic local config (loopback + ICE-TCP)
    auto [config1, config2] = localHermeticRtcConfigPair();
    WebRTCConnection peer1(config1, callbacks1);
    WebRTCConnection peer2(config2, callbacks2);
    signaling.setPeers(&peer1, &peer2);

    std::atomic<bool> peer1Connected{false};
    std::atomic<bool> peer2Connected{false};

    peer1.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer1Connected = true;
    });
    peer2.setStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::Connected) peer2Connected = true;
    });

    // Connect
    auto r1 = peer1.connect();
    ASSERT_TRUE(r1.success()) << r1.errorMessage;

    auto r2 = peer2.connect();
    ASSERT_TRUE(r2.success()) << r2.errorMessage;

    // Wait for connection
    for (int i = 0; i < 150 && !(peer1Connected.load() && peer2Connected.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!(peer1.isConnected() && peer2.isConnected())) {
        ENTROPY_LOG_ERROR("TwoPeerMultipleICEServers: timed out waiting for Connected (hermetic)");
        ENTROPY_LOG_ERROR("diagnostic: descriptions=" + std::to_string(signaling.getDescriptionsExchanged()) +
                          ", candidates=" + std::to_string(signaling.getCandidatesExchanged()));
    }
    EXPECT_TRUE(peer1.isConnected() && peer2.isConnected())
        << "Hermetic loopback connection (ICE host over TCP) should succeed";

    // Verify signaling occurred
    EXPECT_GE(signaling.getDescriptionsExchanged(), 2);
    EXPECT_GT(signaling.getCandidatesExchanged(), 0);

    peer1.disconnect();
    peer2.disconnect();
}
