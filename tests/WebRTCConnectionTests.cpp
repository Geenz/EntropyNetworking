/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/WebRTCConnection.h"

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

    EXPECT_EQ(connection.getType(), ConnectionType::Remote);
    EXPECT_EQ(connection.getState(), ConnectionState::Disconnected);
    EXPECT_FALSE(connection.isConnected());
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

TEST(WebRTCConnectionTests, ConnectionManagerIntegration) {
    ConnectionManager manager(64);

    WebRTCConfig config;
    config.iceServers = {"stun:stun.l.google.com:19302"};

    SignalingCallbacks callbacks;
    callbacks.onLocalDescription = [](const std::string& type, const std::string& sdp) {};
    callbacks.onLocalCandidate = [](const std::string& candidate, const std::string& mid) {};

    auto handle = manager.openRemoteConnection("", config, callbacks);
    ASSERT_TRUE(handle.valid());

    EXPECT_EQ(handle.getType(), ConnectionType::Remote);
    EXPECT_EQ(handle.getState(), ConnectionState::Disconnected);
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
