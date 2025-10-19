/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <memory>
#include <mutex>
#include <atomic>

namespace EntropyEngine::Networking::Testing {

/**
 * @brief Helper for in-process WebRTC signaling between two peers
 *
 * Wires up SignalingCallbacks to automatically exchange SDP offers/answers
 * and ICE candidates between two WebRTCConnection instances in the same process.
 * This eliminates the need for an external signaling server in tests.
 *
 * Usage:
 * @code
 * InProcessSignaling signaling;
 * auto [callbacks1, callbacks2] = signaling.createCallbackPair();
 *
 * WebRTCConnection peer1(config, callbacks1);
 * WebRTCConnection peer2(config, callbacks2);
 * signaling.setPeers(&peer1, &peer2);
 *
 * peer1.connect(); // Generates offer
 * peer2.connect(); // Generates answer
 * // Signaling exchanged automatically
 * @endcode
 */
class InProcessSignaling {
public:
    InProcessSignaling() = default;

    /**
     * @brief Set the peer connections for signaling exchange
     * @param peer1 First peer connection
     * @param peer2 Second peer connection
     */
    void setPeers(WebRTCConnection* peer1, WebRTCConnection* peer2) {
        std::lock_guard lock(_mutex);
        _peer1 = peer1;
        _peer2 = peer2;
    }

    /**
     * @brief Create paired signaling callbacks for two peers
     * @param politeFirst If true, peer1 is polite; otherwise peer2 is polite
     * @return Pair of callbacks for peer1 and peer2
     */
    std::pair<SignalingCallbacks, SignalingCallbacks> createCallbackPair(bool politeFirst = true) {
        _politeFirst = politeFirst;

        SignalingCallbacks callbacks1;
        callbacks1.onLocalDescription = [this](const std::string& type, const std::string& sdp) {
            handlePeer1LocalDescription(type, sdp);
        };
        callbacks1.onLocalCandidate = [this](const std::string& candidate, const std::string& mid) {
            handlePeer1LocalCandidate(candidate, mid);
        };

        SignalingCallbacks callbacks2;
        callbacks2.onLocalDescription = [this](const std::string& type, const std::string& sdp) {
            handlePeer2LocalDescription(type, sdp);
        };
        callbacks2.onLocalCandidate = [this](const std::string& candidate, const std::string& mid) {
            handlePeer2LocalCandidate(candidate, mid);
        };

        return {callbacks1, callbacks2};
    }

    /**
     * @brief Get whether peer1 should be polite
     */
    bool isPeer1Polite() const { return _politeFirst; }

    /**
     * @brief Get number of descriptions exchanged
     */
    int getDescriptionsExchanged() const {
        return _descriptionsExchanged.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get number of candidates exchanged
     */
    int getCandidatesExchanged() const {
        return _candidatesExchanged.load(std::memory_order_relaxed);
    }

private:
    void handlePeer1LocalDescription(const std::string& type, const std::string& sdp) {
        std::lock_guard lock(_mutex);
        if (_peer2) {
            _peer2->setRemoteDescription(type, sdp);
            _descriptionsExchanged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void handlePeer1LocalCandidate(const std::string& candidate, const std::string& mid) {
        std::lock_guard lock(_mutex);
        if (_peer2) {
            _peer2->addRemoteCandidate(candidate, mid);
            _candidatesExchanged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void handlePeer2LocalDescription(const std::string& type, const std::string& sdp) {
        std::lock_guard lock(_mutex);
        if (_peer1) {
            _peer1->setRemoteDescription(type, sdp);
            _descriptionsExchanged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void handlePeer2LocalCandidate(const std::string& candidate, const std::string& mid) {
        std::lock_guard lock(_mutex);
        if (_peer1) {
            _peer1->addRemoteCandidate(candidate, mid);
            _candidatesExchanged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::mutex _mutex;
    WebRTCConnection* _peer1 = nullptr;
    WebRTCConnection* _peer2 = nullptr;
    std::atomic<int> _descriptionsExchanged{0};
    std::atomic<int> _candidatesExchanged{0};
    bool _politeFirst = true;
};

// Hermetic local WebRTC config for CI: loopback-only, ICE-TCP enabled, bounded port range
inline WebRTCConfig localHermeticRtcConfig(bool polite = false) {
    WebRTCConfig cfg;
    cfg.iceServers = {};               // No external STUN
    cfg.bindAddress = "127.0.0.1";   // Loopback only
    cfg.enableIceTcp = true;           // Robust on CI/Windows
    cfg.portRangeBegin = 40000;        // Predictable narrow range
    cfg.portRangeEnd   = 40100;
    cfg.maxMessageSize = 256 * 1024;   // Match default
    cfg.polite = polite;               // Perfect negotiation role
    return cfg;
}

// Helper to create paired configs with one polite peer
inline std::pair<WebRTCConfig, WebRTCConfig> localHermeticRtcConfigPair(bool politeFirst = true) {
    return {localHermeticRtcConfig(politeFirst), localHermeticRtcConfig(!politeFirst)};
}

} // namespace EntropyEngine::Networking::Testing
