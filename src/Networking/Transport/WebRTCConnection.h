/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "NetworkConnection.h"
#include <rtc/rtc.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace EntropyEngine::Networking {

    /**
     * @brief WebRTC-based network connection using data channels
     *
     * Implements NetworkConnection interface using libdatachannel for WebRTC data channels.
     * Supports reliable and unreliable data transfer over WebRTC.
     *
     * Signaling must be handled externally via the SignalingCallbacks.
     */
    class WebRTCConnection : public NetworkConnection {
    public:
        /**
         * @brief Construct a new WebRTC connection
         * @param config WebRTC configuration (ICE servers, etc.)
         * @param signalingCallbacks Callbacks for sending signaling messages
         * @param dataChannelLabel Label for the data channel (default: "entropy-data")
         */
        WebRTCConnection(
            WebRTCConfig config,
            SignalingCallbacks signalingCallbacks,
            std::string dataChannelLabel = "entropy-data"
        );

        ~WebRTCConnection() override;

        // NetworkConnection interface
        Result<void> connect() override;
        Result<void> disconnect() override;
        bool isConnected() const override;

        Result<void> send(const std::vector<uint8_t>& data) override;
        Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
        Result<void> trySend(const std::vector<uint8_t>& data) override;

        ConnectionState getState() const override;
        ConnectionType getType() const override { return ConnectionType::Remote; }
        ConnectionStats getStats() const override;

        /**
         * @brief Set remote description received from signaling
         * @param type SDP type ("offer" or "answer")
         * @param sdp SDP string
         */
        Result<void> setRemoteDescription(const std::string& type, const std::string& sdp);

        /**
         * @brief Add remote ICE candidate received from signaling
         * @param candidate ICE candidate string
         * @param mid Media stream identification tag
         */
        Result<void> addRemoteCandidate(const std::string& candidate, const std::string& mid);

        /**
         * @brief Check if peer connection is created and ready for signaling
         */
        bool isReady() const;

    private:
        void setupPeerConnection();
        void setupDataChannel();
        void setupUnreliableDataChannel();
        void setupDataChannelCallbacks(int channelId, bool isReliable);

        // C API callback adapters (static with user pointer)
        static void onLocalDescriptionCallback(int pc, const char* sdp, const char* type, void* user);
        static void onLocalCandidateCallback(int pc, const char* cand, const char* mid, void* user);
        static void onStateChangeCallback(int pc, rtcState state, void* user);
        static void onDataChannelCallback(int pc, int dc, void* user);
        static void onOpenCallback(int id, void* user);
        static void onClosedCallback(int id, void* user);
        static void onMessageCallback(int id, const char* message, int size, void* user);

        WebRTCConfig _config;
        SignalingCallbacks _signalingCallbacks;
        std::string _dataChannelLabel;
        std::string _unreliableChannelLabel;

        // Using C API handles directly for proper blocking cleanup semantics
        // The C++ wrapper doesn't implement the blocking guarantee documented in the C API
        int _peerConnectionId = -1;
        int _dataChannelId = -1;
        int _unreliableDataChannelId = -1;

        std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
        std::atomic<bool> _destroying{false};
        std::atomic<bool> _makingOffer{false};
        bool _polite = false;
        mutable std::mutex _mutex;
        ConnectionStats _stats;
    };

} // namespace EntropyEngine::Networking
