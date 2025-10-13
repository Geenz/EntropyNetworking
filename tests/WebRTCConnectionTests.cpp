/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Transport/ConnectionManager.h"
#include <thread>
#include <chrono>

using namespace EntropyEngine::Networking;

TEST(WebRTCConnectionTests, CreateConnection) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {
        // Would normally send to signaling server
    };
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {
        // Would normally send to signaling server
    };

    WebRTCConnection connection(config, callbacks);

    EXPECT_EQ(connection.getType(), ConnectionType::WebRTC);
    EXPECT_EQ(connection.getState(), ConnectionState::Disconnected);
    EXPECT_FALSE(connection.isConnected());
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_Connect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {
        // Callback is working
    };
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {
        // Callback is working
    };

    WebRTCConnection connection(config, callbacks);

    auto result = connection.connect();
    EXPECT_TRUE(result.success());
    EXPECT_TRUE(connection.isReady());

    // Note: We don't wait for actual ICE gathering in unit tests
    // That would be an integration test
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_MultipleConnect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    auto result1 = connection.connect();
    EXPECT_TRUE(result1.success());

    // Second connect should fail - already active
    auto result2 = connection.connect();
    EXPECT_TRUE(result2.failed());
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_Disconnect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    auto connectResult = connection.connect();
    ASSERT_TRUE(connectResult.success());

    auto disconnectResult = connection.disconnect();
    EXPECT_TRUE(disconnectResult.success());
    EXPECT_EQ(connection.getState(), ConnectionState::Disconnected);
}

TEST(WebRTCConnectionTests, SendBeforeConnect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    std::vector<uint8_t> data = {1, 2, 3, 4};
    auto result = connection.send(data);

    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.error, NetworkError::ConnectionClosed);
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_StateCallback) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    bool stateChanged = false;
    ConnectionState receivedState = ConnectionState::Disconnected;

    connection.setStateCallback([&stateChanged, &receivedState](ConnectionState state) {
        stateChanged = true;
        receivedState = state;
    });

    connection.connect();
    connection.disconnect();

    // Disconnect should have triggered the callback
    EXPECT_TRUE(stateChanged);
    EXPECT_EQ(receivedState, ConnectionState::Disconnected);
}

TEST(WebRTCConnectionTests, ConnectionManagerIntegration) {
    ConnectionManager manager;

    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    auto result = manager.createWebRTCConnection(config, callbacks);
    ASSERT_TRUE(result.success());

    ConnectionId id = result.value;
    auto* connection = manager.getConnection(id);

    ASSERT_NE(connection, nullptr);
    EXPECT_EQ(connection->getType(), ConnectionType::WebRTC);
    EXPECT_EQ(connection->getState(), ConnectionState::Disconnected);

    connection->release();
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_CustomDataChannelLabel) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks, "custom-channel");

    auto result = connection.connect();
    EXPECT_TRUE(result.success());
}

// Integration test - requires network and takes time
TEST(WebRTCConnectionTests, DISABLED_MultipleICEServers) {
    WebRTCConfig config;
    config.iceServers = {
        "stun:stun.l.google.com:19302",
        "stun:stun1.l.google.com:19302",
        "stun:stun2.l.google.com:19302"
    };

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    auto result = connection.connect();
    EXPECT_TRUE(result.success());
}

TEST(WebRTCConnectionTests, SetRemoteDescriptionBeforeConnect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    // Should fail - peer connection not initialized
    auto result = connection.setRemoteDescription("offer", "fake-sdp");
    EXPECT_TRUE(result.failed());
}

TEST(WebRTCConnectionTests, AddRemoteCandidateBeforeConnect) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    // Should fail - peer connection not initialized
    auto result = connection.addRemoteCandidate("fake-candidate", "0");
    EXPECT_TRUE(result.failed());
}

TEST(WebRTCConnectionTests, GetStats) {
    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    WebRTCConnection connection(config, callbacks);

    auto stats = connection.getStats();
    EXPECT_EQ(stats.bytesSent, 0);
    EXPECT_EQ(stats.bytesReceived, 0);
    EXPECT_EQ(stats.messagesSent, 0);
    EXPECT_EQ(stats.messagesReceived, 0);
}
